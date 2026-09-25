/*
 * session_test.c — unit tests for session.c.
 *
 * The session is the one place two threads meet (the capture callback and the
 * GUI), so most of what is worth testing is *consistency*: a snapshot must
 * never mix state from two different frames, the temperature copy must match
 * the scalars beside it, and the "not ready" state must not leak a plausible
 * temperature.  Those are the failures that would show as a flickering or
 * subtly wrong display rather than a crash.
 *
 * Everything here is synthetic — no camera, no libuvc.
 *
 * build:  via the Makefile (make check)
 */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session.h"

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

static void floatcheck(const char *what, float got, float want, float eps)
{
    if (fabsf(got - want) <= eps) {
        ok(what);
    } else {
        char d[96];
        snprintf(d, sizeof d, "got %.4f want %.4f", (double)got, (double)want);
        fail(what, d);
    }
}

/* Fill a w*h image with a constant. */
static void fill(float *buf, int w, int h, float v)
{
    int i, n = w * h;
    for (i = 0; i < n; i++) buf[i] = v;
}

/* --- lifecycle and the pre-frame state ---------------------------------- */

static void test_lifecycle(void)
{
    dyt_session_t *s;
    dyt_snapshot_t snap;
    uint8_t dummy[64];
    int w = 0, h = 0;

    printf("-- lifecycle --\n");

    s = dyt_session_create();
    if (!s) {
        fail("create", "returned NULL");
        return;
    }
    ok("create");

    /* Nothing has been processed yet: a snapshot must say so rather than
     * hand back a zeroed frame that looks like a real reading. */
    intcheck("snapshot before any frame", dyt_session_snapshot(s, &snap, NULL, 0), -1);
    intcheck("render before any frame",
             dyt_session_render_rgb(s, dummy, sizeof dummy, &w, &h), -1);

    dyt_session_free(s);
    dyt_session_free(NULL);
    ok("free (including NULL)");
}

/* --- the happy path ----------------------------------------------------- */

static void test_process_and_snapshot(void)
{
    dyt_session_t *s = dyt_session_create();
    dyt_snapshot_t snap;
    dyt_frame_info_t fi;
    float img[8 * 4];
    float out[8 * 4];
    int i;

    printf("-- process and snapshot --\n");

    for (i = 0; i < 32; i++) img[i] = 10.0f + (float)i;   /* 10 .. 41 */
    fi.temps = img; fi.width = 8; fi.height = 4;

    intcheck("process", dyt_session_process(s, &fi), 0);

    if (dyt_session_snapshot(s, &snap, out, 32) != 0) {
        fail("snapshot", "returned non-zero");
        dyt_session_free(s);
        return;
    }

    intcheck("width",  snap.width, 8);
    intcheck("height", snap.height, 4);
    intcheck("seq",    (int)snap.seq, 1);
    intcheck("ready",  snap.ready, 1);
    floatcheck("stats min",  snap.stats.lo, 10.0f, 1e-4f);
    floatcheck("stats max",  snap.stats.hi, 41.0f, 1e-4f);
    floatcheck("range lo",   snap.lo, 10.0f, 1e-4f);
    floatcheck("range hi",   snap.hi, 41.0f, 1e-4f);

    /* The temperature copy must be the same frame as the scalars. */
    if (memcmp(out, img, sizeof img) == 0)
        ok("temperature copy matches the frame");
    else
        fail("temperature copy", "differs from the frame");

    /* Hot pixel is the last sample (row 3, col 7); cold is the first. */
    intcheck("hot_x",  snap.stats.hot_x,  7);
    intcheck("hot_y",  snap.stats.hot_y,  3);
    intcheck("cold_x", snap.stats.cold_x, 0);
    intcheck("cold_y", snap.stats.cold_y, 0);

    /* Centre of an 8x4 frame is (4,2) -> index 2*8+4 = 20 -> 10+20 = 30. */
    intcheck("centre_x", snap.centre_x, 4);
    intcheck("centre_y", snap.centre_y, 2);
    floatcheck("centre temperature", snap.centre_c, 30.0f, 1e-4f);

    /* A second frame advances the sequence.  A flat frame is widened by ±1
     * so the renderer has a non-zero span, hence 24..26 rather than 25..25. */
    fill(img, 8, 4, 25.0f);
    dyt_session_process(s, &fi);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("seq advances", (int)snap.seq, 2);
    floatcheck("flat frame widened lo", snap.lo, 24.0f, 1e-4f);
    floatcheck("flat frame widened hi", snap.hi, 26.0f, 1e-4f);

    /* An undersized temperature buffer is reported, not overrun. */
    intcheck("short temps buffer",
             dyt_session_snapshot(s, &snap, out, 4), -2);

    /* A larger frame must grow the internal buffer, not truncate. */
    {
        float big[16 * 8];
        fill(big, 16, 8, 33.0f);
        fi.temps = big; fi.width = 16; fi.height = 8;
        dyt_session_process(s, &fi);
        dyt_session_snapshot(s, &snap, NULL, 0);
        intcheck("grew to the larger frame width",  snap.width, 16);
        intcheck("grew to the larger frame height", snap.height, 8);
        floatcheck("larger frame range", snap.hi, 34.0f, 1e-4f);
    }

    /* Bad arguments. */
    intcheck("NULL frame info", dyt_session_process(s, NULL), -1);
    intcheck("NULL temps",      dyt_session_process(s, &(dyt_frame_info_t){NULL, 4, 4}), -1);

    dyt_session_free(s);
}

