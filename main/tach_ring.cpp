#include "tach_ring.h"
#include "text8x8.h"
#include "digits_font.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ── Platform glue ─────────────────────────────────────────────────────────────
// On the device, frames are pushed through status_blit() and buffers live in
// PSRAM. On the host preview build (no ESP_PLATFORM), use malloc and an
// externally-provided status_blit() that dumps the canvas to disk.
#ifdef ESP_PLATFORM
  #include "esp_heap_caps.h"
  #include "esp_timer.h"
  #include "esp_log.h"
  #include "status_screen.h"
  #define TR_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
  #define PNOW() esp_timer_get_time()
  static int64_t pa_memcpy, pa_overlay, pa_arc, pa_blit; static int pframes;
#else
  #define TR_ALLOC(n) malloc(n)
  #define PNOW() 0
  extern "C" void status_blit(const uint16_t *canvas, int w, int h, int dst_x, int dst_y);
#endif

// ── Panel geometry (466×466 round) ────────────────────────────────────────────
#define W   466
#define H   466
#define CX  233.0f
#define CY  233.0f

// Gauge sweep: value 0..120 maps to screen angle 135°..405° (clockwise, y-down).
#define V_MIN     0.0f
#define V_MAX     120.0f
#define A_START   135.0f          // degrees, value 0 (bottom-left)
#define A_SWEEP   270.0f          // degrees, 0 -> 120

// The ring is a tachometer: the progress arc is driven by engine RPM. Full
// sweep = RPM_FS, so the 0..120 tick scale reads as RPM / 100.
#define RPM_FS    12000.0f

// Radii (panel space). The panel is a 466 round display (radius 233), so the
// arc is pushed close to the rim.
#define R_ARC        226.0f       // track / progress arc centreline (near rim)
#define ARC_HALF     4.0f         // arc half-thickness (≈8 px)
#define R_TICK_OUT   212.0f       // outer end of tick marks (just inside the arc)
#define MAJOR_LEN    20.0f
#define MINOR_LEN    11.0f
#define MAJOR_HALF   1.6f
#define MINOR_HALF   0.8f
#define R_LABEL      168.0f       // tick-number centre radius

// Colours (native RGB565; stored byte-swapped via t8_be565)
#define COL_BG        0x0000
#define COL_TRACK     0x2945      // dim grey arc track
#define COL_TICK_MAJ  0xC618      // light grey
#define COL_TICK_MIN  0x4208      // mid grey
#define COL_LABEL     0xAD55      // tick numbers
#define COL_UNIT      0x6B4D      // KM / H, ODO, GEAR labels

static const float DEG2RAD = 3.14159265358979f / 180.0f;

// ── Framebuffers ──────────────────────────────────────────────────────────────
static uint16_t *s_bg    = NULL;   // static background layer (built once)
static uint16_t *s_frame = NULL;   // per-frame composite

// Precomputed progress-arc band: every pixel of the arc annulus with its angle
// along the sweep and its radial anti-alias coverage. Built once so per-frame
// arc drawing is just a list walk + blend (no per-pixel sqrt/atan2).
typedef struct { uint16_t x, y; uint16_t t10; uint8_t rcov; } arc_px_t;
static arc_px_t *s_arc   = NULL;
static int       s_arc_n = 0;

// Last drawn speed / gear, so those (rarely-changing) elements are only
// re-rendered and re-restored when their value actually changes.
static int s_last_speed = -1;
static int s_last_gear  = -99;

// ── Pixel helpers (canvas is big-endian RGB565) ───────────────────────────────
static inline void unpack_be(uint16_t be, int *r, int *g, int *b) {
    uint16_t c = (uint16_t)((be >> 8) | (be << 8));
    *r = ((c >> 11) & 0x1F) << 3;
    *g = ((c >> 5)  & 0x3F) << 2;
    *b = (c & 0x1F) << 3;
}
static inline uint16_t pack_be(int r, int g, int b) {
    uint16_t c = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    return (uint16_t)((c >> 8) | (c << 8));
}

