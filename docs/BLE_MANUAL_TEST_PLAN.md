# MapNav BLE Feature — Manual Test Plan

## Overview

This document covers end-to-end manual verification of the BLE pairing feature across
the iOS app (`MapNavigationSwiftUI`) and the ESP32 firmware (`Stream_Recv`).

**Code under test:**

| File | Role |
|---|---|
| `MapNavSwiftUI/Bluetooth/BLEConstants.swift` | UUID constants including `wifiCredCharUUID` (0x0005) |
| `MapNavSwiftUI/Bluetooth/BTManager.swift` | `CBCentralManager` state machine; auto-sends WiFi creds + IP/port |
| `MapNavSwiftUI/Bluetooth/BTStatusView.swift` | SwiftUI overlay badge showing connection state |
| `MapNavSwiftUI/Streaming/StreamingStatusView.swift` | `wifiIPAddress()`, `isPersonalHotspotActive()` |
| `MapNavSwiftUI/Streaming/NavigationFrameCapture.swift` | Dual-path broadcast (UDP + optional BLE) |
| `MapNavSwiftUI/NavigationViewModel.swift` | Owns `BTManager`; hotspot monitor; `syncHotspotCreds()`; `openHotspotSettings()` |
| `MapNavSwiftUI/AppPreferences.swift` | `hotspotPassword` persistence |
| `MapNavSwiftUI/ContentView.swift` | Auto-starts BLE and streaming after 2 s on appear; stacks badges |
| `MapNavSwiftUI/SettingsView.swift` | Hotspot status row, "Open Hotspot Settings" button, auto-saving password field |
| `MapNavSwiftUI/Info.plist` | `NSBluetoothAlwaysUsageDescription` |

---

## Required Equipment

- **iPhone** running iOS 16+ (real device — CoreBluetooth is not available in Simulator)
  - Personal Hotspot must be supported (not iPad Wi-Fi-only)
- **Mac** running Xcode with `MapNavigationSwiftUI.xcworkspace` open
- **ESP32-S3** board flashed with the latest NimBLE firmware from `Stream_Recv/`
  - Firmware must advertise service UUID `12AB3456-0000-1000-8000-00805F9B34FB`
  - Exposes all 5 characteristics (`…0001`–`…0005`)
  - Stores WiFi credentials to NVS (`…0005` write) and connects to the hotspot
  - Sends UDP HELLO packets (port 5001 → iOS) after receiving the IP/port writes
  - iPhone **hotspot** is the ESP32's Wi-Fi network (not a separate router)
- *(Optional)* `tcpdump` or Wireshark on a second Mac / laptop on the same hotspot

---

## Phase 0 — Wi-Fi Provisioning Setup

**Pre-condition:** Fresh install or cleared UserDefaults; ESP32 NVS erased (`idf.py erase-flash`).

### 0.1 Enter Hotspot Password

| # | Action | Expected |
|---|--------|----------|
| 1 | Open Settings sheet → scroll to **Wi-Fi Provisioning (ESP32)** | Section shows Hotspot row ("Inactive"), "Open Hotspot Settings" button, read-only SSID, and empty password field |
| 2 | SSID row value | Matches `UIDevice.current.name` exactly (e.g., "Fufuu") |
| 3 | Enter a password in the **Password** field | SecureField accepts input; no "Send" button required |
| 4 | Dismiss Settings and reopen | Password field still shows bullets (value persisted via `AppPreferences.hotspotPassword`) |

### 0.2 Hotspot Detection

| # | Action | Expected |
|---|--------|----------|
| 5 | Enable **Personal Hotspot** on the iPhone | Within ≤ 3 s, Hotspot row changes from "Inactive" (grey) to **"Active" (green ✓)** |
| 6 | BLE badge at top-right | Transitions from `BLE Off` → `Scanning…` automatically (hotspot monitor triggered `startBluetooth()`) |
| 7 | Disable Personal Hotspot | Hotspot row returns to "Inactive" within ≤ 3 s (BLE scan continues unaffected) |
| 8 | Re-enable Personal Hotspot | Hotspot row shows "Active" again; no duplicate BLE scan started (BLE already scanning) |

### 0.3 "Open Hotspot Settings" Button

| # | Action | Expected |
|---|--------|----------|
| 9 | Tap **Open Hotspot Settings** | iOS navigates to **Settings → Personal Hotspot** page |
| 10 | Return to MapNav app | App state unchanged; hotspot status row reflects current interface state |

---

## Phase 1 — Build Verification

### 1.1 Clean Build