/* --- the start-up filler ------------------------------------------------ */

static void test_filler(void)
{
    dyt_session_t *s = dyt_session_create();
    dyt_snapshot_t snap;
    dyt_frame_info_t fi;
    float img[6 * 4];

    printf("-- start-up filler --\n");

    fill(img, 6, 4, DYT_FILLER_C);
    fi.temps = img; fi.width = 6; fi.height = 4;
    dyt_session_process(s, &fi);
    dyt_session_snapshot(s, &snap, NULL, 0);

    intcheck("filler frame is not ready", snap.ready, 0);
    /* A centre temperature must not be invented for a frame that is not a
     * measurement — this is the value the readout would print. */
    if (isnan(snap.centre_c))
        ok("filler frame centre temperature is NaN");
    else
        fail("filler centre temperature", "expected NaN");

    /* The transition to real data clears it. */
    fill(img, 6, 4, 29.0f);
    dyt_session_process(s, &fi);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("live frame is ready", snap.ready, 1);
    floatcheck("live centre temperature", snap.centre_c, 29.0f, 1e-4f);

    dyt_session_free(s);
}

/* --- rendering ---------------------------------------------------------- */

static void test_render(void)
{
    dyt_session_t *s = dyt_session_create();
    dyt_frame_info_t fi;
    float img[8 * 4];
    uint8_t rgb[8 * 4 * 3];
    int w = 0, h = 0, i;

    printf("-- rendering --\n");

    for (i = 0; i < 32; i++) img[i] = 10.0f + (float)i * 3.0f;
    fi.temps = img; fi.width = 8; fi.height = 4;
    dyt_session_process(s, &fi);

    intcheck("render", dyt_session_render_rgb(s, rgb, sizeof rgb, &w, &h), 0);
    intcheck("render width",  w, 8);
    intcheck("render height", h, 4);

    /* The ramp must actually be applied: the coldest and hottest pixels
     * cannot come out the same colour. */
    if (memcmp(rgb, rgb + 31 * 3, 3) != 0)
        ok("render applies the palette ramp");
    else
        fail("render ramp", "coldest and hottest pixels are identical");

    /* An undersized buffer is reported, not overrun. */
    intcheck("short rgb buffer",
             dyt_session_render_rgb(s, rgb, 8, &w, &h), -2);

    dyt_session_free(s);
}

/* --- palettes ----------------------------------------------------------- */

