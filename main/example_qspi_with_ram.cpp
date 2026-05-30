
#include <cstdint>
#include <stdio.h>
#include <string.h>
#include <cstring>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

// #include "draw/lv_img_buf.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
// #include "lv_conf_internal.h"
// #include "misc/lv_color.h"
#include "jpegdec/test_images/tulips.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sockets.h"

// #include "lvgl.h"
#include "esp_lcd_sh8601.h"
#include "touch_bsp.h"
#include "read_lcd_id_bsp.h"
#include "stream_config.h"
#include "JPEGDEC.h"

extern "C" {
    void app_main(void);
}

static const char *TAG = "jpeg_stream_receiver";

// Event group bits for WiFi
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// JPEG stream processing
#define JPEG_SOI_MARKER    0xFFD8  // Start of Image
#define JPEG_EOI_MARKER    0xFFD9  // End of Image

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static const int MAXIMUM_RETRY = 5;

// JPEG buffer and processing
static QueueHandle_t jpeg_frame_queue;

typedef struct {
    uint8_t *data;
    size_t size;
} jpeg_frame_t;

// Display globals (LVGL disabled for performance)
static esp_lcd_panel_handle_t g_panel_handle = NULL;
// static SemaphoreHandle_t lvgl_mux = NULL;
// static lv_disp_t *display = NULL;
// static lv_obj_t *img_obj = NULL;

// JPEG decoder
static JPEGDEC jpeg_dec;
// static uint16_t *decoded_image_buffer = NULL; // Not needed for direct LCD drawing
// static lv_obj_t *stream_img_obj = NULL;

// Function forward declarations (LVGL functions commented out)
// static bool example_lvgl_lock(int timeout_ms);
// static void example_lvgl_unlock(void);
// static void example_lvgl_unlock(void);

#define LCD_HOST    SPI2_HOST

#define EXAMPLE_Rotate_90
#define SH8601_ID 0x86
#define CO5300_ID 0xff
static uint8_t READ_LCD_ID = 0x00; 

#if CONFIG_LV_COLOR_DEPTH == 32
#define LCD_BIT_PER_PIXEL       (24)
#elif CONFIG_LV_COLOR_DEPTH == 16
#define LCD_BIT_PER_PIXEL       (16)
#endif

#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL  1
#define EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL !EXAMPLE_LCD_BK_LIGHT_ON_LEVEL
#define EXAMPLE_PIN_NUM_LCD_CS            (GPIO_NUM_9)
#define EXAMPLE_PIN_NUM_LCD_PCLK          (GPIO_NUM_10) 
#define EXAMPLE_PIN_NUM_LCD_DATA0         (GPIO_NUM_11)
#define EXAMPLE_PIN_NUM_LCD_DATA1         (GPIO_NUM_12)
#define EXAMPLE_PIN_NUM_LCD_DATA2         (GPIO_NUM_13)
#define EXAMPLE_PIN_NUM_LCD_DATA3         (GPIO_NUM_14)
#define EXAMPLE_PIN_NUM_LCD_RST           (GPIO_NUM_21)
#define EXAMPLE_PIN_NUM_BK_LIGHT          (-1)

// The pixel number in horizontal and vertical
#define EXAMPLE_LCD_H_RES              466
#define EXAMPLE_LCD_V_RES              466

// LVGL configuration commented out for direct LCD access
// #define EXAMPLE_LVGL_BUF_HEIGHT        (EXAMPLE_LCD_V_RES / 4)
// #define EXAMPLE_LVGL_TICK_PERIOD_MS    2
// #define EXAMPLE_LVGL_TASK_MAX_DELAY_MS 500
// #define EXAMPLE_LVGL_TASK_MIN_DELAY_MS 1
// #define EXAMPLE_LVGL_TASK_STACK_SIZE   (4 * 1024)
// #define EXAMPLE_LVGL_TASK_PRIORITY     2

