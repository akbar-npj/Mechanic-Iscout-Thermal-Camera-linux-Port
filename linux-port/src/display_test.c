/*
 * display_test.c — unit tests for display.c.
 *
 * The interesting risks here are not arithmetic but classification: an
 * all-NaN frame must not be read as a measurement, the filler window must
 * not swallow a real scene, and locking the range must actually hold it
 * while auto keeps fitting.  Those are the failures that would put a wrong
 * number on screen rather than crash.
 *
 * build:  via the Makefile (make check)
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "display.h"

static int fails;

static void ok(const char *what) { printf("  ok   %s\n", what); }

static void fail(const char *what, const char *detail)
{
    printf("  FAIL %-46s %s\n", what, detail);
    fails++;
}

static int near(float a, float b, float eps) { return fabsf(a - b) <= eps; }

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

/* --- frame statistics --------------------------------------------------- */

static void test_stats(void)
{
    float  ramp[100];
    float  flat[64];
    dyt_frame_stats_t st;
    int    i;

    printf("-- frame statistics --\n");

    for (i = 0; i < 100; i++) ramp[i] = (float)i;

    if (dyt_frame_stats(ramp, 100, 1, &st) != 0) {
        fail("ramp stats", "returned -1");
        return;
    }
    floatcheck("ramp min",  st.lo,   0.0f,  1e-4f);
    floatcheck("ramp max",  st.hi,  99.0f,  1e-4f);
    floatcheck("ramp mean", st.mean, 49.5f, 1e-3f);
    intcheck  ("ramp n_valid",  st.n_valid, 100);
    intcheck  ("ramp not filler", st.all_filler, 0);

    /* NaN samples are skipped, not counted and not compared. */
    ramp[10] = NAN;
    ramp[20] = NAN;
    if (dyt_frame_stats(ramp, 100, 1, &st) != 0) {
        fail("NaN-skipping stats", "returned -1");
    } else {
        intcheck("NaN skipped in count", st.n_valid, 98);
        floatcheck("NaN skipped in min", st.lo, 0.0f, 1e-4f);
        floatcheck("NaN skipped in max", st.hi, 99.0f, 1e-4f);
        /* mean over 98 finite samples: (0..99 sum 4950) - 10 - 20 = 4920 */
        floatcheck("NaN skipped in mean", st.mean, 4920.0f / 98.0f, 1e-3f);
    }

    /* All-NaN must classify as filler (nothing to measure), not as a scene. */
    for (i = 0; i < 64; i++) flat[i] = NAN;
    if (dyt_frame_stats(flat, 64, 1, &st) != 0) {
        fail("all-NaN stats", "returned -1");
    } else {
        intcheck("all-NaN n_valid", st.n_valid, 0);
        intcheck("all-NaN is filler", st.all_filler, 1);
    }

    /* The start-up filler: every sample at 0x8000. */
    for (i = 0; i < 64; i++) flat[i] = DYT_FILLER_C;
    if (dyt_frame_stats(flat, 64, 1, &st) != 0) {
        fail("filler stats", "returned -1");
    } else {
        intcheck("flat filler is filler", st.all_filler, 1);
    }

    /* Clearly inside the window still counts as filler... */
    for (i = 0; i < 64; i++) flat[i] = DYT_FILLER_C + 5.0f;
    dyt_frame_stats(flat, 64, 1, &st);
    intcheck("filler+5 is filler", st.all_filler, 1);

    /* ...and one real sample clears it, regardless of position. */
    for (i = 0; i < 64; i++) flat[i] = DYT_FILLER_C;
    flat[63] = 30.0f;
    dyt_frame_stats(flat, 64, 1, &st);
    intcheck("one real sample clears filler", st.all_filler, 0);
    floatcheck("filler frame min is the real sample", st.lo, 30.0f, 1e-4f);

    /* Clearly outside the window is not filler. */
    for (i = 0; i < 64; i++) flat[i] = DYT_FILLER_C + 20.0f;
    dyt_frame_stats(flat, 64, 1, &st);
    intcheck("filler+20 is not filler", st.all_filler, 0);

    /* A room-temperature scene must never look like filler. */
    for (i = 0; i < 64; i++) flat[i] = 28.0f + (float)(i % 7);
    dyt_frame_stats(flat, 64, 1, &st);
    intcheck("room scene is not filler", st.all_filler, 0);

    /* Extrema coordinates: a 4x3 image with known hottest/coldest pixels.
     * Row-major, so index = y*w + x. */
    {
        float img[12];
        int k;
        for (k = 0; k < 12; k++) img[k] = 20.0f;
        img[1 * 4 + 2] = 50.0f;    /* hot  at (2,1) */
        img[2 * 4 + 0] =  5.0f;    /* cold at (0,2) */
        if (dyt_frame_stats(img, 4, 3, &st) != 0) {
            fail("2-D extrema stats", "returned -1");
        } else {
            intcheck("hot_x",  st.hot_x,  2);
            intcheck("hot_y",  st.hot_y,  1);
            intcheck("cold_x", st.cold_x, 0);
            intcheck("cold_y", st.cold_y, 2);
            floatcheck("2-D min", st.lo,  5.0f, 1e-4f);
            floatcheck("2-D max", st.hi, 50.0f, 1e-4f);
        }
    }

    /* With no finite samples the coordinates are -1, not 0 — a marker must
     * never be drawn at the origin of an empty frame. */
    {
        float nan_img[6];
        int k;
        for (k = 0; k < 6; k++) nan_img[k] = NAN;
        dyt_frame_stats(nan_img, 3, 2, &st);
        if (st.hot_x == -1 && st.hot_y == -1 &&
            st.cold_x == -1 && st.cold_y == -1)
            ok("no extrema on an all-NaN frame");
        else
            fail("all-NaN extrema", "expected -1");
    }

    if (dyt_frame_stats(NULL, 64, 1, &st) < 0 &&
        dyt_frame_stats(flat, 0, 1, &st) < 0 &&
        dyt_frame_stats(flat, 64, 0, &st) < 0 &&
        dyt_frame_stats(flat, 64, 1, NULL) < 0)
        ok("bad arguments return -1");
    else
        fail("bad arguments", "returned success");
}