static void test_palettes(void)
{
    dyt_session_t *s = dyt_session_create();
    dyt_snapshot_t snap;
    dyt_frame_info_t fi;
    float img[4 * 2];

    printf("-- palettes --\n");

    /* A snapshot is only valid once a frame has arrived, so seed one. */
    fill(img, 4, 2, 30.0f);
    fi.temps = img; fi.width = 4; fi.height = 2;
    dyt_session_process(s, &fi);

    /* Built-ins are the fallback, so a session is never palette-less. */
    intcheck("built-in fallback count",
             dyt_session_snapshot(s, &snap, NULL, 0) == 0 ? snap.palette_n : -1,
             DYT_PALETTE_BUILTIN_N);

    /* A real directory replaces them. */
    intcheck("load from palettes/", dyt_session_load_palettes(s, "palettes"), 28);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("snapshot reports the loaded count", snap.palette_n, 28);

    /* An unreadable directory falls back rather than leaving zero palettes. */
    intcheck("unreadable dir falls back",
             dyt_session_load_palettes(s, "/nonexistent-dir"),
             DYT_PALETTE_BUILTIN_N);
    intcheck("NULL dir falls back",
             dyt_session_load_palettes(s, NULL), DYT_PALETTE_BUILTIN_N);

    /* Re-loading the real set after a fallback must not leave the index out
     * of range. */
    dyt_session_load_palettes(s, "palettes");
    dyt_session_set_palette(s, 27);
    dyt_session_load_palettes(s, "palettes");
    dyt_session_snapshot(s, &snap, NULL, 0);
    if (snap.palette >= 0 && snap.palette < snap.palette_n)
        ok("palette index stays in range across a reload");
    else
        fail("palette index after reload", "out of range");

    /* The harder case: the replacement set is *smaller* than the old index.
     * Selecting 27 and then falling back to the 6 built-ins must re-clamp. */
    dyt_session_set_palette(s, 27);
    dyt_session_load_palettes(s, NULL);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("count shrank to built-ins", snap.palette_n, DYT_PALETTE_BUILTIN_N);
    if (snap.palette >= 0 && snap.palette < snap.palette_n)
        ok("palette index re-clamped to the smaller set");
    else
        fail("palette index after shrink", "out of range");

    dyt_session_free(s);
}

/* --- settings ----------------------------------------------------------- */

static void test_settings(void)
{
    dyt_session_t *s = dyt_session_create();
    dyt_snapshot_t snap;
    dyt_frame_info_t fi;
    float img[4 * 2];

    printf("-- settings --\n");

    fill(img, 4, 2, 30.0f);
    img[3] = 60.0f;
    fi.temps = img; fi.width = 4; fi.height = 2;
    dyt_session_process(s, &fi);

    /* Units cycle C -> F -> K -> C and are reported, not converted. */
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("default unit is Celsius", snap.unit == DYT_UNIT_C, 1);
    dyt_session_cycle_unit(s);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("cycle -> Fahrenheit", snap.unit == DYT_UNIT_F, 1);
    dyt_session_cycle_unit(s);
    dyt_session_cycle_unit(s);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("cycle wraps to Celsius", snap.unit == DYT_UNIT_C, 1);

    /* Range: auto follows the frame; toggling latches it. */
    dyt_session_snapshot(s, &snap, NULL, 0);
    floatcheck("auto range tracks the frame", snap.hi, 60.0f, 1e-4f);
    dyt_session_toggle_range(s);
    fill(img, 4, 2, 100.0f);
    dyt_session_process(s, &fi);
    dyt_session_snapshot(s, &snap, NULL, 0);
    floatcheck("locked range holds", snap.hi, 60.0f, 1e-4f);
    dyt_session_toggle_range(s);
    /* The range is resolved per frame, so AUTO takes effect on the next one. */
    dyt_session_process(s, &fi);
    dyt_session_snapshot(s, &snap, NULL, 0);
    floatcheck("auto range resumes (flat frame widened)", snap.hi, 101.0f, 1e-4f);

    dyt_session_set_fixed_range(s, 20.0f, 25.0f);
    dyt_session_snapshot(s, &snap, NULL, 0);
    floatcheck("explicit fixed range lo", snap.lo, 20.0f, 1e-4f);
    floatcheck("explicit fixed range hi", snap.hi, 25.0f, 1e-4f);

    /* Palette cycling. */
    dyt_session_load_palettes(s, "palettes");
    dyt_session_set_palette(s, 0);
    dyt_session_cycle_palette(s, 1);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("palette cycles forward", snap.palette, 1);
    dyt_session_cycle_palette(s, -1);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("palette cycles backward", snap.palette, 0);

    /* Mirror and zoom are pure view state; they must not disturb the frame. */
    dyt_session_toggle_flip_h(s);
    dyt_session_toggle_flip_v(s);
    dyt_session_zoom(s, 3);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("flip_h", snap.xform.flip_h, 1);
    intcheck("flip_v", snap.xform.flip_v, 1);
    intcheck("zoom",   snap.xform.zoom, 4);
    intcheck("flip/zoom left the frame alone", snap.width, 4);

    dyt_session_free(s);
}

