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

#ifdef __cplusplus
}
#endif
