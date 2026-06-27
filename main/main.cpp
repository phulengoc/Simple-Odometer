// ─────────────────────────────────────────────────────────────────────────────
// Minimal ESP32-S3 QSPI AMOLED display template.
//
// Brings up the 466×466 round AMOLED panel (SH8601 or CO5300, auto-detected at
// runtime) over QSPI and draws a few lines of text. Everything is rendered on
// the CPU into an RGB565 framebuffer and blitted straight to the panel — no LVGL.
//
// Use this as a starting point: replace the status_screen_show() call in
// app_main() with your own drawing.
// ─────────────────────────────────────────────────────────────────────────────

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include <math.h>

#include "esp_lcd_sh8601.h"
#include "read_lcd_id_bsp.h"
#include "touch_bsp.h"
#include "status_screen.h"
#include "tach_ring.h"

extern "C" {
    void app_main(void);
}

static const char *TAG = "display";

// ── Panel / bus configuration ─────────────────────────────────────────────────
#define LCD_HOST            SPI2_HOST
#define LCD_BIT_PER_PIXEL   16              // RGB565
#define SH8601_ID           0x86
#define CO5300_ID           0xff

// LCD pins (QSPI on SPI2_HOST)
#define PIN_NUM_LCD_CS      GPIO_NUM_9
#define PIN_NUM_LCD_PCLK    GPIO_NUM_10
#define PIN_NUM_LCD_DATA0   GPIO_NUM_11
#define PIN_NUM_LCD_DATA1   GPIO_NUM_12
#define PIN_NUM_LCD_DATA2   GPIO_NUM_13
#define PIN_NUM_LCD_DATA3   GPIO_NUM_14
#define PIN_NUM_LCD_RST     GPIO_NUM_21

#define LCD_H_RES           466
#define LCD_V_RES           466

static esp_lcd_panel_handle_t g_panel_handle      = NULL;
static SemaphoreHandle_t      g_lcd_trans_done_sem = NULL;

// Vendor-specific init sequences (datasheet command/parameter/delay triples).
static const sh8601_lcd_init_cmd_t sh8601_lcd_init_cmds[] = {
    {0x11, (uint8_t []){0x00},       0, 120},
    {0x44, (uint8_t []){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t []){0x00},       1, 0},
    {0x53, (uint8_t []){0x20},       1, 10},
    {0x51, (uint8_t []){0x00},       1, 10},
    {0x29, (uint8_t []){0x00},       0, 10},
    {0x51, (uint8_t []){0xFF},       1, 0},
};

static const sh8601_lcd_init_cmd_t co5300_lcd_init_cmds[] = {
    {0x11, (uint8_t []){0x00}, 0, 80},
    {0xC4, (uint8_t []){0x80}, 1, 0},
    {0x53, (uint8_t []){0x20}, 1, 1},
    {0x63, (uint8_t []){0xFF}, 1, 1},
    {0x51, (uint8_t []){0x00}, 1, 1},
    {0x29, (uint8_t []){0x00}, 0, 10},
    {0x51, (uint8_t []){0xFF}, 1, 0},
};

// ISR: signals the status_screen blit code that a DMA transfer has completed.
static bool lcd_trans_done_callback(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    BaseType_t high_task_woken = pdFALSE;
    if (g_lcd_trans_done_sem) {
        xSemaphoreGiveFromISR(g_lcd_trans_done_sem, &high_task_woken);
    }
    return high_task_woken == pdTRUE;
}