/* --- range state -------------------------------------------------------- */

static void stats_of(float v, dyt_frame_stats_t *st)
{
    float one = v;
    dyt_frame_stats(&one, 1, 1, st);
}

static void test_range(void)
{
    dyt_display_t d;
    dyt_frame_stats_t a, b, empty;

    printf("-- range state --\n");

    dyt_display_init(&d, 0.0f, 0.0f, 0);
    intcheck("init defaults to AUTO", d.mode == DYT_RANGE_AUTO, 1);

    stats_of(30.0f, &a);   a.hi = 40.0f;
    dyt_display_update(&d, &a);
    floatcheck("auto adopts frame min", d.lo, 30.0f, 1e-4f);
    floatcheck("auto adopts frame max", d.hi, 40.0f, 1e-4f);

    /* A different frame keeps re-fitting while AUTO. */
    stats_of(10.0f, &b);   b.hi = 90.0f;
    dyt_display_update(&d, &b);
    floatcheck("auto refits min", d.lo, 10.0f, 1e-4f);
    floatcheck("auto refits max", d.hi, 90.0f, 1e-4f);

    /* Toggling to FIXED latches the current range and holds it. */
    dyt_display_toggle_mode(&d);
    intcheck("toggle -> FIXED", d.mode == DYT_RANGE_FIXED, 1);
    floatcheck("locked latched lo", d.lo, 10.0f, 1e-4f);
    floatcheck("locked latched hi", d.hi, 90.0f, 1e-4f);
    dyt_display_update(&d, &a);
    floatcheck("locked ignores new frame lo", d.lo, 10.0f, 1e-4f);
    floatcheck("locked ignores new frame hi", d.hi, 90.0f, 1e-4f);

    /* Back to AUTO resumes fitting. */
    dyt_display_toggle_mode(&d);
    intcheck("toggle -> AUTO", d.mode == DYT_RANGE_AUTO, 1);
    dyt_display_update(&d, &a);
    floatcheck("auto resumes fitting", d.lo, 30.0f, 1e-4f);

    /* A flat frame is widened so the renderer has a non-zero span. */
    stats_of(30.0f, &a);
    dyt_display_update(&d, &a);
    floatcheck("flat frame widened lo", d.lo, 29.0f, 1e-4f);
    floatcheck("flat frame widened hi", d.hi, 31.0f, 1e-4f);

    /* An empty frame (no finite samples) leaves the last range alone. */
    memset(&empty, 0, sizeof empty);
    empty.n_valid = 0;
    float before_lo = d.lo, before_hi = d.hi;
    dyt_display_update(&d, &empty);
    floatcheck("empty frame keeps lo", d.lo, before_lo, 1e-4f);
    floatcheck("empty frame keeps hi", d.hi, before_hi, 1e-4f);

    /* set_fixed pins and locks; a degenerate pair is widened. */
    dyt_display_set_fixed(&d, 20.0f, 40.0f);
    intcheck("set_fixed locks", d.mode == DYT_RANGE_FIXED, 1);
    floatcheck("set_fixed lo", d.lo, 20.0f, 1e-4f);
    floatcheck("set_fixed hi", d.hi, 40.0f, 1e-4f);

    dyt_display_set_fixed(&d, 5.0f, 5.0f);
    floatcheck("degenerate fixed widened lo", d.lo, 4.0f, 1e-4f);
    floatcheck("degenerate fixed widened hi", d.hi, 6.0f, 1e-4f);

    /* A caller-supplied inverted pair must be swapped, not widened in place —
     * widening (40,20) by ±1 would leave it still inverted. */
    dyt_display_set_fixed(&d, 40.0f, 20.0f);
    floatcheck("inverted pair swaps lo", d.lo, 20.0f, 1e-4f);
    floatcheck("inverted pair swaps hi", d.hi, 40.0f, 1e-4f);

    /* A non-finite bound falls back to a usable span rather than poisoning
     * every comparison downstream. */
    dyt_display_set_fixed(&d, NAN, NAN);
    if (d.hi > d.lo && isfinite(d.lo) && isfinite(d.hi))
        ok("NaN range falls back to a finite span");
    else
        fail("NaN range", "not finite/spanned");
}