// CPU core assignments for dual-core optimization
#define UDP_RECEIVER_CPU    0  // CPU 0 for network I/O
#define JPEG_DECODER_CPU    1  // CPU 1 for JPEG decoding/LCD drawing

// WiFi event handler
static void event_handler(void* arg, esp_event_base_t event_base,
                         int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Initialize WiFi
static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char*)wifi_config.sta.password, WIFI_PASS);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    // Wait until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
    // number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above)
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    // xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
    // happened.
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

#define EXAMPLE_LVGL_TASK_PRIORITY     2

// ─────────────────────────────────────────────────────────────────────────────
// UDP Streaming Tasks
//
// Protocol (matches iOS UDPStreamSender.swift):
//   ESP32 ──UDP 5001──► iOS   "HELLO\0" every UDP_HELLO_INTERVAL ms
//   ESP32 ◄──UDP 5000── iOS   JPEG frames split into packets:
//     SOI  packet : 0xFF 0xD8  (exactly 2 bytes)
//     DATA packets: raw JPEG   (≤1400 bytes each)
//     EOI  packet : 0xFF 0xD9  (exactly 2 bytes)
// ─────────────────────────────────────────────────────────────────────────────

// ── UDP HELLO task ────────────────────────────────────────────────────────────
// Sends "HELLO" to iOS/Python sender on UDP_DISCOVER_PORT so it learns our IP.
static void udp_hello_task(void *pvParameters)
{
    ESP_LOGI(TAG, "UDP hello task: sending HELLO to %s:%d every %d ms",
             STREAM_SERVER_IP, UDP_DISCOVER_PORT, UDP_HELLO_INTERVAL);

    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(UDP_DISCOVER_PORT);
    inet_pton(AF_INET, STREAM_SERVER_IP, &dest.sin_addr);

    while (1) {
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            ESP_LOGE(TAG, "HELLO: socket() failed errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }

        const char *hello = "HELLO";
        while (1) {
            int n = sendto(sock, hello, strlen(hello) + 1, 0,
                           (struct sockaddr *)&dest, sizeof(dest));
            if (n < 0) {
                ESP_LOGE(TAG, "HELLO: sendto errno %d — recreating socket", errno);
                break;
            }
            ESP_LOGD(TAG, "HELLO sent to %s:%d", STREAM_SERVER_IP, UDP_DISCOVER_PORT);
            vTaskDelay(pdMS_TO_TICKS(UDP_HELLO_INTERVAL));
        }

        close(sock);
    }

    vTaskDelete(NULL);
}

// ── UDP receiver task ─────────────────────────────────────────────────────────
// Receives JPEG frame packets on UDP_STREAM_PORT and feeds assembled frames
// to the display task via jpeg_frame_queue.

typedef enum {
    UDP_STATE_WAITING_SOI = 0,
    UDP_STATE_RECEIVING_DATA,
} udp_rx_state_t;

static void udp_receiver_task(void *pvParameters)
{
    ESP_LOGI(TAG, "UDP receiver task: listening on port %d", UDP_STREAM_PORT);

    static uint8_t pkt_buf[UDP_MAX_PACKET];
    uint8_t  *frame_buf  = NULL;
    size_t    frame_used = 0;
    udp_rx_state_t state = UDP_STATE_WAITING_SOI;
    TickType_t last_data_tick = 0;
    const TickType_t FRAME_TIMEOUT_TICKS = pdMS_TO_TICKS(3000);

    // Pre-allocate frame assembly buffer once (reused across frames)
    frame_buf = (uint8_t *)heap_caps_malloc(MAX_JPEG_FRAME_SIZE, MALLOC_CAP_SIMD);
    if (!frame_buf) {
        ESP_LOGE(TAG, "UDP rx: failed to allocate frame buffer (%d B)", MAX_JPEG_FRAME_SIZE);
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        // ── Create and bind UDP socket ────────────────────────────────────────
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            ESP_LOGE(TAG, "UDP rx: socket() errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }

        int reuse = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        // 1-second receive timeout so we can detect frame assembly timeouts
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_in local = {};
        local.sin_family      = AF_INET;
        local.sin_addr.s_addr = INADDR_ANY;
        local.sin_port        = htons(UDP_STREAM_PORT);

        if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
            ESP_LOGE(TAG, "UDP rx: bind(::%d) errno %d", UDP_STREAM_PORT, errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }

        ESP_LOGI(TAG, "UDP receiver bound on port %d", UDP_STREAM_PORT);
        state      = UDP_STATE_WAITING_SOI;
        frame_used = 0;

        // ── Receive loop ──────────────────────────────────────────────────────
        while (1) {
            int n = recvfrom(sock, pkt_buf, sizeof(pkt_buf), 0, NULL, NULL);

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Receive timeout — check for stuck frame assembly
                    if (state == UDP_STATE_RECEIVING_DATA &&
                        (xTaskGetTickCount() - last_data_tick) > FRAME_TIMEOUT_TICKS) {
                        ESP_LOGW(TAG, "UDP rx: frame assembly timeout — resetting");
                        state      = UDP_STATE_WAITING_SOI;
                        frame_used = 0;
                    }
                    continue;
                }
                ESP_LOGE(TAG, "UDP rx: recvfrom errno %d — rebinding", errno);
                break;
            }

            if (n == 0) continue;

            switch (state) {
            case UDP_STATE_WAITING_SOI:
                // SOI framing packet: exactly 2 bytes 0xFF 0xD8.
                // This is a delimiter only — the JPEG DATA packets that follow
                // already contain the full JPEG including its own 0xFF 0xD8 header.
                if (n == 2 && pkt_buf[0] == 0xFF && pkt_buf[1] == 0xD8) {
                    frame_used     = 0;   // reset assembly buffer
                    state          = UDP_STATE_RECEIVING_DATA;
                    last_data_tick = xTaskGetTickCount();
                    ESP_LOGD(TAG, "UDP rx: SOI — assembling frame");
                }
                break;

            case UDP_STATE_RECEIVING_DATA:
                last_data_tick = xTaskGetTickCount();

                if (n == 2 && pkt_buf[0] == 0xFF && pkt_buf[1] == 0xD9) {
                    // EOI framing packet: the DATA bytes already form a complete JPEG.
                    // Do NOT append 0xFF 0xD9 — it would duplicate the JPEG's own EOI.
                    // Copy to a new heap buffer owned by the display task
                    uint8_t *queued = (uint8_t *)heap_caps_malloc(frame_used, MALLOC_CAP_SIMD);
                    if (queued) {
                        memcpy(queued, frame_buf, frame_used);
                        jpeg_frame_t frm = { .data = queued, .size = frame_used };
                        if (xQueueSend(jpeg_frame_queue, &frm, 0) == pdTRUE) {
                            ESP_LOGI(TAG, "UDP rx: queued frame %u B", (unsigned)frame_used);
                        } else {
                            ESP_LOGD(TAG, "UDP rx: display queue full — dropping frame");
                            free(queued);
                        }
                    } else {
                        ESP_LOGE(TAG, "UDP rx: alloc %u B failed", (unsigned)frame_used);
                    }
                    state      = UDP_STATE_WAITING_SOI;
                    frame_used = 0;

                } else {
                    // DATA packet: append to assembly buffer
                    if (frame_used + (size_t)n > MAX_JPEG_FRAME_SIZE) {
                        ESP_LOGE(TAG, "UDP rx: frame overflow (%zu B) — discarding", frame_used);
                        state      = UDP_STATE_WAITING_SOI;
                        frame_used = 0;
                    } else {
                        memcpy(frame_buf + frame_used, pkt_buf, n);
                        frame_used += (size_t)n;
                    }
                }
                break;
            }
        }

        close(sock);
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
    }

    free(frame_buf);   // never reached, but good practice
    vTaskDelete(NULL);
}


