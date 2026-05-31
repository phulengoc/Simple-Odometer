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
#define NAV_TELEMETRY_PORT   5002   // ESP32 binds here; iOS/Python sends turn-by-turn telemetry here

// Circular panel geometry ────────────────────────────────────────────────────
// The 466×466 AMOLED is ROUND: only the inscribed circle is lit, the four
// corners are dark. All UI must stay within the safe radius (centre ± radius −
// inset). At row y the visible half-width is sqrt(R^2 - (y - CY)^2), so the top
// and bottom rows are narrow caps — content there must be centred and short.
#define PANEL_DIAMETER       466
#define PANEL_CENTER         (PANEL_DIAMETER / 2)        // 233 (cx == cy)
#define PANEL_SAFE_INSET     8                            // px kept clear of the bezel
#define PANEL_SAFE_RADIUS    (PANEL_CENTER - PANEL_SAFE_INSET)  // 225

// Turn-by-turn HUD ──────────────────────────────────────────────────────────
// How long a telemetry packet stays "fresh". After this the HUD stops showing
// stale guidance and the wait screen takes over again.
#define NAV_HUD_FRESH_MS     5000
// Height (px) of the native HUD band reserved at the BOTTOM of the panel. The
// map fills the region above it. Phase 1 still renders the HUD via
// status_screen_show. The band is the bottom circular cap; its content is
// centred and fitted to the per-row visible width (see nav_hud.cpp).
#define NAV_HUD_BAND_HEIGHT  146
// Screen row where the band starts (top of the band). Bottom band → 320.
#define NAV_HUD_BAND_Y       (PANEL_DIAMETER - NAV_HUD_BAND_HEIGHT)

// UDP receive buffer — slightly larger than max send chunk (1400 B)
#define UDP_MAX_PACKET       1500

// Interval between HELLO packets (ms)
#define UDP_HELLO_INTERVAL   1000

// Streamed frame size — must match iOS NavigationFrameCapture.outputSize.
// Letterbox: the map fills the 466-wide top region above the bottom HUD band, so
// the height is the panel minus the band (466 − 146 = 320). The receiver seats
// the image at the top; it cannot upscale.
#define STREAM_WIDTH        466
#define STREAM_HEIGHT       (466 - NAV_HUD_BAND_HEIGHT)   // 320

// Maximum JPEG frame size in bytes
// iOS map at 400×400 @ quality 0.65 stays well under this; 128 KB is ample headroom.
#define MAX_JPEG_FRAME_SIZE (128 * 1024)

// Delay between task restarts on error (ms)
#define RECONNECT_DELAY_MS  2000

#endif // STREAM_CONFIG_H