/* --- palette selection -------------------------------------------------- */

static void test_palette(void)
{
    dyt_display_t d;

    printf("-- palette selection --\n");

    dyt_display_init(&d, 0.0f, 0.0f, 0);
    d.palette_n = 27;

    dyt_display_set_palette(&d, 5);
    intcheck("set_palette", d.palette, 5);
    dyt_display_set_palette(&d, -3);
    intcheck("set_palette clamps low", d.palette, 0);
    dyt_display_set_palette(&d, 99);
    intcheck("set_palette clamps high", d.palette, 26);

    dyt_display_set_palette(&d, 26);
    dyt_display_cycle_palette(&d, 1);
    intcheck("cycle wraps forward", d.palette, 0);
    dyt_display_cycle_palette(&d, -1);
    intcheck("cycle wraps backward", d.palette, 26);
    dyt_display_cycle_palette(&d, -1);
    intcheck("cycle steps backward", d.palette, 25);
}

/* --- mirror and zoom ---------------------------------------------------- */

static void mapcheck(const char *what, const dyt_view_transform_t *t,
                     int sw, int sh, int ox, int oy, int want_sx, int want_sy)
{
    int dw, dh, sx = -1, sy = -1, rc;

    dyt_view_transform_size(t, sw, sh, &dw, &dh);
    rc = dyt_view_transform_map(t, sw, sh, dw, dh, ox, oy, &sx, &sy);

    if (rc != 0 || sx != want_sx || sy != want_sy) {
        char d[96];
        snprintf(d, sizeof d, "rc=%d got (%d,%d) want (%d,%d)",
                 rc, sx, sy, want_sx, want_sy);
        fail(what, d);
    } else {
        ok(what);
    }
}