// Add after the JPEG decoder globals (around line 75)
// Double buffering for LCD drawing with DMA synchronization
#define LCD_DRAW_BUFFER_SIZE (128 * 16 * 2) // Max JPEG MCU block size (128x16) in RGB565 format
static uint16_t *lcd_draw_buffer[2] = {NULL, NULL}; // Two buffers for double buffering
static uint8_t current_buffer_idx = 0;
static SemaphoreHandle_t lcd_trans_done_sem = NULL;

// LCD transaction done callback - called from ISR when DMA transfer completes
static bool lcd_trans_done_callback(esp_lcd_panel_io_handle_t panel_io, 
                                   esp_lcd_panel_io_event_data_t *edata, 
                                   void *user_ctx)
{
    BaseType_t high_task_woken = pdFALSE;
    if (lcd_trans_done_sem) {
        xSemaphoreGiveFromISR(lcd_trans_done_sem, &high_task_woken);
    }
    return high_task_woken == pdTRUE;
}

// JPEG decode callback - optimized with double buffering and DMA synchronization
static int jpeg_decode_callback(JPEGDRAW *pDraw)
{
    // ESP_LOGI(TAG, "Draw x=%d, y=%d, w=%d, h=%d", pDraw->x, pDraw->y, pDraw->iWidth, pDraw->iHeight);
    // Input validation
    if (!g_panel_handle || !pDraw || !pDraw->pPixels) {
        ESP_LOGE(TAG, "Invalid parameters in jpeg_decode_callback");
        return 0; // Stop decoding
    }
    
    // Complete bounds check
    if (pDraw->x >= EXAMPLE_LCD_H_RES || pDraw->y >= EXAMPLE_LCD_V_RES) {
        ESP_LOGE(TAG, "Draw position out of bounds: x=%d, y=%d", pDraw->x, pDraw->y);
        return 1; // Skip this draw but continue
    }
    
    // Calculate safe copy dimensions
    int copy_width = pDraw->iWidth;
    int copy_height = pDraw->iHeight;
    
    if (pDraw->x + copy_width > EXAMPLE_LCD_H_RES) {
        copy_width = EXAMPLE_LCD_H_RES - pDraw->x;
        // ESP_LOGI(TAG, "Copy width > EXAMPLE_LCD_H_RES, adjusted to %d", copy_width);
    }
    
    if (pDraw->y + copy_height > EXAMPLE_LCD_V_RES) {
        copy_height = EXAMPLE_LCD_V_RES - pDraw->y;
        // ESP_LOGI(TAG, "Copy height > EXAMPLE_LCD_V_RES, adjusted to %d", copy_height);
    }
    
    if (copy_width <= 0 || copy_height <= 0) {
        return 1; // Nothing to copy, but continue
    }
    
    // Wait for previous DMA transfer to complete before reusing buffer
    // if (pDraw->x != 384) return 1;

    if (lcd_trans_done_sem) {
        if (xSemaphoreTake(lcd_trans_done_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "LCD transaction timeout waiting for previous transfer");
        }
    }
    
    // Get current buffer for this draw operation
    // uint16_t *draw_buffer = lcd_draw_buffer[current_buffer_idx];
    
    // Copy pixel data to our persistent buffer
    // size_t pixel_count = copy_width * copy_height;
    // memcpy(draw_buffer, pDraw->pPixels, pixel_count * sizeof(uint16_t));
    
    // Start DMA transfer with our persistent buffer
    esp_err_t ret = esp_lcd_panel_draw_bitmap(g_panel_handle, 
                                              pDraw->x, pDraw->y,
                                              pDraw->x + copy_width, pDraw->y + copy_height,
                                              pDraw->pPixels);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCD draw failed at (%d,%d) %dx%d: %s", 
                 pDraw->x, pDraw->y, copy_width, copy_height, esp_err_to_name(ret));
        // Give semaphore back since we're not doing DMA
        if (lcd_trans_done_sem) {
            xSemaphoreGive(lcd_trans_done_sem);
        }
        return 0; // Stop decoding on LCD error
    }
    
    // Switch to other buffer for next draw
    // current_buffer_idx = 1 - current_buffer_idx;
    // vTaskDelay(500);
    return 1; // Continue decoding
}

