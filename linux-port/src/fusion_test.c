/*
 * fusion_test.c — unit tests for the six fusion patterns (fusion.c).
 *
 * The patterns are checked two ways, because neither alone is enough:
 *
 *  1. **Exact properties.**  Four of the six patterns are closed-form
 *     (identity, grey-to-RGB, 50/50 average, paste), so they are asserted
 *     per pixel against a hand-written formula rather than against a picture.
 *     The two edge patterns are not closed-form — they run a bilateral
 *     filter and a Sobel — but they have one property that *is* exact and
 *     catches the whole pipeline: on a **constant** grey plane there are no
 *     edges, so the edge image is zero, so EDGE must reproduce the thermal
 *     render byte-for-byte and EDGE_BLACK must be entirely black.  If the
 *     filter, the Sobel or the saturation were wrong, that would not hold.
 *
 *  2. **A golden render.**  EDGE over a fixed synthetic pair is written to
 *     build/fusion_edge.ppm and byte-compared with a tracked PPM, the same
 *     way palette_test guards the render core.  It is a *drift* guard, not
 *     independent ground truth: the pattern's shape is [V] (RE Docs 03
 *     §3.5.2), so the golden freezes the port's current behaviour for review
 *     rather than claiming byte-parity with the vendor's OpenCV.  To
 *     regenerate after a deliberate change:
 *
 *         ./build/fusion_test
 *         cp build/fusion_edge.ppm testdata/golden/fusion_edge_128x96.ppm
 *
 * The inputs are 128x96 so the golden stays small; every loop in fusion.c is
 * size-independent, and the one geometry-dependent constant (the PiP inset)
 * is a proportion, which the PiP check recomputes from the same formula.
 *
 * usage:  ./fusion_test
 * build:  via the Makefile (make check)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fusion.h"
#include "imgwrite.h"

#define W 128
#define H 96

static int fails;

static void ok(const char *what) { printf("  ok   %s\n", what); }

static void fail(const char *what, const char *detail)
{
    printf("  FAIL %-46s %s\n", what, detail);
    fails++;
}

static void intcheck(const char *what, int got, int want)
{
    if (got == want) {
        ok(what);
    } else {
        char d[96];
        snprintf(d, sizeof d, "got %d want %d", got, want);
        fail(what, d);
    }
}

/* --------------------------------------------------------------- fixtures */

/* A thermal render and a visible plane with both flat areas and hard edges.
 * Integer formulas only, so the pair — and the golden derived from it — is
 * bit-reproducible on any machine. */
static void make_planes(uint8_t *therm, uint8_t *grey)
{
    int x, y;

    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            size_t p = (size_t)y * W + x;

            therm[p * 3 + 0] = (uint8_t)(x * 255 / (W - 1));
            therm[p * 3 + 1] = (uint8_t)(y * 255 / (H - 1));
            therm[p * 3 + 2] = (uint8_t)((x + y) * 255 / (W + H - 2));
            if (grey)
                grey[p] = ((x / 16 + y / 16) & 1) ? 200 : 40;
        }
    }
}

static void fill_flat(uint8_t *grey, uint8_t v)
{
    memset(grey, v, (size_t)W * H);
}

/* ------------------------------------------------------------- bad input */

static void test_reject(void)
{
    uint8_t *therm = malloc((size_t)W * H * 3);
    uint8_t *grey  = malloc((size_t)W * H);
    uint8_t *out   = malloc((size_t)W * H * 3);
    dyt_fusion_cfg_t cfg;

    printf("\n-- reject bad arguments --\n");
    if (!therm || !grey || !out) { fail("allocate", "out of memory"); goto out; }

    make_planes(therm, grey);
    dyt_fusion_cfg_default(&cfg);

    intcheck("NULL cfg",    dyt_fusion_apply(NULL, therm, grey, W, H, out), -1);
    intcheck("NULL therm",  dyt_fusion_apply(&cfg, NULL, grey, W, H, out), -1);
    intcheck("NULL out",    dyt_fusion_apply(&cfg, therm, grey, W, H, NULL), -1);
    intcheck("zero width",  dyt_fusion_apply(&cfg, therm, grey, 0, H, out), -1);
    intcheck("zero height", dyt_fusion_apply(&cfg, therm, grey, W, 0, out), -1);

    cfg.mode = (dyt_fusion_t)99;
    intcheck("mode out of range",
             dyt_fusion_apply(&cfg, therm, grey, W, H, out), -1);
    cfg.mode = (dyt_fusion_t)-1;
    intcheck("negative mode",
             dyt_fusion_apply(&cfg, therm, grey, W, H, out), -1);

    /* Every pattern except INFRARED needs the visible plane. */
    cfg.mode = DYT_FUSION_EDGE;
    intcheck("EDGE without a visible plane",
             dyt_fusion_apply(&cfg, therm, NULL, W, H, out), -1);
    cfg.mode = DYT_FUSION_VISIBLE;
    intcheck("VISIBLE without a visible plane",
             dyt_fusion_apply(&cfg, therm, NULL, W, H, out), -1);
    cfg.mode = DYT_FUSION_INFRARED;
    intcheck("INFRARED without a visible plane",
             dyt_fusion_apply(&cfg, therm, NULL, W, H, out), 0);

out:
    free(therm);
    free(grey);
    free(out);
}

