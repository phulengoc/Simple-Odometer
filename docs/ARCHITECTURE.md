# Architecture

High-level architecture of the **Stream_Recv** firmware: an ESP32-S3 application that receives a live JPEG video stream over WiFi/UDP from the MapNav iOS app and renders it on a 466×466 QSPI AMOLED panel.

This document describes the *big picture* — components, data flow, the concurrency model, and the design constraints that shape the code. For BLE specifics see [`BLE_PAIRING.md`](BLE_PAIRING.md) and [`BLE_MANUAL_TEST_PLAN.md`](BLE_MANUAL_TEST_PLAN.md). For build/flash commands see [`../CLAUDE.md`](../CLAUDE.md).

---

## 1. System Context

This firmware is the **receiver/display** end of a two-device system. The **sender** is the MapNav iOS app (`MapNavigationSwiftUI` / `maplibre-navigation-ios` in the parent workspace), which captures navigation map frames, JPEG-encodes them, and streams them over the local network.

```
┌─────────────────────────┐                          ┌──────────────────────────────┐
│   iOS — MapNav app       │                          │   ESP32-S3 — Stream_Recv     │
│                          │                          │                              │
│  MapLibre nav view       │                          │   466×466 QSPI AMOLED panel  │
│  → frame capture         │      WiFi (same LAN)     │   (SH8601 / CO5300)          │
│  → JPEG encode (q≈0.65)  │ ◄──────────────────────► │                              │
│  → UDPStreamSender       │                          │   NimBLE GATT peripheral     │
└─────────────────────────┘                          └──────────────────────────────┘
        │     ▲                                                  │     ▲
        │     │                                                  │     │
        │     └────── BLE GATT: iOS writes IP/port/WiFi creds ───┘     │
        │                                                              │
        ├────── UDP :5001  ESP32 → iOS   "HELLO" beacon ───────────────┘
        │
        └────── UDP :5000  iOS → ESP32   JPEG frame packets ──────────►
```

Two transports work together:

- **BLE (control plane)** — out-of-band provisioning. The phone hands the ESP32 its WiFi credentials and its own IP address before any IP traffic flows. This lets the device join a phone hotspot it was never pre-configured for.
- **UDP (data plane)** — discovery (`HELLO`) and the JPEG video stream. UDP is chosen over TCP for low, bounded latency; dropped frames are simply skipped rather than retransmitted.

---

## 2. Component Map

| Layer | Unit | Source | Responsibility |
|-------|------|--------|----------------|
| App entry | `app_main()` | `main/example_qspi_with_ram.cpp` | Boot sequence, hardware bring-up, task creation |
| Control plane | NimBLE GATT server | `main/ble_pairing.cpp` / `.h` | Receive IP/port/WiFi creds from iOS; persist to NVS |
| Data plane (out) | `udp_hello_task` | `example_qspi_with_ram.cpp` | Beacon ESP32 presence to iOS on :5001 |
| Data plane (in) | `udp_receiver_task` | `example_qspi_with_ram.cpp` | Reassemble JPEG frames from UDP packets on :5000 |
| Decode + render | `jpeg_display_task` | `example_qspi_with_ram.cpp` | Decode JPEG → draw MCU blocks straight to panel |
| Connectivity | `wifi_*` functions + `wifi_reconnect_task` | `example_qspi_with_ram.cpp` | WiFi STA lifecycle; reconnect on new BLE creds |
| Decoder | BitBank `JPEGDEC` | `components/jpegdec/` | Baseline JPEG decode with ESP32-S3 SIMD assembly |
| Display driver | `esp_lcd_sh8601` | `managed_components/` | QSPI panel I/O, DMA bitmap transfers |
| Touch | `touch_bsp` (FT3168) | `components/touch_bsp/` | I2C capacitive touch (present; not used by stream path) |
| Panel ID | `read_lcd_id_bsp` | `components/read_lcd_id_bsp/` | Runtime detect SH8601 vs CO5300 |
| Config | `stream_config.h` | `main/` | All network/display/sizing tunables |

All tunable constants — WiFi fallback credentials, fallback IP, UDP ports, HELLO interval, frame dimensions, max frame size — live in **`main/stream_config.h`**.

> **Note:** LVGL is a linked dependency and the component is present, but every LVGL code path in `example_qspi_with_ram.cpp` is commented out. The stream path renders directly to the panel for latency. See §6.

---

## 3. Runtime Data Flow

### Control plane (BLE provisioning)

