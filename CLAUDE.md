# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

ESP32-S3 firmware that receives a JPEG video stream over WiFi/UDP from the MapNav iOS app and renders it on a 466×466 QSPI AMOLED panel. The on-device sender counterpart lives in the sibling `MapNavigationSwiftUI` / `maplibre-navigation-ios` projects in the parent workspace; this repo is the **receiver/display** end.

Despite directory and target names inherited from the upstream Espressif `qspi_with_ram` LVGL example, this is now a bespoke streaming app. **`README.md` is the stale upstream example doc** (LVGL dashboard demo) — ignore it. `README_STREAM.md` is closer but predates full JPEG decode (it still says "Android" and "metadata only"); the source is the source of truth.

## Build, Flash, Test

This is an **ESP-IDF 5.3.2** project (CMake + `idf.py`), not an npm project. Always source the IDF toolchain first via the `get_idf` shell alias (it sources `$IDF_PATH/export.sh`); without it `idf.py` / `xtensa-esp32s3-elf-gcc` are not on `PATH`.

```bash
get_idf
idf.py set-target esp32s3     # only needed on a fresh checkout
idf.py build
idf.py -p <PORT> flash monitor   # Ctrl-] to exit monitor
idf.py menuconfig             # editing sdkconfig; defaults live in sdkconfig.defaults
```

There are **no on-device unit tests**. Verification is done with host-side Python tools that speak the UDP protocol so you can exercise the firmware without the iOS app:

```bash
python tests/udp_test_sender.py      # sends synthetic JPEG frames to the ESP32 on :5000
python tests/mjpeg_test_server.py    # serves an MJPEG-style stream
python jpeg_receiver.py              # reference receiver (decodes the same protocol with OpenCV)
```

## Architecture

Entry point is `app_main()` in `main/example_qspi_with_ram.cpp` (~1000 lines, the bulk of the app). BLE provisioning is split into `main/ble_pairing.cpp`. All tunables live in `main/stream_config.h`.

End-to-end flow:

1. **BLE provisioning** (`ble_pairing.cpp`, NimBLE) — advertises as `ESP32-MapNav`, exposes a GATT service where iOS writes its IP, UDP port, and WiFi SSID/password. Credentials persist in NVS. Shared state (`g_ios_ip`, `g_stream_port`, `g_wifi_ssid/pass`) and an event group (`BLE_IP_RECEIVED_BIT`, `BLE_WIFI_CRED_RECEIVED_BIT`) are declared in `ble_pairing.h`.
2. **WiFi STA** — `wifi_reconnect_task` waits for BLE creds (falling back to the hardcoded `WIFI_SSID`/`WIFI_PASS`), then `wifi_connect_to()`. Modem sleep is disabled (`WIFI_PS_NONE`) for low UDP latency.
3. **Discovery** — `udp_hello_task` sends a "HELLO" datagram to the iOS IP on port **5001** every `UDP_HELLO_INTERVAL` ms so the sender learns the ESP32's address. BLE-supplied IP takes precedence over the `STREAM_SERVER_IP` fallback.
4. **Frame reception** — `udp_receiver_task` binds port **5000** and runs a frame-assembly state machine: an SOI packet (`0xFF 0xD8`) starts a frame, raw JPEG chunks (≤1400 B) accumulate into a buffer, an EOI packet (`0xFF 0xD9`) closes it. Completed frames go onto `jpeg_frame_queue`.
5. **Decode + display** — `jpeg_display_task` pulls frames, decodes with the BitBank `JPEGDEC` decoder (`RGB565_BIG_ENDIAN`, `JPEG_USES_DMA`), and `jpeg_decode_callback` draws each MCU block straight to the panel. `lcd_trans_done_callback` (ISR) signals DMA completion via a semaphore.

### Core isolation — the single most important constraint

Read the comment block near the top of `example_qspi_with_ram.cpp` (CPU core assignments) before touching task creation or BLE.

- **Core 0**: NimBLE host (priority 21), WiFi/lwIP, `udp_hello_task`.
- **Core 1**: `udp_receiver_task` (priority 6) and `jpeg_display_task` (priority 5), pinned via `xTaskCreatePinnedToCore`.

NimBLE runs at priority 21 and will preempt any Core-0 task for several ms. If the UDP receiver shares Core 0, those stalls overflow the lwIP UDP mailbox, EOI packets get dropped, and the assembly state machine restarts every frame → **zero frames decoded**. Keeping the receiver on Core 1 removes it from BLE scheduling entirely. The receiver also sits *above* the decoder in priority so it drains packets before decode work runs. `sdkconfig.defaults` raises `CONFIG_LWIP_UDP_RECVMBOX_SIZE` to 32 (~44 KB burst buffer) as further insurance.

### LVGL is intentionally disabled

LVGL is still a dependency and the component is present, but all LVGL paths in `example_qspi_with_ram.cpp` are commented out — the JPEG decoder writes directly to the LCD for latency. Do not re-enable LVGL rendering for the stream path without understanding this trade-off.

## Hardware / Config Facts

- Target: ESP32-S3, 8 MB flash (QIO), SPIRAM in OCT mode @ 80 MHz, CPU @ 240 MHz. Custom `partitions.csv` (6 MB factory app, no OTA).
- Panel: SH8601 **or** CO5300 over QSPI on `SPI2_HOST`, detected at runtime via `read_lcd_id_bsp` (`SH8601_ID 0x86` / `CO5300_ID 0xff`). LCD pins: CS 9, PCLK 10, D0–D3 11–14, RST 21. Touch (FT3168) via `touch_bsp` on I2C (SCL 48 / SDA 47).
- Ports/sizes in `stream_config.h`: stream RX 5000, discovery TX 5001, max frame 128 KB, frame 466×466.
- **WiFi creds and the dev fallback IP are hardcoded in `main/stream_config.h`** — they are real values committed to the repo, not placeholders. Treat changes to that file as touching secrets; BLE provisioning is meant to override them in production.

## Components

Local components in `components/`: `jpegdec` (BitBank decoder with ESP32-S3 SIMD assembly — `s3_simd_*.S`), `touch_bsp`, `read_lcd_id_bsp`, `lvgl`, `ui_bsp`, `sd_card_bsp`, `user_app`. `esp_lcd_sh8601` and `espressif/esp-dsp` are pulled into `managed_components/` by the IDF component manager (declared in `main/idf_component.yml`). `main/CMakeLists.txt` lists the `REQUIRES` set and suppresses C++ designated-initializer warnings.