static void test_transform(void)
{
    dyt_view_transform_t t;
    int dw = 0, dh = 0, i;

    printf("-- mirror and zoom --\n");

    dyt_view_transform_init(&t);
    intcheck("init zoom", t.zoom, DYT_ZOOM_MIN);
    intcheck("init no flip_h", t.flip_h, 0);
    intcheck("init no flip_v", t.flip_v, 0);

    dyt_view_transform_size(&t, 4, 3, &dw, &dh);
    intcheck("identity width", dw, 4);
    intcheck("identity height", dh, 3);

    mapcheck("identity top-left",     &t, 4, 3, 0, 0, 0, 0);
    mapcheck("identity bottom-right", &t, 4, 3, 3, 2, 3, 2);

    /* Zoom clamps at both ends. */
    for (i = 0; i < 20; i++) dyt_view_transform_zoom(&t, 1);
    intcheck("zoom clamps at max", t.zoom, DYT_ZOOM_MAX);
    for (i = 0; i < 20; i++) dyt_view_transform_zoom(&t, -1);
    intcheck("zoom clamps at min", t.zoom, DYT_ZOOM_MIN);

    /* Zoom 2: each source pixel covers a 2x2 block. */
    dyt_view_transform_zoom(&t, 1);
    dyt_view_transform_size(&t, 4, 3, &dw, &dh);
    intcheck("zoom 2 width", dw, 8);
    intcheck("zoom 2 height", dh, 6);
    mapcheck("zoom 2 origin",     &t, 4, 3, 0, 0, 0, 0);
    mapcheck("zoom 2 in-block",   &t, 4, 3, 1, 1, 0, 0);
    mapcheck("zoom 2 next block", &t, 4, 3, 2, 2, 1, 1);
    mapcheck("zoom 2 far corner", &t, 4, 3, 7, 5, 3, 2);

    /* Horizontal mirror: output x=0 reads source x=w-1. */
    dyt_view_transform_init(&t);
    dyt_view_transform_toggle_flip_h(&t);
    intcheck("flip_h set", t.flip_h, 1);
    mapcheck("flip_h left edge",  &t, 4, 3, 0, 0, 3, 0);
    mapcheck("flip_h right edge", &t, 4, 3, 3, 0, 0, 0);
    mapcheck("flip_h y untouched", &t, 4, 3, 0, 2, 3, 2);
    dyt_view_transform_toggle_flip_h(&t);
    intcheck("flip_h clears", t.flip_h, 0);

    /* Vertical mirror. */
    dyt_view_transform_toggle_flip_v(&t);
    mapcheck("flip_v top edge",    &t, 4, 3, 0, 0, 0, 2);
    mapcheck("flip_v bottom edge", &t, 4, 3, 0, 2, 0, 0);
    mapcheck("flip_v x untouched", &t, 4, 3, 3, 0, 3, 2);
    dyt_view_transform_toggle_flip_v(&t);

    /* Both mirrors compose with zoom: output origin reads the far corner. */
    dyt_view_transform_init(&t);
    dyt_view_transform_toggle_flip_h(&t);
    dyt_view_transform_toggle_flip_v(&t);
    dyt_view_transform_zoom(&t, 1);
    mapcheck("flip both + zoom origin", &t, 4, 3, 0, 0, 3, 2);
    mapcheck("flip both + zoom corner", &t, 4, 3, 7, 5, 0, 0);

    /* --- rotation ---------------------------------------------------------
     * A quarter turn swaps the axes, so a 4x3 source comes out 3x4, and the
     * source's top-left corner lands at the output's top-right: the turn is
     * clockwise, the direction the rail's Rotate button steps. */
    printf("-- rotation --\n");

    dyt_view_transform_init(&t);
    intcheck("init no rotation", t.rot, DYT_ROT_NONE);

    dyt_view_transform_rotate(&t, 90);
    intcheck("rotate 90 sets the field", t.rot, DYT_ROT_90);
    dyt_view_transform_size(&t, 4, 3, &dw, &dh);
    intcheck("rot 90 swaps the width",  dw, 3);
    intcheck("rot 90 swaps the height", dh, 4);

    mapcheck("rot 90 top-left goes to the top-right", &t, 4, 3, 2, 0, 0, 0);
    mapcheck("rot 90 top-right goes to the bottom-right", &t, 4, 3, 2, 3, 3, 0);
    mapcheck("rot 90 bottom-left goes to the top-left", &t, 4, 3, 0, 0, 0, 2);
    mapcheck("rot 90 bottom-right goes to the bottom-left", &t, 4, 3, 0, 3, 3, 2);

    /* The plan's own case: the hot pixel of a 4x2 ramp at (3,0) must come out
     * at (1,3) — a quarter turn clockwise sends the top-right corner to the
     * bottom-right — and the output must be 2x4. */
    {
        int hx = -1, hy = -1;
        dyt_view_transform_t r;

        dyt_view_transform_init(&r);
        dyt_view_transform_rotate(&r, 90);
        dyt_view_transform_size(&r, 4, 2, &dw, &dh);
        intcheck("rot 90 of a 4x2 ramp is 2x4", dw * 10 + dh, 24);
        intcheck("rot 90 puts the 4x2 ramp's hot pixel at (1,3)",
                 dyt_view_transform_project(&r, 4, 2, 2, 4, 3, 0, &hx, &hy) == 0
                     ? hx * 10 + hy : -1,
                 13);
    }

    dyt_view_transform_rotate(&t, 90);
    intcheck("rotate 90 again", t.rot, DYT_ROT_180);
    mapcheck("rot 180 top-left goes to the bottom-right", &t, 4, 3, 3, 2, 0, 0);
    mapcheck("rot 180 is its own inverse", &t, 4, 3, 0, 0, 3, 2);

    dyt_view_transform_rotate(&t, 90);
    intcheck("rotate 90 a third time", t.rot, DYT_ROT_270);
    dyt_view_transform_size(&t, 4, 3, &dw, &dh);
    intcheck("rot 270 swaps the axes too", dw * 10 + dh, 34);
    mapcheck("rot 270 top-left goes to the bottom-left", &t, 4, 3, 0, 3, 0, 0);

    dyt_view_transform_rotate(&t, 90);
    intcheck("a fourth turn is back to none", t.rot, DYT_ROT_NONE);
    mapcheck("rot 0 again", &t, 4, 3, 0, 0, 0, 0);

    /* Backwards wraps through 270 rather than going negative, and a delta that
     * is not a quarter turn is rounded rather than dropped. */
    dyt_view_transform_rotate(&t, -90);
    intcheck("a turn the other way lands on 270", t.rot, DYT_ROT_270);
    dyt_view_transform_rotate(&t, 45);
    intcheck("a 45-degree step rounds to a quarter turn", t.rot, DYT_ROT_NONE);
    dyt_view_transform_rotate(&t, 450);
    intcheck("a 450-degree step is a quarter turn plus a full one", t.rot,
             DYT_ROT_90);

    /* Rotation composes with the zoom, and with the mirrors — which are applied
     * *after* it, in output space, so a rotated picture mirrors the way it
     * looks rather than the way its source was. */
    dyt_view_transform_init(&t);
    dyt_view_transform_rotate(&t, 90);
    dyt_view_transform_zoom(&t, 1);
    dyt_view_transform_size(&t, 4, 3, &dw, &dh);
    intcheck("rot 90 + zoom 2 size", dw * 10 + dh, 68);
    mapcheck("rot 90 + zoom 2 origin", &t, 4, 3, 0, 0, 0, 2);

    dyt_view_transform_init(&t);
    dyt_view_transform_rotate(&t, 90);
    dyt_view_transform_toggle_flip_h(&t);
    mapcheck("rot 90 + flip_h mirrors the output", &t, 4, 3, 2, 0, 0, 2);

    /* project() is the inverse of map(): the centre of a magnified block maps
     * back to the pixel it came from.  This is what keeps a marker on the
     * right pixel when the image is mirrored, so it is checked over every
     * flip/rotation/zoom/sr combination rather than one example. */
    {
        dyt_view_transform_t t2;
        int fh, fv, r, z, s, sx, sy, ox, oy, bx, by, bad = 0;

        for (fh = 0; fh < 2; fh++) {
            for (fv = 0; fv < 2; fv++) {
                for (r = 0; r < 4; r++) {
                    for (z = 1; z <= 4; z++) {
                        for (s = 1; s <= 2; s++) {
                            int dw2, dh2, k;

                            dyt_view_transform_init(&t2);
                            t2.flip_h = fh;
                            t2.flip_v = fv;
                            for (k = 0; k < r; k++)
                                dyt_view_transform_rotate(&t2, 90);
                            for (k = 1; k < z; k++)
                                dyt_view_transform_zoom(&t2, 1);
                            dyt_view_transform_set_sr(&t2, s);

                            dyt_view_transform_size(&t2, 4, 3, &dw2, &dh2);

                            for (sy = 0; sy < 3; sy++) {
                                for (sx = 0; sx < 4; sx++) {
                                    if (dyt_view_transform_project(&t2, 4, 3, dw2, dh2,
                                                                   sx, sy, &ox, &oy) != 0 ||
                                        dyt_view_transform_map(&t2, 4, 3, dw2, dh2,
                                                               ox, oy, &bx, &by) != 0 ||
                                        bx != sx || by != sy)
                                        bad = 1;
                                }
                            }
                        }
                    }
                }
            }
        }
        intcheck("project/map round trip over all flip x rot x zoom x sr", bad, 0);
    }

    /* The super-resolution factor magnifies like the zoom does, so the mapping
     * divides by the product and still returns *source* pixels.  This is the
     * property the whole design rests on: a 2x render needs no arithmetic in
     * the front end, because map() already knows the factor. */
    {
        dyt_view_transform_t t3;

        printf("-- super-resolution factor --\n");

        dyt_view_transform_init(&t3);
        intcheck("init sr", t3.sr, DYT_SR_MIN);
        dyt_view_transform_set_sr(&t3, 99);
        intcheck("set_sr clamps at max", t3.sr, DYT_SR_MAX);
        dyt_view_transform_set_sr(&t3, 0);
        intcheck("set_sr clamps at min", t3.sr, DYT_SR_MIN);
        dyt_view_transform_set_sr(NULL, 2);        /* must not crash */
        ok("set_sr tolerates NULL");

        dyt_view_transform_init(&t3);
        dyt_view_transform_set_sr(&t3, 2);
        dyt_view_transform_size(&t3, 4, 3, &dw, &dh);
        intcheck("sr 2 width", dw, 8);
        intcheck("sr 2 height", dh, 6);
        mapcheck("sr 2 origin",     &t3, 4, 3, 0, 0, 0, 0);
        mapcheck("sr 2 in-block",   &t3, 4, 3, 1, 1, 0, 0);
        mapcheck("sr 2 next block", &t3, 4, 3, 2, 2, 1, 1);
        mapcheck("sr 2 far corner", &t3, 4, 3, 7, 5, 3, 2);

        /* sr and zoom compose: 2 x 2 is a 4x4 block per source pixel. */
        dyt_view_transform_init(&t3);
        dyt_view_transform_set_sr(&t3, 2);
        dyt_view_transform_zoom(&t3, 1);
        dyt_view_transform_size(&t3, 4, 3, &dw, &dh);
        intcheck("sr 2 + zoom 2 width", dw, 16);
        mapcheck("sr 2 + zoom 2 origin",     &t3, 4, 3, 0, 0, 0, 0);
        mapcheck("sr 2 + zoom 2 in-block",   &t3, 4, 3, 3, 3, 0, 0);
        mapcheck("sr 2 + zoom 2 next block", &t3, 4, 3, 4, 4, 1, 1);
        mapcheck("sr 2 + zoom 2 far corner", &t3, 4, 3, 15, 11, 3, 2);

        /* The defensive default: a transform that was never initialised has
         * sr == 0, which must read as 1 rather than divide by zero.  A plain
         * `= { 0 }` is how a future caller is most likely to get this wrong. */
        {
            dyt_view_transform_t z0;
            int dw0 = 0, dh0 = 0, sx0 = -1, sy0 = -1;

            memset(&z0, 0, sizeof z0);
            dyt_view_transform_size(&z0, 4, 3, &dw0, &dh0);
            intcheck("zero-init size is the identity",
                     dw0 == 4 && dh0 == 3, 1);
            intcheck("zero-init map is the identity",
                     dyt_view_transform_map(&z0, 4, 3, dw0, dh0, 3, 2,
                                            &sx0, &sy0) == 0 &&
                     sx0 == 3 && sy0 == 2, 1);
        }
    }

    /* project() rejects out-of-range sources and bad arguments. */
    {
        int ox, oy;
        dyt_view_transform_init(&t);
        if (dyt_view_transform_project(&t, 4, 3, 4, 3, -1, 0, &ox, &oy) < 0 &&
            dyt_view_transform_project(&t, 4, 3, 4, 3, 4, 0, &ox, &oy) < 0 &&
            dyt_view_transform_project(&t, 4, 3, 4, 3, 0, 3, &ox, &oy) < 0 &&
            dyt_view_transform_project(NULL, 4, 3, 4, 3, 0, 0, &ox, &oy) < 0)
            ok("project rejects out-of-range and bad args");
        else
            fail("project rejection", "returned success");
    }

    /* Out-of-bounds output coordinates are rejected, not wrapped. */
    dyt_view_transform_init(&t);
    dyt_view_transform_size(&t, 4, 3, &dw, &dh);
    {
        int sx, sy;
        if (dyt_view_transform_map(&t, 4, 3, dw, dh, -1, 0, &sx, &sy) < 0 &&
            dyt_view_transform_map(&t, 4, 3, dw, dh, 0, -1, &sx, &sy) < 0 &&
            dyt_view_transform_map(&t, 4, 3, dw, dh, dw, 0, &sx, &sy) < 0 &&
            dyt_view_transform_map(&t, 4, 3, dw, dh, 0, dh, &sx, &sy) < 0 &&
            dyt_view_transform_map(NULL, 4, 3, dw, dh, 0, 0, &sx, &sy) < 0 &&
            dyt_view_transform_map(&t, 0, 3, dw, dh, 0, 0, &sx, &sy) < 0)
            ok("out-of-bounds and bad args rejected");
        else
            fail("out-of-bounds rejection", "returned success");
    }
}

