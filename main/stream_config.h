#ifndef STREAM_CONFIG_H
#define STREAM_CONFIG_H

// WiFi Configuration
#define WIFI_SSID           "Hazenut"
#define WIFI_PASS           "19101998"

// iOS host IP — FALLBACK used only when BLE pairing has not yet delivered an IP.
// Once the MapNav iOS app connects over BLE and writes its IP, g_ios_ip in
// ble_pairing.h takes precedence and this value is ignored.
// Set to your Mac/iPhone IP for development without BLE (L1/L2 bench testing).
#define STREAM_SERVER_IP    "192.168.1.236"

// BLE peripheral device name — must be short enough to fit in a 31-byte ADV packet
// alongside the Flags field (3 bytes) and name header (2 bytes): max 26 chars.
#define BLE_DEVICE_NAME     "ESP32-MapNav"

// UDP port configuration
#define UDP_STREAM_PORT      5000   // ESP32 binds here; iOS/Python sends JPEG frames here
#define UDP_DISCOVER_PORT    5001   // iOS/Python binds here; ESP32 sends HELLO packets here

// UDP receive buffer — slightly larger than max send chunk (1400 B)
#define UDP_MAX_PACKET       1500

// Interval between HELLO packets (ms)
#define UDP_HELLO_INTERVAL   1000

// Streamed frame size — must match iOS NavigationFrameCapture.outputSide.
// Smaller than the 466×466 panel; the receiver centers it (panel can't upscale).
#define STREAM_WIDTH        400
#define STREAM_HEIGHT       400

// Maximum JPEG frame size in bytes
// iOS map at 400×400 @ quality 0.65 stays well under this; 128 KB is ample headroom.
#define MAX_JPEG_FRAME_SIZE (128 * 1024)

// Delay between task restarts on error (ms)
#define RECONNECT_DELAY_MS  2000

#endif // STREAM_CONFIG_H
