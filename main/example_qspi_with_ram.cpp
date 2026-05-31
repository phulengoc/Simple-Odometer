
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
#include "ble_pairing.h"

// #include "lvgl.h"
#include "esp_lcd_sh8601.h"
#include "touch_bsp.h"
#include "read_lcd_id_bsp.h"
#include "stream_config.h"
#include "status_screen.h"
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
static int  s_retry_num    = 0;
static bool s_wifi_started = false;   // true after esp_wifi_start()
static const int MAXIMUM_RETRY = 5;

// ── JPEG frame pool (P1 + P2) ─────────────────────────────────────────────────
// Fixed pool of PSRAM-backed frame buffers, recycled via two index queues
// instead of malloc/free per frame. The buffer index is the ownership token:
//   s_free_q  : indices the receiver may fill   (seeded with all N at startup)
//   s_ready_q : {index,size} assembled and ready for the display task
// Ownership flows free_q → receiver → ready_q → display → free_q. No allocation,
// copy, or fragmentation in the hot path; memory use is deterministic.
//
// N = 3 gives one slot in flight (1 decoding + 1 queued + 1 filling) so the
// receiver rarely has to drop frames when display rate ≈ network rate.
#define FRAME_POOL_COUNT  3

typedef struct {
    int    idx;     // pool buffer index
    size_t size;    // valid JPEG bytes in that buffer
} frame_msg_t;

static uint8_t      *s_frame_pool[FRAME_POOL_COUNT];
static QueueHandle_t s_free_q;    // queue of int  (free buffer indices)
static QueueHandle_t s_ready_q;   // queue of frame_msg_t (filled frames)

// Status/wait-screen info lines, filled once WiFi is up; reused by the display
// task to repaint the wait screen if the stream stalls.
static char s_status_ssid_line[40] = "";
static char s_status_ip_line[24]   = "";

// ── Streaming-mode power/CPU management ────────────────────────────────────────
// When streaming is confirmed (first decoded frame), we shed Core-0 work that
// competes with the UDP/decode pipeline: tear down BLE (removes the priority-21
// NimBLE host task + WiFi/BT coexistence) and suspend the HELLO discovery beacon.
// HELLO is resumed if the stream stalls so the iOS app can re-discover us.
static TaskHandle_t s_hello_task_handle = NULL;
static bool s_ble_down        = false;   // BLE torn down (one-way)
static bool s_hello_suspended = false;   // HELLO beacon currently suspended

// Centering offset for a streamed image smaller than the 466×466 panel (e.g.
// 400×400). Set per-frame from the decoded dimensions; 0 when it fills the panel.
static int  s_draw_off_x = 0;
static int  s_draw_off_y = 0;
// Clear the panel border around a centered image once before the next frame —
// after boot and after every wait-screen repaint (both paint the full panel).
static bool s_need_border_clear = true;

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

// ── CPU core assignments ──────────────────────────────────────────────────────
// Core 0: NimBLE host (priority 21) + Wi-Fi/lwIP + UDP HELLO sender
// Core 1: UDP receiver + JPEG decoder / LCD draw
//
// The UDP receiver MUST run on Core 1.  NimBLE host runs at
// configMAX_PRIORITIES-4 = priority 21 and can preempt any Core-0 task.
// When it does, the lwIP UDP mailbox overflows and EOI framing packets are
// dropped, causing the UDP frame-assembly state machine to restart on every
// frame → 0 frames decoded.  Moving the receiver to Core 1 removes it from
// BLE scheduling entirely.
#define UDP_HELLO_CPU       0   // Core 0 — co-located with Wi-Fi/lwIP
#define UDP_RECEIVER_CPU    1   // Core 1 — isolated from NimBLE
#define JPEG_DECODER_CPU    1   // Core 1 — decode/display

// Task priorities
#define UDP_HELLO_PRIORITY    3
#define UDP_RECEIVER_PRIORITY 6   // above JPEG decoder so packets drain before decoding
#define JPEG_DECODER_PRIORITY 5

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

