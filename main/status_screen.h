#pragma once

#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Direct-to-panel status / "wait" screen.
//
// LVGL is disabled in this firmware (frames are drawn straight to the panel for
// latency), so this module renders centered text with a built-in 8×8 bitmap
// font directly into RGB565 and blits it over the QSPI panel.
//
// Threading: NOT thread-safe. It shares the panel and the LCD DMA-done
// semaphore with the JPEG decode path, so only ONE task may draw at a time.
// In practice it is called from app_main() during boot (before the streaming
// tasks exist) and from jpeg_display_task() (the only task that draws once the
// stream is running) — never concurrently.
// ─────────────────────────────────────────────────────────────────────────────

/// Bind the screen to the initialized panel and the LCD transfer-done semaphore.
/// Call once from app_main() after the panel and `lcd_trans_done_sem` are ready.
void status_screen_init(esp_lcd_panel_handle_t panel, SemaphoreHandle_t dma_done_sem);

/// Repaint the whole panel with up to four centered lines (pass NULL to skip a
/// line). Line 1 is rendered large (title); lines 2–4 are smaller. Long lines
/// are clipped to the panel width. Blocks until the blit completes.
void status_screen_show(const char *line1, const char *line2,
                        const char *line3, const char *line4);

// ─────────────────────────────────────────────────────────────────────────────
// Shared text/blit primitives (used by status_screen_show and the nav HUD band).
// The 8×8 bitmap font lives in status_screen.cpp; these expose it so other
// direct-to-panel renderers can reuse it without duplicating the glyph table.
// Same threading rule as the rest of the module: ONE task may draw at a time.
// ─────────────────────────────────────────────────────────────────────────────

/// Byte-swap a native RGB565 colour to the panel's big-endian order.
uint16_t status_be565(uint16_t color);

/// Pixel width of `s` rendered at `scale` (8 px per glyph × scale).
int status_text_width(const char *s, int scale);

/// Draw left-aligned text into a caller-owned RGB565 `canvas` of size
/// `canvas_w × canvas_h` (row stride = canvas_w) at top-left (x, y). `color_be`
/// must already be byte-swapped (see status_be565). Pixels outside the canvas
/// are clipped.
void status_draw_text(uint16_t *canvas, int canvas_w, int canvas_h,
                      int x, int y, const char *s, int scale, uint16_t color_be);

/// Blit a CPU-rendered RGB565 canvas region (w × h, stride = w) to the panel at
/// (dst_x, dst_y), strip-by-strip. Takes/restores the LCD DMA-done semaphore and
/// blocks until the final transfer completes. Requires status_screen_init().
void status_blit(const uint16_t *canvas, int w, int h, int dst_x, int dst_y);

#ifdef __cplusplus
}
#endif
