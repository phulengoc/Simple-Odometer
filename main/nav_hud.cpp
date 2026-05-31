#include "nav_hud.h"

#include <string.h>
#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "stream_config.h"
#include "status_screen.h"

static const char *TAG = "nav_hud";

// Panel width (matches EXAMPLE_LCD_H_RES). The band spans the full width.
#define BAND_W  466

// ── Latest decoded telemetry (guarded by s_lock) ─────────────────────────────
typedef struct {
    uint8_t  flags;
    uint8_t  maneuver;       // nav_maneuver_t
    uint8_t  modifier;       // nav_modifier_t
    uint8_t  roundabout_exit;
    uint32_t dist_to_maneuver_m;
    uint32_t dist_remaining_m;
    uint32_t duration_remaining_s;
    char     primary[NAV_HUD_TEXT_MAX];
    char     secondary[NAV_HUD_TEXT_MAX];
    int64_t  rx_us;          // esp_timer_get_time() at receipt; 0 = never
} nav_state_t;

static nav_state_t       s_state = {};
static SemaphoreHandle_t s_lock  = NULL;
static uint32_t          s_seq   = 0;   // bumped on every accepted packet

static inline uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void nav_hud_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

bool nav_hud_update(const uint8_t *pkt, size_t len)
{
    if (!pkt || len < NAV_TELEMETRY_HEADER) return false;
    if (pkt[0] != NAV_TELEMETRY_MAGIC0 || pkt[1] != NAV_TELEMETRY_MAGIC1 ||
        pkt[2] != NAV_TELEMETRY_MAGIC2 || pkt[3] != NAV_TELEMETRY_MAGIC3) {
        return false;
    }
    if (pkt[4] != NAV_TELEMETRY_VERSION) {
        ESP_LOGW(TAG, "telemetry version %u != %u — ignored", pkt[4], NAV_TELEMETRY_VERSION);
        return false;
    }

    uint8_t primary_len   = pkt[21];
    uint8_t secondary_len = pkt[22];
    // Bounds-check the variable-length strings against the actual datagram.
    if ((size_t)NAV_TELEMETRY_HEADER + primary_len + secondary_len > len) {
        ESP_LOGW(TAG, "telemetry string lengths exceed packet (%u/%u, len=%u)",
                 primary_len, secondary_len, (unsigned)len);
        return false;
    }

    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);

    s_state.flags                = pkt[5];
    s_state.maneuver             = pkt[6];
    s_state.modifier             = pkt[7];
    s_state.dist_to_maneuver_m   = rd_u32(&pkt[8]);
    s_state.dist_remaining_m     = rd_u32(&pkt[12]);
    s_state.duration_remaining_s = rd_u32(&pkt[16]);
    s_state.roundabout_exit      = pkt[20];

    size_t pn = primary_len   < NAV_HUD_TEXT_MAX ? primary_len   : NAV_HUD_TEXT_MAX - 1;
    size_t sn = secondary_len < NAV_HUD_TEXT_MAX ? secondary_len : NAV_HUD_TEXT_MAX - 1;
    memcpy(s_state.primary,   &pkt[NAV_TELEMETRY_HEADER],              pn); s_state.primary[pn]   = '\0';
    memcpy(s_state.secondary, &pkt[NAV_TELEMETRY_HEADER + primary_len], sn); s_state.secondary[sn] = '\0';
    s_state.rx_us = esp_timer_get_time();
    s_seq++;

    if (s_lock) xSemaphoreGive(s_lock);
    return true;
}

uint32_t nav_hud_seq(void)
{
    uint32_t seq;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    seq = s_seq;
    if (s_lock) xSemaphoreGive(s_lock);
    return seq;
}

bool nav_hud_is_fresh(uint32_t max_age_ms)
{
    bool fresh = false;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state.rx_us != 0 &&
        (s_state.flags & (NAV_FLAG_NAVIGATING | NAV_FLAG_ARRIVED))) {
        int64_t age_ms = (esp_timer_get_time() - s_state.rx_us) / 1000;
        fresh = age_ms <= (int64_t)max_age_ms;
    }
    if (s_lock) xSemaphoreGive(s_lock);
    return fresh;
}

// ── Phrase + distance formatting ─────────────────────────────────────────────