/* --- concurrency -------------------------------------------------------- */

/* Two alternating frames with different sizes *and* different temperatures.
 * A torn snapshot — scalars from one frame, pixels from the other — would not
 * match either, which is exactly what the reader checks for. */
#define CONC_A_W 8
#define CONC_A_H 4
#define CONC_B_W 16
#define CONC_B_H 8
#define CONC_A_C 11.0f
#define CONC_B_C 22.0f
#define CONC_ITERS 20000

struct conc {
    dyt_session_t *s;
    volatile int   stop;
    long           torn;
    long           reads;
};

static void *writer_thread(void *arg)
{
    struct conc *c = arg;
    float a[CONC_A_W * CONC_A_H], b[CONC_B_W * CONC_B_H];
    dyt_frame_info_t fi;
    int i;

    fill(a, CONC_A_W, CONC_A_H, CONC_A_C);
    fill(b, CONC_B_W, CONC_B_H, CONC_B_C);

    for (i = 0; i < CONC_ITERS && !c->stop; i++) {
        if (i & 1) {
            fi.temps = b; fi.width = CONC_B_W; fi.height = CONC_B_H;
        } else {
            fi.temps = a; fi.width = CONC_A_W; fi.height = CONC_A_H;
        }
        dyt_session_process(c->s, &fi);
    }
    c->stop = 1;
    return NULL;
}

static void test_concurrency(void)
{
    struct conc c;
    dyt_session_t *s = dyt_session_create();
    pthread_t th;
    float out[CONC_B_W * CONC_B_H];
    dyt_snapshot_t snap;

    printf("-- concurrency --\n");

    memset(&c, 0, sizeof c);
    c.s = s;

    if (pthread_create(&th, NULL, writer_thread, &c) != 0) {
        fail("concurrency", "could not start the writer thread");
        dyt_session_free(s);
        return;
    }

    while (!c.stop) {
        if (dyt_session_snapshot(s, &snap, out, CONC_B_W * CONC_B_H) != 0)
            continue;

        c.reads++;

        /* Whichever frame this is, every part of it must agree. */
        int is_a = (snap.width == CONC_A_W && snap.height == CONC_A_H);
        int is_b = (snap.width == CONC_B_W && snap.height == CONC_B_H);
        if (!is_a && !is_b) {
            c.torn++;
            continue;
        }

        float want_c = is_a ? CONC_A_C : CONC_B_C;
        float want_lo = is_a ? CONC_A_C : CONC_B_C;
        int   n      = snap.width * snap.height;
        int   i, mismatch = 0;

        for (i = 0; i < n; i++)
            if (out[i] != want_c) { mismatch = 1; break; }

        if (mismatch || snap.stats.lo != want_lo ||
            snap.stats.hi != want_lo || snap.centre_c != want_c)
            c.torn++;
    }

    pthread_join(th, NULL);

    if (c.reads < 100)
        fail("concurrency", "too few snapshots to be meaningful");
    else if (c.torn == 0)
        ok("no torn snapshots across the thread boundary");
    else {
        char d[96];
        snprintf(d, sizeof d, "%ld of %ld snapshots were inconsistent",
                 c.torn, c.reads);
        fail("torn snapshots", d);
    }

    dyt_session_free(s);
}

/* --- measurement tools -------------------------------------------------- */