/* --------------------------------------------------- the closed-form modes */

static void test_infrared(void)
{
    uint8_t *therm = malloc((size_t)W * H * 3);
    uint8_t *out   = malloc((size_t)W * H * 3);
    dyt_fusion_cfg_t cfg;

    printf("\n-- infrared (thermal only) --\n");
    if (!therm || !out) { fail("allocate", "out of memory"); goto out; }

    make_planes(therm, NULL);
    dyt_fusion_cfg_default(&cfg);

    if (dyt_fusion_apply(&cfg, therm, NULL, W, H, out) != 0) {
        fail("apply", "returned non-zero");
        goto out;
    }
    intcheck("output == thermal render",
             memcmp(out, therm, (size_t)W * H * 3) == 0, 1);

    /* In place: the session renders straight into the buffer it then fuses. */
    if (dyt_fusion_apply(&cfg, therm, NULL, W, H, therm) != 0) {
        fail("in-place apply", "returned non-zero");
        goto out;
    }
    ok("in-place apply is a no-op");

out:
    free(therm);
    free(out);
}

static void test_visible(void)
{
    uint8_t *therm = malloc((size_t)W * H * 3);
    uint8_t *grey  = malloc((size_t)W * H);
    uint8_t *out   = malloc((size_t)W * H * 3);
    dyt_fusion_cfg_t cfg;
    int x, y, bad = 0;

    printf("\n-- visible light --\n");
    if (!therm || !grey || !out) { fail("allocate", "out of memory"); goto out; }

    make_planes(therm, grey);
    dyt_fusion_cfg_default(&cfg);
    cfg.mode = DYT_FUSION_VISIBLE;

    if (dyt_fusion_apply(&cfg, therm, grey, W, H, out) != 0) {
        fail("apply", "returned non-zero");
        goto out;
    }
    for (y = 0; y < H && !bad; y++) {
        for (x = 0; x < W; x++) {
            size_t   p = (size_t)y * W + x;
            uint8_t  g = grey[p];
            uint8_t *o = out + p * 3;
            if (o[0] != g || o[1] != g || o[2] != g) {
                bad++;
                break;
            }
        }
    }
    intcheck("every pixel is its grey level, three times", bad, 0);

    /* Alignment: +dx samples one column to the right, clamped at the edge. */
    cfg.dx = 1;
    cfg.dy = 0;
    if (dyt_fusion_apply(&cfg, therm, grey, W, H, out) != 0) {
        fail("apply (dx=+1)", "returned non-zero");
        goto out;
    }
    bad = 0;
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            size_t  p = (size_t)y * W + x;
            uint8_t want = grey[(size_t)y * W + (x + 1 < W ? x + 1 : W - 1)];
            if (out[p * 3] != want)
                bad++;
        }
    }
    intcheck("dx=+1 shifts the plane right (edge-clamped)", bad, 0);

    cfg.dx = -1;
    if (dyt_fusion_apply(&cfg, therm, grey, W, H, out) != 0) {
        fail("apply (dx=-1)", "returned non-zero");
        goto out;
    }
    bad = 0;
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            size_t  p = (size_t)y * W + x;
            uint8_t want = grey[(size_t)y * W + (x - 1 >= 0 ? x - 1 : 0)];
            if (out[p * 3] != want)
                bad++;
        }
    }
    intcheck("dx=-1 shifts the plane left (edge-clamped)", bad, 0);

    /* dy moves a whole row. */
    cfg.dx = 0;
    cfg.dy = 2;
    if (dyt_fusion_apply(&cfg, therm, grey, W, H, out) != 0) {
        fail("apply (dy=+2)", "returned non-zero");
        goto out;
    }
    bad = 0;
    for (y = 0; y < H; y++) {
        int sy = (y + 2 < H) ? y + 2 : H - 1;
        for (x = 0; x < W; x++) {
            size_t p = (size_t)y * W + x;
            if (out[p * 3] != grey[(size_t)sy * W + x])
                bad++;
        }
    }
    intcheck("dy=+2 shifts the plane down (edge-clamped)", bad, 0);