// ── WiFi driver one-time initialisation ──────────────────────────────────────
// Sets up the event group, netif, event handlers, and WiFi driver.
// Does NOT connect — call wifi_connect_to() for that.
static void wifi_driver_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    // Disable modem sleep — keeps radio always-on for low-latency UDP.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "WiFi driver initialised (not connected yet)");
}

// ── WiFi connect / reconnect ──────────────────────────────────────────────────
// Sets credentials, starts (or restarts) the WiFi station, and blocks until
// the connection is established or the maximum retries are exhausted.
// Safe to call multiple times — used by the reconnect task when new BLE creds
// arrive.
static bool wifi_connect_to(const char *ssid, const char *pass)
{
    ESP_LOGI(TAG, "WiFi: connecting to SSID \"%s\" …", ssid);

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid,     ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password,  pass, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable    = true;
    wifi_config.sta.pmf_cfg.required   = false;

    // Reset retry counter and clear result bits before each attempt.
    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    if (!s_wifi_started) {
        // First call — start the driver; WIFI_EVENT_STA_START fires event_handler
        // which calls esp_wifi_connect().
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
    } else {
        // Subsequent call — driver already running; just reconnect with new config.
        // Disconnect triggers WIFI_EVENT_STA_DISCONNECTED → event_handler reconnects.
        esp_wifi_disconnect();
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi: connected to \"%s\"", ssid);
        return true;
    }
    ESP_LOGE(TAG, "WiFi: failed to connect to \"%s\"", ssid);
    return false;
}

