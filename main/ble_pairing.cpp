/**
 * ble_pairing.cpp
 *
 * BLE Peripheral (GATT Server) for MapNav streaming pairing.
 *
 * Role in the system
 * ──────────────────
 * This file is the ESP32 side of a one-time pairing handshake:
 *
 *   iOS (Central)                  ESP32 (Peripheral)
 *   ──────────────                 ─────────────────────────────────────
 *   Write ipChar   ──────────────► store IP  → g_ios_ip
 *   Write portChar ──────────────► store port → g_stream_port
 *                                  set BLE_IP_RECEIVED_BIT
 *                                  → udp_hello_task unblocks and starts
 *                                    sending HELLO to the correct iOS IP
 *   Write ctrlChar "STOP" ───────► (reserved – no action yet)
 *
 * Actual JPEG streaming uses UDP (see example_qspi_with_ram.cpp).
 * BLE is only used to exchange IP/port; after that the iOS app uses UDP.
 *
 * BLE Service / Characteristic UUIDs  (must match BTManager.swift / BLEConstants.swift)
 * ─────────────────────────────────────────────────────────────────────────────────────
 *  Service  : 12AB3456-0000-1000-8000-00805F9B34FB
 *  IP       : 12AB3456-0001-1000-8000-00805F9B34FB   (Write)
 *  Port     : 12AB3456-0002-1000-8000-00805F9B34FB   (Write)
 *  Control  : 12AB3456-0003-1000-8000-00805F9B34FB   (Write | Notify)
 *  WiFiCred : 12AB3456-0005-1000-8000-00805F9B34FB   (Write)
 *             Payload: UTF-8 JSON {"ssid":"…","pass":"…"} (≤150 bytes)
 *
 * NimBLE stores 128-bit UUIDs in little-endian byte order.
 */

#include "ble_pairing.h"
#include "stream_config.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_pairing";

// ─────────────────────────────────────────────────────────────────────────────
// Shared state (declared extern in ble_pairing.h)
// ─────────────────────────────────────────────────────────────────────────────

char               g_ios_ip[16]       = {};   // empty → fallback to STREAM_SERVER_IP
uint16_t           g_stream_port      = UDP_DISCOVER_PORT;
EventGroupHandle_t g_ble_event_group  = NULL;
char               g_wifi_ssid[33]    = {};   // populated by wifiCred BLE write
char               g_wifi_pass[64]    = {};

static SemaphoreHandle_t s_ip_mutex  = NULL; // guards g_ios_ip writes

// ─────────────────────────────────────────────────────────────────────────────
// 128-bit UUIDs in NimBLE little-endian order
//
// Standard form: 12AB3456-XXXX-1000-8000-00805F9B34FB
// In memory  BE: 12 AB 34 56 XX XX 10 00 80 00 00 80 5F 9B 34 FB
// NimBLE  LE  : FB 34 9B 5F 80 00 00 80 00 10 XX XX 56 34 AB 12
// ─────────────────────────────────────────────────────────────────────────────

// Service (suffix 0000)
static const ble_uuid128_t s_svc_uuid =
    BLE_UUID128_INIT(0xFB,0x34,0x9B,0x5F,0x80,0x00,0x00,0x80,
                     0x00,0x10,0x00,0x00,0x56,0x34,0xAB,0x12);

// IP characteristic (suffix 0001)
static const ble_uuid128_t s_ip_uuid =
    BLE_UUID128_INIT(0xFB,0x34,0x9B,0x5F,0x80,0x00,0x00,0x80,
                     0x00,0x10,0x01,0x00,0x56,0x34,0xAB,0x12);

// Port characteristic (suffix 0002)
static const ble_uuid128_t s_port_uuid =
    BLE_UUID128_INIT(0xFB,0x34,0x9B,0x5F,0x80,0x00,0x00,0x80,
                     0x00,0x10,0x02,0x00,0x56,0x34,0xAB,0x12);

