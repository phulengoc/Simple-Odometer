#!/usr/bin/env python3
"""
MJPEG Streaming Test Server for ESP32 JPEG Stream Receiver  —  L1 Test
=======================================================================
Mimics the iOS MapNavSwiftUI MJPEGStreamingServer exactly:

  HTTP/1.1 200 OK
  Content-Type: multipart/x-mixed-replace; boundary=MapNavFrame
  ...
  --MapNavFrame\r\n
  Content-Type: image/jpeg\r\n
  Content-Length: N\r\n
  \r\n
  <N JPEG bytes>\r\n
  (repeats forever)

Usage
-----
  # Default: stream TestJPG.jpg at 15 fps on port 8888
  python3 tests/mjpeg_test_server.py

  # Custom image / rate
  python3 tests/mjpeg_test_server.py --fps 20 --image Test_IMG_SRC/TestJPG.jpg

  # Match iOS server port and FPS
  python3 tests/mjpeg_test_server.py --port 8888 --fps 30

After the server starts, update stream_config.h:
  #define STREAM_SERVER_IP  "<this machine IP>"
  #define STREAM_SERVER_PORT 8888

Then build, flash, and monitor the ESP32.  The display should show the test image.

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

BOUNDARY = "MapNavFrame"


# ── JPEG helpers ─────────────────────────────────────────────────────────────

def load_jpeg(path: str) -> bytes:
    """Load a JPEG file and verify it starts with FF D8."""
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:2] != b"\xff\xd8":
        raise ValueError(f"Not a JPEG file: {path}")
    return data


def build_mjpeg_frame(jpeg_bytes: bytes) -> bytes:
    """
    Wrap a JPEG buffer in one MJPEG multipart frame exactly as iOS does:
      --MapNavFrame\r\n
      Content-Type: image/jpeg\r\n
      Content-Length: N\r\n\r\n
      <jpeg bytes>\r\n
    """
    header = (
        f"--{BOUNDARY}\r\n"
        f"Content-Type: image/jpeg\r\n"
        f"Content-Length: {len(jpeg_bytes)}\r\n"
        f"\r\n"
    ).encode("ascii")
    return header + jpeg_bytes + b"\r\n"


# ── Client handler ────────────────────────────────────────────────────────────

def handle_client(
    conn: socket.socket,
    addr: tuple,
    frame_packet: bytes,
    fps: float,
    stop_event: threading.Event,
) -> None:
    """Stream MJPEG frames to one connected client until it disconnects."""
    tag = f"{addr[0]}:{addr[1]}"
    print(f"[+] {tag}  connected")

    frame_interval = 1.0 / fps
    frame_count = 0
    t_start = time.monotonic()

    try:
        # ── Drain the incoming HTTP request ───────────────────────────────────
        conn.settimeout(5.0)
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = conn.recv(2048)
            if not chunk:
                return
            buf += chunk

        # ── Send HTTP response headers ────────────────────────────────────────
        http_headers = (
            "HTTP/1.1 200 OK\r\n"
            f"Content-Type: multipart/x-mixed-replace; boundary={BOUNDARY}\r\n"
            "Cache-Control: no-cache, no-store, must-revalidate\r\n"
            "Connection: keep-alive\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n"
        ).encode("ascii")
        conn.sendall(http_headers)

        conn.settimeout(15.0)

        # ── Streaming loop ────────────────────────────────────────────────────
        while not stop_event.is_set():
            t0 = time.monotonic()

            conn.sendall(frame_packet)
            frame_count += 1

            if frame_count % 30 == 0:
                elapsed = time.monotonic() - t_start
                fps_actual = frame_count / elapsed if elapsed > 0 else 0
                throughput = (len(frame_packet) * frame_count) / elapsed / 1024 if elapsed > 0 else 0
                print(
                    f"    [{tag}] frame {frame_count:>5}  "
                    f"{fps_actual:>5.1f} fps  {throughput:>7.1f} KB/s"
                )

            sleep = frame_interval - (time.monotonic() - t0)
            if sleep > 0:
                time.sleep(sleep)

    except (BrokenPipeError, ConnectionResetError, OSError) as exc:
        print(f"[-] {tag}  disconnected ({exc})")
    finally:
        conn.close()
        elapsed = time.monotonic() - t_start
        fps_avg = frame_count / elapsed if elapsed > 0 else 0
        print(f"[-] {tag}  done — {frame_count} frames, {fps_avg:.1f} fps avg")


# ── Server ────────────────────────────────────────────────────────────────────

def run_server(host: str, port: int, jpeg_bytes: bytes, fps: float) -> None:
    frame_packet = build_mjpeg_frame(jpeg_bytes)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # SO_REUSEPORT is needed on macOS to reclaim a port from TIME_WAIT
    if hasattr(socket, "SO_REUSEPORT"):
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    try:
        srv.bind((host, port))
    except OSError as exc:
        if exc.errno == 48:  # EADDRINUSE (macOS) / 98 on Linux
            print(f"\nError: port {port} is already in use.")
            print("Possible causes:")
            print(f"  • The iOS MapNavSwiftUI app is running (it binds port {port} for MJPEG).")
            print(f"    → Stop the iOS app OR use --port 8889 for this test server.")
            print(f"      (and update STREAM_SERVER_PORT in stream_config.h accordingly)")
            print(f"  • A previous mjpeg_test_server.py is still running.")
            print(f"    → Kill it:  lsof -ti :{port} | xargs kill")
        else:
            print(f"\nError binding to {host}:{port}: {exc}")
        srv.close()
        sys.exit(1)
    srv.listen(5)
    srv.settimeout(1.0)

    stop_event = threading.Event()

    # Print the machine IPs so the user knows which one to put in stream_config.h
    import socket as _sock
    hostname = _sock.gethostname()
    try:
        local_ip = _sock.gethostbyname(hostname)
    except Exception:
        local_ip = "unknown"

    print()
    print("=" * 60)
    print(f"  MJPEG Test Server  (L1 — no iOS app needed)")
    print("=" * 60)
    print(f"  Listening  : {host or '0.0.0.0'}:{port}")
    print(f"  Machine IP : {local_ip}  (use this in stream_config.h)")
    print(f"  Image      : {len(jpeg_bytes):,} bytes  ({len(jpeg_bytes)/1024:.1f} KB)")
    print(f"  Frame rate : {fps} fps")
    print()
    print("  ESP32 stream_config.h must have:")
    print(f'    #define STREAM_SERVER_IP   "{local_ip}"')
    print(f'    #define STREAM_SERVER_PORT  {port}')
    print()
    print("  Press Ctrl-C to stop.")
    print("=" * 60)
    print()

    try:
        while True:
            try:
                conn, addr = srv.accept()
            except socket.timeout:
                continue
            t = threading.Thread(
                target=handle_client,
                args=(conn, addr, frame_packet, fps, stop_event),
                daemon=True,
            )
            t.start()
    except KeyboardInterrupt:
        print("\n[!] Shutting down...")
        stop_event.set()
    finally:
        srv.close()


# ── Entry point ───────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="MJPEG test server for ESP32 (L1 test — no iOS app required)"
    )
    parser.add_argument("--host", default="0.0.0.0", help="Bind address (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=8888, help="TCP port (default: 8888)")
    parser.add_argument("--fps", type=float, default=15.0, help="Frame rate (default: 15)")
    parser.add_argument(
        "--image",
        default=None,
        help="JPEG image to stream (default: Test_IMG_SRC/TestJPG.jpg)",
    )
    args = parser.parse_args()

    # Resolve the default test image relative to this script
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
    jpeg_bytes = load_jpeg(img_path)

    run_server(args.host, args.port, jpeg_bytes, args.fps)


if __name__ == "__main__":
    main()