| # | Action | Expected |
|---|--------|----------|
| 1 | Open `MapNavigationSwiftUI.xcworkspace` in Xcode | Workspace opens; no missing package errors |
| 2 | Select scheme `MapNavSwiftUI` targeting a real device | Scheme resolves; device is reachable |
| 3 | Product → Clean Build Folder | Succeeds |
| 4 | Product → Build | **0 errors, 0 warnings** related to Bluetooth files |
| 5 | Xcode Navigator shows `Bluetooth/` group with 3 files | `BLEConstants.swift`, `BTManager.swift`, `BTStatusView.swift` all visible |

### 1.2 Permissions Declaration

| # | Action | Expected |
|---|--------|----------|
| 6 | Open `Info.plist` | `NSBluetoothAlwaysUsageDescription` key present with a non-empty string |
| 7 | Install and launch the app on the device for the first time | System dialog: *"MapNav Would Like to Use Bluetooth"* |
| 8 | Tap **Allow** | App continues without crash; no immediate error badge |

---

## Phase 2 — BLE Discovery & Auto-Pairing

**Pre-condition:** ESP32 powered on and advertising. Bluetooth enabled on iPhone.

### 2.1 Auto-Connect (Happy Path)

| # | Action | Expected |
|---|--------|----------|
| 9 | Launch app fresh | After ~2 s (`onAppear` delay), BT badge appears at top-right above the streaming badge |
| 10 | Wait up to 10 s | Badge transitions: `BLE Off` (grey) → `Scanning…` (blue) → `Connecting <name>` (orange, spinner) → `Pairing…` (orange, spinner) → `Syncing…` (orange, spinner) → **`Ready`** (indigo) |
| 11 | Badge when `Ready` | Indigo capsule with a pulsing white ring (ring visible because `connectedDeviceCount > 0`) |
| 12 | Device-name row below badge | Shows ESP32 board name (e.g., `ESP32-MapNav`) with a green checkmark |

### 2.2 Multiple ESP32 Devices Present

| # | Action | Expected |
|---|--------|----------|
| 13 | Power on a second ESP32 advertising the same service UUID | Both appear in `btManager.discoveredDevices` |
| 14 | Tap the **`Scanning…`** badge | Device-list picker drops down listing both peripheral names |
| 15 | Tap one device name | App connects to that device; badge advances to `Ready` and shows chosen device name |

### 2.3 ESP32 Not Present at Launch

| # | Action | Expected |
|---|--------|----------|
| 16 | Kill app, power off ESP32, relaunch | Badge shows `Scanning…` (blue) indefinitely — no crash |
| 17 | Power on ESP32 while app is scanning | App auto-connects within ~5 s (auto-connect heuristic: `discoveredDevices.count == 1 \|\| name.hasPrefix("ESP32")`) |

### 2.4 Bluetooth Off on iPhone

| # | Action | Expected |
|---|--------|----------|
| 18 | Disable Bluetooth in iOS Settings | Badge shows `Error` (red) with message `"Bluetooth is turned off."` |
| 19 | Re-enable Bluetooth | Badge recovers to `Scanning…` then `Ready` automatically |

---

## Phase 2.5 — Automatic Credential Provisioning

**Pre-condition:** Personal Hotspot enabled; password entered in Settings; ESP32 NVS erased or credentials cleared.

### 2.5.1 Credential Write Log (iOS)

| # | Action | Expected |
|---|--------|----------|
| 18a | Open Xcode console while app connects | After characteristics line, **before** IP write: `[BLE] → WiFi creds: ssid=<device-name> pass=<redacted> (<n> bytes)` |
| 18b | Timing gap in logs | Approximately 0.3 s gap between credential write and `[BLE] → IP: 172.20.10.1` |
| 18c | No manual action needed | Credentials appear without pressing any button in the UI |

### 2.5.2 Credential Receipt (ESP32 serial)

| # | Action | Expected |
|---|--------|----------|
| 18d | ESP32 serial monitor during pairing | `WiFi creds stored to NVS: ssid=<device-name>` |
| 18e | 3-second window before disconnect | Serial shows `"Port received — starting 3 s BLE term window"` |
| 18f | BLE terminates after window | `"Pairing complete — terminating BLE"` then `"Advertising … (slow/1.28s interval)"` |
| 18g | ESP32 connects to hotspot | `[WIFI] Connecting to SSID: <device-name>` → `[WIFI] Connected, IP: 172.20.10.x` |

### 2.5.3 NVS Persistence (Credential Survives Reboot)