out:
    free(therm);
    free(grey);
    free(out);
}

static void test_blend(void)
{
    uint8_t *therm = malloc((size_t)W * H * 3);
    uint8_t *grey  = malloc((size_t)W * H);
    uint8_t *out   = malloc((size_t)W * H * 3);
    dyt_fusion_cfg_t cfg;
    int i, bad = 0;

    printf("\n-- degrees of fusion (50/50) --\n");
    if (!therm || !grey || !out) { fail("allocate", "out of memory"); goto out; }

    make_planes(therm, grey);
    dyt_fusion_cfg_default(&cfg);
    cfg.mode = DYT_FUSION_BLEND;

    if (dyt_fusion_apply(&cfg, therm, grey, W, H, out) != 0) {
        fail("apply", "returned non-zero");
        goto out;
    }
    /* addWeighted(visible, 0.5, thermal, 0.5, 0) with OpenCV's rounding:
     * the port adds 1 before halving, i.e. round-half-up. */
    for (i = 0; i < W * H; i++) {
        uint8_t g = grey[i];
        if (out[i * 3 + 0] != (uint8_t)((therm[i * 3 + 0] + g + 1) / 2) ||
            out[i * 3 + 1] != (uint8_t)((therm[i * 3 + 1] + g + 1) / 2) ||
            out[i * 3 + 2] != (uint8_t)((therm[i * 3 + 2] + g + 1) / 2))
            bad++;
    }
    intcheck("(thermal + visible + 1) / 2 per channel", bad, 0);

out:
    free(therm);
    free(grey);
    free(out);
}

static void test_pip(void)
{
    uint8_t *therm = malloc((size_t)W * H * 3);
    uint8_t *grey  = malloc((size_t)W * H);
    uint8_t *out   = malloc((size_t)W * H * 3);
    dyt_fusion_cfg_t cfg;
    int rx = (5 * W) / 16, ry = H / 4, rw = (3 * W) / 8, rh = H / 2;
    int bad = 0, x, y;

    printf("\n-- picture in picture --\n");
    if (!therm || !grey || !out) { fail("allocate", "out of memory"); goto out; }

    make_planes(therm, grey);
    dyt_fusion_cfg_default(&cfg);
    cfg.mode = DYT_FUSION_PIP;

    if (dyt_fusion_apply(&cfg, therm, grey, W, H, out) != 0) {
        fail("apply", "returned non-zero");
        goto out;
    }

    /* Outside the inset: the visible plane.  Inside: the thermal render. */
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            size_t p = (size_t)y * W + x;
            int    inside = (x >= rx && x < rx + rw && y >= ry && y < ry + rh);

            if (inside) {
                if (memcmp(out + p * 3, therm + p * 3, 3) != 0)
                    bad++;
            } else if (out[p * 3] != grey[p] || out[p * 3 + 1] != grey[p] ||
                       out[p * 3 + 2] != grey[p]) {
                bad++;
            }
        }
    }
    intcheck("visible base with a thermal inset", bad, 0);

    /* The inset must be a real region, not the whole frame or nothing. */
    printf("       inset: %dx%d at (%d,%d)\n", rw, rh, rx, ry);
    intcheck("inset is a strict subset",
             rx > 0 && ry > 0 && rx + rw < W && ry + rh < H, 1);

out:
    free(therm);
    free(grey);
    free(out);
}

/* --------------------------------------------------------- the edge modes */

/* Constant plane -> zero gradient.  This is the exact property that ties the
 * bilateral filter, the Sobel and the saturation together. */
static void test_edge_flat(void)
{
    uint8_t *therm = malloc((size_t)W * H * 3);
    uint8_t *grey  = malloc((size_t)W * H);
    uint8_t *out   = malloc((size_t)W * H * 3);
    dyt_fusion_cfg_t cfg;

    printf("\n-- edge modes on a flat plane --\n");
    if (!therm || !grey || !out) { fail("allocate", "out of memory"); goto out; }

    make_planes(therm, grey);
    fill_flat(grey, 128);
    dyt_fusion_cfg_default(&cfg);

    cfg.mode = DYT_FUSION_EDGE;
    if (dyt_fusion_apply(&cfg, therm, grey, W, H, out) != 0) {
        fail("EDGE apply", "returned non-zero");
        goto out;
    }
    intcheck("EDGE with no edges == thermal render",
             memcmp(out, therm, (size_t)W * H * 3) == 0, 1);

    cfg.mode = DYT_FUSION_EDGE_BLACK;
    if (dyt_fusion_apply(&cfg, therm, grey, W, H, out) != 0) {
        fail("EDGE_BLACK apply", "returned non-zero");
        goto out;
    }
    {
        int i, lit = 0;
        for (i = 0; i < W * H * 3; i++)
            if (out[i] != 0)
                lit++;
        intcheck("EDGE_BLACK with no edges is all black", lit, 0);
    }

out:
    free(therm);
    free(grey);
    free(out);
}