```
iOS app  ──connect──►  NimBLE GATT (ble_pairing.cpp)
            writes:
              • WiFi SSID/password  ─► g_wifi_ssid / g_wifi_pass  ─► NVS  ─► BLE_WIFI_CRED_RECEIVED_BIT
              • iOS IP address      ─► g_ios_ip                          ─► BLE_IP_RECEIVED_BIT
              • UDP port            ─► g_stream_port
```

Shared state is exchanged through globals declared in `ble_pairing.h`, guarded by a FreeRTOS **event group** (`g_ble_event_group`) so tasks can block until the data they need has arrived.

### Data plane (video stream)

```
                  Core 0                                    Core 1
   ┌──────────────────────────────────┐      ┌────────────────────────────────────────┐
   │ udp_hello_task                    │      │ udp_receiver_task        (priority 6)   │
   │  every 1 s: sendto(iOS:5001,      │      │  recvfrom(:5000)                        │
   │             "HELLO")              │      │  frame-assembly state machine:          │
   └──────────────────────────────────┘      │    WAITING_SOI ──0xFFD8──► RECEIVING_DATA│
                  │ (iOS learns our IP,       │    RECEIVING_DATA: append DATA chunks    │
                  │  starts streaming)        │    RECEIVING_DATA ──0xFFD9──► emit frame │
                  ▼                           │                                          │
   iOS sends JPEG frames to :5000 ──────────► │  completed frame ──► xQueueSend          │
                                              └───────────────────────┬──────────────────┘
                                                                      │ jpeg_frame_queue
                                                                      │ (depth 2)
                                              ┌───────────────────────▼──────────────────┐
                                              │ jpeg_display_task        (priority 5)     │
                                              │  xQueueReceive → JPEGDEC.openRAM/decode   │
                                              │  per-MCU callback ─► esp_lcd_panel_draw   │
                                              │  free(frame.data)                         │
                                              └────────────────────────────────────────┘
                                                                      │
                                                                      ▼  QSPI + DMA
                                                            466×466 AMOLED panel
```

### Wire protocol (matches iOS `UDPStreamSender.swift`)

Each JPEG frame is sent as a sequence of UDP datagrams:

| Packet | Bytes | Meaning |
|--------|-------|---------|
| SOI | `0xFF 0xD8` (exactly 2) | Frame-start **delimiter** |
| DATA | raw JPEG, ≤1400 B each | Frame payload (already contains its own JPEG header) |
| EOI | `0xFF 0xD9` (exactly 2) | Frame-end **delimiter** |

The 2-byte SOI/EOI packets are *framing markers only* — they are not appended to the buffer, because each DATA payload is itself a complete JPEG bitstream (with its own `FFD8…FFD9`). The receiver accumulates DATA into a pre-allocated 128 KB SIMD-capable buffer, then copies the finished frame to a fresh heap buffer owned by the display task.

---

## 4. Concurrency Model & the Core-Isolation Constraint

This is the **single most important architectural decision** in the firmware. The dual-core ESP32-S3 tasks are pinned deliberately:

| Task | Core | Priority | Stack | Rationale |
|------|------|----------|-------|-----------|
| NimBLE host | 0 | 21 | — | RF subsystem; pinned via `CONFIG_BT_NIMBLE_PINNED_TO_CORE_0` |
| `udp_hello_task` | 0 | 3 | 4 KB | Co-located with WiFi/lwIP RF stack |
| `wifi_reconnect_task` | 0 | 3 | 4 KB | Watches for new BLE creds |
| `udp_receiver_task` | **1** | 6 | 8 KB | **Isolated from BLE scheduling** |
| `jpeg_display_task` | **1** | 5 | 8 KB | Decode/draw; below receiver so packets drain first |

**Why the receiver must live on Core 1:** the NimBLE host runs at priority 21 (`configMAX_PRIORITIES − 4`) and will preempt any lower-priority Core-0 task for several milliseconds during BLE events. If the UDP receiver shared Core 0, those stalls would overflow the lwIP UDP mailbox, **EOI framing packets would be dropped**, and the assembly state machine would restart on every frame → **zero frames decoded**. Pinning the receiver to Core 1 removes it from BLE scheduling entirely.

Three further defenses against packet loss (in `sdkconfig.defaults` / receiver setup):

1. **`CONFIG_LWIP_UDP_RECVMBOX_SIZE = 32`** (default 6) — ~44 KB of burst buffering to ride out preemption.
2. **`SO_RCVBUF = 64 KB`** socket buffer — absorbs bursts at the socket layer.
3. **Mid-frame SOI guard + 500 ms assembly timeout** — if an EOI is lost anyway, an incoming SOI (or timeout) discards the partial frame and re-syncs instead of overflowing.

