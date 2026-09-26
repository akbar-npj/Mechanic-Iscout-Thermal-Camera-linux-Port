/*
 * compare_test.c — unit tests for compare.c.
 *
 * The risks worth testing are the ones that would put a wrong number or a
 * wrong pixel on screen rather than crash: the difference grid's hot pixel
 * not landing where the two grids differ, a NaN in one plane leaking into
 * the stats as a bogus 0, the threshold count using >= instead of >, and
 * the blend rounding disagreeing with fusion.c's DYT_FUSION_BLEND.
 *
 * The fixture is a pair of 8x4 grids: a ramp, and the same ramp with one
 * pixel lifted, so every difference is zero except at that one pixel.
 *
 * build:  via the Makefile (make check)
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "compare.h"

static int fails;

static void ok(const char *what) { printf("  ok   %s\n", what); }

static void fail(const char *what, const char *detail)
{
    printf("  FAIL %-46s %s\n", what, detail);
    fails++;
}

static int near(float a, float b, float eps) { return fabsf(a - b) <= eps; }

static void intcheck(const char *what, long got, long want)
{
    if (got == want) {
        ok(what);
    } else {
        char d[96];
        snprintf(d, sizeof d, "got %ld want %ld", got, want);
        fail(what, d);
    }
}

static void floatcheck(const char *what, float got, float want, float eps)
{
    if (near(got, want, eps)) {
        ok(what);
    } else {
        char d[96];
        snprintf(d, sizeof d, "got %.4f want %.4f", (double)got, (double)want);
        fail(what, d);
    }
}

static void nancheck(const char *what, float got)
{
    if (isnan(got))
        ok(what);
    else {
        char d[96];
        snprintf(d, sizeof d, "got %.4f want NaN", (double)got);
        fail(what, d);
    }
}

/* --- fixture ------------------------------------------------------------ */

#define W 8
#define H 4
#define NPIX (W * H)

static void build_ramp(float *t)
{
    int x, y;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            t[y * W + x] = 10.0f + (float)(y * W + x);   /* 10 .. 41 */
}

/* The reference is the ramp with pixel (3,2) — index 19 — lifted by 5 C. */
#define HOT_X 3
#define HOT_Y 2
#define HOT_IDX (HOT_Y * W + HOT_X)
#define HOT_DELTA 5.0f

static void build_ref(float *t)
{
    build_ramp(t);
    t[HOT_IDX] += HOT_DELTA;
}

/* --- difference grid ---------------------------------------------------- */

static void test_diff(void)
{
    float a[NPIX], b[NPIX], d[NPIX];
    int   i;

    build_ramp(a);
    build_ref(b);

    intcheck("diff: ok", dyt_compare_diff(a, b, W, H, d), 0);

    /* Every pixel agrees except the hot one, which is -HOT_DELTA (a-b). */
    for (i = 0; i < NPIX; i++) {
        if (i == HOT_IDX)
            continue;
        floatcheck("diff: agreeing pixel is 0", d[i], 0.0f, 1e-6f);
        break;                  /* one check is enough; the rest are the same */
    }
    floatcheck("diff: hot pixel is -delta", d[HOT_IDX], -HOT_DELTA, 1e-6f);

    /* Swapping the inputs negates the difference. */
    intcheck("diff: swapped ok", dyt_compare_diff(b, a, W, H, d), 0);
    floatcheck("diff: swapped hot is +delta", d[HOT_IDX], HOT_DELTA, 1e-6f);

    /* A NaN in one plane propagates to that pixel of the diff only. */
    build_ramp(a);
    build_ref(b);
    a[5] = NAN;
    intcheck("diff: NaN pair ok", dyt_compare_diff(a, b, W, H, d), 0);
    nancheck("diff: NaN pixel is NaN", d[5]);
    floatcheck("diff: NaN did not touch hot pixel", d[HOT_IDX], -HOT_DELTA, 1e-6f);

    intcheck("diff: NULL a rejected", dyt_compare_diff(NULL, b, W, H, d), -1);
    intcheck("diff: NULL b rejected", dyt_compare_diff(a, NULL, W, H, d), -1);
    intcheck("diff: NULL out rejected", dyt_compare_diff(a, b, W, H, NULL), -1);
}

