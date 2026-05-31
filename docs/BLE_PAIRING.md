# MapNav BLE Pairing & Wi-Fi Provisioning — Technical Reference

## Overview

BLE serves two purposes in the MapNav system:

1. **Wi-Fi credential provisioning** — the iOS app writes the Personal Hotspot SSID and
   password to the ESP32 once (stored to NVS flash). The ESP32 uses these credentials to
   join the hotspot on every subsequent boot.
2. **IP/port exchange** — the iOS app writes its current hotspot IP address and UDP stream
   port to the ESP32 so it knows where to send UDP HELLO packets and receive JPEG frames.

Both steps happen in the same BLE session, after which the ESP32 terminates the connection
to free the 2.4 GHz radio for Wi-Fi UDP streaming.

```
 iOS (Central)                          ESP32 (Peripheral)
 ─────────────────────────────────────────────────────────
 1. Enable Personal Hotspot (172.20.10.1)
 2. App detects bridge100/ap1 → auto-starts BLE scan
 3. Connect
 4. Write WiFi creds ──── BLE ──────────► parse JSON → save to NVS
                                          (g_wifi_ssid, g_wifi_pass set)
    [0.3 s delay]
 5. Write IP ────────────BLE ───────────► store g_ios_ip
 6. Write Port ──────────BLE ───────────► store g_stream_port
                                          start ble_term_task (3 s timer)
    [3 s grace window — creds already written]
                                          ble_gap_terminate() ◄─(ESP32)
 7. Receive disconnect event
                                          wifi_connect_to(ssid, pass)
                                          restart slow advertising (1.28 s)
 ─ pairing complete ─────────────────────────────────────
 8. Bind UDP socket on port 5000
                    ◄──────── UDP HELLO ── udp_hello_task unblocks
 9. Send JPEG frames ─────── UDP ────────► udp_receiver_task assembles
                                           jpeg_display_task decodes → LCD
```

---

## GATT Service Layout

**Service UUID**: `12AB3456-0000-1000-8000-00805F9B34FB`

| Characteristic | UUID suffix | Properties | Payload |
|---|---|---|---|
| IP Address | `…0001` | Write (with response) | UTF-8 IPv4 string, e.g. `"172.20.10.1"` (7–15 bytes) |
| UDP Port | `…0002` | Write (with response) | 2-byte little-endian `uint16_t`, e.g. `0x88 0x13` = 5000 |
| Control | `…0003` | Write-without-response + Notify | ASCII command: `"START"` / `"STOP"` / `"ACK"` |
| Stream | `…0004` | Write (without response) | JPEG frame chunks — SOI/DATA/EOI (optional BLE streaming path) |
| **WiFi Credentials** | **`…0005`** | **Write (with response)** | **UTF-8 JSON: `{"ssid":"…","pass":"…"}`** (20–150 bytes) |

### 128-bit UUID byte layout (NimBLE little-endian)

```
Standard BE:  12 AB 34 56  XX XX  10 00  80 00  00 80 5F 9B 34 FB
NimBLE LE:    FB 34 9B 5F  80 00  00 80  00 10  XX XX  56 34 AB 12
                                                 ^^^^^ ← suffix (0001..0005)
```

---

## Wi-Fi Credential Provisioning

### Design goals

- **One-time setup** — credentials are saved to ESP32 NVS flash; subsequent boots
  auto-connect without repeating the BLE provisioning flow.
- **No manual button** — iOS pushes credentials automatically the moment GATT
  characteristics are discovered, before the user can interact.
- **Hotspot-first IP** — `wifiIPAddress()` prefers `bridge100` / `ap1` (the iPhone
  Personal Hotspot interface, always `172.20.10.1`) over `en0` (Wi-Fi client).
- **3-second BLE window** — the ESP32 starts a countdown after receiving the port
  write instead of immediately terminating BLE, giving iOS time to write credentials
  in the same session even if the writes arrive out of order.

### Write ordering on iOS

```
didDiscoverCharacteristics fires
  │
  ├─ pendingWiFiSSID non-empty?  ──yes──► sendWiFiCredentials()  ← char 0x0005
  │                                       (UTF-8 JSON, withResponse)
  │    [0.3 s delay]
  ├─► sendIPAndPort()                     ← chars 0x0001 + 0x0002
  │    [0.5 s delay]
  └─► markReady()
```