/* --- grayscale plateau / sigma mapping ---------------------------------- */

/* A 100-pixel ramp, grey[i] = i, so the CDF is exactly acc[L] = L+1 (and 100
 * for every L >= 99).  Every percentile below is then checkable by hand. */
static void test_gray_plateau(void)
{
    uint8_t grey[100];
    int     acc[DYT_GRAY_LEVELS];
    long    total = 0;
    int     i, lo = -1, hi = -1;
    dyt_gray_params_t p;

    for (i = 0; i < 100; i++)
        grey[i] = (uint8_t)i;

    intcheck("gray: cdf build", dyt_gray_cdf_build(grey, 100, acc, &total), 0);
    intcheck("gray: cdf total == 100", (int)total, 100);
    intcheck("gray: cdf at 0", acc[0], 1);
    intcheck("gray: cdf at 9", acc[9], 10);
    intcheck("gray: cdf at 99", acc[99], 100);
    intcheck("gray: cdf saturates above the data", acc[200], 100);

    /* 10th/90th percentile window: 10% of pixels are <= 9, 90% are <= 89. */
    intcheck("gray: plateau window",
             dyt_gray_plateau_window(acc, total, 10, 90, &lo, &hi), 0);
    intcheck("gray: plateau lo == 9", lo, 9);
    intcheck("gray: plateau hi == 89", hi, 89);

    /* Full range. */
    intcheck("gray: full window",
             dyt_gray_plateau_window(acc, total, 0, 100, &lo, &hi), 0);
    intcheck("gray: full lo == 0", lo, 0);
    intcheck("gray: full hi == 99", hi, 99);

    /* A flat plane must still yield hi > lo, or the stretch divides by zero. */
    {
        uint8_t flat[16];
        int     facc[DYT_GRAY_LEVELS];
        long    ftotal = 0;
        int     j;
        for (j = 0; j < 16; j++) flat[j] = 0;
        dyt_gray_cdf_build(flat, 16, facc, &ftotal);
        intcheck("gray: flat plane window ok",
                 dyt_gray_plateau_window(facc, ftotal, 0, 100, &lo, &hi), 0);
        if (hi > lo)
            ok("gray: flat plane widened so hi > lo");
        else
            fail("gray: flat plane widening", "hi <= lo");
    }

    intcheck("gray: window bad args",
             dyt_gray_plateau_window(NULL, 100, 0, 100, &lo, &hi), -1);
    intcheck("gray: window empty plane",
             dyt_gray_plateau_window(acc, 0, 0, 100, &lo, &hi), -1);
    intcheck("gray: cdf bad args",
             dyt_gray_cdf_build(NULL, 100, acc, &total), -1);

    /* Defaults. */
    dyt_gray_params_default(&p);
    intcheck("gray: default lo", p.lo, 0);
    intcheck("gray: default hi", p.hi, DYT_GRAY_LEVELS - 1);
    intcheck("gray: default detail clamp", p.detail_clamp,
             DYT_GRAY_DETAIL_CLAMP);

    /* The linear term maps the window onto [64,192].  With an even Deta the
     * endpoints land exactly on 64 and 192. */
    p.lo = 0; p.hi = 254;
    floatcheck("gray: linear at lo == 64", dyt_gray_linear(0, &p), 64.0f, 1e-6f);
    floatcheck("gray: linear at hi == 192", dyt_gray_linear(254, &p), 192.0f,
               1e-6f);
    floatcheck("gray: linear at mid == 128", dyt_gray_linear(127, &p), 128.0f,
               1e-6f);
    floatcheck("gray: linear below lo clamps to 64", dyt_gray_linear(-5, &p),
               64.0f, 1e-6f);
    floatcheck("gray: linear above hi clamps to 192", dyt_gray_linear(999, &p),
               192.0f, 1e-6f);
    /* An odd Deta truncates toward zero on the low side, exactly as the
     * kernel's integer division does — 0 maps to 65, not 64. */
    p.lo = 0; p.hi = 255;
    floatcheck("gray: odd Deta truncates as the kernel does",
               dyt_gray_linear(0, &p), 65.0f, 1e-6f);
    floatcheck("gray: odd Deta hi == 192", dyt_gray_linear(255, &p), 192.0f,
               1e-6f);

    /* The plateau term is the CDF scaled to 0..255.  acc[0] == 1 because a
     * single pixel of the ramp is 0, and acc[50] == 51. */
    floatcheck("gray: plateau at 0 == 2", dyt_gray_plateau(0, acc, total, &p),
               2.0f, 1e-6f);
    floatcheck("gray: plateau at 50 == 130",
               dyt_gray_plateau(50, acc, total, &p), 130.0f, 1e-6f);
    floatcheck("gray: plateau at 99 == 255",
               dyt_gray_plateau(99, acc, total, &p), 255.0f, 1e-6f);
    floatcheck("gray: plateau with no plane is 0",
               dyt_gray_plateau(0, acc, 0, &p), 0.0f, 1e-6f);

    /* The blend: an even split of the two terms, with no detail.
     *   v=255 -> 192*0.5 + 255*0.5 = 223.5 -> 223
     *   v=0   ->  65*0.5 +   2*0.5 =  33.5 ->  33  */
    dyt_gray_params_default(&p);
    p.lo = 0; p.hi = 255;
    intcheck("gray: map at 255", dyt_gray_map(255, acc, total, &p, 0), 223);
    intcheck("gray: map at 0",   dyt_gray_map(0, acc, total, &p, 0), 33);

    /* The detail term is added before the clamp. */
    intcheck("gray: detail +30", dyt_gray_map(255, acc, total, &p, 30), 253);
    intcheck("gray: detail -30", dyt_gray_map(255, acc, total, &p, -30), 193);
    intcheck("gray: detail clamps high",
             dyt_gray_map(255, acc, total, &p, 1000), 255);
    intcheck("gray: detail clamps low",
             dyt_gray_map(0, acc, total, &p, -1000), 0);

    /* Whole-plane render, with and without a detail plane.  grey[99] == 99,
     * so it maps to 114*0.5 + 255*0.5 = 184.5 -> 184. */
    {
        uint8_t out[100];
        int     det[100];
        intcheck("gray: render",
                 dyt_gray_render(grey, 100, acc, total, &p, NULL, out), 0);
        intcheck("gray: render at index 99", out[99], 184);
        for (i = 0; i < 100; i++) det[i] = 0;
        intcheck("gray: render with detail",
                 dyt_gray_render(grey, 100, acc, total, &p, det, out), 0);
        intcheck("gray: render bad args",
                 dyt_gray_render(NULL, 100, acc, total, &p, NULL, out), -1);
    }
}