// extern const lv_img_dsc_t map; // LVGL type commented out
// Simplified JPEG display task - decodes directly to LCD (no LVGL)
static void jpeg_display_task(void *pvParameters)
{
    jpeg_frame_t frame;
    static int frame_count = 0;
    
    // Wait for panel handle to be initialized
    while (g_panel_handle == NULL) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    ESP_LOGI(TAG, "JPEG display task started - drawing directly to LCD");
    
    while (1) {
        if (xQueueReceive(jpeg_frame_queue, &frame, portMAX_DELAY)) {
            ESP_LOGD(TAG, "Received JPEG frame: %d bytes", frame.size);
            frame_count++;

            bool decode_success = false;
            
            // Measure decode time
            int64_t decode_start = esp_timer_get_time();
            
            // Open and decode JPEG with direct LCD callback
            if (jpeg_dec.openRAM(frame.data, frame.size, jpeg_decode_callback)) {
                ESP_LOGD(TAG, "JPEG opened: %dx%d, bpp: %d", 
                        jpeg_dec.getWidth(), jpeg_dec.getHeight(), jpeg_dec.getBpp());
                
                // Set pixel format to RGB565 for direct LCD output
                jpeg_dec.setPixelType(RGB565_BIG_ENDIAN);
                
                // int64_t decode_only_start = esp_timer_get_time();
                if (jpeg_dec.decode(0, 0, JPEG_USES_DMA)) {
                    // int64_t decode_end = esp_timer_get_time();
                    // float decode_time_ms = (decode_end - decode_only_start) / 1000.0f;
                    // float total_time_ms = (decode_end - decode_start) / 1000.0f;
                    // ESP_LOGI(TAG, "Frame %d: decode=%0.2fms, total=%0.2fms, fps=%0.1f", 
                            // frame_count, decode_time_ms, total_time_ms, 1000.0f / total_time_ms);
                    decode_success = true;
                }
                jpeg_dec.close();
            } else {
                ESP_LOGE(TAG, "Failed to open JPEG frame %d", frame_count);
            }

            if (!decode_success) {
                ESP_LOGE(TAG, "Decode failed for frame %d", frame_count);
            }
            
            // Free the frame data
            free(frame.data);
        }
    }
}