// Control characteristic (suffix 0003)
static const ble_uuid128_t s_ctrl_uuid =
    BLE_UUID128_INIT(0xFB,0x34,0x9B,0x5F,0x80,0x00,0x00,0x80,
                     0x00,0x10,0x03,0x00,0x56,0x34,0xAB,0x12);

// WiFi credential characteristic (suffix 0005)
// iOS writes UTF-8 JSON: {"ssid":"…","pass":"…"}
static const ble_uuid128_t s_wificred_uuid =
    BLE_UUID128_INIT(0xFB,0x34,0x9B,0x5F,0x80,0x00,0x00,0x80,
                     0x00,0x10,0x05,0x00,0x56,0x34,0xAB,0x12);

// ─────────────────────────────────────────────────────────────────────────────
// NVS helpers — persist WiFi credentials across reboots
// ─────────────────────────────────────────────────────────────────────────────

#define NVS_NAMESPACE  "mapnav_wifi"
#define NVS_KEY_SSID   "ssid"
#define NVS_KEY_PASS   "pass"

static void save_wifi_creds_to_nvs(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed — WiFi creds not saved");
        return;
    }
    nvs_set_str(h, NVS_KEY_SSID, ssid);
    nvs_set_str(h, NVS_KEY_PASS, pass);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "WiFi creds saved to NVS: ssid=%s", ssid);
}

bool ble_load_wifi_creds_from_nvs(char *ssid_out, char *pass_out)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t ssid_len = 33, pass_len = 64;
    bool ok = (nvs_get_str(h, NVS_KEY_SSID, ssid_out, &ssid_len) == ESP_OK) &&
              (nvs_get_str(h, NVS_KEY_PASS, pass_out, &pass_len) == ESP_OK) &&
              (ssid_out[0] != '\0');
    nvs_close(h);
    if (ok) {
        ESP_LOGI(TAG, "WiFi creds loaded from NVS: ssid=%s", ssid_out);
    }
    return ok;
}

// ─────────────────────────────────────────────────────────────────────────────
// Minimal JSON parser for {"ssid":"…","pass":"…"}
// ─────────────────────────────────────────────────────────────────────────────

static bool parse_wifi_cred_json(const char *json,
                                 char *ssid_out, size_t ssid_max,
                                 char *pass_out, size_t pass_max)
{
    // Extract value after "ssid":"
    const char *p = strstr(json, "\"ssid\":\"");
    if (!p) return false;
    p += 8;
    const char *end = strchr(p, '"');
    if (!end) return false;
    size_t len = (size_t)(end - p);
    if (len == 0 || len >= ssid_max) return false;
    strncpy(ssid_out, p, len);
    ssid_out[len] = '\0';

    // Extract value after "pass":"
    p = strstr(json, "\"pass\":\"");
    if (!p) return false;
    p += 8;
    end = strchr(p, '"');
    if (!end) return false;
    len = (size_t)(end - p);
    if (len >= pass_max) return false;
    strncpy(pass_out, p, len);
    pass_out[len] = '\0';

    return (ssid_out[0] != '\0');
}

// ─────────────────────────────────────────────────────────────────────────────
// Delayed BLE termination task
//
// After iOS writes the port characteristic we hold the BLE connection open for
// BLE_TERM_WINDOW_MS to give iOS time to also write WiFi credentials.
// This avoids needing a second BLE session just for credential provisioning.
// ─────────────────────────────────────────────────────────────────────────────

#define BLE_TERM_WINDOW_MS  3000   // 3-second window for WiFi cred delivery

// Set true by ble_pairing_deinit(); makes all NimBLE-touching contexts bail out
// so the host can be freed without a use-after-free.
static volatile bool s_ble_shutting_down = false;
// Handle of the pending ble_term_task (NULL when none is running). deinit waits
// on this so it never frees the host while the term task is calling into it.
static TaskHandle_t  s_term_task = NULL;