| # | Action | Expected |
|---|--------|----------|
| 18h | Power cycle ESP32 (no BLE session) | Serial shows `[NVS] Loaded creds: ssid=<device-name>` immediately after boot |
| 18i | ESP32 joins hotspot without BLE pairing | `[WIFI] Connected` within ~5 s of boot |
| 18j | UDP HELLO arrives without BLE step | Streaming badge shows `×1` after ESP32's HELLO lands on port 5001 — but note iOS IP must still be written; ESP32 needs BLE session for updated IP unless IP is the same as previously stored |

---

## Phase 3 — IP / Port Exchange

**Goal:** Confirm the ESP32 received the correct iOS Wi-Fi IP and UDP stream port after BLE `Ready`.

### 3.1 Log Verification

| # | Action | Expected |
|---|--------|----------|
| 20 | Open Xcode → Debug Area console while app connects | Log lines in order: `[BLE] Scanning for service…` → `Discovered: ESP32-MapNav` → `Connected` → `MTU negotiated` → `Characteristics discovered — IP:true PORT:true CTRL:true STREAM:true WIFICRED:true` |
| 20a | WiFi cred write precedes IP | `[BLE] → WiFi creds: ssid=…` appears **before** `[BLE] → IP:` |
| 21 | IP log line | `[BLE] → IP: 172.20.10.1` (hotspot IP, not `192.168.x.x`) |
| 21a | Port log line | `[BLE] → Port: 5000` |
| 22 | Completion log | `[BLE] ✓ Ready — device: ESP32-MapNav` |

### 3.2 HELLO Packet Arrival

| # | Action | Expected |
|---|--------|----------|
| 23 | On Mac: `sudo tcpdump -i any udp port 5001` while app connects | After badge reaches `Ready`, UDP HELLO packets arrive from the ESP32's IP on port 5001 within 1 s |
| 24 | Xcode console | `[UDP] HELLO from 192.168.x.x — active clients: 1` appears |
| 25 | Streaming badge client count | Streaming badge transitions from `0` clients to `×1` (ESP32 counted as active) |

### 3.3 Correct Port Delivered