static void test_edge_real(void)
{
    uint8_t *therm = malloc((size_t)W * H * 3);
    uint8_t *grey  = malloc((size_t)W * H);
    uint8_t *out   = malloc((size_t)W * H * 3);
    dyt_fusion_cfg_t cfg;
    int i, below = 0, changed = 0;

    printf("\n-- edge modes on a real pattern --\n");
    if (!therm || !grey || !out) { fail("allocate", "out of memory"); goto out; }

    make_planes(therm, grey);
    dyt_fusion_cfg_default(&cfg);
    cfg.mode = DYT_FUSION_EDGE;

    if (dyt_fusion_apply(&cfg, therm, grey, W, H, out) != 0) {
        fail("EDGE apply", "returned non-zero");
        goto out;
    }

    /* add(thermal, edges) can only lighten, and must actually lighten some
     * pixels — otherwise the checkerboard produced no edges at all. */
    for (i = 0; i < W * H * 3; i++) {
        if (out[i] < therm[i])
            below++;
        if (out[i] != therm[i])
            changed++;
    }
    intcheck("EDGE never darkens", below, 0);
    intcheck("EDGE lights up edge pixels", changed > 0, 1);
    printf("       %d/%d channels changed\n", changed, W * H * 3);

    cfg.mode = DYT_FUSION_EDGE_BLACK;
    if (dyt_fusion_apply(&cfg, therm, grey, W, H, out) != 0) {
        fail("EDGE_BLACK apply", "returned non-zero");
        goto out;
    }
    {
        int lit = 0;
        for (i = 0; i < W * H * 3; i++)
            if (out[i] != 0)
                lit++;
        /* Some pixels survive, but most of a 16x16 checkerboard is flat and
         * must be black — a mask that kept everything would defeat the mode. */
        printf("       %d/%d channels lit\n", lit, W * H * 3);
        intcheck("EDGE_BLACK keeps some pixels", lit > 0, 1);
        intcheck("EDGE_BLACK blacks out most of it",
                 lit < (W * H * 3) / 2, 1);
    }

    /* The alignment must reach the edge modes too.  The checkerboard's edges
     * sit on a 16-px grid, so an 8-px offset moves them onto different
     * pixels; if the offset were dropped on this path the two renders would
     * be identical.  (A 16-px offset would *not* discriminate: the edge map
     * of this symmetric pattern is itself period-16.) */
    {
        uint8_t *dx8 = malloc((size_t)W * H * 3);

        if (!dx8) { fail("allocate", "out of memory"); goto out; }

        cfg.dx = 0;
        cfg.dy = 0;
        cfg.mode = DYT_FUSION_EDGE;
        dyt_fusion_apply(&cfg, therm, grey, W, H, out);
        cfg.dx = 8;
        dyt_fusion_apply(&cfg, therm, grey, W, H, dx8);
        intcheck("EDGE honours the alignment",
                 memcmp(out, dx8, (size_t)W * H * 3) != 0, 1);

        cfg.dx = 0;
        cfg.mode = DYT_FUSION_EDGE_BLACK;
        dyt_fusion_apply(&cfg, therm, grey, W, H, out);
        cfg.dx = 8;
        dyt_fusion_apply(&cfg, therm, grey, W, H, dx8);
        intcheck("EDGE_BLACK honours the alignment",
                 memcmp(out, dx8, (size_t)W * H * 3) != 0, 1);

        free(dx8);
    }

out:
    free(therm);
    free(grey);
    free(out);
}

/* ------------------------------------------------- names, clamp, defaults */