`WIFI_PS_NONE` disables WiFi modem sleep so the radio stays hot for low-latency UDP.

---

## 5. Boot & Credential-Resolution Sequence

`app_main()` executes in this order:

```
1.  nvs_flash_init()                      — required by both WiFi and BLE
2.  ble_pairing_init()                    — start GATT advertising immediately
3.  xQueueCreate(jpeg_frame_queue, 2)
4.  read_lcd_id()                         — detect SH8601 (0x86) vs CO5300
5.  SPI bus + panel IO + panel driver     — QSPI @ 40 MHz, quad mode
6.  esp_lcd_panel_reset/init/disp_on      — g_panel_handle goes live
7.  lcd_trans_done_sem (DMA sync)
8.  Touch_Init()
9.  wifi_driver_init()                    — driver only, no connection yet
10. credential resolution (see below)
11. wifi_connect_to(ssid, pass)
12. create tasks: udp_hello(C0) · udp_rx(C1) · jpeg_display(C1) · wifi_recon(C0)
```

**WiFi credential precedence** (step 10):

```
NVS (previously provisioned)  ──found──►  use it
        │ not found
        ▼
wait ≤30 s for BLE creds      ──received──►  use BLE creds (g_wifi_ssid/pass)
        │ timeout
        ▼
hardcoded WIFI_SSID/WIFI_PASS in stream_config.h  (dev fallback)
```

**iOS-IP precedence** for the HELLO target (in `udp_hello_task`): wait ≤30 s for `BLE_IP_RECEIVED_BIT`; if it arrives use `g_ios_ip` (and keep switching live if it changes on reconnect); otherwise fall back to `STREAM_SERVER_IP`. After boot, `wifi_reconnect_task` keeps watching `BLE_WIFI_CRED_RECEIVED_BIT` so the device can re-associate if the phone pushes new hotspot credentials.

---

## 6. Rendering Path & Notable Trade-offs

- **No framebuffer, no LVGL.** `JPEGDEC.decode()` invokes `jpeg_decode_callback()` once per MCU block; the callback pushes that block straight to the panel via `esp_lcd_panel_draw_bitmap()`. This minimizes both RAM (no full-frame RGB565 buffer) and latency, at the cost of tearing being possible mid-frame.
- **DMA back-pressure.** `lcd_trans_done_callback` (an ISR) gives `lcd_trans_done_sem` when each QSPI DMA transfer completes; the decode callback takes the semaphore before issuing the next block, so decode never outruns the panel.
- **Pixel format** is fixed to `RGB565_BIG_ENDIAN` to match the panel's byte order (`CONFIG_LV_COLOR_16_SWAP`).
- **Frame queue depth is 2.** If the decoder falls behind, `udp_receiver_task` drops the newest completed frame rather than blocking the network path — preferring freshness/latency over completeness.
- **Double-buffer scaffolding exists but is disabled** (`lcd_draw_buffer[2]`, commented). The current path draws directly from the decoder's own MCU buffer.

---

## 7. Hardware & Memory

- **MCU:** ESP32-S3 @ 240 MHz, dual Xtensa LX7 core.
- **Flash:** 8 MB, QIO mode. Custom partition table (`partitions.csv`): `nvs` (24 KB), `phy_init` (4 KB), `factory` app (6 MB). No OTA.
- **PSRAM:** Octal SPIRAM @ 80 MHz, with instruction/rodata fetch enabled — needed for the 128 KB frame-assembly buffer plus per-frame heap allocations.
- **Panel:** SH8601 **or** CO5300 over `SPI2_HOST` QSPI (CS 9, PCLK 10, D0–D3 11–14, RST 21), 16 bpp, 40 MHz, quad mode. Init command set chosen at runtime by detected panel ID.
- **Touch:** FT3168 over I2C (SCL 48, SDA 47) via `touch_bsp` — initialized but not wired into the stream path.

---

## 8. Testing the Architecture Without the Phone

Host-side Python tools speak the same UDP protocol so each stage can be exercised independently of the iOS app and BLE:

- `tests/udp_test_sender.py` — sends synthetic JPEG frames to the ESP32 on :5000 (exercises the receiver + decode + render path; bypass BLE by setting `STREAM_SERVER_IP`).
- `tests/mjpeg_test_server.py` — serves an MJPEG-style stream.
- `jpeg_receiver.py` (repo root) — reference receiver that decodes the same wire protocol with OpenCV, useful for validating the iOS sender.

There are **no on-device unit tests**; verification is integration-style via these tools plus the serial monitor.