// LCD initialization commands
static const sh8601_lcd_init_cmd_t sh8601_lcd_init_cmds[] = 
{
    {0x11, (uint8_t []){0x00}, 0, 120},
    {0x44, (uint8_t []){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t []){0x00}, 1, 0},
    {0x53, (uint8_t []){0x20}, 1, 10},
    {0x51, (uint8_t []){0x00}, 1, 10},
    {0x29, (uint8_t []){0x00}, 0, 10},
    {0x51, (uint8_t []){0xFF}, 1, 0},
};

static const sh8601_lcd_init_cmd_t co5300_lcd_init_cmds[] = 
{
    {0x11, (uint8_t []){0x00}, 0, 80},   
    {0xC4, (uint8_t []){0x80}, 1, 0},
    {0x53, (uint8_t []){0x20}, 1, 1},
    {0x63, (uint8_t []){0xFF}, 1, 1},
    {0x51, (uint8_t []){0x00}, 1, 1},
    {0x29, (uint8_t []){0x00}, 0, 10},
    {0x51, (uint8_t []){0xFF}, 1, 0},
};

// LVGL callback functions commented out for direct LCD access
/*
static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}

static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
    const int offsetx1 = (READ_LCD_ID == SH8601_ID) ? area->x1 : area->x1 + 0x06;
    const int offsetx2 = (READ_LCD_ID == SH8601_ID) ? area->x2 : area->x2 + 0x06;
    const int offsety1 = area->y1;
    const int offsety2 = area->y2;
    
#if LCD_BIT_PER_PIXEL == 24
    uint8_t *to = (uint8_t *)color_map;
    uint8_t temp = 0;
    uint16_t pixel_num = (offsetx2 - offsetx1 + 1) * (offsety2 - offsety1 + 1);

    // Special dealing for first pixel
    temp = color_map[0].ch.blue;
    *to++ = color_map[0].ch.red;
    *to++ = color_map[0].ch.green;
    *to++ = temp;
    // Normal dealing for other pixels
    for (int i = 1; i < pixel_num; i++) {
        *to++ = color_map[i].ch.red;
        *to++ = color_map[i].ch.green;
        *to++ = color_map[i].ch.blue;
    }
#endif

    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
}

void example_lvgl_rounder_cb(struct _lv_disp_drv_t *disp_drv, lv_area_t *area)
{
    uint16_t x1 = area->x1;
    uint16_t x2 = area->x2;
    uint16_t y1 = area->y1;
    uint16_t y2 = area->y2;

    area->x1 = (x1 >> 1) << 1;
    area->y1 = (y1 >> 1) << 1;
    area->x2 = ((x2 >> 1) << 1) + 1;
    area->y2 = ((y2 >> 1) << 1) + 1;
}

static void example_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    uint16_t tp_x;
    uint16_t tp_y;
    uint8_t win = getTouch(&tp_x, &tp_y);
    if (win) {
        data->point.x = tp_x;
        data->point.y = tp_y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void example_increase_lvgl_tick(void *arg)
{
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

static bool example_lvgl_lock(int timeout_ms)
{
    assert(lvgl_mux && "Display must be initialized first");
    const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;
}

static void example_lvgl_unlock(void)
{
    assert(lvgl_mux && "Display must be initialized first");
    xSemaphoreGive(lvgl_mux);
}

static void example_lvgl_port_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL task");
    uint32_t task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
    while (1) {
        if (example_lvgl_lock(-1)) {
            task_delay_ms = lv_timer_handler();
            example_lvgl_unlock();
        }
        if (task_delay_ms > EXAMPLE_LVGL_TASK_MAX_DELAY_MS) {
            task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
        } else if (task_delay_ms < EXAMPLE_LVGL_TASK_MIN_DELAY_MS) {
            task_delay_ms = EXAMPLE_LVGL_TASK_MIN_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}
*/

