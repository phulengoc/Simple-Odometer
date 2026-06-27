# CO5300 Panel Horizontal Offset (image shifted left)

## Symptom

On the 466×466 round AMOLED, the rendered UI was **not horizontally centred**:

- a thin **black margin** along the **right** edge, and
- content **clipped** on the **left** edge (e.g. the RPM arc was cut off on the
  left while the right had empty pixels).

The framebuffer itself was symmetric (the host preview rendered the arc perfectly
centred at panel-centre 233,233), so this was a **panel-addressing** problem, not
a drawing-math problem.

## Root cause

This board auto-detects its controller at runtime via `read_lcd_id()`:

- `SH8601` → ID `0x86`
- `CO5300` → ID `0xff`  ← **this board**

The **CO5300's active area does not start at column 0** — it begins **6 columns
in**. Its addressable RAM is effectively offset, so a bitmap written to logical
columns `0..465` lands on the panel such that:

- logical columns `0..5` fall outside the visible area on the left (clipped), and
- panel columns `466..471` on the right are never written (black margin).

Net effect: the whole image is shifted **6 px to the left**.

The SH8601 has no such offset.

This is a known quirk of the upstream Espressif `qspi_with_ram` example: its LVGL
flush callback added `+0x06` to the X coordinates **only for the CO5300**:

```c
const int offsetx1 = (READ_LCD_ID == SH8601_ID) ? area->x1 : area->x1 + 0x06;
```

When the firmware was rewritten to draw straight to the panel (no LVGL), that
per-flush `+6` adjustment was dropped, so nothing compensated for the offset.

## How it was diagnosed

- The arc geometry is centred at `(233, 233)` with symmetric endpoints, and the
  **host preview** (same renderer, dumped to PNG) showed it perfectly centred →
  the math was correct.
- The shift was only visible on the **physical panel**, and the direction
  (right margin + left clip = image shifted left) matched a **start-of-line
  column offset** on the controller.
- The detected controller was the CO5300 (`LCD ID: 0xFF` in the boot log), which
  is exactly the one the upstream example special-cased with `+0x06`.

## Fix

Set the panel's X gap once, right after init, for the CO5300 only. `esp_lcd`
applies this gap to every subsequent `draw_bitmap`, so all drawing
(`tach_ring`, `status_screen`) is shifted into the true active area
automatically — no per-draw changes needed.

`main/main.cpp`, after `esp_lcd_panel_disp_on_off(...)`:

```c
// The CO5300 controller's active area starts 6 columns in; without this gap
// the image is drawn 6 px too far left (right edge black, left edge clipped).
// The SH8601 needs no offset.
if (lcd_id != SH8601_ID) {
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 6, 0));
}
```

Verified on device: arc and content are now horizontally centred.

## Notes

- `lcd_id` comes from `read_lcd_id()` (`components/read_lcd_id_bsp`), read before
  the SPI bus is brought up.
- Only the **X** gap is needed (`6, 0`); the panel has no vertical offset.
- If a future panel variant shows a residual shift, this `6` is the value to
  adjust (it is the controller's column start offset, `0x06`).
- Because the gap lives in the panel driver, it also covers the boot/status
  screen path (`status_screen.cpp`) — there is nothing per-call to remember.
```