The 0.3 s gap ensures the ESP32's `wificred_chr_access` callback has committed to NVS
before the IP/port writes arrive and trigger the BLE termination timer.

### JSON wire format

```json
{"ssid":"Fufuu","pass":"myhotspotpass"}
```

- Both values are escaped: `"` → `\"`
- Length must be 20–150 bytes (validated by `wificred_chr_access`)

---

## iOS Implementation

### Hotspot detection

`isPersonalHotspotActive()` in `StreamingStatusView.swift` enumerates network
interfaces via POSIX `getifaddrs()` and returns `true` when `bridge100` or `ap1` is
up with an IPv4 address.

```swift
func isPersonalHotspotActive() -> Bool {
    let hotspotInterfaces: Set<String> = ["bridge100", "ap1"]
    var ifaddr: UnsafeMutablePointer<ifaddrs>?
    guard getifaddrs(&ifaddr) == 0 else { return false }
    defer { freeifaddrs(ifaddr) }
    var ptr = ifaddr
    while let current = ptr {
        defer { ptr = current.pointee.ifa_next }
        guard current.pointee.ifa_addr.pointee.sa_family == UInt8(AF_INET) else { continue }
        if hotspotInterfaces.contains(String(cString: current.pointee.ifa_name)) { return true }
    }
    return false
}
```

### IP address resolution

`wifiIPAddress()` (also in `StreamingStatusView.swift`) returns the best local IPv4
for streaming clients:

| Priority | Interface | Condition |
|---|---|---|
| 1 | `bridge100` | iPhone is hosting a Personal Hotspot (always `172.20.10.1`) |
| 2 | `ap1` | Alternative hotspot interface on newer iOS versions |
| 3 | `en0` | iPhone joined a Wi-Fi network as a client |

### Auto-scan on hotspot activation

`NavigationViewModel.startHotspotMonitor()` creates a `Timer` that fires every 3 s on
the `.common` run-loop mode:

```swift
let active = isPersonalHotspotActive()
if active != self.isHotspotActive {
    self.isHotspotActive = active
    // Auto-start BLE scan the moment the hotspot turns on.
    if active, case .idle = self.bluetoothState {
        self.startBluetooth()
    }
}
```

### Credential sync

`NavigationViewModel.syncHotspotCreds()` pushes the current hotspot SSID and password
into `BTManager.pendingWiFiSSID` / `pendingWiFiPass`:

```swift
bluetoothManager.pendingWiFiSSID = UIDevice.current.name  // default hotspot SSID
bluetoothManager.pendingWiFiPass = hotspotPassword         // from AppPreferences
```

Called at init and every time `hotspotPassword` changes (via Combine sink).

### Persistence

`AppPreferences.hotspotPassword` wraps `UserDefaults.standard` under the key
`"pref_hotspot_pass"`. The SSID is never persisted — it is always `UIDevice.current.name`
because iOS sets the hotspot SSID to the device name by default.

### BTConnectionState lifecycle (updated)

```
.idle
  └─ start() called (or auto-started by hotspot monitor)
       └─ .scanning
            └─ peripheral discovered → auto-connect (single device)
                 └─ .connecting(name:)
                      └─ peripheral connected → discoverServices
                           └─ .discovering
                                └─ all 5 characteristics found
                                     └─ .exchanging
                                          │  [optional] sendWiFiCredentials() ← 0x0005
                                          │  [0.3 s]
                                          │  sendIPAndPort() ← 0x0001 + 0x0002
                                          │  [0.5 s]
                                          └─ .ready
```

### Thread model

| Queue | Purpose |
|---|---|
| `cbQueue` (serial, `.utility`) | All `CBCentralManagerDelegate` / `CBPeripheralDelegate` callbacks |
| `btStreamQueue` (serial, `.userInitiated`) | JPEG chunked BLE writes (optional path) |
| `DispatchQueue.main` | All `@Published` property updates for SwiftUI |

---

## ESP32 Implementation

### `ble_pairing_init()` — startup sequence