| # | Action | Expected |
|---|--------|----------|
| 26 | ESP32 serial monitor | `"BLE port chr: 5000"` (or firmware's port log); little-endian bytes `0x88 0x13` in BLE trace |
| 27 | ESP32 then disconnects | Serial shows `"Pairing complete — terminating BLE to free RF for streaming"` followed by `"Advertising as "ESP32-MapNav" (slow/1.28s interval)…"` |

> **Why disconnect matters:** The ESP32 terminates the BLE connection immediately after
> receiving both writes so the 2.4 GHz radio is freed for Wi-Fi UDP. With an active BLE
> connection, RF coexistence drops ~50% of UDP packets at the 100 ms connection interval.

---

## Phase 4 — UDP Streaming (Primary Path)

**Goal:** Confirm existing UDP streaming operates correctly after BLE pairing.

### 4.1 Streaming Badge

| # | Action | Expected |
|---|--------|----------|
| 28 | After BLE is `Ready` and HELLO packets arrive | Streaming badge shows **`LIVE ×1`** (red pulsing dot) |
| 29 | On ESP32 display | Live 466×466 JPEG map frames render at ~4–30 fps (depends on Wi-Fi quality) |

### 4.2 Navigation + Streaming

| # | Action | Expected |
|---|--------|----------|
| 30 | Tap route planner → select Amsterdam → Rotterdam → **Navigate** | `NavigationViewController` becomes the capture source view |
| 31 | Watch ESP32 display during navigation | Real-time turn-by-turn map renders; no frame overflow warnings on serial |
| 32 | Simulate at 3× speed in Settings | No crashes; `isEncoding` guard prevents frame backlog |

### 4.3 Regression — UDP Works Without BLE

| # | Action | Expected |
|---|--------|----------|
| 33 | Tap the BT badge to stop Bluetooth | Badge returns to `BLE Off` (grey) |
| 34 | Streaming badge | Still shows `LIVE ×1`; UDP delivery unaffected |
| 35 | ESP32 continues displaying map | Frame delivery continues via HELLO/UDP discovery |

---

## Phase 5 — BLE Frame Streaming (Optional Path)

> BLE frame streaming is an opt-in secondary path for cases where Wi-Fi is unavailable.
> It is **disabled by default** (`bleStreamEnabled = false`).

### 5.1 Enable BLE Streaming

| # | Action | Expected |
|---|--------|----------|
| 36 | Open Settings → scroll to **Bluetooth (ESP32)** section | Section visible with Status, Connected Device (if ready), Scan/Stop button, BLE Frame Streaming toggle |
| 37 | Observe toggle when BLE state ≠ `.ready` | Toggle is **greyed out and disabled** |
| 38 | With BLE state = `.ready`, toggle **BLE Frame Streaming** ON | Toggle enables; `btManager.bleStreamEnabled = true` |

### 5.2 BLE Frame Delivery

| # | Action | Expected |
|---|--------|----------|
| 39 | With BLE streaming ON, start navigation | Xcode console: no crash; `broadcast()` executes on `btStreamQueue` |
| 40 | ESP32 serial during BLE streaming | `streamChar` write-without-response packets arrive: SOI (`0xFF 0xD8`), DATA chunks, EOI (`0xFF 0xD9`) |
| 41 | ESP32 display via BLE path | Low-fps (2–5 fps) map renders — BLE throughput is lower than UDP |
| 42 | `isTransmitting` guard under sustained streaming | No memory overflow; overlapping frames are silently dropped |

### 5.3 Dual-Path

| # | Action | Expected |
|---|--------|----------|
| 43 | Both UDP and BLE streaming active simultaneously | Two independent deliveries; each path operates without interfering with the other |
| 44 | Disable Wi-Fi (Airplane mode, Bluetooth ON) | UDP path stops (`clientCount` drops to 0); BLE path continues delivering frames |

---

## Phase 6 — Error Recovery & Edge Cases

### 6.1 ESP32 Powers Off During Streaming

| # | Action | Expected |
|---|--------|----------|
| 45 | Power off ESP32 mid-stream | BT badge returns to `Scanning…` automatically (disconnect triggers rescan); UDP `clientCount` drops to 0 after 5 s TTL |
| 46 | Power ESP32 back on | App auto-reconnects; badge reaches `Ready`; HELLO resumes; streaming badge shows `×1` again |

### 6.2 End Navigation

| # | Action | Expected |
|---|--------|----------|
| 47 | Tap **End Navigation** | `NavigationViewModel.endNavigation()` calls `bluetoothManager.sendControl("STOP")` |
| 48 | Xcode console | `[BLE] → CTRL: STOP` |
| 49 | ESP32 serial | Control characteristic receives `"STOP"` payload |

### 6.3 App Backgrounded

| # | Action | Expected |
|---|--------|----------|
| 50 | Press Home button while streaming | BLE Central suspends (iOS system behaviour — no background BLE Central mode) |
| 51 | Return to foreground | BLE state restores; if disconnected, app re-enters `Scanning…` automatically |

### 6.4 Settings Scan / Stop Button

| # | Action | Expected |
|---|--------|----------|
| 52 | Settings → Bluetooth → tap **Stop Bluetooth** button | Button shows `Stop Bluetooth` (red, `xmark.circle`) when active; tapping calls `stopBluetooth()`; badge goes to `BLE Off` |
| 53 | Tap **Start Scanning** | Scan resumes (blue `dot.radiowaves` icon); auto-connects when ESP32 found |
| 54 | After an error: button label | Shows **Retry Scan** (blue) |

---

## Phase 7 — UI & Accessibility Checks

| # | Check | Expected |
|---|-------|----------|
| 55 | Both badges visible simultaneously | BT badge (top) above streaming badge (bottom); no overlap; respects safe area on all iPhone models including Dynamic Island |
| 56 | BT badge colours | `BLE Off`=grey (`systemFill`), `Scanning…`=blue, `Connecting/Pairing/Syncing`=orange, `Ready`=indigo, `Error`=red |
| 57 | Settings Status row colour | Shows `Ready` in **green** (distinct from badge indigo — this is the Form row text colour) |
| 58 | Pulsing ring when `Ready` | White ring pulse visible when `connectedDeviceCount > 0`; animation repeats indefinitely |
| 59 | Pulsing ring when idle | No ring visible; only the Bluetooth SF Symbol icon shows |
| 60 | Device name row truncates | Long names truncate with `…` rather than overflowing the badge |
| 61 | Settings footer text | `"MapNav scans for ESP32 devices and automatically shares the Wi-Fi IP and stream port over BLE so the ESP32 knows where to send its HELLO packets."` visible below the BT section |

---

## Quick Smoke Test (20 min)

For a rapid daily check, run steps in this order:

```
1 → 3 → 6 → 7 → 8 → 5 → 6(hotspot row) → 18a → 18d → 18g → 20 → 21 → 23 → 24 → 28 → 30 → 31 → 47 → 48 → 45 → 46
```

This covers: build → permissions → hotspot password entered → hotspot activated →
BLE auto-scan → credential write (iOS log + ESP32 serial) → WiFi connect → IP exchange →
HELLO confirmation → streaming active → navigation → STOP → disconnect/reconnect.

---

## Xcode Console Log Lines to Monitor

| Log prefix | Source | Meaning |
|---|---|---|
| `[BLE] Scanning for service…` | `BTManager.scan()` | Scan started |
| `[BLE] Discovered: <name> RSSI:<n>` | `BTManager` CBCentralManagerDelegate | Peripheral found |
| `[BLE] Connected to: <name>` | `BTManager` `didConnect` | GATT connection established |
| `[BLE] Negotiated MTU (write-without-response): <n> bytes` | `BTManager` `didConnect` | ATT MTU set for BLE streaming |
| `[BLE] Characteristics discovered — IP:true PORT:true CTRL:true STREAM:true WIFICRED:true` | `BTManager` `didDiscoverCharacteristics` | All 5 characteristics found |
| `[BLE] → WiFi creds: ssid=<name> pass=<redacted> (<n> bytes)` | `BTManager.sendWiFiCredentials` | JSON credential payload written to 0x0005 |
| `[BLE] → IP: 172.20.10.1` | `BTManager.sendIPAndPort` | Hotspot IP written to ESP32 |
| `[BLE] → Port: 5000` | `BTManager.sendIPAndPort` | Stream port written to ESP32 |
| `[BLE] ✓ Ready — device: <name>` | `BTManager.markReady()` | Exchange complete; ready state entered |
| `[BLE] → CTRL: STOP` | `BTManager.sendControl` | Stop command sent on `endNavigation()` |
| `[BLE] Disconnected from: <name>` | `BTManager` `didDisconnect` | ESP32 disconnected (expected after pairing) |
| `[UDP] HELLO from <IP> — active clients: 1` | `UDPStreamSender.readHello()` | ESP32 joined hotspot and acknowledged IP/port |

---

## ESP32 Firmware Pre-Test Checklist

Before running the iOS tests, confirm the flashed firmware:

- [ ] Advertises service UUID `12AB3456-0000-1000-8000-00805F9B34FB`
- [ ] Exposes all 5 characteristics (`…0001`–`…0005`)
- [ ] IP char (`…0001`) writable with response
- [ ] Port char (`…0002`) writable with response
- [ ] Control char (`…0003`) writable without response + notifiable
- [ ] Stream char (`…0004`) writable without response
- [ ] **WiFi Cred char (`…0005`) writable with response**
- [ ] On WiFi cred write → parses JSON, saves to NVS namespace `"mapnav_wifi"`, sets `BLE_WIFI_CRED_RECEIVED_BIT`
- [ ] On IP write → stores UTF-8 IP, sets `BLE_IP_RECEIVED_BIT`
- [ ] On Port write → stores LE uint16, spawns `ble_term_task` (3 s timer)
- [ ] After `ble_term_task` fires → calls `ble_gap_terminate()` to free RF
- [ ] After disconnect → resumes advertising at slow interval (1.28 s)
- [ ] On boot with NVS creds → `ble_load_wifi_creds_from_nvs()` returns `true`, connects without BLE wait
- [ ] `wifi_reconnect_task` blocks on `BLE_WIFI_CRED_RECEIVED_BIT`; re-provisions on live write
- [ ] UDP HELLO loop (`udp_hello_task`) unblocks and sends to stored IP, port 5001
- [ ] Serial boot log shows: `Tasks: hello=CPU0 pri3 | rx=CPU1 pri6 | jpeg=CPU1 pri5`
- [ ] WiFi modem sleep disabled (`esp_wifi_set_ps(WIFI_PS_NONE)`)

---

## Known Limitations

| Limitation | Detail |
|---|---|
| No Simulator support | `CBCentralManager` requires real hardware; Simulator always reports `.unsupported` |
| BLE streaming fps | BLE throughput ~2–5 fps vs. UDP ~30 fps; BLE path is intended for Wi-Fi-unavailable scenarios |
| Background BLE | iOS suspends the Central when the app is backgrounded (no `bluetooth-central` background mode declared); reconnect happens automatically on foreground |
| Single connection | `MAX_CONNECTIONS=1` on ESP32 firmware; pairing a second iOS device requires rebooting the ESP32 |
| Fallback credentials in firmware | `stream_config.h` contains hardcoded fallback Wi-Fi credentials — do not commit to public repositories |
| iOS cannot enable hotspot programmatically | iOS does not expose a public API to turn on Personal Hotspot; "Open Hotspot Settings" deep-links the user to the settings page |
| BLE credential security | WiFi credentials are sent unencrypted over the BLE link during the ≤3 s pairing window |
| NVS credential storage | Credentials are stored unencrypted in ESP32 NVS flash; physical access can extract them with `esptool.py read_flash` |
