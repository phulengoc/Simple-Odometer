#ifndef STREAM_CONFIG_H
#define STREAM_CONFIG_H

// WiFi Configuration
#define WIFI_SSID           "Hazenut"
#define WIFI_PASS           "19101998"

// iOS host IP — used for HELLO discovery packets
// L1 test : set to the computer IP running tests/udp_test_sender.py
// L2 test : set to the iOS phone / simulator IP (visible in stream status)
#define STREAM_SERVER_IP    "192.168.1.236"   // Mac running iOS Simulator (MapNavSwiftUI)

// UDP port configuration
#define UDP_STREAM_PORT      5000   // ESP32 binds here; iOS/Python sends JPEG frames here
#define UDP_DISCOVER_PORT    5001   // iOS/Python binds here; ESP32 sends HELLO packets here

// UDP receive buffer — slightly larger than max send chunk (1400 B)
#define UDP_MAX_PACKET       1500

// Interval between HELLO packets (ms)
#define UDP_HELLO_INTERVAL   1000

// Display configuration — matches iOS NavigationFrameCapture outputSide
#define STREAM_WIDTH        466
#define STREAM_HEIGHT       466

// Maximum JPEG frame size in bytes
// iOS map at 466×466 @ quality 0.65 can reach ~90 KB; 128 KB gives ample headroom.
#define MAX_JPEG_FRAME_SIZE (128 * 1024)

// Delay between task restarts on error (ms)
#define RECONNECT_DELAY_MS  2000

#endif // STREAM_CONFIG_H