```
nvs_flash_init()         ← must be called first (in app_main)
  └─ ble_pairing_init()
       ├─ xEventGroupCreate()     → g_ble_event_group
       ├─ xSemaphoreCreateMutex() → s_ip_mutex (guards g_ios_ip writes)
       ├─ nimble_port_init()
       ├─ ble_svc_gap_init() / ble_svc_gatt_init()
       ├─ ble_gatts_count_cfg() / ble_gatts_add_svcs()  → register GATT table
       │    Registers: 0x0001 IP  0x0002 Port  0x0003 Ctrl  0x0004 Stream  0x0005 WiFiCred
       ├─ ble_hs_cfg.sync_cb = ble_on_sync → triggers ble_app_advertise()
       └─ nimble_port_freertos_init(nimble_host_task)
            └─ NimBLE host task: priority 21 (configMAX_PRIORITIES-4), Core 0
```

### Advertising parameters

| Phase | Min interval | Max interval | When |
|---|---|---|---|
| Pre-pairing (fast) | 100 ms (`0x00A0`) | 150 ms (`0x00C0`) | `g_ios_ip` is empty |
| Post-pairing (slow) | 1280 ms (`0x0800`) | 2560 ms (`0x1000`) | `g_ios_ip` is set |

### GATT write callbacks

**`wificred_chr_access`** (suffix `0005`)
- Validates length 20–150 bytes
- Calls `parse_wifi_cred_json()` — minimal `strstr` / `strchr` JSON parser extracting
  `ssid` and `pass` values
- Copies into `g_wifi_ssid` (≤32 chars + NUL) and `g_wifi_pass` (≤63 chars + NUL)
- Calls `save_wifi_creds_to_nvs()` — opens NVS namespace `"mapnav_wifi"`, writes both
  strings, commits, closes
- Sets `BLE_WIFI_CRED_RECEIVED_BIT` in `g_ble_event_group`

**`ip_chr_access`** (suffix `0001`)
- Validates length 7–15 bytes
- Copies into `g_ios_ip` under `s_ip_mutex`
- Sets `BLE_IP_RECEIVED_BIT` in `g_ble_event_group`

**`port_chr_access`** (suffix `0002`)
- Validates exactly 2 bytes
- Stores little-endian `uint16_t` into `g_stream_port`
- Spawns `ble_term_task` (3-second countdown before `ble_gap_terminate`) so iOS has a
  window to write the WiFi credential in the same session

**`ctrl_chr_access`** (suffix `0003`)
- Logs the command string; reserved for future use

### BLE termination timer

```c
#define BLE_TERM_WINDOW_MS 3000

static void ble_term_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(BLE_TERM_WINDOW_MS));
    ble_gap_terminate(s_conn_handle, BLE_ERR_CONN_TERM_LOCAL);
    vTaskDelete(NULL);
}
// Spawned from port_chr_access() after storing the port value.
```

**Why a timer instead of immediate disconnect:** iOS writes WiFi credentials before
IP/port (0x0005 → 0x0001 → 0x0002 sequence). However, the first time a user opens the
app the credential write may arrive concurrently with or after the port write. The 3-second
window ensures the credential write has landed before BLE is torn down.

### WiFi credential selection (boot)

`app_main()` selects credentials in priority order:

```
1. NVS flash  ← ble_load_wifi_creds_from_nvs() returns non-empty strings
2. BLE live   ← wait up to 30 s for BLE_WIFI_CRED_RECEIVED_BIT (cleared on read)
3. Fallback   ← WIFI_SSID / WIFI_PASS constants in stream_config.h
```

If NVS credentials exist the device connects immediately on boot without waiting for BLE.

### WiFi reconnect task

`wifi_reconnect_task()` runs on Core 0 and blocks on `BLE_WIFI_CRED_RECEIVED_BIT`:

```c
// Waits for a live BLE credential write (bit is auto-cleared after waking)
xEventGroupWaitBits(g_ble_event_group, BLE_WIFI_CRED_RECEIVED_BIT,
                    pdTRUE, pdTRUE, portMAX_DELAY);
wifi_connect_to(g_wifi_ssid, g_wifi_pass);
```

This allows re-provisioning at any time without rebooting the ESP32.

### NVS storage layout

| Namespace | Key | Type | Value |
|---|---|---|---|
| `mapnav_wifi` | `ssid` | `NVS_TYPE_STR` | Personal Hotspot SSID (≤32 chars) |
| `mapnav_wifi` | `pass` | `NVS_TYPE_STR` | Personal Hotspot password (≤63 chars) |

