#!/usr/bin/env python3
"""
UDP JPEG Stream Sender for ESP32  —  L1 Test
=============================================
Mimics the iOS UDPStreamSender exactly:

  • Listens on UDP port 5001 for HELLO packets from ESP32
  • Discovers the ESP32 IP from the first HELLO received
    (use --esp-ip <IP> to skip discovery and send immediately)
  • Sends JPEG frames at the target FPS to ESP32:5000
  • Frame wire format:
      SOI  packet  :  0xFF 0xD8         (exactly 2 bytes)
      DATA packets :  raw JPEG bytes    (up to 1400 B each)
      EOI  packet  :  0xFF 0xD9         (exactly 2 bytes)

Usage
-----
  # Default: discover ESP32 via HELLO, stream TestJPG.jpg at 15 fps
  python3 tests/udp_test_sender.py

  # Fixed ESP32 IP, custom rate
  python3 tests/udp_test_sender.py --esp-ip 192.168.1.100 --fps 20

  # Custom image
  python3 tests/udp_test_sender.py --image path/to/frame.jpg --fps 15

  # Run alongside the Python HELLO simulator (no physical ESP32)
  python3 tests/udp_test_sender.py --esp-ip 127.0.0.1

After starting the sender, flash the ESP32 (or run the test HELLO helper).
The display should show the streamed image once HELLO packets flow.

Dependencies
------------
  Standard library only (socket, threading, time, argparse, os, sys).
  No pip packages required.
"""

import argparse
import os
import socket
import sys
import threading
import time

DISCOVER_PORT = 5001   # iOS/we bind here; ESP32 sends HELLO to us
STREAM_PORT   = 5000   # ESP32 binds here; we send frames here
CHUNK_SIZE    = 1400   # max bytes per UDP DATA packet (matches kChunkSize in Swift)

SOI = b"\xff\xd8"
EOI = b"\xff\xd9"


# ── JPEG helpers ──────────────────────────────────────────────────────────────

def load_jpeg(path: str) -> bytes:
    """Load a JPEG file and verify it starts with FF D8."""
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:2] != b"\xff\xd8":
        raise ValueError(f"Not a JPEG file: {path}")
    return data


def jpeg_to_packets(jpeg: bytes) -> list:
    """Split a JPEG into [SOI] + [DATA chunks...] + [EOI]."""
    packets = [SOI]
    offset = 0
    while offset < len(jpeg):
        end = min(offset + CHUNK_SIZE, len(jpeg))
        packets.append(jpeg[offset:end])
        offset = end
    packets.append(EOI)
    return packets


# ── Discovery listener ────────────────────────────────────────────────────────

class DiscoveryListener(threading.Thread):
    """
    Listens on UDP DISCOVER_PORT for HELLO packets from the ESP32.
    Records source IPs and their last-seen timestamps.
    """

    CLIENT_TTL = 5.0   # seconds

    def __init__(self, sock):
        super().__init__(daemon=True)
        self._sock = sock
        self._clients = {}          # ip → last-seen monotonic timestamp
        self._lock = threading.Lock()
        self._new_client_event = threading.Event()

    def run(self):
        while True:
            try:
                _, addr = self._sock.recvfrom(64)
                ip = addr[0]
                with self._lock:
                    is_new = ip not in self._clients
                    self._clients[ip] = time.monotonic()
                if is_new:
                    print(f"[HELLO] ESP32 discovered at {ip}")
                    self._new_client_event.set()
            except OSError:
                break

    def wait_for_client(self, timeout: float = 60.0) -> bool:
        """Block until at least one HELLO is received (or timeout)."""
        return self._new_client_event.wait(timeout)

    def active_ips(self) -> list:
        cutoff = time.monotonic() - self.CLIENT_TTL
        with self._lock:
            return [ip for ip, ts in self._clients.items() if ts >= cutoff]


# ── Sender ────────────────────────────────────────────────────────────────────