// "TURN RIGHT", "ROUNDABOUT 2nd exit", "ARRIVE", … derived from type + modifier.
static const char *maneuver_phrase(const nav_state_t *st, char *scratch, size_t n)
{
    if (st->flags & NAV_FLAG_REROUTING) return "REROUTING";

    switch (st->maneuver) {
    case NAV_MANEUVER_ARRIVE: return "ARRIVE";
    case NAV_MANEUVER_DEPART: return "HEAD OUT";
    case NAV_MANEUVER_MERGE:  return "MERGE";
    case NAV_MANEUVER_ON_RAMP:  return "TAKE RAMP";
    case NAV_MANEUVER_OFF_RAMP: return "EXIT RAMP";
    case NAV_MANEUVER_FORK:
        return st->modifier >= NAV_MOD_SLIGHT_LEFT ? "KEEP LEFT" : "KEEP RIGHT";
    case NAV_MANEUVER_ROUNDABOUT:
        if (st->roundabout_exit > 0) {
            const char *ord = (st->roundabout_exit == 1) ? "1st" :
                              (st->roundabout_exit == 2) ? "2nd" :
                              (st->roundabout_exit == 3) ? "3rd" : "Nth";
            snprintf(scratch, n, "ROUNDABOUT %s", ord);
            return scratch;
        }
        return "ROUNDABOUT";
    default: break;   // NAV_MANEUVER_TURN / CONTINUE → fall through to modifier
    }

    switch (st->modifier) {
    case NAV_MOD_STRAIGHT:     return "CONTINUE";
    case NAV_MOD_SLIGHT_RIGHT: return "SLIGHT RIGHT";
    case NAV_MOD_RIGHT:        return "TURN RIGHT";
    case NAV_MOD_SHARP_RIGHT:  return "SHARP RIGHT";
    case NAV_MOD_SLIGHT_LEFT:  return "SLIGHT LEFT";
    case NAV_MOD_LEFT:         return "TURN LEFT";
    case NAV_MOD_SHARP_LEFT:   return "SHARP LEFT";
    case NAV_MOD_UTURN:        return "U-TURN";
    default:                   return "CONTINUE";
    }
}

// Distance to the next maneuver, e.g. "300 m" / "1.2 km" / "880 ft" / "0.6 mi".
static void format_distance(uint32_t meters, bool metric, char *out, size_t n)
{
    if (metric) {
        if (meters < 1000) {
            uint32_t rounded = ((meters + 5) / 10) * 10;   // nearest 10 m
            snprintf(out, n, "%u m", (unsigned)rounded);
        } else {
            snprintf(out, n, "%.1f km", meters / 1000.0);
        }
    } else {
        double feet = meters * 3.28084;
        if (feet < 1000) {
            uint32_t rounded = ((uint32_t)(feet + 5) / 10) * 10;
            snprintf(out, n, "%u ft", (unsigned)rounded);
        } else {
            snprintf(out, n, "%.1f mi", meters / 1609.344);
        }
    }
}

// ETA summary, e.g. "12 min  4.2 km" / "1h 5m  60 mi".
static void format_eta(const nav_state_t *st, bool metric, char *out, size_t n)
{
    char dist_remain[24];
    format_distance(st->dist_remaining_m, metric, dist_remain, sizeof(dist_remain));
    uint32_t mins = (st->duration_remaining_s + 30) / 60;
    if (mins >= 60) {
        snprintf(out, n, "%uh %um  %s", (unsigned)(mins / 60), (unsigned)(mins % 60), dist_remain);
    } else {
        snprintf(out, n, "%u min  %s", (unsigned)mins, dist_remain);
    }
}

void nav_hud_format_lines(char *l1, char *l2, char *l3, char *l4, size_t each)
{
    nav_state_t st;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    st = s_state;
    if (s_lock) xSemaphoreGive(s_lock);

    bool metric = (st.flags & NAV_FLAG_METRIC) != 0;

    // l1 — distance to maneuver (large title)
    format_distance(st.dist_to_maneuver_m, metric, l1, each);

    // l2 — maneuver instruction
    char scratch[NAV_HUD_TEXT_MAX];
    snprintf(l2, each, "%s", maneuver_phrase(&st, scratch, sizeof(scratch)));

    // l3 — primary road name (may be empty)
    snprintf(l3, each, "%s", st.primary);

    // l4 — ETA: remaining time + remaining distance
    format_eta(&st, metric, l4, each);
}

// Visible half-width of the round panel at a SCREEN row, inset by the safe
// margin. 0 outside the circle.
static int safe_half_width(int screen_y)
{
    int dy = screen_y - PANEL_CENTER;
    int v  = PANEL_SAFE_RADIUS * PANEL_SAFE_RADIUS - dy * dy;
    if (v <= 0) return 0;
    return (int)lroundf(sqrtf((float)v));
}