void test_jpeg_decoder()
{
    jpeg_frame_t frame;

    uint8_t* jpeg_data = (uint8_t*) heap_caps_malloc(sizeof(tulips), MALLOC_CAP_SIMD);
    memcpy(jpeg_data, tulips, sizeof(tulips));
    frame.data = jpeg_data;
    frame.size = sizeof(tulips);

    // Send frame to queue for processing
    if (xQueueSend(jpeg_frame_queue, &frame, 0) == pdTRUE) {
        ESP_LOGI(TAG, "Test JPEG frame queued successfully");
    } else {
        ESP_LOGW(TAG, "Failed to queue test JPEG frame");
    }

}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Create JPEG frame queue
    jpeg_frame_queue = xQueueCreate(2, sizeof(jpeg_frame_t));
    if (jpeg_frame_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create JPEG frame queue");
        return;
    }

    // Read LCD ID
    READ_LCD_ID = read_lcd_id();
    ESP_LOGI(TAG, "LCD ID: 0x%02X", READ_LCD_ID);

    // Initialize display (LVGL variables commented out)
    // static lv_disp_draw_buf_t disp_buf;
    // static lv_disp_drv_t disp_drv;

    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK;
    buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;
    buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;
    buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
    buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
    buscfg.max_transfer_sz = EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * LCD_BIT_PER_PIXEL / 8;
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS;
    io_config.dc_gpio_num = -1;
    io_config.spi_mode = 0;
    io_config.pclk_hz = 40*1000*1000;
    io_config.trans_queue_depth = 10;
    io_config.on_color_trans_done = lcd_trans_done_callback;
    io_config.user_ctx = NULL;
    io_config.lcd_cmd_bits = 32;
    io_config.lcd_param_bits = 8;
    io_config.flags.quad_mode = true;
    sh8601_vendor_config_t vendor_config = {
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BIT_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    vendor_config.init_cmds = (READ_LCD_ID == SH8601_ID) ? sh8601_lcd_init_cmds : co5300_lcd_init_cmds;
    vendor_config.init_cmds_size = (READ_LCD_ID == SH8601_ID) ? 
                                   sizeof(sh8601_lcd_init_cmds) / sizeof(sh8601_lcd_init_cmds[0]) : 
                                   sizeof(co5300_lcd_init_cmds) / sizeof(co5300_lcd_init_cmds[0]);
    
    ESP_LOGI(TAG, "Install SH8601 panel driver");
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    
    // Save panel handle globally for direct LCD access
    g_panel_handle = panel_handle;
    ESP_LOGI(TAG, "LCD panel initialized for direct access");
    
    // Create semaphore for LCD DMA synchronization
    lcd_trans_done_sem = xSemaphoreCreateBinary();
    if (!lcd_trans_done_sem) {
        ESP_LOGE(TAG, "Failed to create LCD transaction semaphore");
        return;
    }
    xSemaphoreGive(lcd_trans_done_sem); // Initially available
    ESP_LOGI(TAG, "LCD DMA synchronization semaphore created");
    
    // Allocate double buffers for LCD drawing (DMA-capable memory)
    // lcd_draw_buffer[0] = (uint16_t*)heap_caps_malloc(LCD_DRAW_BUFFER_SIZE, MALLOC_CAP_SPIRAM|MALLOC_CAP_DMA);
    // lcd_draw_buffer[1] = (uint16_t*)heap_caps_malloc(LCD_DRAW_BUFFER_SIZE, MALLOC_CAP_SPIRAM|MALLOC_CAP_DMA);
    // if (!lcd_draw_buffer[0] || !lcd_draw_buffer[1]) {
    //     ESP_LOGE(TAG, "Failed to allocate LCD draw buffers");
    //     return;
    // }
    // ESP_LOGI(TAG, "Allocated double buffers for LCD: %d bytes each", LCD_DRAW_BUFFER_SIZE);

    // Initialize touch
    Touch_Init();

#if EXAMPLE_PIN_NUM_BK_LIGHT >= 0
    ESP_LOGI(TAG, "Turn on LCD backlight");
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << EXAMPLE_PIN_NUM_BK_LIGHT
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
    gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, EXAMPLE_LCD_BK_LIGHT_ON_LEVEL);
