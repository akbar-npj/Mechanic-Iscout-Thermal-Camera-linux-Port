/*
 * palette_test.c — unit tests for the palette / render core (palette.c).
 *
 * Two kinds of check, deliberately:
 *
 *  1. Independent assertions.  The iron-red ramp is compared against the
 *     sampled values recorded in RE Docs 06 §1.2, which came from the vendor
 *     binary — not from this code.  Structural properties (row-major layout,
 *     clamping, NaN, degenerate ranges) are checked on small synthetic
 *     images so a stride or ordering bug cannot hide.
 *
 *  2. A golden render.  A real frozen frame (testdata/mode1000_256x192.raw)
 *     is rendered at a fixed range and byte-compared against a tracked PPM,
 *     the same way `dumpframe --check` guards the thermometry.  This catches
 *     drift on real data.  To regenerate after a deliberate change:
 *
 *         ./build/palette_test palettes testdata/mode1000_256x192.raw \
 *             testdata/golden/mode1000_iron_30-31.2.ppm
 *         cp build/palette_render.ppm testdata/golden/mode1000_iron_30-31.2.ppm
 *
 * usage:  ./palette_test <palettes-dir> <frame.raw> <golden.ppm>
 * build:  via the Makefile (make palette-test)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgwrite.h"
#include "palette.h"

static int fails;

static const char *const vendor_files[DYT_PALETTE_BUILTIN_N] = {
    "01-iron-red.dat", "02-rainbow.dat", "03-red-hot.dat",
    "04-black-hot.dat", "05-white-hot.dat", "06-cool-blue.dat"
};

static void check_int(const char *what, int got, int want)
{
    if (got != want) {
        printf("  FAIL %-46s got %d want %d\n", what, got, want);
        fails++;
    } else {
        printf("  ok   %-46s %d\n", what, got);
    }
}

static void check_rgb(const char *what, const uint8_t *got,
                      int r, int g, int b)
{
    if (got[0] != r || got[1] != g || got[2] != b) {
        printf("  FAIL %-46s got %d,%d,%d want %d,%d,%d\n",
               what, got[0], got[1], got[2], r, g, b);
        fails++;
    } else {
        printf("  ok   %-46s %d,%d,%d\n", what, got[0], got[1], got[2]);
    }
}

static void path_join(char *out, size_t n, const char *dir, const char *file)
{
    snprintf(out, n, "%s/%s", dir, file);
}

/* ------------------------------------------- vendor palettes load + match */

static void test_vendor_palettes(const char *dir)
{
    dyt_palette_t p;
    char path[512];
    int i;

    printf("\n-- vendor palettes (%s) --\n", dir);

    for (i = 0; i < DYT_PALETTE_BUILTIN_N; i++) {
        path_join(path, sizeof path, dir, vendor_files[i]);
        if (dyt_palette_load(&p, path) != 0) {
            printf("  FAIL load %s\n", path);
            fails++;
        }
    }
    printf("  ok   loaded all %d vendor palettes\n", DYT_PALETTE_BUILTIN_N);

    /* iron-red vs RE Docs 06 §1.2 — independent ground truth, sampled every
     * 32 entries.  This is the check that would catch a wrong byte order
     * (BGR instead of RGB) or a reversed ramp. */
    path_join(path, sizeof path, dir, "01-iron-red.dat");
    if (dyt_palette_load(&p, path) != 0) {
        printf("  FAIL cannot reload iron-red\n");
        fails++;
        return;
    }
    {
        static const struct { int i, r, g, b; } want[] = {
            {   0,   0,   0,   0 }, {  32,  52,   0, 141 },
            {  64, 141,   0, 157 }, {  96, 199,  13, 136 },
            { 128, 231,  69,  24 }, { 160, 245, 120,   0 },
            { 192, 254, 178,   1 }, { 224, 255, 227,  44 },
            { 255, 255, 255, 245 },
        };
        char what[64];
        for (unsigned k = 0; k < sizeof want / sizeof want[0]; k++) {
            snprintf(what, sizeof what, "iron-red[%d]", want[k].i);
            check_rgb(what, p.rgb + want[k].i * 3, want[k].r, want[k].g, want[k].b);
        }
    }
}

/* --------------------------------------------------- reject malformed input */

static void test_reject(const char *dir)
{
    dyt_palette_t p;
    const char *bad = "/tmp/dyt_bad_palette.tmp";
    FILE *f;
    int i;

    printf("\n-- reject malformed palettes --\n");

    check_int("missing file", dyt_palette_load(&p, "/nonexistent/palette.dat"), -1);

    /* Short (767 B) and long (769 B) must both be refused: silently accepting
     * either would paint the image from whatever bytes happened to be there. */
    for (i = 0; i < 2; i++) {
        int n = DYT_PALETTE_BYTES - 1 + 2 * i;   /* 767, then 769 */
        char what[64];
        f = fopen(bad, "wb");
        if (!f) { printf("  FAIL cannot create %s\n", bad); fails++; continue; }
        for (int k = 0; k < n; k++) fputc(k & 0xff, f);
        fclose(f);
        snprintf(what, sizeof what, "%d-byte file", n);
        check_int(what, dyt_palette_load(&p, bad), -1);
    }
    remove(bad);

    (void)dir;
}