// Worst-case (narrowest) safe half-width spanned by a `scale`-tall glyph whose
// top sits at canvas row `cy`. The band is blitted at NAV_HUD_BAND_Y, so a
// canvas row maps to screen row NAV_HUD_BAND_Y + cy. For the bottom cap the
// bottom of the glyph is the tighter fit; checking both ends is band-position
// independent.
static int row_half_width(int cy, int scale)
{
    int top = NAV_HUD_BAND_Y + cy;
    int bot = NAV_HUD_BAND_Y + cy + 8 * scale - 1;
    int ht  = safe_half_width(top);
    int hb  = safe_half_width(bot);
    return ht < hb ? ht : hb;
}

// Draw `s` horizontally centred on the panel axis at canvas row `cy`.
static void draw_centered(uint16_t *canvas, int W, int H, int cy,
                          const char *s, int scale, uint16_t color)
{
    int x = PANEL_CENTER - status_text_width(s, scale) / 2;
    status_draw_text(canvas, W, H, x, cy, s, scale, color);
}

// Copy `in` into `out`, truncating with a trailing '.' if it is wider than the
// circular safe width available at canvas row `cy` for the given `scale`.
static void fit_centered(const char *in, int scale, int cy, char *out, size_t n)
{
    int glyph_w   = 8 * scale;
    int max_chars = (2 * row_half_width(cy, scale)) / glyph_w;
    int len = (int)strlen(in);
    if (max_chars < 1) { out[0] = '\0'; return; }
    if (len <= max_chars) { snprintf(out, n, "%s", in); return; }
    int keep = max_chars - 1;
    if (keep > (int)n - 2) keep = (int)n - 2;
    if (keep < 0) keep = 0;
    memcpy(out, in, keep);
    out[keep]     = '.';
    out[keep + 1] = '\0';
}

// ── Maneuver icons (procedural, scalable) ────────────────────────────────────
//
// Drawn from line primitives so a single routine covers every turn angle. The
// arrow is a vertical shaft that bends toward the exit heading, capped with a
// chevron head; roundabout/u-turn/arrive/reroute are special-cased.

static inline void put_px(uint16_t *c, int W, int H, int x, int y, uint16_t col)
{
    if (x >= 0 && x < W && y >= 0 && y < H) c[(size_t)y * W + x] = col;
}

// Stamp a filled square of side ~`t` centred at (x, y) — gives lines thickness.
static void stamp(uint16_t *c, int W, int H, int x, int y, int t, uint16_t col)
{
    int h = t / 2;
    for (int dy = -h; dy <= h; ++dy)
        for (int dx = -h; dx <= h; ++dx)
            put_px(c, W, H, x + dx, y + dy, col);
}