#endif

    // LVGL initialization commented out for direct LCD performance
    // ESP_LOGI(TAG, "Initialize LVGL library");
    // lv_init();
    
    // Check available memory (LVGL buffers not needed)
    ESP_LOGI(TAG, "Free heap without LVGL buffers: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "Free DMA heap: %d bytes", heap_caps_get_free_size(MALLOC_CAP_DMA));
    
    /*
    // Calculate buffer size
    size_t buffer_size = EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_BUF_HEIGHT * sizeof(lv_color_t);
    ESP_LOGI(TAG, "Allocating LVGL buffers, size: %d bytes each", buffer_size);
    
    // Allocate draw buffers for LVGL - try double buffering first, fall back to single
    lv_color_t *buf1 = (lv_color_t*)heap_caps_malloc(buffer_size, MALLOC_CAP_DMA);
    if (!buf1) {
        ESP_LOGE(TAG, "Failed to allocate buf1");
        return;
    }
    ESP_LOGI(TAG, "buf1 allocated successfully");
    
    ESP_LOGI(TAG, "Free DMA heap: %d bytes", heap_caps_get_free_size(MALLOC_CAP_DMA));

    lv_color_t *buf2 = (lv_color_t*)heap_caps_malloc(buffer_size, MALLOC_CAP_DMA);
    if (!buf2) {
        ESP_LOGW(TAG, "Failed to allocate buf2, using single buffering");
        buf2 = NULL;
    } else {
        ESP_LOGI(TAG, "buf2 allocated successfully, using double buffering");
    }
    
    // Initialize LVGL draw buffers (buf2 can be NULL for single buffering)
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_BUF_HEIGHT);

    ESP_LOGI(TAG, "Register display driver to LVGL");
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = EXAMPLE_LCD_H_RES;
    disp_drv.ver_res = EXAMPLE_LCD_V_RES;
    disp_drv.flush_cb = example_lvgl_flush_cb;
    disp_drv.rounder_cb = example_lvgl_rounder_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
#ifdef EXAMPLE_Rotate_90
    disp_drv.sw_rotate = 1;
    disp_drv.rotated = LV_DISP_ROT_270;
#endif
    display = lv_disp_drv_register(&disp_drv);

    ESP_LOGI(TAG, "Install LVGL tick timer");
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &example_increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

    // Register touch input device
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.disp = display;
    indev_drv.read_cb = example_lvgl_touch_cb;
    lv_indev_drv_register(&indev_drv);

    // Create LVGL mutex and task
    lvgl_mux = xSemaphoreCreateMutex();
    assert(lvgl_mux);
    xTaskCreate(example_lvgl_port_task, "LVGL", EXAMPLE_LVGL_TASK_STACK_SIZE, NULL, EXAMPLE_LVGL_TASK_PRIORITY, NULL);

    // Create initial UI
    if (example_lvgl_lock(-1)) {
        // Create a simple welcome screen
        lv_obj_t *label = lv_label_create(lv_scr_act());
        lv_label_set_text(label, "JPEG Stream Receiver\nConnecting to WiFi...");
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

        // Create placeholder for stream info
        img_obj = lv_obj_create(lv_scr_act());
        lv_obj_set_size(img_obj, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
        
        example_lvgl_unlock();
    }
    */

    // Initialize WiFi
    ESP_LOGI(TAG, "Starting WiFi initialization");
    wifi_init_sta();

    // Start UDP tasks on Core 0 (network I/O)
    xTaskCreatePinnedToCore(udp_hello_task,    "udp_hello", 4096, NULL, 3, NULL, UDP_RECEIVER_CPU);
    xTaskCreatePinnedToCore(udp_receiver_task, "udp_rx",    8192, NULL, 4, NULL, UDP_RECEIVER_CPU);
    ESP_LOGI(TAG, "UDP hello+receiver tasks created on CPU %d", UDP_RECEIVER_CPU);

    // Start JPEG display task on Core 1 (decode/display)
    xTaskCreatePinnedToCore(jpeg_display_task, "jpeg_display", 8192, NULL, 5, NULL, JPEG_DECODER_CPU);
    ESP_LOGI(TAG, "JPEG display task created on CPU %d", JPEG_DECODER_CPU);

    ESP_LOGI(TAG, "JPEG Stream Receiver initialized — UDP mode");
    ESP_LOGI(TAG, "Sending HELLO to %s:%d every %d ms",
             STREAM_SERVER_IP, UDP_DISCOVER_PORT, UDP_HELLO_INTERVAL);
    ESP_LOGI(TAG, "Listening for JPEG frames on UDP port %d", UDP_STREAM_PORT);

    // test_jpeg_decoder();

}



