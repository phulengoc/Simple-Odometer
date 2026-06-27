# ESP32-S3 QSPI AMOLED — Display Template

A minimal ESP-IDF starting point for the 466×466 round AMOLED panel (SH8601 or
CO5300, auto-detected at runtime) driven over QSPI, with the FT3168 touch
controller. It brings the panel up, draws a few lines of text, and shows the
touch coordinates when you press the screen. Use it as the base for your own UI.

Rendering is done on the CPU into an RGB565 framebuffer and blitted straight to
the panel (no LVGL), using a small built-in 8×8 bitmap font.

## Layout

```
main/
  main.cpp            app_main(): panel bring-up, demo text, touch poll loop
  status_screen.cpp   CPU text rendering + DMA blit to the panel (8×8 font)
  status_screen.h
components/
  read_lcd_id_bsp/    reads the panel controller ID for SH8601/CO5300 detection
  touch_bsp/          FT3168 touch controller over I2C (Touch_Init / getTouch)
managed_components/   esp_lcd_sh8601 panel driver (+ cmake_utilities), via IDF
                      component manager
partitions.csv        8 MB flash, single 6 MB factory app (no OTA)
sdkconfig.defaults    target/flash/PSRAM/clock defaults
```

## Hardware

- ESP32-S3, 8 MB flash (QIO), Octal PSRAM @ 80 MHz, CPU @ 240 MHz.
- Panel over QSPI on `SPI2_HOST`: CS 9, PCLK 10, D0–D3 11–14, RST 21.
- FT3168 touch over I2C: SCL 48, SDA 47 (see `components/touch_bsp`).

Pins and the panel init sequences live at the top of `main/main.cpp`.

## Build, flash, monitor

This is an ESP-IDF project. Source the toolchain first (`. $IDF_PATH/export.sh`),
then:

```bash
idf.py set-target esp32s3     # first checkout only
idf.py build
idf.py -p <PORT> flash monitor   # Ctrl-] to exit the monitor
```

The first build downloads `esp_lcd_sh8601` into `managed_components/`.

## Drawing your own content

Replace the `status_screen_show(...)` call at the end of `app_main()`. For custom
graphics, render RGB565 into a buffer and call `status_blit()`, or draw directly
with `esp_lcd_panel_draw_bitmap(g_panel_handle, ...)`. Note RGB565 must be
written big-endian for this panel — see `status_be565()`.
