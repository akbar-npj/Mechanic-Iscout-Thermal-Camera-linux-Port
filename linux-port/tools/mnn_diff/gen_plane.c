/*
 * gen_plane.c — generate the differential's input plane.
 *
 * The differential needs a **non-constant** 256x192 plane.  A constant one is
 * useless as a gate: the MNN session tensor is NC4HW4, so a wrong-layout fill
 * produces exactly the same (wrong) frame as a right-layout fill, and the
 * comparison passes.  See src/mnn_runtime.cpp.
 *
 * The field is deliberately smooth and thermal-like rather than a ramp or
 * noise — a Gaussian blob on a linear gradient — so the comparison is driven by
 * something close to what the model actually sees.  (tools/mnn_diff/harness.c
 * has its own sawtooth ramp for quick manual runs; that is not the fixture.)
 *
 * Output is 256*192 uint16 little-endian, values 0..255 — the 8-bit range the
 * app hands the model.  Regenerating this must reproduce
 * tools/mnn_diff/out/in_plane.raw byte-for-byte.
 *
 * build: cc -O2 -o build/gen_plane gen_plane.c -lm
 * run:   ./build/gen_plane out/in_plane.raw
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#define W 256
#define H 192

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "out/in_plane.raw";
    FILE       *f   = fopen(out, "wb");
    int         x, y;

    if (!f) {
        perror(out);
        return 1;
    }
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            double dx   = (x - 96.0) / 70.0;
            double dy   = (y - 80.0) / 55.0;
            double blob = 200.0 * exp(-(dx * dx + dy * dy));
            double ramp = 30.0 + 40.0 * (double)x / W + 20.0 * (double)y / H;
            double v    = blob + ramp;
            uint16_t u;

            if (v < 0.0)
                v = 0.0;
            if (v > 255.0)
                v = 255.0;
            u = (uint16_t)(v + 0.5);
            fwrite(&u, sizeof u, 1, f);
        }
    }
    fclose(f);
    printf("wrote %s (%d uint16)\n", out, W * H);
    return 0;
}