// Alpha-blend an (r,g,b) source over the canvas pixel with coverage cov [0..255].
static inline void blend(uint16_t *canvas, int x, int y, int r, int g, int b, int cov) {
    if (x < 0 || x >= W || y < 0 || y >= H || cov <= 0) return;
    size_t i = (size_t)y * W + x;
    if (cov >= 255) { canvas[i] = pack_be(r, g, b); return; }
    int br, bg, bb;
    unpack_be(canvas[i], &br, &bg, &bb);
    canvas[i] = pack_be(br + (r - br) * cov / 255,
                        bg + (g - bg) * cov / 255,
                        bb + (b - bb) * cov / 255);
}

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// ── Anti-aliased thick line segment (used for tick marks) ─────────────────────
static void draw_seg_aa(uint16_t *canvas, float x0, float y0, float x1, float y1,
                        float half, uint16_t color) {
    int r, g, b; unpack_be(color, &r, &g, &b);
    float minx = fminf(x0, x1) - half - 1, maxx = fmaxf(x0, x1) + half + 1;
    float miny = fminf(y0, y1) - half - 1, maxy = fmaxf(y0, y1) + half + 1;
    int ix0 = (int)floorf(minx), ix1 = (int)ceilf(maxx);
    int iy0 = (int)floorf(miny), iy1 = (int)ceilf(maxy);
    float dx = x1 - x0, dy = y1 - y0;
    float len2 = dx * dx + dy * dy;
    for (int y = iy0; y <= iy1; ++y) {
        for (int x = ix0; x <= ix1; ++x) {
            float px = x - x0, py = y - y0;
            float t = len2 > 0 ? clampf((px * dx + py * dy) / len2, 0, 1) : 0;
            float cxp = px - t * dx, cyp = py - t * dy;
            float dist = sqrtf(cxp * cxp + cyp * cyp);
            float cov = clampf(half + 0.5f - dist, 0, 1);
            if (cov > 0) blend(canvas, x, y, r, g, b, (int)(cov * 255));
        }
    }
}

// ── Anti-aliased ring arc (track + progress) ──────────────────────────────────
// Draws the annular band of radius R (half-thickness `half`) from value sweep
// angle a0_deg to a1_deg (both measured from A_START, clockwise).
static void draw_arc(uint16_t *canvas, float R, float half,
                     float t0_deg, float t1_deg, uint16_t color) {
    if (t1_deg <= t0_deg) return;
    int r, g, b; unpack_be(color, &r, &g, &b);
    float Ro = R + half + 1;
    int ix0 = (int)floorf(CX - Ro), ix1 = (int)ceilf(CX + Ro);
    int iy0 = (int)floorf(CY - Ro), iy1 = (int)ceilf(CY + Ro);
    for (int y = iy0; y <= iy1; ++y) {
        for (int x = ix0; x <= ix1; ++x) {
            float dx = x - CX, dy = y - CY;
            float d = sqrtf(dx * dx + dy * dy);
            float rc = clampf(half + 0.5f - fabsf(d - R), 0, 1);
            if (rc <= 0) continue;
            // angle measured from A_START, clockwise, normalised to [0,360)
            float a = atan2f(dy, dx) / DEG2RAD;          // -180..180
            float t = a - A_START;
            while (t < 0) t += 360.0f;
            while (t >= 360.0f) t -= 360.0f;
            if (t < t0_deg || t > t1_deg) continue;
            // angular AA at the two caps (≈1 px of arc length)
            float edge = fminf(t - t0_deg, t1_deg - t) * DEG2RAD * R;
            float ac = clampf(edge + 0.5f, 0, 1);
            int cov = (int)(rc * ac * 255);
            if (cov > 0) blend(canvas, x, y, r, g, b, cov);
        }
    }
}

// Point on the gauge at value v, radius rr.
static inline void gauge_pt(float v, float rr, float *px, float *py) {
    float a = (A_START + (v - V_MIN) / (V_MAX - V_MIN) * A_SWEEP) * DEG2RAD;
    *px = CX + rr * cosf(a);
    *py = CY + rr * sinf(a);
}