static void thick_line(uint16_t *c, int W, int H, int x0, int y0, int x1, int y1,
                       int t, uint16_t col)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        stamp(c, W, H, x0, y0, t, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Arc of a ring, angles in degrees (0° = +x, CW). For roundabout/reroute.
static void arc(uint16_t *c, int W, int H, int cx, int cy, int r,
                int a0, int a1, int t, uint16_t col)
{
    for (int a = a0; a <= a1; a += 5) {
        float rad = a * (float)M_PI / 180.0f;
        stamp(c, W, H, cx + (int)lroundf(r * cosf(rad)),
                       cy + (int)lroundf(r * sinf(rad)), t, col);
    }
}

// Chevron arrowhead at (tx, ty) pointing along heading `th` (rad from up).
static void arrow_head(uint16_t *c, int W, int H, int tx, int ty, float th,
                       int size, int t, uint16_t col)
{
    float dxv = sinf(th),  dyv = -cosf(th);   // forward
    float pxv = cosf(th),  pyv =  sinf(th);   // perpendicular
    float hh = size * 0.34f, hw = size * 0.26f;
    int bx = tx - (int)lroundf(dxv * hh), by = ty - (int)lroundf(dyv * hh);
    thick_line(c, W, H, tx, ty, bx + (int)lroundf(pxv * hw), by + (int)lroundf(pyv * hw), t, col);
    thick_line(c, W, H, tx, ty, bx - (int)lroundf(pxv * hw), by - (int)lroundf(pyv * hw), t, col);
}

// Turn/continue arrow: shaft up from the bottom, bending to heading `th`.
static void draw_arrow(uint16_t *c, int W, int H, int cx, int cy, int size,
                       uint16_t col, float th)
{
    int t = size * 13 / 100; if (t < 3) t = 3;
    int half = size / 2;
    int tx = cx + (int)lroundf(half * sinf(th));
    int ty = cy - (int)lroundf(half * cosf(th));
    thick_line(c, W, H, cx, cy + half, cx, cy, t, col);   // shaft
    thick_line(c, W, H, cx, cy, tx, ty, t, col);          // bend to exit
    arrow_head(c, W, H, tx, ty, th, size, t, col);
}

static void draw_uturn(uint16_t *c, int W, int H, int cx, int cy, int size, uint16_t col)
{
    int t = size * 13 / 100; if (t < 3) t = 3;
    int half = size / 2;
    int o = size * 26 / 100;
    int topy = cy - half + o;
    thick_line(c, W, H, cx + o, cy + half, cx + o, topy, t, col);   // right shaft up
    arc(c, W, H, cx, topy, o, 180, 360, t, col);                    // U over the top
    thick_line(c, W, H, cx - o, topy, cx - o, cy + half / 3, t, col); // left shaft down
    arrow_head(c, W, H, cx - o, cy + half / 3, (float)M_PI, size, t, col); // head down
}

static void draw_roundabout(uint16_t *c, int W, int H, int cx, int cy, int size,
                            uint16_t col, float th)
{
    int t = size * 11 / 100; if (t < 3) t = 3;
    int r = size * 28 / 100;
    arc(c, W, H, cx, cy, r, 0, 359, t, col);                       // the circle
    thick_line(c, W, H, cx, cy + size / 2, cx, cy + r, t, col);    // entry stub (bottom)
    int sx = cx + (int)lroundf(r * sinf(th)),  sy = cy - (int)lroundf(r * cosf(th));
    int ex = cx + (int)lroundf((size / 2) * sinf(th)),
        ey = cy - (int)lroundf((size / 2) * cosf(th));
    thick_line(c, W, H, sx, sy, ex, ey, t, col);                   // exit spoke
    arrow_head(c, W, H, ex, ey, th, size, t, col);
}

// Destination flag.
static void draw_flag(uint16_t *c, int W, int H, int cx, int cy, int size, uint16_t col)
{
    int t = size * 12 / 100; if (t < 3) t = 3;
    int half = size / 2;
    int px = cx - size / 4;
    thick_line(c, W, H, px, cy + half, px, cy - half, t, col);     // pole
    int fh = size * 40 / 100, fw = size * 55 / 100;
    for (int i = 0; i < fh; ++i) {                                 // filled pennant
        int len = fw * (fh - i) / fh;
        thick_line(c, W, H, px, cy - half + i, px + len, cy - half + i, 2, col);
    }
}

static float modifier_theta(uint8_t mod)
{
    switch (mod) {
    case NAV_MOD_SLIGHT_RIGHT: return  40.0f;
    case NAV_MOD_RIGHT:        return  85.0f;
    case NAV_MOD_SHARP_RIGHT:  return 130.0f;
    case NAV_MOD_SLIGHT_LEFT:  return -40.0f;
    case NAV_MOD_LEFT:         return -85.0f;
    case NAV_MOD_SHARP_LEFT:   return -130.0f;
    default:                   return   0.0f;   // straight
    }
}

// Draw the maneuver icon centred at (cx, cy) in canvas coords.
static void draw_maneuver_icon(uint16_t *c, int W, int H, int cx, int cy,
                               int size, uint16_t col, const nav_state_t *st)
{
    float th = modifier_theta(st->modifier) * (float)M_PI / 180.0f;
    switch (st->maneuver) {
    case NAV_MANEUVER_ARRIVE:     draw_flag(c, W, H, cx, cy, size, col); break;
    case NAV_MANEUVER_ROUNDABOUT: draw_roundabout(c, W, H, cx, cy, size, col, th); break;
    case NAV_MANEUVER_DEPART:     draw_arrow(c, W, H, cx, cy, size, col, 0.0f); break;
    default:
        if (st->modifier == NAV_MOD_UTURN) draw_uturn(c, W, H, cx, cy, size, col);
        else                               draw_arrow(c, W, H, cx, cy, size, col, th);
        break;
    }
}

// Counter-rotating-arrows glyph for the rerouting state.
static void draw_reroute_icon(uint16_t *c, int W, int H, int cx, int cy, int size, uint16_t col)
{
    int t = size * 11 / 100; if (t < 3) t = 3;
    int r = size * 30 / 100;
    arc(c, W, H, cx, cy, r, 20, 320, t, col);
    // arrowhead tangent at the 320° end, pointing CW
    float a = 320 * (float)M_PI / 180.0f;
    int ex = cx + (int)lroundf(r * cosf(a)), ey = cy + (int)lroundf(r * sinf(a));
    arrow_head(c, W, H, ex, ey, a + (float)M_PI / 2.0f, size, t, col);
}

// ── HUD band (bottom circular cap, icon + text, state-aware) ─────────────────
//
// Layout (canvas rows cy; screen row = NAV_HUD_BAND_Y + cy):
//
//        ╭───────────────────────╮  cy=1  chord separator (map above)
//       ╱     ⮕  300 m            ╲     cy~6   icon (52px) + distance (scale 3)
//      │     Prins Hendrikkade     │    cy=66  road  (scale 2, truncated)
//       ╲      12 min  3.9 km     ╱     cy=90  ETA   (scale 2)
//        ╰───────────────────────╯  y=466
//
// Reroute / arrive replace the icon+distance row with a dedicated icon + label.
void nav_hud_draw_band(void)
{
    const int W = BAND_W;
    const int H = NAV_HUD_BAND_HEIGHT;

    uint16_t *canvas = (uint16_t *)heap_caps_malloc((size_t)W * H * 2, MALLOC_CAP_SPIRAM);
    if (!canvas) {
        ESP_LOGE(TAG, "band canvas alloc failed");
        return;
    }

    nav_state_t st;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    st = s_state;
    if (s_lock) xSemaphoreGive(s_lock);

    bool metric = (st.flags & NAV_FLAG_METRIC) != 0;

    uint16_t bg    = status_be565(0x0000);   // black
    uint16_t white = status_be565(0xFFFF);
    uint16_t green = status_be565(0x07E0);
    uint16_t amber = status_be565(0xFD20);
    uint16_t grey  = status_be565(0xAD55);
    uint16_t sep   = status_be565(0x39E7);   // dim grey separator
    for (size_t i = 0; i < (size_t)W * H; ++i) canvas[i] = bg;

    // Chord separator just inside the top edge of the band (the map/band line).
    int half = safe_half_width(NAV_HUD_BAND_Y + 1);
    for (int yy = 1; yy <= 2; ++yy) {
        uint16_t *line = canvas + (size_t)yy * W;
        for (int xx = PANEL_CENTER - half; xx <= PANEL_CENTER + half; ++xx) {
            if (xx >= 0 && xx < W) line[xx] = sep;
        }
    }

    const int ICON = 52;
    const int icon_cy = 32;                 // icon centre row (spans ~6..58)
    char src[NAV_HUD_TEXT_MAX];
    char buf[NAV_HUD_TEXT_MAX];

    if (st.flags & NAV_FLAG_REROUTING) {
        // Rerouting: spinner icon + label, keep ETA.
        draw_reroute_icon(canvas, W, H, PANEL_CENTER, icon_cy, ICON, amber);
        draw_centered(canvas, W, H, 64, "Rerouting", 3, amber);
        format_eta(&st, metric, src, sizeof(src));
        fit_centered(src, 2, 96, buf, sizeof(buf));
        draw_centered(canvas, W, H, 96, buf, 2, grey);

    } else if (st.flags & NAV_FLAG_ARRIVED) {
        // Arrival: flag + "Arrived" + destination name.
        draw_flag(canvas, W, H, PANEL_CENTER, icon_cy, ICON, green);
        draw_centered(canvas, W, H, 64, "Arrived", 3, green);
        if (st.primary[0]) {
            fit_centered(st.primary, 2, 96, buf, sizeof(buf));
            draw_centered(canvas, W, H, 96, buf, 2, white);
        }

    } else {
        // Normal: [icon][distance] centred as a group, then road, then ETA.
        format_distance(st.dist_to_maneuver_m, metric, src, sizeof(src));
        int dist_w  = status_text_width(src, 3);
        int gap     = 14;
        int group_w = ICON + gap + dist_w;
        int start_x = PANEL_CENTER - group_w / 2;
        draw_maneuver_icon(canvas, W, H, start_x + ICON / 2, icon_cy, ICON, white, &st);
        status_draw_text(canvas, W, H, start_x + ICON + gap, icon_cy - 12, src, 3, white);

        if (st.primary[0]) {
            fit_centered(st.primary, 2, 66, buf, sizeof(buf));
            draw_centered(canvas, W, H, 66, buf, 2, white);
        }
        format_eta(&st, metric, src, sizeof(src));
        fit_centered(src, 2, 90, buf, sizeof(buf));
        draw_centered(canvas, W, H, 90, buf, 2, grey);
    }

    status_blit(canvas, W, H, 0, NAV_HUD_BAND_Y);
    heap_caps_free(canvas);
}