/* ---------------------------------------------------------- index mapping */

static void test_index(void)
{
    printf("\n-- palette index mapping --\n");

    check_int("t == lo",           dyt_palette_index(0.0f,   0.0f, 100.0f), 0);
    check_int("t == hi",           dyt_palette_index(100.0f, 0.0f, 100.0f), 255);
    check_int("t == mid",          dyt_palette_index(50.0f,  0.0f, 100.0f), 128);
    check_int("t below range",     dyt_palette_index(-10.0f, 0.0f, 100.0f), 0);
    check_int("t above range",     dyt_palette_index(200.0f, 0.0f, 100.0f), 255);
    check_int("degenerate hi==lo", dyt_palette_index(5.0f,   5.0f,   5.0f), 127);
    check_int("inverted range",    dyt_palette_index(5.0f,  10.0f,   0.0f), 127);
    check_int("NaN is total",      dyt_palette_index(NAN,    0.0f, 100.0f), 0);
}

/* ------------------------------------------- render: layout, clamp, NaN */

static void test_render(void)
{
    dyt_palette_t p;
    float a[4] = { 0.0f, 36.7f, 73.3f, 110.0f };
    float nan_img[3] = { NAN, 55.0f, NAN };
    uint8_t rgb_a[12], rgb_b[12], rgb_n[9];

    printf("\n-- render --\n");

    if (dyt_palette_builtin(&p, 0) != 0) { printf("  FAIL builtin\n"); fails++; return; }

    /* Row-major: the same 4-value sequence laid out 4x1 and 1x4 must produce
     * the same 4 pixels.  This is the stride/ordering check. */
    if (dyt_render_rgb(a, 4, 1, &p, 0.0f, 110.0f, rgb_a) != 0 ||
        dyt_render_rgb(a, 1, 4, &p, 0.0f, 110.0f, rgb_b) != 0) {
        printf("  FAIL render returned error\n");
        fails++;
        return;
    }
    if (memcmp(rgb_a, rgb_b, sizeof rgb_a) != 0) {
        printf("  FAIL 4x1 and 1x4 layouts disagree (stride/order bug)\n");
        fails++;
    } else {
        printf("  ok   4x1 and 1x4 render identically\n");
    }

    /* Endpoints must land exactly on the palette's first and last entries. */
    check_rgb("coldest == palette[0]",   rgb_a, p.rgb[0], p.rgb[1], p.rgb[2]);
    check_rgb("hottest == palette[255]",
              rgb_a + 9, p.rgb[255 * 3], p.rgb[255 * 3 + 1], p.rgb[255 * 3 + 2]);

    /* Clamping: out-of-range temperatures reuse the end colours. */
    {
        float hot[1] = { 1e6f }, cold[1] = { -1e6f };
        uint8_t rh[3], rc[3];
        dyt_render_rgb(hot,  1, 1, &p, 0.0f, 110.0f, rh);
        dyt_render_rgb(cold, 1, 1, &p, 0.0f, 110.0f, rc);
        check_rgb("clamp above == palette[255]", rh,
                  p.rgb[255 * 3], p.rgb[255 * 3 + 1], p.rgb[255 * 3 + 2]);
        check_rgb("clamp below == palette[0]", rc, p.rgb[0], p.rgb[1], p.rgb[2]);
    }

    /* NaN must be mid-grey, not a palette colour. */
    dyt_render_rgb(nan_img, 3, 1, &p, 0.0f, 110.0f, rgb_n);
    check_rgb("NaN pixel", rgb_n, 0x80, 0x80, 0x80);
    check_rgb("finite pixel next to NaN", rgb_n + 3,
              p.rgb[128 * 3], p.rgb[128 * 3 + 1], p.rgb[128 * 3 + 2]);

    /* Bad arguments are refused rather than crashing. */
    check_int("render NULL temps", dyt_render_rgb(NULL, 2, 2, &p, 0, 1, rgb_a), -1);
    check_int("render zero width", dyt_render_rgb(a, 0, 1, &p, 0, 1, rgb_a), -1);
}

/* -------------------------------------------------------------- min / max */

static void test_minmax(void)
{
    float v[5] = { 3.0f, 1.0f, NAN, 5.0f, -2.0f };
    float nan_all[3] = { NAN, NAN, NAN };
    float lo = 0, hi = 0;

    printf("\n-- min/max --\n");

    check_int("minmax rc", dyt_render_minmax(v, 5, &lo, &hi), 0);
    check_int("min", (int)(lo * 100), -200);
    check_int("max", (int)(hi * 100), 500);
    check_int("all-NaN", dyt_render_minmax(nan_all, 3, &lo, &hi), -1);
    check_int("empty",   dyt_render_minmax(v, 0, &lo, &hi), -1);
}

/* --------------------------------------------------------------- built-ins */

