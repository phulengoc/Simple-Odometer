# CLAUDE.md

Guidance for Claude Code (claude.ai/code) when working in this repository.

## What This Is

A minimal ESP-IDF template for an ESP32-S3 driving a 466×466 round QSPI AMOLED
panel (SH8601 or CO5300, auto-detected at runtime via `read_lcd_id_bsp`) with an
FT3168 touch controller. It initializes the panel, draws text, and echoes touch
coordinates — a clean base to build a display app on.

Rendering is CPU-side into an RGB565 framebuffer, blitted straight to the panel
(no LVGL). The font/blit primitives live in `main/status_screen.{cpp,h}`.

## Build, Flash, Monitor

ESP-IDF project (CMake + `idf.py`). Source the toolchain first with the `get_idf`
shell alias (activates ESP-IDF 5.5.2 here; sets `IDF_PATH`, the xtensa-esp-elf
toolchain, and the Python venv). Shell state does not persist between commands,
so chain it: `get_idf && idf.py build`.

```bash
get_idf && idf.py build
get_idf && idf.py -p <PORT> flash monitor   # Ctrl-] to exit
idf.py menuconfig             # defaults live in sdkconfig.defaults
```

`set-target esp32s3` is not needed on a fresh checkout: there is no committed
`sdkconfig` (it's regenerated), and the target is auto-detected as `esp32s3` from
`sdkconfig.defaults`. The first build has the IDF component manager resolve
`esp_lcd_sh8601` (+ `cmake_utilities`) and regenerate `dependencies.lock`.

Verified building clean with ESP-IDF 5.5.2 (GCC 14.2.0). App image
`build/amoled_display_template.bin` ≈ 262 KB, ~96% of the 6 MB app partition free.

## Architecture

- `main/main.cpp` — `app_main()`: SPI/QSPI bus, panel IO, panel driver bring-up,
  touch init, then a poll loop that redraws the touch coordinates on press. Pins
  and panel init command sequences are at the top of this file.
- `components/touch_bsp` — FT3168 touch over I2C (`Touch_Init`, `getTouch`).
- `main/status_screen.{cpp,h}` — CPU text rendering with an 8×8 bitmap font and a
  strip-by-strip DMA blit. NOT thread-safe: only one task may draw at a time. The
  blit shares an LCD DMA-done semaphore that `lcd_trans_done_callback` (ISR in
  main.cpp) gives on each completed transfer.
- `components/read_lcd_id_bsp` — reads the panel controller ID for detection.
- `esp_lcd_sh8601` (panel driver) + `cmake_utilities` are pulled into
  `managed_components/` by the IDF component manager (see `main/idf_component.yml`).

## Hardware / Config Facts

- ESP32-S3, 8 MB flash (QIO), Octal PSRAM @ 80 MHz, CPU @ 240 MHz. Custom
  `partitions.csv` (6 MB factory app, no OTA).
- Panel over QSPI on `SPI2_HOST`: CS 9, PCLK 10, D0–D3 11–14, RST 21. Panel is
  466×466, RGB565, big-endian byte order (see `status_be565`).
- The **CO5300** variant (LCD ID `0xff`) needs a 6-column X gap
  (`esp_lcd_panel_set_gap(panel, 6, 0)`) or the image is shifted 6 px left; the
  SH8601 (`0x86`) does not. See `docs/CO5300_PANEL_OFFSET.md`.
- FT3168 touch over I2C: SCL 48, SDA 47.
- The full-screen framebuffer (~434 KB) is allocated in PSRAM, so `CONFIG_SPIRAM`
  must stay enabled.
