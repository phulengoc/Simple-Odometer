#!/usr/bin/env python3
"""
Nav Telemetry Sender for ESP32  —  Turn-by-turn HUD test
========================================================
Sends synthetic turn-by-turn telemetry packets to the ESP32 so the HUD can be
exercised without the iOS app or BLE. Mirrors the iOS NavTelemetry wire format
and the firmware parser in `main/nav_hud.{h,cpp}`.

Wire format (little-endian), 24-byte header + two UTF-8 strings:

    off sz  field
    0   4   magic 'M','N','A','V'
    4   1   version (= 1)
    5   1   flags (bit0 navigating, bit1 rerouting, bit2 arrived)
    6   1   maneuverType      (0 turn,1 depart,2 arrive,3 roundabout,
                               4 merge,5 onRamp,6 offRamp,7 fork,8 continue)
    7   1   maneuverModifier  (0 straight,1 slightR,2 right,3 sharpR,
                               4 slightL,5 left,6 sharpL,7 uTurn)
    8   4   distanceToManeuver_m  (uint32)
    12  4   distanceRemaining_m   (uint32)
    16  4   durationRemaining_s   (uint32)
    20  1   roundaboutExit (0 = n/a)
    21  1   primaryLen
    22  1   secondaryLen
    23  1   reserved (0)
    24  ..  primaryText, secondaryText

Usage
-----
  # Replay a scripted route to a fixed ESP32 IP
  python3 tests/nav_telemetry_sender.py --esp-ip 192.168.1.100

  # Discover the ESP32 IP from its HELLO beacon (port 5001), then stream
  python3 tests/nav_telemetry_sender.py

  # One-shot single packet (handy for a quick check)
  python3 tests/nav_telemetry_sender.py --esp-ip 192.168.1.100 --once

Dependencies: standard library only.
"""

import argparse
import socket
import struct
import sys
import time

NAV_PORT = 5002
DISCOVER_PORT = 5001
HEADER = struct.Struct("<4sBBBBIIIBBBB")  # 24 bytes

# maneuverType
TURN, DEPART, ARRIVE, ROUNDABOUT, MERGE, ON_RAMP, OFF_RAMP, FORK, CONTINUE = range(9)
# maneuverModifier
STRAIGHT, SLIGHT_R, RIGHT, SHARP_R, SLIGHT_L, LEFT, SHARP_L, UTURN = range(8)
# flags
NAVIGATING, REROUTING, ARRIVED, METRIC = 0x01, 0x02, 0x04, 0x08

# Set by main() from --imperial; OR'd into every navigating packet.
UNIT_FLAG = METRIC


def pack(maneuver, modifier, dist_to, dist_remain, dur_remain,
         primary="", secondary="", flags=NAVIGATING, roundabout_exit=0):
    if flags & NAVIGATING:
        flags |= UNIT_FLAG
    p = primary.encode("utf-8")[:47]
    s = secondary.encode("utf-8")[:47]
    header = HEADER.pack(
        b"MNAV", 1, flags, maneuver, modifier,
        int(dist_to), int(dist_remain), int(dur_remain),
        roundabout_exit, len(p), len(s), 0,
    )
    return header + p + s


# A scripted demo route: (hold_seconds, packet) tuples.
def demo_route():
    return [
        (3, pack(DEPART,   STRAIGHT, 0,    4200, 720, "Damrak")),
        (3, pack(TURN,     RIGHT,    300,  4200, 720, "Prins Hendrikkade")),
        (3, pack(TURN,     RIGHT,    120,  3900, 660, "Prins Hendrikkade")),
        (3, pack(ROUNDABOUT, RIGHT,  800,  3500, 600, "Nieuwezijds Voorburgwal", roundabout_exit=2)),
        (3, pack(TURN,     LEFT,     250,  2400, 420, "Raadhuisstraat")),
        (2, pack(TURN,     STRAIGHT, 0,    2200, 400, "", flags=NAVIGATING | REROUTING)),
        (3, pack(MERGE,    SLIGHT_R, 1500, 2000, 360, "S100")),
        (3, pack(TURN,     SHARP_L,  90,   600,  120, "Marnixstraat")),
        (3, pack(TURN,     UTURN,    150,  500,  100, "Marnixstraat")),
        (3, pack(ROUNDABOUT, RIGHT,  300,  300,  60,  "Nassaukade", roundabout_exit=1)),
        (3, pack(ARRIVE,   STRAIGHT, 40,   40,   10,  "Destination", flags=NAVIGATING | ARRIVED)),
    ]


def discover_esp_ip(timeout=15.0):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", DISCOVER_PORT))
    sock.settimeout(timeout)
    print(f"[telem] waiting for HELLO on :{DISCOVER_PORT} (≤{timeout:.0f}s)…")
    try:
        _data, addr = sock.recvfrom(64)
        print(f"[telem] discovered ESP32 at {addr[0]}")
        return addr[0]
    except socket.timeout:
        return None
    finally:
        sock.close()


def main():
    ap = argparse.ArgumentParser(description="Synthetic turn-by-turn telemetry sender for ESP32")
    ap.add_argument("--esp-ip", help="ESP32 IP (skip HELLO discovery)")
    ap.add_argument("--once", action="store_true", help="send a single packet and exit")
    ap.add_argument("--loop", action="store_true", help="repeat the demo route forever")
    ap.add_argument("--imperial", action="store_true", help="send mi/ft units instead of km/m")
    args = ap.parse_args()

    global UNIT_FLAG
    UNIT_FLAG = 0 if args.imperial else METRIC

    esp_ip = args.esp_ip or discover_esp_ip()
    if not esp_ip:
        print("[telem] no ESP32 discovered — pass --esp-ip <IP>", file=sys.stderr)
        return 1

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (esp_ip, NAV_PORT)
    print(f"[telem] sending telemetry to {esp_ip}:{NAV_PORT}")

    if args.once:
        sock.sendto(pack(TURN, RIGHT, 300, 4200, 720, "Main Street"), dest)
        print("[telem] sent one packet")
        return 0

    route = demo_route()
    while True:
        for hold, packet in route:
            # Resend at ~2 Hz while holding each step so the HUD ETA stays fresh.
            end = time.time() + hold
            while time.time() < end:
                sock.sendto(packet, dest)
                time.sleep(0.5)
        if not args.loop:
            break
    print("[telem] route complete")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n[telem] stopped")