static void test_measurement(void)
{
    dyt_session_t *s = dyt_session_create();
    dyt_snapshot_t snap;
    float ramp[32], prof[64];
    dyt_frame_info_t fi = { ramp, 8, 4 };
    int i, n;

    printf("-- measurement --\n");

    if (!s) {
        fail("measurement create", "NULL");
        return;
    }

    for (i = 0; i < 32; i++)
        ramp[i] = 10.0f + (float)i;          /* 10 .. 41 */
    dyt_session_process(s, &fi);

    /* Point probe. */
    dyt_session_set_tool(s, DYT_TOOL_POINT);
    dyt_session_set_point(s, 0, 2, 1);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("point: tool reported", (int)snap.tool, DYT_TOOL_POINT);
    intcheck("point: reading ok", snap.point_ok, 1);
    floatcheck("point: reading == 20", snap.point_c, 20.0f, 1e-5f);

    /* A point outside the frame has no reading, rather than a bogus one. */
    dyt_session_set_point(s, 0, 99, 99);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("point: outside has no reading", snap.point_ok, 0);

    /* Box ROI. */
    dyt_session_set_tool(s, DYT_TOOL_BOX);
    dyt_session_set_point(s, 0, 2, 1);
    dyt_session_set_point(s, 1, 3, 2);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("box: tool reported", (int)snap.tool, DYT_TOOL_BOX);
    intcheck("box: ok", snap.roi_ok, 1);
    intcheck("box: n == 4", snap.roi.n, 4);
    floatcheck("box: min == 20", snap.roi.min, 20.0f, 1e-5f);
    floatcheck("box: max == 29", snap.roi.max, 29.0f, 1e-5f);
    floatcheck("box: mean == 24.5", snap.roi.mean, 24.5f, 1e-5f);
    floatcheck("box: median == 24.5", snap.roi.median, 24.5f, 1e-5f);

    /* A NaN inside the box is skipped, not counted. */
    ramp[2 * 8 + 2] = NAN;                   /* was 28 */
    dyt_session_process(s, &fi);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("box: NaN skipped", snap.roi.n, 3);
    floatcheck("box: mean without the NaN", snap.roi.mean, 23.3333f, 1e-3f);

    /* A box dragged entirely off the frame is empty, not an error. */
    dyt_session_set_point(s, 0, -50, -50);
    dyt_session_set_point(s, 1, -40, -40);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("box: fully outside is not ok", snap.roi_ok, 0);

    /* Line profile. */
    ramp[2 * 8 + 2] = 10.0f + 2 * 8 + 2;     /* restore 28 */
    dyt_session_process(s, &fi);
    dyt_session_set_tool(s, DYT_TOOL_LINE);
    dyt_session_set_point(s, 0, 0, 0);
    dyt_session_set_point(s, 1, 7, 0);
    n = dyt_session_profile(s, prof, 64);
    intcheck("line: profile count == 8", n, 8);
    floatcheck("line: profile start == 10", prof[0], 10.0f, 1e-5f);
    floatcheck("line: profile end == 17", prof[7], 17.0f, 1e-5f);
    intcheck("line: small cap -> -8", dyt_session_profile(s, prof, 3), -8);

    /* Switching away from LINE yields an empty profile. */
    dyt_session_set_tool(s, DYT_TOOL_POINT);
    intcheck("line: empty when the tool is not LINE",
             dyt_session_profile(s, prof, 64), 0);

    /* Clearing the points leaves the tool selected but unplaced. */
    dyt_session_clear_points(s);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("clear: p0 unset", snap.p0.x, -1);
    intcheck("clear: p1 unset", snap.p1.x, -1);

    dyt_session_free(s);
}

/* --- alarms ------------------------------------------------------------- */

static void test_alarm(void)
{
    dyt_session_t *s = dyt_session_create();
    dyt_snapshot_t snap;
    float ramp[32], flat[32], filler[32];
    dyt_frame_info_t fi = { ramp, 8, 4 };
    dyt_frame_info_t ff = { flat, 8, 4 };
    dyt_frame_info_t ffi = { filler, 8, 4 };
    int i;

    printf("-- alarms --\n");

    if (!s) {
        fail("alarm create", "NULL");
        return;
    }

    for (i = 0; i < 32; i++)
        ramp[i] = 10.0f + (float)i;          /* 10 .. 41 */
    for (i = 0; i < 32; i++)
        flat[i] = 25.0f;                     /* inside the band */
    for (i = 0; i < 32; i++)
        filler[i] = DYT_FILLER_C;

    dyt_session_process(s, &fi);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("alarm: off by default", snap.alarm_on, 0);
    intcheck("alarm: none while off", (int)snap.alarm, DYT_ALARM_NONE);

    dyt_session_set_alarm(s, 20.0f, 30.0f, 2.0f);
    dyt_session_process(s, &fi);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("alarm: armed", snap.alarm_on, 1);
    intcheck("alarm: 10..41 trips both", (int)snap.alarm, DYT_ALARM_BOTH);
    floatcheck("alarm: lo reported", snap.alarm_lo, 20.0f, 1e-5f);
    floatcheck("alarm: hi reported", snap.alarm_hi, 30.0f, 1e-5f);

    /* A frame entirely inside the band clears both sides. */
    dyt_session_process(s, &ff);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("alarm: in-band frame clears", (int)snap.alarm, DYT_ALARM_NONE);

    /* The isotherm is the alarm band, and counts what falls inside it. */
    dyt_session_set_isotherm(s, 1);
    dyt_session_process(s, &fi);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("isotherm: on", snap.iso_on, 1);
    intcheck("isotherm: band lo", (int)snap.iso_lo, 20);
    intcheck("isotherm: band hi", (int)snap.iso_hi, 30);
    intcheck("isotherm: counts 20..30 (inclusive)", (int)snap.iso.count, 11);

    /* The start-up filler decodes to 238.85 C and must never trip an alarm. */
    dyt_session_alarm_reset(s);
    dyt_session_process(s, &ffi);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("alarm: filler is not ready", snap.ready, 0);
    intcheck("alarm: filler does not trip", (int)snap.alarm, DYT_ALARM_NONE);

    dyt_session_alarm_disable(s);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("alarm: disarmed", snap.alarm_on, 0);
    intcheck("alarm: none after disarm", (int)snap.alarm, DYT_ALARM_NONE);

    dyt_session_free(s);
}