// ── Static background layer ───────────────────────────────────────────────────
static void build_background(void) {
    uint16_t bg = t8_be565(COL_BG);
    for (size_t i = 0; i < (size_t)W * H; ++i) s_bg[i] = bg;

    // Track arc (full sweep)
    draw_arc(s_bg, R_ARC, ARC_HALF, 0, A_SWEEP, t8_be565(COL_TRACK));

    // Tick marks every 5 units; majors (longer/thicker) every 20. No numeric
    // labels — the ring is just marks.
    for (int v = 0; v <= 120; v += 5) {
        int major = (v % 20 == 0);
        float len  = major ? MAJOR_LEN : MINOR_LEN;
        float half = major ? MAJOR_HALF : MINOR_HALF;
        uint16_t col = t8_be565(major ? COL_TICK_MAJ : COL_TICK_MIN);
        float x0, y0, x1, y1;
        gauge_pt(v, R_TICK_OUT, &x0, &y0);
        gauge_pt(v, R_TICK_OUT - len, &x1, &y1);
        draw_seg_aa(s_bg, x0, y0, x1, y1, half, col);
    }
}

// ── Dynamic overlay ───────────────────────────────────────────────────────────
#define COL_PROGRESS  0xFFFF      // white progress arc
#define COL_SPEED     0xFFFF      // centre number
#define COL_GEAR      0xFFFF
#define COL_ODO       0xFFFF
#define COL_SLOT      0x10A2      // odometer digit well

// Layout (panel space)
#define SPEED_CY      182.0f      // vertical centre of the big speed number
#define GEAR_CX       128         // gear indicator centre x
#define GEAR_TOP      296
#define ODO_CX        285         // centre of the odometer reel row
#define ODO_TOP       300         // top of the centred (resting) digit
#define ODO_DIGITS    6
#define ODO_GAP       4           // px between odo slots (horizontal)
#define ODO_PEEK      12          // px of the window above/below the resting digit
#define ODO_ROW_GAP   16          // vertical gap between digits on the drum
#define ODO_DOT_GAP   14          // extra space before the units reel for the decimal dot

static void fill_rect(uint16_t *cv, int x0, int y0, int w, int h, uint16_t color_be) {
    for (int y = y0; y < y0 + h; ++y) {
        if (y < 0 || y >= H) continue;
        for (int x = x0; x < x0 + w; ++x) {
            if (x < 0 || x >= W) continue;
            cv[(size_t)y * W + x] = color_be;
        }
    }
}

// Anti-aliased rounded-rectangle outline via a signed-distance field (each pixel
// is evaluated once, so corners blend cleanly without overlap doubling).
static void draw_round_rect(uint16_t *cv, float x, float y, float w, float h,
                            float rad, float half, uint16_t color) {
    int r, g, b; unpack_be(color, &r, &g, &b);
    float cx = x + w / 2, cy = y + h / 2;
    float ix = w / 2 - rad, iy = h / 2 - rad;     // inner half-extents
    int x0 = (int)floorf(x - half - 1), x1 = (int)ceilf(x + w + half + 1);
    int y0 = (int)floorf(y - half - 1), y1 = (int)ceilf(y + h + half + 1);
    for (int py = y0; py <= y1; ++py) {
        for (int px = x0; px <= x1; ++px) {
            float dx = fabsf(px - cx) - ix;
            float dy = fabsf(py - cy) - iy;
            float ax = dx > 0 ? dx : 0, ay = dy > 0 ? dy : 0;
            float d = sqrtf(ax * ax + ay * ay) + fminf(fmaxf(dx, dy), 0.0f) - rad;
            float cov = clampf(half + 0.5f - fabsf(d), 0.0f, 1.0f);
            if (cov > 0) blend(cv, px, py, r, g, b, (int)(cov * 255));
        }
    }
}

