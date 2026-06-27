#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Pure (no ESP-IDF deps) 8×8 bitmap text primitives, rendering into a caller-
// owned big-endian RGB565 canvas. Shared by the on-device UI and the host-side
// preview harness, so this unit must stay free of FreeRTOS / esp_* includes.
//
// The panel expects big-endian RGB565, so colours are byte-swapped — pass
// colours through rgb565_be() (or t8_be565()).
// ─────────────────────────────────────────────────────────────────────────────
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Byte-swap a native RGB565 colour to the panel's big-endian order.
uint16_t t8_be565(uint16_t color);

/// Pixel width of `s` rendered at `scale` (8 px per glyph × scale).
int t8_text_width(const char *s, int scale);

/// Draw left-aligned text into a `canvas_w × canvas_h` big-endian RGB565 canvas
/// at top-left (x, y). `color_be` must already be byte-swapped. Clipped to canvas.
void t8_draw_text(uint16_t *canvas, int canvas_w, int canvas_h,
                  int x, int y, const char *s, int scale, uint16_t color_be);

/// Convenience: draw text horizontally centred on `cx`.
void t8_draw_text_centered(uint16_t *canvas, int canvas_w, int canvas_h,
                           int cx, int y, const char *s, int scale, uint16_t color_be);

#ifdef __cplusplus
}
#endif
