#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Tach Ring demo UI — a 466×466 round AMOLED tachometer/speedometer.
//
// Renders a 270° gauge (track + progress arc, ticks, labels), a large centre
// speed number, a gear digit, and a rolling odometer, all CPU-side into a
// big-endian RGB565 framebuffer, then blits via status_blit().
//
// The renderer is platform-agnostic (pure pixel math). On the device it pushes
// frames through status_blit() (status_screen.cpp); a host harness can provide
// its own status_blit() to dump frames to disk for preview.
// ─────────────────────────────────────────────────────────────────────────────

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float  rpm;         // engine RPM — drives the outer progress arc
    float  speed_kmh;   // vehicle speed — shown as the centre number
    int    gear;        // gear digit shown bottom-left
    double odo_km;      // odometer value (fractional part drives the reel roll)
} tach_state_t;

/// Allocate framebuffers and build the static background layer (ring, ticks,
/// labels) once. Call after the panel + status_screen are initialised.
void tach_ring_init(void);

/// Render one frame for the given state and blit it to the panel.
void tach_ring_render(const tach_state_t *s);

#ifdef __cplusplus
}
#endif