// Small anti-aliased filled disc (the odometer decimal point).
static void fill_disc(uint16_t *cv, float cx, float cy, float rad, int r, int g, int b) {
    int x0 = (int)floorf(cx - rad - 1), x1 = (int)ceilf(cx + rad + 1);
    int y0 = (int)floorf(cy - rad - 1), y1 = (int)ceilf(cy + rad + 1);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            float dx = x - cx, dy = y - cy;
            float cov = clampf(rad + 0.5f - sqrtf(dx * dx + dy * dy), 0.0f, 1.0f);
            if (cov > 0) blend(cv, x, y, r, g, b, (int)(cov * 255));
        }
    }
}

// Restore a rectangle of the persistent frame from the static background (used
// to erase the previous frame's dynamic content without a full-frame memcpy).
static void restore_rect(int x0, int y0, int w, int h) {
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > W) w = W - x0;
    if (y0 + h > H) h = H - y0;
    for (int y = 0; y < h; ++y) {
        size_t off = (size_t)(y0 + y) * W + x0;
        memcpy(s_frame + off, s_bg + off, (size_t)w * 2);
    }
}

// Blit one anti-aliased coverage glyph (8-bit alpha) at (x, y).
static void draw_cov_glyph(uint16_t *cv, const uint8_t *glyph, int gw, int gh,
                           int x, int y, int r, int g, int b) {
    for (int row = 0; row < gh; ++row) {
        int yy = y + row;
        if (yy < 0 || yy >= H) continue;
        const uint8_t *gr = glyph + (size_t)row * gw;
        for (int col = 0; col < gw; ++col) {
            int cov = gr[col];
            if (cov) blend(cv, x + col, yy, r, g, b, cov);
        }
    }
}

// Big centred number (speed). Monospace cells, centred on cx.
static void draw_big_number(uint16_t *cv, int value, float cx, float cy_center,
                            int r, int g, int b) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", value);
    int n = (int)strlen(buf);
    int gw = DIGITS_BIG_W, gh = DIGITS_BIG_H;
    int x = (int)(cx - n * gw / 2.0f);
    int y = (int)(cy_center - gh / 2.0f);
    for (int i = 0; i < n; ++i) {
        int d = buf[i] - '0';
        draw_cov_glyph(cv, DIGITS_BIG[d], gw, gh, x + i * gw, y, r, g, b);
    }
}

// Gear indicator centred at cx: a green "N" for neutral (gear <= 0), otherwise
// the white gear digit.
static void draw_gear(uint16_t *cv, int gear, int cx, int top) {
    int gw = DIGITS_MID_W, gh = DIGITS_MID_H;
    if (gear <= 0) {
        draw_cov_glyph(cv, GEAR_N, gw, gh, cx - gw / 2, top, 0, 220, 70);   // green N
    } else {
        if (gear > 9) gear = 9;
        draw_cov_glyph(cv, DIGITS_MID[gear], gw, gh, cx - gw / 2, top, 255, 255, 255);
    }
}

// Coverage glyph blit at a sub-pixel vertical position, clipped to [cy0, cy1).
// Uses vertical "gather" resampling: each destination row linearly samples
// between the two source rows and is blended exactly once. This conserves
// coverage (a solid interior stays fully opaque at any sub-pixel offset), so the
// glyph doesn't pulse in brightness as it rolls — unlike a "scatter" split,
// where overlapping over-blends dip the interior to ~75% at half-pixel offsets.
static void draw_cov_glyph_clip(uint16_t *cv, const uint8_t *glyph, int gw, int gh,
                                int x, float yf, int cy0, int cy1,
                                int r, int g, int b) {
    int dy0 = (int)floorf(yf);
    int dy1 = (int)ceilf(yf + gh);
    for (int dyi = dy0; dyi <= dy1; ++dyi) {
        if (dyi < cy0 || dyi >= cy1 || dyi < 0 || dyi >= H) continue;
        float srcf = dyi - yf;                 // source-row coordinate for this dest row
        int   sr   = (int)floorf(srcf);
        float f    = srcf - sr;
        const uint8_t *r0 = (sr     >= 0 && sr     < gh) ? glyph + (size_t)sr * gw       : NULL;
        const uint8_t *r1 = (sr + 1 >= 0 && sr + 1 < gh) ? glyph + (size_t)(sr + 1) * gw : NULL;
        if (!r0 && !r1) continue;
        for (int col = 0; col < gw; ++col) {
            int c0 = r0 ? r0[col] : 0;
            int c1 = r1 ? r1[col] : 0;
            int cov = (int)(c0 * (1.0f - f) + c1 * f);
            if (cov > 0) blend(cv, x + col, dyi, r, g, b, cov);
        }
    }
}