static void ble_term_task(void *arg)
{
    uint16_t conn_handle = (uint16_t)(uintptr_t)arg;

    // Sleep the pairing window in small steps so a shutdown can pre-empt us
    // promptly — we must NOT call into the NimBLE host once teardown begins.
    const TickType_t step = pdMS_TO_TICKS(100);
    for (TickType_t waited = 0; waited < pdMS_TO_TICKS(BLE_TERM_WINDOW_MS); waited += step) {
        if (s_ble_shutting_down) { s_term_task = NULL; vTaskDelete(NULL); }
        vTaskDelay(step);
    }

    if (!s_ble_shutting_down) {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "BLE term: disconnecting after %d ms pairing window", BLE_TERM_WINDOW_MS);
            ble_gap_terminate(conn_handle, BLE_ERR_CONN_TERM_LOCAL);
        }
    }
    s_term_task = NULL;   // signal deinit that we are done touching the host
    vTaskDelete(NULL);
}

// ─────────────────────────────────────────────────────────────────────────────
// GATT access callbacks
// ─────────────────────────────────────────────────────────────────────────────

static int ip_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_REQ_NOT_SUPPORTED;

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 7 || len > 15) {
        ESP_LOGW(TAG, "IP write ignored: bad length %u", len);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    char buf[16] = {};
    ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, NULL);

    xSemaphoreTake(s_ip_mutex, portMAX_DELAY);
    strncpy(g_ios_ip, buf, 15);
    g_ios_ip[15] = '\0';
    xSemaphoreGive(s_ip_mutex);

    ESP_LOGI(TAG, "BLE ← IP: %s", g_ios_ip);

    // Signal HELLO task only after both IP is set
    xEventGroupSetBits(g_ble_event_group, BLE_IP_RECEIVED_BIT);
    return 0;
}

static int port_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_REQ_NOT_SUPPORTED;

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len != 2) {
        ESP_LOGW(TAG, "Port write ignored: expected 2 bytes, got %u", len);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint16_t port_le = 0;
    ble_hs_mbuf_to_flat(ctxt->om, &port_le, sizeof(port_le), NULL);
    g_stream_port = port_le;   // already little-endian on ESP32

    ESP_LOGI(TAG, "BLE ← Port: %u", g_stream_port);

    // ── Pairing almost complete: IP + port both received ────────────────────
    // Hold the BLE connection open for BLE_TERM_WINDOW_MS so that iOS has a
    // chance to also write WiFi credentials in the same BLE session.
    // A background task closes the connection after the window expires.
    // After disconnection, advertising resumes at slow interval (1.28 s).
    if (g_ios_ip[0] != '\0') {
        ESP_LOGI(TAG, "IP+Port received — holding BLE %d ms for WiFi cred window",
                 BLE_TERM_WINDOW_MS);
        // 4096-byte stack: this task calls NimBLE host APIs (ble_gap_conn_find /
        // ble_gap_terminate) whose call chain overflows a 2048-byte stack.
        if (!s_ble_shutting_down) {
            xTaskCreate(ble_term_task, "ble_term", 4096,
                        (void *)(uintptr_t)conn_handle, 5, &s_term_task);
        }
    }

    return 0;
}

static int ctrl_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_REQ_NOT_SUPPORTED;

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    char cmd[16] = {};
    ble_hs_mbuf_to_flat(ctxt->om, cmd, sizeof(cmd) - 1, NULL);
    ESP_LOGI(TAG, "BLE ← CTRL: %s", cmd);

    // Reserved for future use (e.g. "STOP" to blank display)
    return 0;
}