static void test_builtins(void)
{
    dyt_palette_t p;
    int i;

    printf("\n-- built-in ramps --\n");

    for (i = 0; i < DYT_PALETTE_BUILTIN_N; i++) {
        if (dyt_palette_builtin(&p, i) != 0 ||
            dyt_palette_builtin_name(i) == NULL) {
            printf("  FAIL builtin %d\n", i);
            fails++;
        }
    }
    check_int("builtin out of range", dyt_palette_builtin(&p, -1), -1);
    check_int("builtin name out of range",
              dyt_palette_builtin_name(DYT_PALETTE_BUILTIN_N) == NULL ? -1 : 0, -1);

    /* The iron built-in is anchored on the vendor values, so its endpoints
     * must match the real palette exactly. */
    if (dyt_palette_builtin(&p, 0) == 0) {
        check_rgb("builtin iron[0]",   p.rgb, 0, 0, 0);
        check_rgb("builtin iron[255]", p.rgb + 255 * 3, 255, 255, 245);
    }
}

/* ------------------------------------------------------------ golden render */

static void test_golden(const char *frame_path, const char *golden_path,
                        const char *dir)
{
    dyt_palette_t p;
    char ppath[512];
    FILE *f;
    long sz;
    uint16_t *raw = NULL;
    float *temps = NULL;
    uint8_t *rgb = NULL;
    const float LO = 30.0f, HI = 31.2f;   /* brackets the frozen frame */
    int w = 256, h = 192, k;
    size_t n;

    printf("\n-- golden render (%s) --\n", golden_path);

    path_join(ppath, sizeof ppath, dir, "01-iron-red.dat");
    if (dyt_palette_load(&p, ppath) != 0) {
        printf("  FAIL cannot load iron-red\n");
        fails++;
        return;
    }

    f = fopen(frame_path, "rb");
    if (!f) { printf("  FAIL cannot open %s\n", frame_path); fails++; return; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz != (long)w * h * 2) {
        printf("  FAIL frame is %ld bytes, expected %d\n", sz, w * h * 2);
        fclose(f);
        fails++;
        return;
    }
    raw = malloc((size_t)sz);
    temps = malloc((size_t)w * h * sizeof(float));
    rgb = malloc((size_t)w * h * 3);
    if (!raw || !temps || !rgb) { printf("  FAIL out of memory\n"); fails++; goto out; }
    if (fread(raw, 1, (size_t)sz, f) != (size_t)sz) {
        printf("  FAIL short read\n"); fails++; goto out;
    }

    /* Mode 1000 law (RE Docs 04 §4.5.4): the raw sample is Kelvin x 64. */
    n = (size_t)w * h;
    for (k = 0; k < (int)n; k++)
        temps[k] = (float)raw[k] / 64.0f - 273.15f;

    if (dyt_render_rgb(temps, w, h, &p, LO, HI, rgb) != 0) {
        printf("  FAIL render\n"); fails++; goto out;
    }

    if (dyt_write_ppm("build/palette_render.ppm", rgb, w, h) != 0) {
        printf("  FAIL cannot write build/palette_render.ppm\n"); fails++; goto out;
    }
    printf("  ok   wrote build/palette_render.ppm (%dx%d)\n", w, h);

    {
        uint8_t *g = NULL;
        int gw = 0, gh = 0;
        if (dyt_read_ppm(golden_path, &g, &gw, &gh) != 0) {
            printf("  FAIL cannot read golden %s\n", golden_path);
            fails++;
        } else if (gw != w || gh != h) {
            printf("  FAIL golden is %dx%d, expected %dx%d\n", gw, gh, w, h);
            fails++;
        } else if (memcmp(g, rgb, (size_t)w * h * 3) != 0) {
            int px = 0;
            while (px < w * h && memcmp(g + px * 3, rgb + px * 3, 3) == 0) px++;
            printf("  FAIL golden mismatch, first at pixel %d (row %d, col %d): "
                   "golden %d,%d,%d  port %d,%d,%d\n", px, px / w, px % w,
                   g[px * 3], g[px * 3 + 1], g[px * 3 + 2],
                   rgb[px * 3], rgb[px * 3 + 1], rgb[px * 3 + 2]);
            fails++;
        } else {
            printf("  ok   matches golden byte-for-byte (%d bytes)\n", w * h * 3);
        }
        free(g);
    }

out:
    free(raw);
    free(temps);
    free(rgb);
    if (f) fclose(f);
}

int main(int argc, char **argv)
{
    const char *pdir   = argc > 1 ? argv[1] : "palettes";
    const char *frame  = argc > 2 ? argv[2] : "testdata/mode1000_256x192.raw";
    const char *golden = argc > 3 ? argv[3]
                                  : "testdata/golden/mode1000_iron_30-31.2.ppm";

    printf("=== palette_test (palette + render core) ===\n");

    test_vendor_palettes(pdir);
    test_reject(pdir);
    test_index();
    test_render();
    test_minmax();
    test_builtins();
    test_golden(frame, golden, pdir);

    printf("\n=== %s ===\n", fails ? "FAIL" : "ALL PASS");
    return fails ? 1 : 0;
}