static void test_gray_detail(void)
{
    uint8_t flat[81], impulse[81];
    int     det[81];
    int     i;

    /* A flat plane has no detail anywhere: bilateral == gaussian == flat.
     * The kernel's truncating cast makes the ratio come out just under the
     * true integer, so the result is -1 rather than 0 — assert the magnitude
     * rather than an exact zero (see the note on dyt_gray_detail). */
    for (i = 0; i < 81; i++) flat[i] = 100;
    intcheck("detail: flat plane",
             dyt_gray_detail(flat, 9, 9, 3.0f, 20.0f, 5, 30, det), 0);
    {
        int max_abs = 0;
        for (i = 0; i < 81; i++) {
            int a = det[i] < 0 ? -det[i] : det[i];
            if (a > max_abs) max_abs = a;
        }
        intcheck("detail: flat plane is ~zero (truncation artefact)",
                 max_abs <= 1, 1);
    }

    /* An isolated bright pixel: the bilateral filter keeps it, the Gaussian
     * blurs it away, so the centre detail is strongly positive. */
    for (i = 0; i < 81; i++) impulse[i] = 100;
    impulse[4 * 9 + 4] = 200;
    intcheck("detail: impulse",
             dyt_gray_detail(impulse, 9, 9, 3.0f, 20.0f, 5, 30, det), 0);
    intcheck("detail: impulse centre is clamped to +30", det[4 * 9 + 4], 30);
    intcheck("detail: impulse leaves the far corner ~zero", det[0] >= -1 && det[0] <= 1, 1);

    /* With a clamp wide enough not to bind, the raw detail is visible. */
    intcheck("detail: impulse with a wide clamp",
             dyt_gray_detail(impulse, 9, 9, 3.0f, 20.0f, 5, 1000, det), 0);
    if (det[4 * 9 + 4] > 30)
        ok("detail: centre exceeds 30 before clamping");
    else
        fail("detail: unclamped centre", "expected > 30");

    intcheck("detail: NULL plane rejected",
             dyt_gray_detail(NULL, 9, 9, 3.0f, 20.0f, 5, 30, det), -1);
    intcheck("detail: NULL out rejected",
             dyt_gray_detail(flat, 9, 9, 3.0f, 20.0f, 5, 30, NULL), -1);
    intcheck("detail: zero sigma rejected",
             dyt_gray_detail(flat, 9, 9, 0.0f, 20.0f, 5, 30, det), -1);
    intcheck("detail: negative sigma rejected",
             dyt_gray_detail(flat, 9, 9, 3.0f, -1.0f, 5, 30, det), -1);
}

int main(void)
{
    printf("=== display_test ===\n");
    test_stats();
    test_range();
    test_palette();
    test_transform();
    test_gray_plateau();
    test_gray_detail();
    printf("=== %s ===\n", fails ? "FAIL" : "ALL PASS");
    return fails ? 1 : 0;
}