### FreeRTOS task layout

| Task | Core | Priority | Notes |
|---|---|---|---|
| `nimble_host` | 0 | 21 (`MAX-4`) | NimBLE stack — fixed by ESP-IDF |
| `wifi` driver | 0 | 23 | Wi-Fi driver |
| `wifi_reconnect` | 0 | 3 | Re-provisions WiFi on BLE credential write |
| `udp_hello` | 0 | 3 | Sends `"HELLO"` every 1 s to `g_ios_ip:5001` |
| `udp_rx` | **1** | **6** | Assembles JPEG frames — **must be Core 1** |
| `jpeg_display` | 1 | 5 | Decodes JPEG → SPI LCD |

> **Critical:** `udp_rx` on Core 1 is essential. NimBLE (priority 21, Core 0) would
> preempt any Core 0 task every ~100 ms, causing EOI packet loss and broken frames.

---

## File Map

### ESP32 firmware (`Stream_Recv/`)

| File | Role |
|---|---|
| `main/ble_pairing.h` | Public API + shared globals: `g_ios_ip`, `g_stream_port`, `g_ble_event_group`, `g_wifi_ssid`, `g_wifi_pass`, `ble_load_wifi_creds_from_nvs()` |
| `main/ble_pairing.cpp` | NimBLE GATT server; all 5 characteristic handlers; NVS helpers; `ble_term_task` |
| `main/example_qspi_with_ram.cpp` | `app_main`, `wifi_driver_init`, `wifi_connect_to`, `wifi_reconnect_task`, UDP tasks, JPEG decoder |
| `main/stream_config.h` | `BLE_DEVICE_NAME`, port/IP constants, fallback `WIFI_SSID` / `WIFI_PASS` |
| `sdkconfig.defaults` | Kconfig for BT stack, NimBLE, lwIP mailbox tuning |

### iOS app (`MapNavigationSwiftUI/`)

| File | Role |
|---|---|
| `Bluetooth/BLEConstants.swift` | UUIDs including `wifiCredCharUUID` (suffix `0005`), wire-format markers |
| `Bluetooth/BTManager.swift` | `CBCentralManager` state machine; `pendingWiFiSSID/Pass`; auto-sends creds on `didDiscoverCharacteristics` |
| `Bluetooth/BTStatusView.swift` | SwiftUI badge showing connection state |
| `Streaming/StreamingStatusView.swift` | `wifiIPAddress()` (bridge100 → ap1 → en0 priority), `isPersonalHotspotActive()` |
| `NavigationViewModel.swift` | Owns `BTManager`; hotspot monitor timer; `syncHotspotCreds()`; `openHotspotSettings()` |
| `AppPreferences.swift` | `hotspotPassword` persistence key (`"pref_hotspot_pass"`) |
| `SettingsView.swift` | Hotspot status row, "Open Hotspot Settings" button, password field (auto-saved) |

---

## UDP Streaming Protocol

Unchanged from the pre-provisioning implementation. The JPEG framing protocol is shared
between UDP and BLE paths:

```
Sender:                       Receiver state machine:
───────────────────────────── ─────────────────────────────────────
[SOI]  0xFF 0xD8  (2 B)     → WAITING_SOI → RECEIVING_DATA, frame_used=0
[DATA] raw JPEG chunk         → append to frame buffer (≤1400 B/packet)
[DATA] raw JPEG chunk         → ...
[EOI]  0xFF 0xD9  (2 B)     → copy frame_buf → heap alloc → enqueue
                                → WAITING_SOI
```

**Resilience rule:** A new SOI while `RECEIVING_DATA` discards the partial frame and
restarts assembly (logged as `"SOI mid-frame — dropped X B"`).

### Port assignments

| Port | Direction | Purpose |
|---|---|---|
| 5000 | iOS → ESP32 | JPEG frame data (SOI / DATA / EOI packets) |
| 5001 | ESP32 → iOS | `"HELLO"` discovery heartbeat, 1 s interval |

---

## sdkconfig Tuning