/* --- difference stats --------------------------------------------------- */

static void test_stats(void)
{
    float a[NPIX], b[NPIX];
    dyt_compare_stats_t s;

    build_ramp(a);
    build_ref(b);

    intcheck("stats: ok",
             dyt_compare_stats(a, b, W, H, 2.0f, &s), 0);
    intcheck("stats: n == 32", s.n, 32);
    /* The diff is 0 everywhere except one -5 pixel, so min = -5, max = 0. */
    floatcheck("stats: min == -5", s.min, -HOT_DELTA, 1e-6f);
    floatcheck("stats: max == 0", s.max, 0.0f, 1e-6f);
    /* mean = -5/32 */
    floatcheck("stats: mean == -5/32", s.mean, -HOT_DELTA / (float)NPIX, 1e-6f);
    floatcheck("stats: max_abs == 5", s.max_abs, HOT_DELTA, 1e-6f);
    intcheck("stats: max_abs_x == 3", s.max_abs_x, HOT_X);
    intcheck("stats: max_abs_y == 2", s.max_abs_y, HOT_Y);
    /* threshold 2.0 > test: only the |5| pixel exceeds it. */
    intcheck("stats: beyond == 1", s.beyond, 1);

    /* Swapping the inputs flips the sign of min/max/mean but not max_abs or
     * the location, which is the mutation-verify the plan calls for. */
    intcheck("stats: swapped ok",
             dyt_compare_stats(b, a, W, H, 2.0f, &s), 0);
    floatcheck("stats: swapped min == 0", s.min, 0.0f, 1e-6f);
    floatcheck("stats: swapped max == 5", s.max, HOT_DELTA, 1e-6f);
    floatcheck("stats: swapped mean == +5/32", s.mean, HOT_DELTA / (float)NPIX, 1e-6f);
    floatcheck("stats: swapped max_abs unchanged", s.max_abs, HOT_DELTA, 1e-6f);
    intcheck("stats: swapped max_abs_x unchanged", s.max_abs_x, HOT_X);
    intcheck("stats: swapped max_abs_y unchanged", s.max_abs_y, HOT_Y);
    intcheck("stats: swapped beyond unchanged", s.beyond, 1);

    /* Threshold of 0 excludes exact matches (strict >). */
    intcheck("stats: thresh 0 excludes matches",
             dyt_compare_stats(a, b, W, H, 0.0f, &s), 0);
    intcheck("stats: thresh 0 beyond == 1", s.beyond, 1);

    /* Threshold above the largest delta counts nothing. */
    intcheck("stats: thresh too high beyond == 0",
             dyt_compare_stats(a, b, W, H, 100.0f, &s), 0);
    intcheck("stats: thresh too high beyond == 0 (val)", s.beyond, 0);

    /* Two differing pixels: the second hot pixel is +3 at (0,0). */
    build_ramp(a);
    build_ref(b);
    b[0] += 3.0f;
    intcheck("stats: two diffs ok",
             dyt_compare_stats(a, b, W, H, 2.0f, &s), 0);
    intcheck("stats: two diffs beyond == 2", s.beyond, 2);
    /* max_abs is 5 (the -5 pixel), not 3. */
    floatcheck("stats: two diffs max_abs == 5", s.max_abs, HOT_DELTA, 1e-6f);
    intcheck("stats: two diffs max_abs at hot pixel", s.max_abs_x, HOT_X);
    intcheck("stats: two diffs max_abs at hot pixel y", s.max_abs_y, HOT_Y);

    /* A NaN pair is skipped, not counted. */
    build_ramp(a);
    build_ref(b);
    a[0] = NAN;
    intcheck("stats: NaN skipped",
             dyt_compare_stats(a, b, W, H, 2.0f, &s), 0);
    intcheck("stats: NaN n == 31", s.n, 31);
    /* The hot pixel is still counted. */
    intcheck("stats: NaN beyond still 1", s.beyond, 1);
    floatcheck("stats: NaN max_abs still 5", s.max_abs, HOT_DELTA, 1e-6f);

    /* All-NaN: no reading at all. */
    {
        float na[NPIX], nb[NPIX];
        int i;
        for (i = 0; i < NPIX; i++) { na[i] = NAN; nb[i] = NAN; }
        intcheck("stats: all-NaN ok",
                 dyt_compare_stats(na, nb, W, H, 2.0f, &s), 0);
        intcheck("stats: all-NaN n == 0", s.n, 0);
        nancheck("stats: all-NaN min is NaN", s.min);
        nancheck("stats: all-NaN mean is NaN", s.mean);
        nancheck("stats: all-NaN max_abs is NaN", s.max_abs);
        intcheck("stats: all-NaN max_abs_x is -1", s.max_abs_x, -1);
    }

    intcheck("stats: NULL a rejected",
             dyt_compare_stats(NULL, b, W, H, 2.0f, &s), -1);
    intcheck("stats: NULL out rejected",
             dyt_compare_stats(a, b, W, H, 2.0f, NULL), -1);
}