/* --- fusion (Phase 5) --------------------------------------------------- */

/* The visible plane and the fusion pattern are two pieces of state that must
 * agree: a plane only counts when its geometry matches the frame, and a
 * pattern only takes effect when a plane is there.  Both are checked here
 * rather than only in fusion_test, because the *wiring* — that render_rgb
 * actually fuses, and that the fallback is the thermal picture — lives in the
 * session, not in fusion.c. */
static void test_fusion(void)
{
    dyt_session_t *s = dyt_session_create();
    dyt_snapshot_t snap;
    dyt_frame_info_t fi;
    float img[8 * 4];
    uint8_t grey[8 * 4], rgb[8 * 4 * 3];
    int i, w = 0, h = 0, bad;

    printf("-- fusion --\n");

    for (i = 0; i < 32; i++) img[i] = 20.0f + (float)i;
    fi.temps = img; fi.width = 8; fi.height = 4;

    /* A plane before any frame has no geometry to match. */
    memset(grey, 100, sizeof grey);
    intcheck("visible before any frame",
             dyt_session_process_visible(s, grey, 8, 4), -1);

    intcheck("process", dyt_session_process(s, &fi), 0);

    /* A plane of the wrong size is refused, not stretched. */
    intcheck("visible wrong width",
             dyt_session_process_visible(s, grey, 4, 4), -1);
    intcheck("visible wrong height",
             dyt_session_process_visible(s, grey, 8, 2), -1);
    intcheck("visible NULL plane",
             dyt_session_process_visible(s, NULL, 8, 4), -1);
    intcheck("visible zero width",
             dyt_session_process_visible(s, grey, 0, 4), -1);

    if (dyt_session_snapshot(s, &snap, NULL, 0) != 0) {
        fail("snapshot", "returned non-zero");
        dyt_session_free(s);
        return;
    }
    intcheck("refused planes left none installed", snap.have_visible, 0);
    intcheck("default pattern is infrared", (int)snap.fusion, DYT_FUSION_INFRARED);
    intcheck("default pattern name",
             strcmp(snap.fusion_name, "ir") == 0, 1);
    intcheck("infrared is not 'active'", snap.fusion_active, 0);

    /* A matching plane installs, and the stats come from the plane itself. */
    for (i = 0; i < 32; i++) grey[i] = (uint8_t)(40 + i * 4);   /* 40..164 */
    intcheck("visible plane installs",
             dyt_session_process_visible(s, grey, 8, 4), 0);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("have_visible", snap.have_visible, 1);
    intcheck("visible n", snap.visible.n, 32);
    intcheck("visible min", snap.visible.min, 40);
    intcheck("visible max", snap.visible.max, 164);
    intcheck("visible mean", (int)(snap.visible.mean + 0.5), 102);

    /* Selecting a pattern that needs the plane makes it active. */
    dyt_session_set_fusion(s, DYT_FUSION_EDGE);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("pattern selected", (int)snap.fusion, DYT_FUSION_EDGE);
    intcheck("pattern name", strcmp(snap.fusion_name, "edge") == 0, 1);
    intcheck("fusion is active", snap.fusion_active, 1);

    /* An out-of-range index is ignored, not clamped onto a neighbour. */
    dyt_session_set_fusion(s, DYT_FUSION_EDGE);
    dyt_session_set_fusion(s, (dyt_fusion_t)99);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("bad index ignored", (int)snap.fusion, DYT_FUSION_EDGE);
    dyt_session_set_fusion(s, (dyt_fusion_t)-1);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("negative index ignored", (int)snap.fusion, DYT_FUSION_EDGE);

    /* The cycle walks the vendor's order and wraps both ways. */
    dyt_session_set_fusion(s, DYT_FUSION_INFRARED);
    dyt_session_cycle_fusion(s, 1);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("cycle +1", (int)snap.fusion, DYT_FUSION_VISIBLE);
    dyt_session_cycle_fusion(s, -1);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("cycle -1", (int)snap.fusion, DYT_FUSION_INFRARED);
    dyt_session_cycle_fusion(s, -1);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("cycle wraps backwards", (int)snap.fusion, DYT_FUSION_EDGE_BLACK);
    for (i = 0; i < DYT_FUSION_N; i++)
        dyt_session_cycle_fusion(s, 1);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("a full cycle returns to the start",
             (int)snap.fusion, DYT_FUSION_EDGE_BLACK);

    /* Alignment is clamped to the vendor's ±40. */
    dyt_session_set_fusion_align(s, 1000, -1000);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("align clamped high", snap.fusion_dx, DYT_FUSION_ALIGN_MAX);
    intcheck("align clamped low",  snap.fusion_dy, -DYT_FUSION_ALIGN_MAX);
    dyt_session_set_fusion_align(s, 0, 0);
    dyt_session_adjust_fusion_align(s, 1, 1);
    dyt_session_adjust_fusion_align(s, 1, 1);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("align nudges accumulate", snap.fusion_dx, 2);
    intcheck("align nudges accumulate (y)", snap.fusion_dy, 2);
    dyt_session_set_fusion_align(s, DYT_FUSION_ALIGN_MAX, 0);
    dyt_session_adjust_fusion_align(s, 5, 0);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("nudge stops at the clamp", snap.fusion_dx, DYT_FUSION_ALIGN_MAX);

    /* Render must actually fuse: a flat grey plane in VISIBLE mode makes every
     * output pixel that grey level, whatever the palette and range are. */
    dyt_session_set_fusion_align(s, 0, 0);
    memset(grey, 77, sizeof grey);
    dyt_session_process_visible(s, grey, 8, 4);
    dyt_session_set_fusion(s, DYT_FUSION_VISIBLE);
    intcheck("render (visible)",
             dyt_session_render_rgb(s, rgb, sizeof rgb, &w, &h), 0);
    bad = 0;
    for (i = 0; i < 32; i++)
        if (rgb[i * 3] != 77 || rgb[i * 3 + 1] != 77 || rgb[i * 3 + 2] != 77)
            bad++;
    intcheck("render is the visible plane, not the thermal render", bad, 0);

    /* And infrared still renders the thermal picture. */
    dyt_session_set_fusion(s, DYT_FUSION_INFRARED);
    {
        uint8_t thermal[8 * 4 * 3];
        dyt_session_render_rgb(s, thermal, sizeof thermal, &w, &h);
        intcheck("infrared render differs from the visible render",
                 memcmp(rgb, thermal, sizeof thermal) != 0, 1);
    }

    dyt_session_free(s);

    /* Without a plane, a pattern that needs one falls back to thermal and is
     * reported as inactive rather than silently fusing nothing. */
    s = dyt_session_create();
    fi.temps = img; fi.width = 8; fi.height = 4;
    dyt_session_process(s, &fi);
    dyt_session_set_fusion(s, DYT_FUSION_EDGE);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("no plane: pattern kept", (int)snap.fusion, DYT_FUSION_EDGE);
    intcheck("no plane: not active", snap.fusion_active, 0);
    intcheck("no plane: have_visible", snap.have_visible, 0);
    intcheck("no plane: render still succeeds",
             dyt_session_render_rgb(s, rgb, sizeof rgb, &w, &h), 0);
    dyt_session_free(s);
}

