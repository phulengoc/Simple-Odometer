# Simple-Odometer

Simple-Odometer is an ESP-IDF firmware project for an ESP32-S3 driving a
466x466 round QSPI AMOLED display. It renders a custom tachometer/speedometer UI
with a rolling mechanical-style odometer, gear indicator, and RPM-driven outer
arc directly into RGB565 framebuffers, without LVGL.

The current application is a self-running demo: speed sweeps through simulated
gear bands, RPM eases through each shift, and the odometer rolls continuously so
the display behavior can be inspected on real hardware.

## Hardware Preview

Real hardware photos:

<TBD>

Close-up of the round AMOLED tachometer UI:

<TBD>

Odometer reel / gear indicator detail:

<TBD>

## Features

- 466x466 round AMOLED UI rendered CPU-side into RGB565 buffers.
- 270-degree RPM arc with anti-aliased tick marks.
- Large center speed readout in km/h.
- Gear indicator with neutral state.
- Rolling odometer reels with fractional movement and drum shading.
- SH8601 / CO5300 panel controller auto-detection.
- CO5300 horizontal offset compensation.
- FT3168 touch controller initialization over I2C.
- Host preview tooling for rendering the gauge frame without flashing hardware.

## Hardware

- Tested dev board: [Waveshare ESP32-S3-Touch-AMOLED-1.43C](https://www.waveshare.com/esp32-s3-touch-amoled-1.43c.htm?srsltid=AfmBOoroXqveHNwoGJR4NJwJvz6fQ2-zKf6xAC5W_67eQjcCgJj34KcY).
- ESP32-S3 with 8 MB flash and octal PSRAM.
- 466x466 round AMOLED panel over QSPI.
- Supported panel controllers: SH8601 and CO5300.
- FT3168 touch controller over I2C.

Panel pins:

| Signal | GPIO |
| --- | ---: |
| LCD CS | 9 |
| LCD PCLK | 10 |
| LCD D0 | 11 |
| LCD D1 | 12 |
| LCD D2 | 13 |
| LCD D3 | 14 |
| LCD RST | 21 |
| Touch SDA | 47 |
| Touch SCL | 48 |

## Project Layout

```text
main/
  main.cpp            Panel bring-up, demo state simulation, render loop
  tach_ring.cpp       Tachometer / odometer UI renderer
  tach_ring.h         Renderer API and tach state
  status_screen.cpp   Startup text screen and shared DMA strip blit path
  digits_font.h       Generated anti-aliased digit coverage glyphs
  text8x8.c/.h        Small bitmap text font support

components/
  read_lcd_id_bsp/    Panel controller ID detection
  touch_bsp/          FT3168 touch initialization and read helpers

tools/
  host_preview.cpp    Desktop preview harness for the tach renderer
  preview.sh          Builds the host preview and emits a PNG
  gen_digits.py       Regenerates the committed digit font header

docs/
  CO5300_PANEL_OFFSET.md
```

## Build, Flash, Monitor

This is an ESP-IDF project. Source your ESP-IDF environment first, then build:

```bash
idf.py build
```

Flash and monitor:

```bash
idf.py -p <PORT> flash monitor
```

If your local setup uses the `get_idf` helper, chain it with the build command:

```bash
get_idf && idf.py build
```

## Host Preview

The tach renderer can be compiled on the host to generate a preview image:

```bash
tools/preview.sh
```

Optional arguments:

```bash
tools/preview.sh <speed> <gear> <odo> <rpm> <out.png>
```

Example:

```bash
tools/preview.sh 72 4 4905.8 6500 tools/preview.png
```

The preview path is useful for layout iteration, but the physical display path
still matters because the panel uses big-endian RGB565 transfers and the CO5300
variant needs a 6-pixel X gap.

## Notes

- Rendering is intentionally direct: no LVGL, no scene graph, no GPU pipeline.
- Full-frame buffers live in PSRAM; DMA transfers use internal RAM staging
  strips.
- The CO5300 panel variant is shifted horizontally unless
  `esp_lcd_panel_set_gap(panel, 6, 0)` is applied after panel init.
- `main/digits_font.h` is generated and committed so normal firmware builds do
  not need Python font tooling.