static int wificred_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_REQ_NOT_SUPPORTED;

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    // Sanity-check length: {"ssid":"x","pass":"y"} is at least 20 bytes; cap at 150.
    if (len < 20 || len > 150) {
        ESP_LOGW(TAG, "WiFi cred write ignored: bad length %u", len);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    char json[151] = {};
    ble_hs_mbuf_to_flat(ctxt->om, json, sizeof(json) - 1, NULL);

    char ssid[33] = {};
    char pass[64] = {};
    if (!parse_wifi_cred_json(json, ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGW(TAG, "WiFi cred: JSON parse failed");
        return BLE_ATT_ERR_UNLIKELY;
    }

    // Store globally (picked up by wifi_reconnect_task in main)
    strncpy(g_wifi_ssid, ssid, sizeof(g_wifi_ssid) - 1);
    strncpy(g_wifi_pass, pass, sizeof(g_wifi_pass) - 1);

    // Persist to NVS so next boot auto-connects without BLE
    save_wifi_creds_to_nvs(ssid, pass);

    ESP_LOGI(TAG, "BLE ← WiFi creds: ssid=%s pass=<redacted>", ssid);

    // Signal main-app tasks that new credentials are available
    xEventGroupSetBits(g_ble_event_group, BLE_WIFI_CRED_RECEIVED_BIT);

    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// GATT service table
// ─────────────────────────────────────────────────────────────────────────────

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                // IP address characteristic — iOS writes its Wi-Fi IP here
                .uuid       = &s_ip_uuid.u,
                .access_cb  = ip_chr_access,
                .flags      = BLE_GATT_CHR_F_WRITE,
            },
            {
                // UDP stream port — iOS writes the port as 2-byte LE uint16
                .uuid       = &s_port_uuid.u,
                .access_cb  = port_chr_access,
                .flags      = BLE_GATT_CHR_F_WRITE,
            },
            {
                // Control commands — "START" / "STOP"
                .uuid       = &s_ctrl_uuid.u,
                .access_cb  = ctrl_chr_access,
                .flags      = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                // WiFi credentials — iOS writes UTF-8 JSON {"ssid":"…","pass":"…"}
                // ESP32 saves to NVS and signals wifi_reconnect_task to reconnect.
                .uuid       = &s_wificred_uuid.u,
                .access_cb  = wificred_chr_access,
                .flags      = BLE_GATT_CHR_F_WRITE,
            },
            { 0 }   // terminator
        },
    },
    { 0 }   // terminator
};

// ─────────────────────────────────────────────────────────────────────────────
// Advertising
// ─────────────────────────────────────────────────────────────────────────────

static void ble_app_advertise(void);

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "iOS connected (conn_handle=%d)", event->connect.conn_handle);
        } else {
            ESP_LOGW(TAG, "Connect failed, status=%d — restarting adv", event->connect.status);
            ble_app_advertise();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "iOS disconnected (reason=0x%02x) — restarting adv",
                 event->disconnect.reason);
        ble_app_advertise();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "Advertising cycle complete — restarting");
        ble_app_advertise();
        break;

    default:
        break;
    }
    return 0;
}

// ── Advertising interval constants ────────────────────────────────────────────
// BLE adv interval unit = 0.625 ms.
// Fast (pre-pairing):  100 ms min / 150 ms max  — quick discovery
// Slow (post-pairing): 1280 ms min / 2560 ms max — minimal radio / CPU load
//   while JPEG UDP streaming is active.  Reduces BLE wakeups ~10× vs. fast
//   mode, dramatically cutting the NimBLE-task preemptions that cause UDP
//   packet drops and the resulting frame-overflow errors.
#define BLE_ADV_FAST_MIN  0x00A0  // 100 ms
#define BLE_ADV_FAST_MAX  0x00C0  // 150 ms
#define BLE_ADV_SLOW_MIN  0x0800  // 1280 ms
#define BLE_ADV_SLOW_MAX  0x1000  // 2560 ms