/* The raw device payload (Phase 6).  The capture layer's own accessor is only
 * valid on the frame callback thread, so the session keeps the copy the still
 * writer reads; this pins that copy, its geometry guard, and its read-back. */
static void test_raw_payload(void)
{
    dyt_session_t *s = dyt_session_create();
    dyt_snapshot_t snap;
    dyt_frame_info_t fi;
    float     img[8 * 4];
    uint16_t  raw[8 * 8], out[8 * 8], small[8 * 8];
    int       i, n;

    printf("-- raw payload --\n");

    for (i = 0; i < 32; i++) img[i] = 20.0f + (float)i;
    fi.temps = img; fi.width = 8; fi.height = 4;

    for (i = 0; i < 64; i++) raw[i] = (uint16_t)(0x1000 + i * 7);

    /* Before any frame there is no geometry to match against. */
    intcheck("payload before any frame",
             dyt_session_process_raw(s, raw, 64, 8, 8), -1);
    intcheck("read before any frame", dyt_session_raw(s, out, 64), 0);

    intcheck("process", dyt_session_process(s, &fi), 0);

    /* Rejections: the payload must describe the frame just processed. */
    intcheck("payload NULL",
             dyt_session_process_raw(s, NULL, 64, 8, 8), -1);
    intcheck("payload wrong width",
             dyt_session_process_raw(s, raw, 64, 4, 8), -1);
    intcheck("sample count disagrees with the geometry",
             dyt_session_process_raw(s, raw, 63, 8, 8), -1);
    intcheck("payload shorter than the thermal plane",
             dyt_session_process_raw(s, raw, 16, 8, 2), -1);
    intcheck("payload zero rows",
             dyt_session_process_raw(s, raw, 0, 8, 0), -1);

    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("nothing installed yet", snap.have_raw, 0);
    intcheck("raw_n is zero", snap.raw_n, 0);

    /* A dual-half payload: 8 rows for a 4-row thermal plane.  That is the
     * normal case, not an error — the visible half is in there too. */
    intcheck("dual-half payload installs",
             dyt_session_process_raw(s, raw, 64, 8, 8), 0);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("have_raw", snap.have_raw, 1);
    intcheck("raw_n", snap.raw_n, 64);
    intcheck("raw_total_rows", snap.raw_total_rows, 8);

    n = dyt_session_raw(s, out, 64);
    intcheck("read back the sample count", n, 64);
    intcheck("the samples are byte-identical",
             n == 64 && memcmp(out, raw, sizeof raw) == 0, 1);

    /* A larger buffer is fine; a smaller one reports what it needs. */
    intcheck("a larger buffer is fine", dyt_session_raw(s, out, 128), 64);
    intcheck("a too-small buffer reports the requirement",
             dyt_session_raw(s, small, 63), -64);

    /* A payload does not survive a frame of a different width: the snapshot
     * must stop offering it, so a still cannot mix two geometries. */
    {
        float small_img[4 * 4];
        dyt_frame_info_t f3 = { small_img, 4, 4 };

        for (i = 0; i < 16; i++) small_img[i] = 10.0f + (float)i;
        dyt_session_process(s, &f3);
        dyt_session_snapshot(s, &snap, NULL, 0);
        intcheck("a stale payload is not offered", snap.have_raw, 0);
        intcheck("and the read reports none", dyt_session_raw(s, out, 64), 0);
    }

    intcheck("read NULL out", dyt_session_raw(s, NULL, 64), -1);
    intcheck("read zero cap", dyt_session_raw(s, out, 0), -1);

    dyt_session_free(s);

    /* A session that never received one reports none, not an error. */
    s = dyt_session_create();
    dyt_session_process(s, &fi);
    intcheck("no payload: read yields none", dyt_session_raw(s, out, 64), 0);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("no payload: snapshot says so", snap.have_raw, 0);
    dyt_session_free(s);
}

int main(void)
{
    printf("=== session_test ===\n");
    test_lifecycle();
    test_process_and_snapshot();
    test_filler();
    test_render();
    test_palettes();
    test_settings();
    test_measurement();
    test_alarm();
    test_fusion();
    test_raw_payload();
    test_concurrency();
    printf("=== %s ===\n", fails ? "FAIL" : "ALL PASS");
    return fails ? 1 : 0;
}