// Vertical curvature shading over a slot window: darken toward the top/bottom
// edges so the reel reads as a rounded drum rather than a flat slot.
static void drum_shade(uint16_t *cv, int x, int y, int w, int h) {
    float cyf = y + h / 2.0f, half = h / 2.0f;
    for (int yy = y; yy < y + h; ++yy) {
        if (yy < 0 || yy >= H) continue;
        float t = (yy - cyf) / half;          // -1 .. 1 across the window
        int alpha = (int)(t * t * 215.0f);    // 0 at centre, ~0.84 black at edges
        if (alpha <= 0) continue;
        for (int xx = x; xx < x + w; ++xx) {
            if (xx >= 0 && xx < W) blend(cv, xx, yy, 0, 0, 0, alpha);
        }
    }
}

// Mechanical odometer drum: each reel shows its digit with the neighbouring
// digits peeking above/below through a curved window. The units reel rolls
// continuously with the fractional km; each higher reel rolls only while the
// reels to its right are carrying over (classic odometer behaviour).
static void draw_odo(uint16_t *cv, double odo) {
    if (odo < 0) odo = 0;
    int gw = DIGITS_SMALL_W, gh = DIGITS_SMALL_H;
    int slot_w = gw + ODO_GAP;
    int total  = ODO_DIGITS * slot_w - ODO_GAP + ODO_DOT_GAP;
    int x0     = ODO_CX - total / 2;
    int win_top = ODO_TOP - ODO_PEEK;
    int win_h   = gh + 2 * ODO_PEEK;
    int cy0 = win_top, cy1 = win_top + win_h;

    for (int i = 0; i < ODO_DIGITS; ++i) {
        int p  = ODO_DIGITS - 1 - i;          // place value (0 = units, rightmost)
        // The units reel is pushed right by ODO_DOT_GAP to make room for the dot.
        int sx = x0 + i * slot_w + (i == ODO_DIGITS - 1 ? ODO_DOT_GAP : 0);
        // The units reel (rightmost) is an inverted drum: white background with
        // a black digit. The higher reels are white digits on a grey well.
        bool units = (p == 0);
        uint16_t slot = t8_be565(units ? 0xFFFF : COL_SLOT);
        int dcol = units ? 0 : 255;          // digit colour (black on units reel)
        fill_rect(cv, sx - 2, win_top - 2, gw + 4, win_h + 4, slot);

        double scale = pow(10.0, p);
        double xval  = odo / scale;
        double fp    = xval - floor(xval);    // fraction within this reel's step
        int    dp    = ((long)floor(xval)) % 10;
        if (dp < 0) dp += 10;

        double thr = 1.0 - 1.0 / scale;       // reel only rolls in its final 1/10^p
        double o   = (fp <= thr) ? 0.0 : (fp - thr) / (1.0 - thr);

        // Stack the neighbouring digits spaced by the row pitch (glyph height +
        // gap) so consecutive digits are clearly separated as the reel turns.
        int pitch = gh + ODO_ROW_GAP;
        for (int k = -1; k <= 2; ++k) {
            int dk = ((dp + k) % 10 + 10) % 10;
            float y = ODO_TOP - o * pitch + k * pitch;   // sub-pixel position
            draw_cov_glyph_clip(cv, DIGITS_SMALL[dk], gw, gh, sx, y, cy0, cy1,
                                dcol, dcol, dcol);
        }
        drum_shade(cv, sx - 2, win_top - 2, gw + 4, win_h + 4);   // match the fill rect
    }

    // The decimal point and the rounded frame are static — drawn once into the
    // background (see draw_odo_frame), not here, so they aren't redrawn each frame.
}