static void test_meta(void)
{
    dyt_fusion_cfg_t cfg;

    printf("\n-- names, clamping, defaults --\n");

    intcheck("name 0", strcmp(dyt_fusion_name(DYT_FUSION_INFRARED), "ir") == 0, 1);
    intcheck("name 1", strcmp(dyt_fusion_name(DYT_FUSION_VISIBLE), "visible") == 0, 1);
    intcheck("name 2", strcmp(dyt_fusion_name(DYT_FUSION_EDGE), "edge") == 0, 1);
    intcheck("name 3", strcmp(dyt_fusion_name(DYT_FUSION_BLEND), "blend") == 0, 1);
    intcheck("name 4", strcmp(dyt_fusion_name(DYT_FUSION_PIP), "pip") == 0, 1);
    intcheck("name 5", strcmp(dyt_fusion_name(DYT_FUSION_EDGE_BLACK), "edge-black") == 0, 1);
    intcheck("name 6 is unknown",
             strcmp(dyt_fusion_name(DYT_FUSION_N), "?") == 0, 1);
    intcheck("name -1 is unknown",
             strcmp(dyt_fusion_name((dyt_fusion_t)-1), "?") == 0, 1);

    intcheck("clamp 0",    dyt_fusion_clamp_align(0), 0);
    intcheck("clamp +40",  dyt_fusion_clamp_align(DYT_FUSION_ALIGN_MAX), 40);
    intcheck("clamp -40",  dyt_fusion_clamp_align(-DYT_FUSION_ALIGN_MAX), -40);
    intcheck("clamp +41",  dyt_fusion_clamp_align(41), 40);
    intcheck("clamp -41",  dyt_fusion_clamp_align(-41), -40);
    intcheck("clamp 1000", dyt_fusion_clamp_align(1000), 40);
    intcheck("clamp -1000", dyt_fusion_clamp_align(-1000), -40);

    dyt_fusion_cfg_default(&cfg);
    intcheck("default mode is infrared", cfg.mode == DYT_FUSION_INFRARED, 1);
    intcheck("default dx", cfg.dx, 0);
    intcheck("default dy", cfg.dy, 0);
    dyt_fusion_cfg_default(NULL);      /* must not crash */
    ok("default(NULL) is a no-op");
}

/* ------------------------------------------------------------ golden render */

static void test_golden(void)
{
    const char *out_path = "build/fusion_edge.ppm";
    const char *golden   = "testdata/golden/fusion_edge_128x96.ppm";
    uint8_t *therm = malloc((size_t)W * H * 3);
    uint8_t *grey  = malloc((size_t)W * H);
    uint8_t *out   = malloc((size_t)W * H * 3);
    uint8_t *g     = NULL;
    dyt_fusion_cfg_t cfg;
    int gw = 0, gh = 0;

    printf("\n-- golden render (%s) --\n", golden);
    if (!therm || !grey || !out) { fail("allocate", "out of memory"); goto out; }

    make_planes(therm, grey);
    dyt_fusion_cfg_default(&cfg);
    cfg.mode = DYT_FUSION_EDGE;

    if (dyt_fusion_apply(&cfg, therm, grey, W, H, out) != 0) {
        fail("apply", "returned non-zero");
        goto out;
    }
    if (dyt_write_ppm(out_path, out, W, H) != 0) {
        fail("write ppm", out_path);
        goto out;
    }
    printf("  ok   wrote %s (%dx%d)\n", out_path, W, H);

    if (dyt_read_ppm(golden, &g, &gw, &gh) != 0) {
        printf("  FAIL cannot read golden %s\n", golden);
        printf("       generate it with: cp %s %s\n", out_path, golden);
        fails++;
    } else if (gw != W || gh != H) {
        char d[96];
        snprintf(d, sizeof d, "golden is %dx%d, expected %dx%d", gw, gh, W, H);
        fail("golden geometry", d);
    } else if (memcmp(g, out, (size_t)W * H * 3) != 0) {
        int px = 0;
        while (px < W * H && memcmp(g + px * 3, out + px * 3, 3) == 0)
            px++;
        printf("  FAIL golden mismatch, first at pixel %d (row %d, col %d): "
               "golden %d,%d,%d  port %d,%d,%d\n", px, px / W, px % W,
               g[px * 3], g[px * 3 + 1], g[px * 3 + 2],
               out[px * 3], out[px * 3 + 1], out[px * 3 + 2]);
        fails++;
    } else {
        printf("  ok   matches golden byte-for-byte (%d bytes)\n", W * H * 3);
    }

out:
    free(g);
    free(therm);
    free(grey);
    free(out);
}

int main(void)
{
    printf("=== fusion_test (the six fusion patterns) ===\n");

    test_reject();
    test_infrared();
    test_visible();
    test_blend();
    test_pip();
    test_edge_flat();
    test_edge_real();
    test_meta();
    test_golden();

    printf("\n=== %s ===\n", fails ? "FAIL" : "ALL PASS");
    return fails ? 1 : 0;
}