/* --- blend -------------------------------------------------------------- */

static void blend_check(const char *what, uint8_t got, uint8_t want)
{
    if (got == want) {
        ok(what);
    } else {
        char d[96];
        snprintf(d, sizeof d, "got %u want %u", got, want);
        fail(what, d);
    }
}

static void test_blend(void)
{
    uint8_t a[NPIX * 3], b[NPIX * 3], out[NPIX * 3];
    int     i;

    /* a = 10, b = 20 per channel -> (10 + 20 + 1) / 2 = 15 (round-half-up). */
    for (i = 0; i < NPIX * 3; i++) { a[i] = 10; b[i] = 20; }
    intcheck("blend: ok", dyt_compare_blend_rgb(a, b, W, H, out), 0);
    blend_check("blend: (10+20+1)/2 == 15", out[0], 15);

    /* a = 20, b = 20 -> 20 (exact, +1 does not cross the .5 boundary). */
    for (i = 0; i < NPIX * 3; i++) { a[i] = 20; b[i] = 20; }
    intcheck("blend: equal ok", dyt_compare_blend_rgb(a, b, W, H, out), 0);
    blend_check("blend: (20+20+1)/2 == 20", out[0], 20);

    /* a = 0, b = 255 -> sat8((0+255+1)/2) = sat8(128) = 128. */
    for (i = 0; i < NPIX * 3; i++) { a[i] = 0; b[i] = 255; }
    intcheck("blend: extremes ok", dyt_compare_blend_rgb(a, b, W, H, out), 0);
    blend_check("blend: (0+255+1)/2 == 128", out[0], 128);

    /* a = 255, b = 255 -> 255 (clamp does not fire). */
    for (i = 0; i < NPIX * 3; i++) { a[i] = 255; b[i] = 255; }
    intcheck("blend: high ok", dyt_compare_blend_rgb(a, b, W, H, out), 0);
    blend_check("blend: (255+255+1)/2 == 255", out[0], 255);

    /* In-place blend (out aliases a) must work — fusion.c allows the same. */
    for (i = 0; i < NPIX * 3; i++) { a[i] = 10; b[i] = 20; }
    intcheck("blend: in-place ok", dyt_compare_blend_rgb(a, b, W, H, a), 0);
    blend_check("blend: in-place == 15", a[0], 15);

    intcheck("blend: NULL a rejected",
             dyt_compare_blend_rgb(NULL, b, W, H, out), -1);
    intcheck("blend: NULL out rejected",
             dyt_compare_blend_rgb(a, b, W, H, NULL), -1);
}

int main(void)
{
    printf("=== compare_test (diff / stats / blend) ===\n");

    test_diff();
    test_stats();
    test_blend();

    if (fails) {
        printf("=== %d FAILURE(S) ===\n", fails);
        return 1;
    }
    printf("=== ALL PASS ===\n");
    return 0;
}