// ── WiFi reconnect task ───────────────────────────────────────────────────────
// Waits for the BLE_WIFI_CRED_RECEIVED_BIT (set by wificred_chr_access when
// iOS writes new hotspot credentials) and reconnects with the new SSID/pass.
// Runs on Core 0 alongside the WiFi / NimBLE stack.
static void wifi_reconnect_task(void *pvParameters)
{
    while (1) {
        // pdTRUE: clear the bit after reading so subsequent writes re-trigger
        EventBits_t bits = xEventGroupWaitBits(
            g_ble_event_group, BLE_WIFI_CRED_RECEIVED_BIT,
            pdTRUE, pdTRUE, portMAX_DELAY);

        if (bits & BLE_WIFI_CRED_RECEIVED_BIT) {
            ESP_LOGI(TAG, "WiFi reconnect triggered by BLE creds: ssid=%s", g_wifi_ssid);
            wifi_connect_to(g_wifi_ssid, g_wifi_pass);
        }
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
// Sends "HELLO\0" to iOS on UDP_DISCOVER_PORT so the iOS UDPStreamSender
// learns the ESP32 IP and starts sending JPEG frames.
//
// IP resolution order:
//   1. Wait up to BLE_IP_WAIT_MS for iOS to deliver its IP over BLE.
//   2. If BLE IP arrives   → use g_ios_ip (updated live on reconnect).
//   3. If timeout elapses  → fall back to hardcoded STREAM_SERVER_IP.
//
#define BLE_IP_WAIT_MS  30000   // wait up to 30 s for BLE pairing before fallback

static void udp_hello_task(void *pvParameters)
{
    // ── Wait for BLE to deliver iOS IP (or fall back to hardcoded value) ──────
    ESP_LOGI(TAG, "HELLO task: waiting up to %d ms for BLE IP…", BLE_IP_WAIT_MS);
    EventBits_t bits = xEventGroupWaitBits(
        g_ble_event_group, BLE_IP_RECEIVED_BIT,
        pdFALSE,   // do NOT clear the bit — other tasks may also check it
        pdTRUE,
        pdMS_TO_TICKS(BLE_IP_WAIT_MS));

    char target_ip[16];
    if (bits & BLE_IP_RECEIVED_BIT) {
        // Copy under mutex so we get a consistent string
        strncpy(target_ip, g_ios_ip, 15);
        target_ip[15] = '\0';
        ESP_LOGI(TAG, "HELLO task: using BLE-provided IP %s", target_ip);
    } else {
        strncpy(target_ip, STREAM_SERVER_IP, 15);
        target_ip[15] = '\0';
        ESP_LOGW(TAG, "HELLO task: BLE timeout — falling back to %s", target_ip);
    }

    ESP_LOGI(TAG, "HELLO task: sending to %s:%d every %d ms",
             target_ip, UDP_DISCOVER_PORT, UDP_HELLO_INTERVAL);

    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(UDP_DISCOVER_PORT);
    inet_pton(AF_INET, target_ip, &dest.sin_addr);

    while (1) {
        // If a new IP arrived over BLE since we started, switch to it.
        if (g_ios_ip[0] != '\0' && strncmp(g_ios_ip, target_ip, 15) != 0) {
            strncpy(target_ip, g_ios_ip, 15);
            target_ip[15] = '\0';
            inet_pton(AF_INET, target_ip, &dest.sin_addr);
            ESP_LOGI(TAG, "HELLO task: IP updated to %s", target_ip);
        }

        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            ESP_LOGE(TAG, "HELLO: socket() failed errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }

        const char *hello = "HELLO";
        while (1) {
            // Live IP update check each iteration
            if (g_ios_ip[0] != '\0' && strncmp(g_ios_ip, target_ip, 15) != 0) {
                strncpy(target_ip, g_ios_ip, 15);
                target_ip[15] = '\0';
                inet_pton(AF_INET, target_ip, &dest.sin_addr);
                ESP_LOGI(TAG, "HELLO task: IP updated to %s (live)", target_ip);
            }

            int n = sendto(sock, hello, strlen(hello) + 1, 0,
                           (struct sockaddr *)&dest, sizeof(dest));
            if (n < 0) {
                // Network may be transiently down (e.g. ENETUNREACH during a
                // WiFi reconnect). Back off before recreating the socket so we
                // don't tight-loop and flood the console.
                ESP_LOGW(TAG, "HELLO: sendto errno %d — retrying", errno);
                vTaskDelay(pdMS_TO_TICKS(UDP_HELLO_INTERVAL));
                break;
            }
            ESP_LOGD(TAG, "HELLO → %s:%d", target_ip, UDP_DISCOVER_PORT);
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
    int       cur_idx    = -1;     // pool buffer currently being filled (-1 = none)
    uint8_t  *frame_buf  = NULL;   // == s_frame_pool[cur_idx] while filling
    size_t    frame_used = 0;
    udp_rx_state_t state = UDP_STATE_WAITING_SOI;
    TickType_t last_data_tick = 0;
    const TickType_t FRAME_TIMEOUT_TICKS = pdMS_TO_TICKS(500);   // 500 ms — fast recovery on BLE preemption

    // Frame buffers are pre-allocated once in app_main (PSRAM pool); this task
    // never allocates. It borrows a buffer from s_free_q per frame, assembles
    // directly into it, and hands it to the display task via s_ready_q.
    // RX_RECYCLE() returns any in-flight buffer to the pool — it MUST be called
    // on every abandon path or the pool slowly drains and the receiver stalls.
    #define RX_RECYCLE() do { \
        if (cur_idx >= 0) { xQueueSend(s_free_q, &cur_idx, 0); cur_idx = -1; frame_buf = NULL; } \
    } while (0)

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

        // Larger socket receive buffer: absorbs UDP bursts that arrive while the
        // NimBLE host task (priority 21) preempts this task (priority 4) on Core 0.
        int rcvbuf = 65536;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

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
        RX_RECYCLE();
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
                        RX_RECYCLE();
                        state      = UDP_STATE_WAITING_SOI;
                        frame_used = 0;
                    }
                    continue;
                }
                ESP_LOGE(TAG, "UDP rx: recvfrom errno %d — rebinding", errno);
                RX_RECYCLE();
                break;
            }

            if (n == 0) continue;

            switch (state) {
            case UDP_STATE_WAITING_SOI:
                // SOI framing packet: exactly 2 bytes 0xFF 0xD8.
                // This is a delimiter only — the JPEG DATA packets that follow
                // already contain the full JPEG including its own 0xFF 0xD8 header.
                if (n == 2 && pkt_buf[0] == 0xFF && pkt_buf[1] == 0xD8) {
                    // Borrow a pool buffer to assemble into. If none is free the
                    // display task is behind — drop this frame (stay WAITING_SOI)
                    // rather than block the socket and overflow the UDP mailbox.
                    if (xQueueReceive(s_free_q, &cur_idx, 0) != pdTRUE) {
                        cur_idx = -1;
                        ESP_LOGD(TAG, "UDP rx: no free buffer — dropping frame");
                        break;
                    }
                    frame_buf      = s_frame_pool[cur_idx];
                    frame_used     = 0;
                    state          = UDP_STATE_RECEIVING_DATA;
                    last_data_tick = xTaskGetTickCount();
                    ESP_LOGD(TAG, "UDP rx: SOI — assembling frame (buf %d)", cur_idx);
                }
                break;

            case UDP_STATE_RECEIVING_DATA:
                last_data_tick = xTaskGetTickCount();

                // ── Guard: new SOI arrived before EOI ────────────────────────
                // This happens when NimBLE (priority 21) preempts this task and
                // the EOI framing packet is dropped from the lwIP mailbox.
                // Without this check the assembly buffer accumulates data from
                // multiple frames until it exceeds MAX_JPEG_FRAME_SIZE.
                if (n == 2 && pkt_buf[0] == 0xFF && pkt_buf[1] == 0xD8) {
                    ESP_LOGW(TAG, "UDP rx: SOI mid-frame — dropped %zu B (BLE preemption?), restarting",
                             frame_used);
                    frame_used = 0;   // discard partial frame, stay in RECEIVING_DATA
                    break;
                }

                if (n == 2 && pkt_buf[0] == 0xFF && pkt_buf[1] == 0xD9) {
                    // EOI framing packet: pool[cur_idx] now holds a complete JPEG
                    // (the DATA bytes already include the JPEG's own 0xFFD9, so we
                    // do not append the marker). Hand the buffer to the display
                    // task by index — zero-copy ownership transfer.
                    if (frame_used > 0 && cur_idx >= 0) {
                        frame_msg_t msg = { .idx = cur_idx, .size = frame_used };
                        if (xQueueSend(s_ready_q, &msg, 0) == pdTRUE) {
                            cur_idx = -1; frame_buf = NULL;   // ownership → display
                            ESP_LOGD(TAG, "UDP rx: queued frame %u B", (unsigned)frame_used);
                        } else {
                            ESP_LOGD(TAG, "UDP rx: display queue full — dropping frame");
                            RX_RECYCLE();   // give the buffer back, drop the frame
                        }
                    } else {
                        RX_RECYCLE();       // empty frame — recycle the buffer
                    }
                    state      = UDP_STATE_WAITING_SOI;
                    frame_used = 0;

                } else {
                    // DATA packet: append directly into the pool buffer
                    if (frame_used + (size_t)n > MAX_JPEG_FRAME_SIZE) {
                        ESP_LOGE(TAG, "UDP rx: frame overflow (%zu B) — discarding", frame_used);
                        RX_RECYCLE();
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

    #undef RX_RECYCLE
    vTaskDelete(NULL);   // never reached
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
    
    // Destination on the panel. The streamed image may be smaller than the
    // 466×466 panel (e.g. 400×400), so center it with the per-frame offset.
    int dst_x = pDraw->x + s_draw_off_x;
    int dst_y = pDraw->y + s_draw_off_y;

    // Complete bounds check
    if (dst_x >= EXAMPLE_LCD_H_RES || dst_y >= EXAMPLE_LCD_V_RES) {
        return 1; // off-screen — skip this block but continue
    }

    // Calculate safe copy dimensions
    int copy_width = pDraw->iWidth;
    int copy_height = pDraw->iHeight;

    if (dst_x + copy_width > EXAMPLE_LCD_H_RES) {
        copy_width = EXAMPLE_LCD_H_RES - dst_x;
    }
    if (dst_y + copy_height > EXAMPLE_LCD_V_RES) {
        copy_height = EXAMPLE_LCD_V_RES - dst_y;
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
                                              dst_x, dst_y,
                                              dst_x + copy_width, dst_y + copy_height,
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
    frame_msg_t frame;
    static int frame_count = 0;

    // Wait for panel handle to be initialized
    while (g_panel_handle == NULL) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "JPEG display task started - drawing directly to LCD");

    bool waiting_shown = false;   // true while the wait screen is displayed

    while (1) {
        // Time-bounded wait so we can repaint the wait screen if the stream stalls.
        if (!xQueueReceive(s_ready_q, &frame, pdMS_TO_TICKS(3000))) {
            if (!waiting_shown) {
                status_screen_show("ESP32 MapNav", "Waiting for stream",
                                   s_status_ssid_line[0] ? s_status_ssid_line : NULL,
                                   s_status_ip_line[0]   ? s_status_ip_line   : NULL);
                waiting_shown = true;
                s_need_border_clear = true;   // wait screen painted full panel
            }
            // Stream stalled — resume the HELLO beacon so the iOS app can
            // re-discover us. (BLE stays down; its teardown is one-way.)
            if (s_hello_task_handle && s_hello_suspended) {
                vTaskResume(s_hello_task_handle);
                s_hello_suspended = false;
                ESP_LOGI(TAG, "Stream stalled — HELLO beacon resumed");
            }
            continue;
        }

        {
            // A frame arrived — it will overwrite the wait screen.
            uint8_t *jpeg = s_frame_pool[frame.idx];
            waiting_shown = false;
            ESP_LOGD(TAG, "Received JPEG frame: %u bytes (buf %d)", (unsigned)frame.size, frame.idx);
            frame_count++;

            bool decode_success = false;

            // Sanity-check framing before decoding — protects JPEGDEC from a
            // corrupt/merged buffer (must start 0xFFD8 and end 0xFFD9).
            bool valid = (frame.size >= 4 &&
                          jpeg[0] == 0xFF && jpeg[1] == 0xD8 &&
                          jpeg[frame.size - 2] == 0xFF && jpeg[frame.size - 1] == 0xD9);

            // Open and decode JPEG with direct LCD callback
            if (valid && jpeg_dec.openRAM(jpeg, frame.size, jpeg_decode_callback)) {
                ESP_LOGD(TAG, "JPEG opened: %dx%d, bpp: %d",
                        jpeg_dec.getWidth(), jpeg_dec.getHeight(), jpeg_dec.getBpp());

                // Set pixel format to RGB565 for direct LCD output
                jpeg_dec.setPixelType(RGB565_BIG_ENDIAN);

                // Center the (possibly sub-panel) image; the decode callback adds
                // this offset to every MCU block.
                int iw = jpeg_dec.getWidth();
                int ih = jpeg_dec.getHeight();
                s_draw_off_x = (EXAMPLE_LCD_H_RES - iw) / 2;
                s_draw_off_y = (EXAMPLE_LCD_V_RES - ih) / 2;
                if (s_draw_off_x < 0) s_draw_off_x = 0;
                if (s_draw_off_y < 0) s_draw_off_y = 0;

                // Black out the panel once so no stale border surrounds a
                // smaller-than-panel image (after boot / after a wait screen).
                if (s_need_border_clear && (iw < EXAMPLE_LCD_H_RES || ih < EXAMPLE_LCD_V_RES)) {
                    status_screen_show(NULL, NULL, NULL, NULL);
                    s_need_border_clear = false;
                }

                if (jpeg_dec.decode(0, 0, JPEG_USES_DMA)) {
                    decode_success = true;
                }
                jpeg_dec.close();
            } else {
                ESP_LOGW(TAG, "Frame %d: %s", frame_count,
                         valid ? "JPEG open failed" : "bad SOI/EOI framing — skipped");
            }

            if (!decode_success) {
                ESP_LOGD(TAG, "Decode failed for frame %d", frame_count);
            }

            // ── Enter streaming mode on the first proven frame ────────────────
            // The pipeline works (IP discovered, frame decoded), so shed Core-0
            // load: tear down BLE once, and suspend the HELLO beacon while frames
            // flow. Both are restored/handled appropriately on stall.
            if (decode_success) {
                if (!s_ble_down) {
                    s_ble_down = true;
                    ESP_LOGI(TAG, "Streaming active — tearing down BLE");
                    ble_pairing_deinit();
                }
                if (s_hello_task_handle && !s_hello_suspended) {
                    vTaskSuspend(s_hello_task_handle);
                    s_hello_suspended = true;
                    ESP_LOGI(TAG, "Streaming active — HELLO beacon suspended");
                }
            }

            // Return the buffer to the pool (replaces free()).
            xQueueSend(s_free_q, &frame.idx, 0);
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
    int idx;
    if (xQueueReceive(s_free_q, &idx, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Test JPEG: no free pool buffer");
        return;
    }
    memcpy(s_frame_pool[idx], tulips, sizeof(tulips));

    frame_msg_t frame = { .idx = idx, .size = sizeof(tulips) };
    if (xQueueSend(s_ready_q, &frame, 0) == pdTRUE) {
        ESP_LOGI(TAG, "Test JPEG frame queued successfully");
    } else {
        ESP_LOGW(TAG, "Failed to queue test JPEG frame");
        xQueueSend(s_free_q, &idx, 0);   // recycle on failure
    }
}

void app_main(void)
{
    // Initialize NVS (required by both Wi-Fi and BLE)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ── BLE pairing ───────────────────────────────────────────────────────────
    // Start advertising so the iOS MapNav app can connect and write its IP/port.
    // The udp_hello_task will block until BLE_IP_RECEIVED_BIT is set (or 30 s).
    ble_pairing_init();

    // ── JPEG frame pool (P1 + P2) ──────────────────────────────────────────────
    // Pre-allocate FRAME_POOL_COUNT PSRAM frame buffers and seed the free-index
    // queue. Buffers are recycled for the life of the program — no per-frame
    // malloc/free, no fragmentation, deterministic memory use.
    s_free_q  = xQueueCreate(FRAME_POOL_COUNT, sizeof(int));
    s_ready_q = xQueueCreate(FRAME_POOL_COUNT, sizeof(frame_msg_t));
    if (!s_free_q || !s_ready_q) {
        ESP_LOGE(TAG, "Failed to create frame pool queues");
        return;
    }
    for (int i = 0; i < FRAME_POOL_COUNT; i++) {
        s_frame_pool[i] = (uint8_t *)heap_caps_malloc(MAX_JPEG_FRAME_SIZE, MALLOC_CAP_SPIRAM);
        if (!s_frame_pool[i]) {
            ESP_LOGE(TAG, "Failed to allocate frame pool buffer %d (%d B PSRAM)",
                     i, MAX_JPEG_FRAME_SIZE);
            return;
        }
        xQueueSend(s_free_q, &i, 0);   // all buffers start free
    }
    ESP_LOGI(TAG, "JPEG frame pool: %d × %d KB in PSRAM",
             FRAME_POOL_COUNT, MAX_JPEG_FRAME_SIZE / 1024);

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

    // Bind the wait/status screen to the panel and show a boot message so the
    // display is never blank while WiFi/BLE/streaming come up.
    status_screen_init(panel_handle, lcd_trans_done_sem);
    status_screen_show("ESP32 MapNav", "Starting up...", NULL, NULL);
    
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

    // ── WiFi initialisation ───────────────────────────────────────────────────
    // One-time driver setup (no connection yet)
    wifi_driver_init();

    // ── Credential resolution ─────────────────────────────────────────────────
    // Priority: NVS (previously provisioned) → BLE (iOS sends hotspot creds)
    //           → hardcoded fallback in stream_config.h
    char wifi_ssid[33] = {};
    char wifi_pass[64] = {};

    if (ble_load_wifi_creds_from_nvs(wifi_ssid, wifi_pass)) {
        ESP_LOGI(TAG, "Using NVS WiFi creds: ssid=%s", wifi_ssid);
    } else {
        ESP_LOGI(TAG, "No NVS creds — waiting up to 30 s for BLE provisioning …");
        status_screen_show("ESP32 MapNav", "Pair phone via BLE",
                           "to send Wi-Fi setup", NULL);
        EventBits_t bits = xEventGroupWaitBits(
            g_ble_event_group, BLE_WIFI_CRED_RECEIVED_BIT,
            pdTRUE,   // clear bit — reconnect_task watches for future updates
            pdTRUE,
            pdMS_TO_TICKS(30000));

        if (bits & BLE_WIFI_CRED_RECEIVED_BIT) {
            strncpy(wifi_ssid, g_wifi_ssid, sizeof(wifi_ssid) - 1);
            strncpy(wifi_pass, g_wifi_pass, sizeof(wifi_pass) - 1);
            ESP_LOGI(TAG, "Using BLE-provisioned creds: ssid=%s", wifi_ssid);
        } else {
            strncpy(wifi_ssid, WIFI_SSID, sizeof(wifi_ssid) - 1);
            strncpy(wifi_pass, WIFI_PASS,  sizeof(wifi_pass)  - 1);
            ESP_LOGW(TAG, "BLE timeout — falling back to hardcoded SSID: %s", wifi_ssid);
        }
    }

    status_screen_show("ESP32 MapNav", "Connecting Wi-Fi", wifi_ssid, NULL);

    bool wifi_ok = wifi_connect_to(wifi_ssid, wifi_pass);

    // Build the status info lines (SSID + assigned IP) used by the wait screen.
    snprintf(s_status_ssid_line, sizeof(s_status_ssid_line), "Wi-Fi: %s", wifi_ssid);
    if (wifi_ok) {
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip_info = {};
        if (sta && esp_netif_get_ip_info(sta, &ip_info) == ESP_OK) {
            snprintf(s_status_ip_line, sizeof(s_status_ip_line), "IP: " IPSTR,
                     IP2STR(&ip_info.ip));
        }
        status_screen_show("ESP32 MapNav", "Waiting for stream",
                           s_status_ssid_line, s_status_ip_line);

        // Streaming prerequisites met (WiFi up + provisioned). Touch is unused
        // during streaming — put the controller to sleep now. (BLE + HELLO are
        // shed later, on the first decoded frame — see jpeg_display_task.)
        Touch_Sleep();
    } else {
        status_screen_show("ESP32 MapNav", "Wi-Fi failed",
                           "check credentials", NULL);
    }

    // Core 0: HELLO sender lives with Wi-Fi / NimBLE (same RF subsystem)
    xTaskCreatePinnedToCore(udp_hello_task,    "udp_hello",    4096, NULL,
                            UDP_HELLO_PRIORITY,    &s_hello_task_handle, UDP_HELLO_CPU);

    // Core 1: receiver + decoder — completely isolated from NimBLE on Core 0
    xTaskCreatePinnedToCore(udp_receiver_task, "udp_rx",       8192, NULL,
                            UDP_RECEIVER_PRIORITY, NULL, UDP_RECEIVER_CPU);
    xTaskCreatePinnedToCore(jpeg_display_task, "jpeg_display", 8192, NULL,
                            JPEG_DECODER_PRIORITY, NULL, JPEG_DECODER_CPU);

    // Core 0: monitors BLE_WIFI_CRED_RECEIVED_BIT and reconnects if iOS
    // sends updated hotspot credentials after initial boot
    xTaskCreatePinnedToCore(wifi_reconnect_task, "wifi_recon", 4096, NULL,
                            3, NULL, UDP_HELLO_CPU);

    ESP_LOGI(TAG, "Tasks: hello=CPU%d pri%d | rx=CPU%d pri%d | jpeg=CPU%d pri%d | recon=CPU%d",
             UDP_HELLO_CPU,    UDP_HELLO_PRIORITY,
             UDP_RECEIVER_CPU, UDP_RECEIVER_PRIORITY,
             JPEG_DECODER_CPU, JPEG_DECODER_PRIORITY,
             UDP_HELLO_CPU);

    ESP_LOGI(TAG, "JPEG Stream Receiver initialized — UDP mode");
    ESP_LOGI(TAG, "WiFi: connected to \"%s\"", wifi_ssid);
    ESP_LOGI(TAG, "Sending HELLO to <BLE-IP or %s>:%d every %d ms",
             STREAM_SERVER_IP, UDP_DISCOVER_PORT, UDP_HELLO_INTERVAL);
    ESP_LOGI(TAG, "Listening for JPEG frames on UDP port %d", UDP_STREAM_PORT);

    // test_jpeg_decoder();

}