```kconfig
# BLE stack
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_PINNED_TO_CORE_0=y   # keep NimBLE on Core 0, away from UDP rx
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1
CONFIG_BT_NIMBLE_SM_LEGACY=n          # no legacy pairing — save RAM
CONFIG_BT_NIMBLE_SM_SC=n              # no secure pairing — save RAM

# lwIP UDP mailbox
CONFIG_LWIP_UDP_RECVMBOX_SIZE=32      # ≈ 44 KB burst buffer (32 × 1400 B)
```

WiFi modem sleep disabled at runtime after `esp_wifi_start()`:

```c
esp_wifi_set_ps(WIFI_PS_NONE);
```

---

## Build & Flash

### Prerequisites

```sh
source /Users/ple/.espressif/tools/activate_idf_v5.5.2.sh
# or use the shell alias:  get_idf
```

### First-time build (sdkconfig regeneration)

```sh
cd Stream_Recv
rm -f sdkconfig      # ensure BT Kconfig options are applied from sdkconfig.defaults
idf.py build
```

### Incremental build & flash

```sh
idf.py build
idf.py -p /dev/cu.usbmodem31101 flash
```

### Monitor

```sh
idf.py -p /dev/cu.usbmodem31101 monitor
```

---

## Troubleshooting

### ESP32 does not connect to hotspot after BLE pairing

1. Check ESP32 serial for `[WIFI] Connecting to SSID: <name>`.  
   If absent, `BLE_WIFI_CRED_RECEIVED_BIT` was never set — the iOS write failed.
2. Confirm `wificred_chr_access` logs `"WiFi creds stored to NVS: ssid=<name>"`.
3. Verify the JSON was well-formed: `{"ssid":"…","pass":"…"}` with no extra whitespace
   or special characters outside the values.
4. Ensure ESP32 is within BLE range when app sends credentials (~10 m).

### Credentials not sent automatically from iOS

- `viewModel.hotspotPassword` must be non-empty — check Settings → Wi-Fi Provisioning.
- `BTManager.pendingWiFiSSID` / `pendingWiFiPass` are set by `syncHotspotCreds()` which
  runs at `NavigationViewModel.init()`. Confirm it was called before the BLE session.
- Xcode console should show `[BLE] → WiFi creds: ssid=<name> pass=<redacted>`.

### App does not auto-start BLE when hotspot is enabled

- Hotspot monitor polls every 3 s — wait up to 3 s after enabling the hotspot.
- The auto-scan only fires when `bluetoothState == .idle`. If BLE is already
  scanning/connecting, the timer has nothing to do.
- On iOS 17+, `bridge100` is the canonical hotspot interface; `ap1` is used on some
  older models. Both are checked. If neither is up, `isPersonalHotspotActive()` returns
  `false` — verify the interface name with `ifconfig` on a Mac on the same hotspot.

### "BLE timeout — falling back to `STREAM_SERVER_IP`"

The `udp_hello_task` waited 30 s without `BLE_IP_RECEIVED_BIT`.  
Causes: iOS app not running, BLE permission denied, ESP32 not advertising.  
The fallback IP (`STREAM_SERVER_IP` in `stream_config.h`) must be the development
machine's IP for bench testing without a phone.

### "SOI mid-frame — dropped X B (BLE preemption?)"

EOI packet was lost before the next SOI arrived. Should be rare (< 1 %) after the three
coexistence fixes: BLE disconnect-after-pairing, `udp_rx` on Core 1, WiFi modem sleep
off. Verify the boot log shows:

```
Tasks: hello=CPU0 pri3 | rx=CPU1 pri6 | jpeg=CPU1 pri5
```

### `nimble/nimble_port.h: No such file or directory`

`CONFIG_BT_ENABLED` not set. Delete `sdkconfig` and rebuild.

### iOS — no BLE badge appears

`Info.plist` must contain `NSBluetoothAlwaysUsageDescription`. System permission
dialog must be accepted on first launch.

---

## Security Notes

- BLE uses no bonding, no pairing, and no encryption. Credentials are sent in plaintext
  over the BLE link. This is acceptable for a local, short-range provisioning session;
  the window is open for ≤ 3 s once pairing begins.
- Credentials are stored unencrypted in ESP32 NVS flash (no secure storage partition).
  Physical access to the device could extract them with `esptool.py read_flash`.
- `stream_config.h` contains hardcoded fallback credentials. Do not commit this file
  to public repositories.
- The BLE device name (`ESP32-MapNav`) is broadcast in plaintext in advertising packets.