// Draw the static drum decorations (decimal point + rounded frame) once, into
// the background layer. These never change, so the per-frame restore brings them
// back for free and they stay out of the hot path.
static void draw_odo_frame(uint16_t *bg) {
    int gw = DIGITS_SMALL_W, gh = DIGITS_SMALL_H;
    int slot_w = gw + ODO_GAP;
    int total  = ODO_DIGITS * slot_w - ODO_GAP + ODO_DOT_GAP;
    int x0     = ODO_CX - total / 2;
    int win_top = ODO_TOP - ODO_PEEK, win_h = gh + 2 * ODO_PEEK;

    // Decimal point in the gap between the last grey reel and the units reel.
    float gx = x0 + (ODO_DIGITS - 2) * slot_w + gw;
    float ux = x0 + (ODO_DIGITS - 1) * slot_w + ODO_DOT_GAP;
    fill_disc(bg, (gx + ux) / 2.0f, ODO_TOP + gh - 4.0f, 3.0f, 255, 255, 255);

    // Rounded frame around the whole drum.
    float L = x0 - 6.0f;
    float R = x0 + (ODO_DIGITS - 1) * slot_w + ODO_DOT_GAP + gw + 6.0f;
    float T = win_top - 6.0f;
    float B = win_top + win_h + 6.0f;
    draw_round_rect(bg, L, T, R - L, B - T, 8.0f, 0.5f, t8_be565(0x4208));
}

// Fast progress arc: walk the precomputed band, blend pixels up to `span_deg`
// with angular-cap anti-aliasing at the leading edge. No transcendentals.
static void draw_arc_fast(uint16_t *cv, float span_deg, int r, int g, int b) {
    for (int i = 0; i < s_arc_n; ++i) {
        float t = s_arc[i].t10 * 0.1f;
        if (t > span_deg) continue;
        float edge = fminf(t, span_deg - t) * DEG2RAD * R_ARC;   // px to nearest cap
        float ac = clampf(edge + 0.5f, 0.0f, 1.0f);
        int cov = (int)(s_arc[i].rcov * ac);
        if (cov > 0) blend(cv, s_arc[i].x, s_arc[i].y, r, g, b, cov);
    }
}