static void ble_app_advertise(void)
{
    // Don't (re)start advertising once teardown has begun — the disconnect and
    // adv-complete events fire during deinit and would touch a freed host.
    if (s_ble_shutting_down) return;

    // ── Main advertisement: flags + device name ──────────────────────────────
    struct ble_hs_adv_fields adv_fields = {};
    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    const char *name = ble_svc_gap_device_name();
    adv_fields.name           = (const uint8_t *)name;
    adv_fields.name_len       = strlen(name);
    adv_fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields error %d", rc);
        return;
    }

    // ── Scan response: 128-bit service UUID (iOS uses this to filter devices) ─
    struct ble_hs_adv_fields rsp_fields = {};
    rsp_fields.uuids128           = &s_svc_uuid;
    rsp_fields.num_uuids128       = 1;
    rsp_fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_rsp_set_fields error %d", rc);
        return;
    }

    // ── Start undirected connectable advertising ──────────────────────────────
    // After the first successful IP exchange, use slow advertising so that
    // periodic BLE wakeups don't preempt the UDP receiver on Core 0.
    struct ble_gap_adv_params adv_params = {};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    bool paired = (g_ios_ip[0] != '\0');
    adv_params.itvl_min = paired ? BLE_ADV_SLOW_MIN : BLE_ADV_FAST_MIN;
    adv_params.itvl_max = paired ? BLE_ADV_SLOW_MAX : BLE_ADV_FAST_MAX;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_handler, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_adv_start error %d", rc);
    } else {
        ESP_LOGI(TAG, "Advertising as \"%s\" (%s interval) …", name,
                 paired ? "slow/1.28s" : "fast/100ms");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// NimBLE host sync / reset callbacks
// ─────────────────────────────────────────────────────────────────────────────

static void ble_on_sync(void)
{
    ESP_LOGI(TAG, "BLE host synced — starting advertising");

    // Use a random static address if public address is unavailable
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr failed: %d", rc);
        return;
    }

    ble_app_advertise();
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset (reason %d)", reason);
}

// Task entry point for the NimBLE host (runs on its own core)
static void nimble_host_task(void *param)
{
    nimble_port_run();        // blocks until nimble_port_stop() is called
    nimble_port_freertos_deinit();
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

void ble_pairing_init(void)
{
    // Shared state initialisation
    g_ble_event_group = xEventGroupCreate();
    s_ip_mutex        = xSemaphoreCreateMutex();
    configASSERT(g_ble_event_group);
    configASSERT(s_ip_mutex);

    // Initialise the NimBLE transport layer (ESP-IDF manages the BT controller)
    ESP_ERROR_CHECK(nimble_port_init());

    // Register GATT services
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(BLE_DEVICE_NAME);

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "gatts_count_cfg: %d", rc); return; }

    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "gatts_add_svcs: %d", rc); return; }

    // Hook host callbacks
    ble_hs_cfg.sync_cb  = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;

    // Spin up the NimBLE host task (pinned to Core 0, low-ish priority)
    nimble_port_freertos_init(nimble_host_task);

    ESP_LOGI(TAG, "BLE pairing init done — service UUID 12AB3456-0000-1000-8000-00805F9B34FB");
}

void ble_pairing_deinit(void)
{
    static bool done = false;
    if (done) return;
    done = true;

    // 1. Signal all NimBLE-touching contexts to stand down. This stops the
    //    disconnect/adv-complete callbacks from re-advertising and tells any
    //    pending ble_term_task to bail out instead of calling into the host.
    s_ble_shutting_down = true;

    // 2. Wait (bounded) for a pending ble_term_task to finish using the host,
    //    so we never free it out from under an in-flight ble_gap_* call.
    for (int i = 0; i < 25 && s_term_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));   // ~500 ms max; term task exits within ~100 ms
    }

    // 3. Stop advertising, stop the host run loop (returns from nimble_port_run()
    //    in nimble_host_task, which self-deinits its task), then free host + ctlr.
    ble_gap_adv_stop();
    int rc = nimble_port_stop();
    if (rc == 0) {
        nimble_port_deinit();
        ESP_LOGI(TAG, "NimBLE stopped & deinitialised — BT radio released to WiFi");
    } else {
        ESP_LOGW(TAG, "nimble_port_stop failed: %d — BLE left running", rc);
    }
}
