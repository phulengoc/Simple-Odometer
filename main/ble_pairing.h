#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Shared state — updated by BLE GATT writes from iOS.
//
// Access rules:
//   • g_ios_ip      : read from any task, written only in BLE callback (with mutex).
//   • g_stream_port : read from any task, written only in BLE callback (atomic uint16).
//   • g_ble_event_group : set BLE_IP_RECEIVED_BIT once iOS has written a valid IP.
// ─────────────────────────────────────────────────────────────────────────────

/// IPv4 address string written by iOS over BLE (e.g. "192.168.1.5").
/// Empty until the first BLE write; then the HELLO task uses it instead of
/// the hardcoded STREAM_SERVER_IP fallback.
extern char     g_ios_ip[16];

/// UDP port iOS is listening on for JPEG frames (default 5000).
/// Written by iOS over the port characteristic.
extern uint16_t g_stream_port;

/// Set BLE_IP_RECEIVED_BIT when a valid IP has been written by iOS.
/// Set BLE_WIFI_CRED_RECEIVED_BIT when valid WiFi credentials have been written.
extern EventGroupHandle_t g_ble_event_group;
#define BLE_IP_RECEIVED_BIT        BIT0
#define BLE_WIFI_CRED_RECEIVED_BIT BIT1

/// WiFi SSID received from iOS via the wifiCred characteristic (suffix 0x0005).
/// Empty until first BLE write. Used by wifi_connect_to() and persisted in NVS.
extern char g_wifi_ssid[33];   // max 32 chars + NUL

/// WiFi password received from iOS via the wifiCred characteristic.
extern char g_wifi_pass[64];   // max 63 chars + NUL

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/// Initialise the NimBLE stack, register the MapNav GATT service, and start
/// advertising.  Call once from app_main() after nvs_flash_init().
void ble_pairing_init(void);

/// Load saved WiFi credentials from NVS flash.
/// Copies into ssid_out (≥33 bytes) and pass_out (≥64 bytes).
/// Returns true if valid non-empty credentials were found.
bool ble_load_wifi_creds_from_nvs(char *ssid_out, char *pass_out);

#ifdef __cplusplus
}
#endif