// Precompute the progress-arc band once (the expensive sqrt/atan2 scan happens
// here, not per frame). Two passes: count, then fill.
static void build_arc_band(void) {
    float R = R_ARC, half = ARC_HALF;
    float Ro = R + half + 1;
    int ix0 = (int)floorf(CX - Ro), ix1 = (int)ceilf(CX + Ro);
    int iy0 = (int)floorf(CY - Ro), iy1 = (int)ceilf(CY + Ro);

    for (int pass = 0; pass < 2; ++pass) {
        int n = 0;
        for (int y = iy0; y <= iy1; ++y) {
            for (int x = ix0; x <= ix1; ++x) {
                float dx = x - CX, dy = y - CY;
                float d = sqrtf(dx * dx + dy * dy);
                float rc = clampf(half + 0.5f - fabsf(d - R), 0.0f, 1.0f);
                if (rc <= 0) continue;
                float a = atan2f(dy, dx) / DEG2RAD;
                float t = a - A_START;
                while (t < 0) t += 360.0f;
                while (t >= 360.0f) t -= 360.0f;
                if (t > A_SWEEP) continue;               // skip the bottom gap
                if (pass == 1) {
                    s_arc[n].x = (uint16_t)x;
                    s_arc[n].y = (uint16_t)y;
                    s_arc[n].t10 = (uint16_t)(t * 10.0f);
                    s_arc[n].rcov = (uint8_t)(rc * 255.0f);
                }
                ++n;
            }
        }
        if (pass == 0) {
            s_arc_n = n;
            s_arc = (arc_px_t *)TR_ALLOC((size_t)n * sizeof(arc_px_t));
            if (!s_arc) { s_arc_n = 0; return; }
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────
void tach_ring_init(void) {
    s_bg    = (uint16_t *)TR_ALLOC((size_t)W * H * 2);
    s_frame = (uint16_t *)TR_ALLOC((size_t)W * H * 2);
    if (!s_bg || !s_frame) return;
    build_background();
    draw_odo_frame(s_bg);                        // static drum frame, baked in once
    build_arc_band();
    memcpy(s_frame, s_bg, (size_t)W * H * 2);   // seed the persistent frame once
}

void tach_ring_render(const tach_state_t *s) {
    if (!s_bg || !s_frame) return;
    int64_t t0 = PNOW();

    // Erase last frame's dynamic content from the persistent frame. The arc band
    // (sparse pixel list) and the odo drum change every frame; the speed number
    // and gear change rarely, so they're only restored/redrawn on change.
    for (int i = 0; i < s_arc_n; ++i) {
        size_t idx = (size_t)s_arc[i].y * W + s_arc[i].x;
        s_frame[idx] = s_bg[idx];
    }
    int total = ODO_DIGITS * (DIGITS_SMALL_W + ODO_GAP) - ODO_GAP + ODO_DOT_GAP;
    restore_rect(ODO_CX - total / 2 - 9, ODO_TOP - ODO_PEEK - 9,
                 total + 18, DIGITS_SMALL_H + 2 * ODO_PEEK + 18);

    int sp = (int)lroundf(s->speed_kmh);
    if (sp < 0) sp = 0;
    bool speed_changed = (sp != s_last_speed);
    if (speed_changed)
        restore_rect((int)CX - 3 * DIGITS_BIG_W / 2 - 4, (int)SPEED_CY - DIGITS_BIG_H / 2 - 4,
                     3 * DIGITS_BIG_W + 8, DIGITS_BIG_H + 8);

    bool gear_changed = (s->gear != s_last_gear);
    if (gear_changed)
        restore_rect(GEAR_CX - DIGITS_MID_W / 2 - 4, GEAR_TOP - 4,
                     DIGITS_MID_W + 8, DIGITS_MID_H + 8);
    int64_t t1 = PNOW();

    // Draw. Arc + odo every frame; speed/gear only when changed.
    float span = clampf(s->rpm, 0.0f, RPM_FS) / RPM_FS * A_SWEEP;
    int64_t a0 = PNOW();
    if (span > 0.3f) draw_arc_fast(s_frame, span, 255, 255, 255);
#ifdef ESP_PLATFORM
    pa_arc += PNOW() - a0;
#else
    (void)a0;
#endif
    draw_odo(s_frame, s->odo_km);             // full fractional km drives the roll
    if (speed_changed) {
        draw_big_number(s_frame, sp, CX, SPEED_CY, 255, 255, 255);
        s_last_speed = sp;
    }
    if (gear_changed) {
        draw_gear(s_frame, s->gear, GEAR_CX, GEAR_TOP);
        s_last_gear = s->gear;
    }

    int64_t t2 = PNOW();
    status_blit(s_frame, W, H, 0, 0);
    int64_t t3 = PNOW();
#ifdef ESP_PLATFORM
    pa_memcpy += t1 - t0; pa_overlay += t2 - t1; pa_blit += t3 - t2;
    if (++pframes >= 60) {
        ESP_LOGI("tach_prof", "memcpy %lld | overlay %lld (arc %lld) | blit %lld us",
                 pa_memcpy / 60, pa_overlay / 60, pa_arc / 60, pa_blit / 60);
        pa_memcpy = pa_overlay = pa_arc = pa_blit = 0; pframes = 0;
    }
#else
    (void)t0; (void)t1; (void)t2; (void)t3;
#endif
}
