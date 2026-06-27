// Host-side preview harness for the Tach Ring renderer.
//
// Compiles main/tach_ring.cpp + main/text8x8.c on the host (no ESP-IDF), feeds a
// chosen state, and dumps the resulting frame to a PPM file so the UI can be
// reviewed without hardware. status_blit() here just writes the canvas to disk.
//
// Build/run is driven by tools/preview.sh.

#include "tach_ring.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#define PW 466
#define PH 466

static const char *g_out = "tools/preview_out.ppm";

// Provided to tach_ring.cpp (host build). Canvas is big-endian RGB565.
extern "C" void status_blit(const uint16_t *canvas, int w, int h, int dx, int dy) {
    (void)dx; (void)dy;
    FILE *f = fopen(g_out, "wb");
    if (!f) { perror("fopen"); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) {
        uint16_t be = canvas[i];
        uint16_t c = (uint16_t)((be >> 8) | (be << 8));   // native RGB565
        unsigned char r = (unsigned char)(((c >> 11) & 0x1F) << 3);
        unsigned char g = (unsigned char)(((c >> 5)  & 0x3F) << 2);
        unsigned char b = (unsigned char)((c & 0x1F) << 3);
        unsigned char px[3] = { r, g, b };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    // args: speed gear odo rpm [out]
    tach_state_t st = {};
    st.speed_kmh = (argc > 1) ? (float)atof(argv[1]) : 37.0f;
    st.gear      = (argc > 2) ? atoi(argv[2]) : 3;
    st.odo_km    = (argc > 3) ? atof(argv[3]) : 4905.5;
    st.rpm       = (argc > 4) ? (float)atof(argv[4]) : 6000.0f;
    if (argc > 5) g_out = argv[5];

    tach_ring_init();
    tach_ring_render(&st);
    fprintf(stderr, "wrote %s (rpm=%.0f speed=%.0f gear=%d odo=%.1f)\n",
            g_out, st.rpm, st.speed_kmh, st.gear, st.odo_km);
    return 0;
}