def run_sender(esp_ip, jpeg, fps, discover_port, stream_port):
    """Main send loop."""
    packets = jpeg_to_packets(jpeg)
    frame_interval = 1.0 / fps
    total_packet_bytes = sum(len(p) for p in packets)

    # ── Discovery socket (we receive HELLO here) ──────────────────────────────
    disc_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    disc_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        disc_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    disc_sock.bind(("0.0.0.0", discover_port))
    disc_sock.settimeout(1.0)

    # ── Send socket ───────────────────────────────────────────────────────────
    send_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    listener = DiscoveryListener(disc_sock)
    listener.start()

    print()
    print("=" * 60)
    print("  UDP Test Sender  (L1 — no iOS app needed)")
    print("=" * 60)
    print(f"  Listening for HELLO  : 0.0.0.0:{discover_port}")
    if esp_ip:
        print(f"  Sending to (fixed)   : {esp_ip}:{stream_port}")
    else:
        print(f"  Sending to (auto)    : <discovered via HELLO>:{stream_port}")
    print(f"  Image                : {len(jpeg):,} bytes  ({len(jpeg)/1024:.1f} KB)")
    print(f"  Packets/frame        : {len(packets)} "
          f"({len(packets)-2} data + 1 SOI + 1 EOI)")
    print(f"  Frame rate           : {fps} fps")
    print()
    print("  Press Ctrl-C to stop.")
    print("=" * 60)
    print()

    # Pre-seed listener with the fixed IP so active_ips() returns it immediately
    if esp_ip:
        with listener._lock:
            listener._clients[esp_ip] = time.monotonic() + 1e9   # never expires

    # If auto-discover, wait for first HELLO before starting
    if not esp_ip:
        print("[!] Waiting for HELLO from ESP32 (up to 60 s)...")
        if not listener.wait_for_client(60.0):
            print("[!] No HELLO received in 60 s — check ESP32 and STREAM_SERVER_IP in stream_config.h")
            disc_sock.close()
            send_sock.close()
            return

    frame_count = 0
    t_start = time.monotonic()

    try:
        while True:
            t0 = time.monotonic()

            targets = [esp_ip] if esp_ip else listener.active_ips()
            if not targets:
                print("[!] No active ESP32 (no HELLO for >5 s) — waiting...")
                time.sleep(1.0)
                continue

            for ip in targets:
                for pkt in packets:
                    send_sock.sendto(pkt, (ip, stream_port))

            frame_count += 1
            if frame_count % 30 == 0:
                elapsed = time.monotonic() - t_start
                fps_actual = frame_count / elapsed if elapsed > 0 else 0
                throughput = (total_packet_bytes * frame_count) / elapsed / 1024 if elapsed > 0 else 0
                print(
                    f"  frame {frame_count:>5}  "
                    f"{fps_actual:>5.1f} fps  "
                    f"{throughput:>7.1f} KB/s  "
                    f"targets={targets}"
                )

            sleep = frame_interval - (time.monotonic() - t0)
            if sleep > 0:
                time.sleep(sleep)

    except KeyboardInterrupt:
        print("\n[!] Stopping...")
    finally:
        disc_sock.close()
        send_sock.close()
        elapsed = time.monotonic() - t_start
        fps_avg = frame_count / elapsed if elapsed > 0 else 0
        print(f"  Done — {frame_count} frames, {fps_avg:.1f} fps avg")


# ── Entry point ───────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="UDP test sender for ESP32 (L1 test — no iOS app required)"
    )
    parser.add_argument(
        "--esp-ip", default=None,
        help="ESP32 IP to send to (default: auto-discover via HELLO packets)"
    )
    parser.add_argument(
        "--discover-port", type=int, default=DISCOVER_PORT,
        help=f"UDP port to listen for HELLO on (default: {DISCOVER_PORT})"
    )
    parser.add_argument(
        "--stream-port", type=int, default=STREAM_PORT,
        help=f"UDP port to send frames to on ESP32 (default: {STREAM_PORT})"
    )
    parser.add_argument("--fps", type=float, default=15.0,
                        help="Frame rate (default: 15)")
    parser.add_argument(
        "--image", default=None,
        help="JPEG to stream (default: Test_IMG_SRC/TestJPG.jpg)"
    )
    args = parser.parse_args()

    if args.image:
        img_path = args.image
    else:
        script_dir = os.path.dirname(os.path.abspath(__file__))
        img_path = os.path.normpath(
            os.path.join(script_dir, "..", "Test_IMG_SRC", "TestJPG.jpg")
        )

    if not os.path.exists(img_path):
        print(f"Error: image not found: {img_path}", file=sys.stderr)
        print("Pass --image <path-to-jpeg> to specify a different file.", file=sys.stderr)
        sys.exit(1)

    print(f"Loading: {img_path}")
    jpeg = load_jpeg(img_path)

    run_sender(
        esp_ip=args.esp_ip,
        jpeg=jpeg,
        fps=args.fps,
        discover_port=args.discover_port,
        stream_port=args.stream_port,
    )


if __name__ == "__main__":
    main()