void app_main(void)
{
    // ── Detect which panel controller is fitted (SH8601 vs CO5300) ────────────
    uint8_t lcd_id = read_lcd_id();
    ESP_LOGI(TAG, "LCD ID: 0x%02X", lcd_id);

    // ── SPI (QSPI) bus ────────────────────────────────────────────────────────
    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num     = PIN_NUM_LCD_PCLK;
    buscfg.data0_io_num    = PIN_NUM_LCD_DATA0;
    buscfg.data1_io_num    = PIN_NUM_LCD_DATA1;
    buscfg.data2_io_num    = PIN_NUM_LCD_DATA2;
    buscfg.data3_io_num    = PIN_NUM_LCD_DATA3;
    buscfg.max_transfer_sz = LCD_H_RES * LCD_V_RES * LCD_BIT_PER_PIXEL / 8;
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // ── Panel IO ──────────────────────────────────────────────────────────────
    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num         = PIN_NUM_LCD_CS;
    io_config.dc_gpio_num         = -1;
    io_config.spi_mode            = 0;
    io_config.pclk_hz             = 80 * 1000 * 1000;   // 80 MHz QSPI for faster blit
    io_config.trans_queue_depth   = 10;
    io_config.on_color_trans_done = lcd_trans_done_callback;
    io_config.lcd_cmd_bits        = 32;
    io_config.lcd_param_bits      = 8;
    io_config.flags.quad_mode     = true;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                             &io_config, &io_handle));

    // ── Panel driver ──────────────────────────────────────────────────────────
    ESP_LOGI(TAG, "Install panel driver");
    sh8601_vendor_config_t vendor_config = {};
    vendor_config.flags.use_qspi_interface = 1;
    if (lcd_id == SH8601_ID) {
        vendor_config.init_cmds      = sh8601_lcd_init_cmds;
        vendor_config.init_cmds_size = sizeof(sh8601_lcd_init_cmds) / sizeof(sh8601_lcd_init_cmds[0]);
    } else {
        vendor_config.init_cmds      = co5300_lcd_init_cmds;
        vendor_config.init_cmds_size = sizeof(co5300_lcd_init_cmds) / sizeof(co5300_lcd_init_cmds[0]);
    }

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = PIN_NUM_LCD_RST;
    panel_config.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = LCD_BIT_PER_PIXEL;
    panel_config.vendor_config  = &vendor_config;

    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    // The CO5300 controller's active area starts 6 columns in; without this gap
    // the image is drawn 6 px too far left (right edge black, left edge clipped).
    // The SH8601 needs no offset.
    if (lcd_id != SH8601_ID) {
        ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 6, 0));
    }
    g_panel_handle = panel_handle;

    // ── DMA-done semaphore (shared with the status_screen blit) ───────────────
    g_lcd_trans_done_sem = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(g_lcd_trans_done_sem ? ESP_OK : ESP_ERR_NO_MEM);
    xSemaphoreGive(g_lcd_trans_done_sem);   // initially available

    ESP_LOGI(TAG, "Panel ready (%dx%d) — drawing", LCD_H_RES, LCD_V_RES);

    // ── Touch controller (FT3168 over I2C) ────────────────────────────────────
    Touch_Init();

    // status_screen shares the panel + DMA-done semaphore with the tach renderer.
    status_screen_init(panel_handle, g_lcd_trans_done_sem);
    status_screen_show("Tach Ring", "starting...", NULL, NULL);

    // ── Tach Ring demo ────────────────────────────────────────────────────────
    // Self-running: speed sweeps 0..~118 km/h, the odometer accumulates (time-
    // compressed so the reels visibly roll), the gear follows the speed, and the
    // RPM climbs through each gear then drops on the upshift. The displayed RPM
    // is eased toward its target each frame so the arc glides smoothly.
    tach_ring_init();
    ESP_LOGI(TAG, "Tach Ring demo running");

    const float dt = 0.033f;     // ~30 fps
    const float band[7] = { 0, 15, 35, 60, 85, 105, 120 };   // gear speed bands
    const float RPM_IDLE = 1000.0f, RPM_SHIFT = 8000.0f;

    float t = 0.0f;
    double odo = 4905.0;
    float rpm_disp = RPM_IDLE;
    tach_state_t st = {};
    while (true) {
        float speed = 59.0f * (1.0f - cosf(t * 0.5f));        // eased 0..118 sweep
        int gear = speed < 2 ? 0                              // neutral at standstill
                 : speed < 15 ? 1 : speed < 35 ? 2 : speed < 60 ? 3
                 : speed < 85 ? 4 : speed < 105 ? 5 : 6;

        // RPM = idle in neutral, else position of the speed within the gear band.
        float rpm_target = RPM_IDLE;
        if (gear > 0) {
            float lo = band[gear - 1], hi = band[gear];
            float frac = (speed - lo) / (hi - lo);
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            rpm_target = RPM_IDLE + frac * (RPM_SHIFT - RPM_IDLE);
        }
        rpm_disp += (rpm_target - rpm_disp) * 0.15f;          // smooth easing

        odo += speed * dt * 0.02;                             // time-compressed km

        st.rpm       = rpm_disp;
        st.speed_kmh = speed;
        st.gear      = gear;
        st.odo_km    = odo;

        int64_t t0 = esp_timer_get_time();
        tach_ring_render(&st);
        int64_t render_us = esp_timer_get_time() - t0;

        static int fc = 0; static int64_t acc = 0;
        acc += render_us;
        if (++fc >= 30) {
            ESP_LOGI(TAG, "render avg %lld us/frame (%.1f fps render-bound)",
                     acc / fc, 1e6f / (acc / fc));
            fc = 0; acc = 0;
        }

        t += dt;
        vTaskDelay(pdMS_TO_TICKS(2));   // minimal yield; frame rate is render-bound
    }
}
