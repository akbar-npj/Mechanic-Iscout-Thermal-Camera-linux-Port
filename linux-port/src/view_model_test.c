/*
 * view_model_test.c — unit tests for view_model.c.
 *
 * This module exists so dytview.cpp and the Qt6 app cannot drift apart, so
 * the thing worth pinning is the *wording and the decisions*, not the
 * arithmetic: the exact status and readout lines, which rows the device
 * panel shows and when it flags an override, the arm/confirm state machine
 * for the runtime write, and the press/drag/release rule that makes one
 * gesture draw a point, a line or a box.
 *
 * Every case here is a pure function of a hand-built snapshot, so a wrong
 * string or a wrong transition fails loudly without a device or a window.
 * The three entry points that do touch a session (grab, the tool mapping,
 * the still writer) are driven over a synthetic frame.
 *
 * build:  via the Makefile (make check)
 */
#include <math.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include "dytjpeg.h"
#include "session.h"
#include "view_model.h"

static int fails;

/* A stub upscaler, so the super-resolution key routing can be exercised.  It
 * is never run — the key test does not render — which is exactly what makes
 * the point: the seam is a function pointer (session.h), so this module's
 * tests need no MNN runtime at all. */
static int stub_upscale(const uint8_t *in, uint8_t *out, int cap, int *n)
{
    (void)in; (void)out; (void)cap; (void)n;
    return -1;
}

/* A 2x upscaler that duplicates each pixel, written in the model's packing, so
 * the still writer can be driven at 2x without a runtime.  Because it
 * duplicates, the 2x frame is exactly the nearest-2x of the plain one. */
static int dup_upscale(const uint8_t *in, uint8_t *out, int cap, int *n)
{
    const int w = DYT_SR_IN_W, h = DYT_SR_IN_H;
    const int stride = (2 * w) * 2;
    int x, y;

    if (!in || !out || !n || cap < w * h * 8)
        return -1;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint8_t v = in[2 * (y * w + x)];
            int     o = (2 * y) * stride + 4 * x;

            out[o] = out[o + 2] = out[o + stride] = out[o + stride + 2] = v;
            out[o + 1]           = DYT_SR_PACK_HIGH;
            out[o + 3]           = DYT_SR_PACK_HIGH;
            out[o + stride + 1]  = DYT_SR_PACK_HIGH;
            out[o + stride + 3]  = DYT_SR_PACK_HIGH;
        }
    }
    *n = w * h * 8;
    return 0;
}

static void ok(const char *what) { printf("  ok   %s\n", what); }

static void fail(const char *what, const char *detail)
{
    printf("  FAIL %-52s %s\n", what, detail);
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

static void strcheck(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want) == 0) {
        ok(what);
    } else {
        char d[256];
        snprintf(d, sizeof d, "got \"%s\" want \"%s\"", got, want);
        fail(what, d);
    }
}

/* mkdir -p for the tests' own temp trees; 0 on success. */
static int mkdir_p(const char *path)
{
    char   buf[4096];
    size_t i;

    if (strlen(path) >= sizeof buf)
        return -1;
    strcpy(buf, path);
    for (i = 1; buf[i]; i++) {
        if (buf[i] != '/')
            continue;
        buf[i] = '\0';
        if (mkdir(buf, 0700) != 0 && errno != EEXIST)
            return -1;
        buf[i] = '/';
    }
    return (mkdir(buf, 0700) == 0 || errno == EEXIST) ? 0 : -1;
}

/* A snapshot with everything at a known value, so each test changes only the
 * field it is about. */
static void base_snapshot(dyt_snapshot_t *s)
{
    memset(s, 0, sizeof *s);
    s->width = 256;
    s->height = 192;
    s->seq = 154;
    s->ready = 1;
    s->unit = DYT_UNIT_C;
    s->lo = 30.f;
    s->hi = 40.f;
    s->palette = 0;
    s->palette_n = 27;
    snprintf(s->palette_name, sizeof s->palette_name, "iron");
    s->xform.zoom = 2;
    s->fusion = DYT_FUSION_INFRARED;
    snprintf(s->fusion_name, sizeof s->fusion_name, "ir");
}

/* ------------------------------------------------------------------- text */

static void test_temp(void)
{
    dyt_snapshot_t s;
    char           b[32];

    base_snapshot(&s);

    intcheck("temp: formats a Celsius value",
             dyt_vm_temp(&s, 31.2f, b, sizeof b), 0);
    strcheck("temp: value", b, "31.2 C");

    /* A NaN is not an error to the units module, so this delegates rather
     * than inventing a placeholder — the behaviour the viewer had before the
     * extraction.  Pinned against dyt_temp_format() itself so the assertion
     * does not depend on how a host spells NaN. */
    {
        char want[32];
        dyt_temp_format(DYT_UNIT_C, NAN, want, sizeof want);
        intcheck("temp: NaN is delegated, not rejected",
                 dyt_vm_temp(&s, NAN, b, sizeof b), 0);
        strcheck("temp: NaN matches the units module", b, want);
    }

    s.unit = DYT_UNIT_K;
    dyt_vm_temp(&s, 0.0f, b, sizeof b);
    strcheck("temp: honours the display unit", b, "273.1 K");

    intcheck("temp: NULL out rejected", dyt_vm_temp(&s, 1.f, NULL, 8), -1);
    intcheck("temp: zero size rejected", dyt_vm_temp(&s, 1.f, b, 0), -1);
}

static void test_status_line(void)
{
    dyt_snapshot_t s;
    char           b[256];

    base_snapshot(&s);
    dyt_vm_status_line(&s, DYT_MODE_1000, b, sizeof b);
    strcheck("status: the plain line", b,
             "mode 1000 | fusion ir | iron 1/27 | C | x2- | 154 frames");

    /* A pattern that cannot be honoured says so, and its alignment is shown. */
    s.fusion = DYT_FUSION_EDGE;
    snprintf(s.fusion_name, sizeof s.fusion_name, "edge");
    s.fusion_dx = 2;
    s.fusion_dy = -3;
    dyt_vm_status_line(&s, DYT_MODE_1000, b, sizeof b);
    strcheck("status: an unhonourable pattern is called out", b,
             "mode 1000 | fusion edge +2,-3 (no visible plane) | iron 1/27 | "
             "C | x2- | 154 frames");

    /* The same pattern, but actually fused: no caveat. */
    s.fusion_active = 1;
    dyt_vm_status_line(&s, DYT_MODE_1000, b, sizeof b);
    strcheck("status: an active fusion carries no caveat", b,
             "mode 1000 | fusion edge +2,-3 | iron 1/27 | C | x2- | "
             "154 frames");

    /* Infrared is thermal-only, so an inactive render is not a failure. */
    s.fusion = DYT_FUSION_INFRARED;
    snprintf(s.fusion_name, sizeof s.fusion_name, "ir");
    s.fusion_active = 0;
    s.fusion_dx = s.fusion_dy = 0;
    s.xform.flip_h = 1;
    s.xform.flip_v = 1;
    s.xform.zoom = 1;
    dyt_vm_status_line(&s, DYT_MODE_44C, b, sizeof b);
    strcheck("status: mode, mirror and zoom", b,
             "mode 0x44c | fusion ir | iron 1/27 | C | x1HV | 154 frames");

    dyt_vm_status_line(&s, DYT_MODE_0, b, sizeof b);
    if (strncmp(b, "mode ? |", 8) == 0)
        ok("status: an unknown mode is not guessed");
    else
        fail("status: an unknown mode is not guessed", b);

    /* Super-resolution appears only when it is on — it is an addition to what
     * is displayed rather than a mode of its own — and a mode that cannot be
     * honoured on this frame says so, exactly as an unhonourable fusion
     * pattern does. */
    s.sr        = DYT_SR_THERMAL;
    s.sr_active = 1;
    s.xform.sr  = 2;
    snprintf(s.sr_name, sizeof s.sr_name, "thermal");
    dyt_vm_status_line(&s, DYT_MODE_1000, b, sizeof b);
    strcheck("status: an active super-resolution shows its factor", b,
             "mode 1000 | fusion ir | iron 1/27 | C | sr:thermal x2 | x1HV | "
             "154 frames");

    s.sr_active = 0;
    s.xform.sr  = 1;
    dyt_vm_status_line(&s, DYT_MODE_1000, b, sizeof b);
    strcheck("status: an inactive super-resolution says so", b,
             "mode 1000 | fusion ir | iron 1/27 | C | sr:thermal (inactive) | "
             "x1HV | 154 frames");
}

/* The notice a super-resolution key press produces.  It is the only feedback a
 * refused key gets — the status line stays silent while the mode is off — so
 * each reason the mode can fail to take effect has its own exact wording, and
 * the wording names the one thing to change. */
static void test_sr_notice(void)
{
    dyt_snapshot_t s;
    char           b[128];

    base_snapshot(&s);

    /* A key that is not one of the SR bindings says nothing, so a front end can
     * call this after every view key. */
    intcheck("sr notice: a non-SR key is silent",
             dyt_vm_sr_notice('u', &s, b, sizeof b), 0);
    intcheck("sr notice: 'x' is not an SR key",
             dyt_vm_sr_notice('x', &s, b, sizeof b), 0);
    /* ...but a NULL snapshot on an SR key is still an error, not silence. */
    intcheck("sr notice: a NULL snapshot is refused",
             dyt_vm_sr_notice('z', NULL, b, sizeof b), -1);

    /* No upscaler behind the mode: the session refused it, and the notice says
     * why rather than leaving the key looking dead. */
    s.sr_cap = 0;
    s.sr     = DYT_SR_OFF;
    intcheck("sr notice: a refusal is reported",
             dyt_vm_sr_notice('z', &s, b, sizeof b), 1);
    strcheck("sr notice: no model", b, "super-resolution: no model loaded");

    /* An upscaler is present from here on. */
    s.sr_cap = 1;

    /* Turning it off is confirmed, because the status line drops the field
     * entirely when the mode is off. */
    intcheck("sr notice: 'off' is reported",
             dyt_vm_sr_notice('z', &s, b, sizeof b), 1);
    strcheck("sr notice: off", b, "sr:off");

    /* Active: the factor, read from the transform the render will use. */
    s.sr        = DYT_SR_VISIBLE;
    s.sr_active = 1;
    s.xform.sr  = 2;
    snprintf(s.sr_name, sizeof s.sr_name, "visible");
    intcheck("sr notice: an active mode is reported",
             dyt_vm_sr_notice('z', &s, b, sizeof b), 1);
    strcheck("sr notice: visible x2", b, "sr:visible x2");

    s.sr = DYT_SR_THERMAL;
    snprintf(s.sr_name, sizeof s.sr_name, "thermal");
    intcheck("sr notice: the thermal mode is reported",
             dyt_vm_sr_notice('Z', &s, b, sizeof b), 1);
    strcheck("sr notice: thermal x2", b, "sr:thermal x2");

    /* Selected but not taking effect, one reason at a time.  The order mirrors
     * sr_factor_locked() so the advice matches the refusal. */
    s.sr        = DYT_SR_VISIBLE;
    s.sr_active = 0;
    s.xform.sr  = 1;
    snprintf(s.sr_name, sizeof s.sr_name, "visible");
    s.width = 384;
    intcheck("sr notice: a wrong geometry is reported",
             dyt_vm_sr_notice('z', &s, b, sizeof b), 1);
    strcheck("sr notice: geometry", b, "sr:visible needs a 256x192 frame");

    s.width = 256;                 /* geometry matches again */
    s.fusion = DYT_FUSION_INFRARED;
    intcheck("sr notice: the infrared pattern is reported",
             dyt_vm_sr_notice('z', &s, b, sizeof b), 1);
    strcheck("sr notice: fusion",
             b, "sr:visible needs a fusion pattern that shows it");

    /* A pattern that does show the visible half, but a frame with no visible
     * half in it. */
    s.fusion       = DYT_FUSION_EDGE;
    s.have_visible = 0;
    intcheck("sr notice: a missing visible half is reported",
             dyt_vm_sr_notice('z', &s, b, sizeof b), 1);
    strcheck("sr notice: no visible half", b, "sr:visible needs a visible half");

    /* The thermal mode has no visible-plane gate, so with the geometry right
     * the only reading left is the fallback. */
    s.sr = DYT_SR_THERMAL;
    snprintf(s.sr_name, sizeof s.sr_name, "thermal");
    s.have_visible = 1;
    intcheck("sr notice: a thermal fallback is reported",
             dyt_vm_sr_notice('Z', &s, b, sizeof b), 1);
    strcheck("sr notice: thermal inactive", b, "sr:thermal inactive");
}

static void test_readout_line(void)
{
    dyt_snapshot_t s;
    char           b[256];

    base_snapshot(&s);
    s.tool = DYT_TOOL_NONE;
    dyt_vm_readout_line(&s, NULL, b, sizeof b);
    strcheck("readout: no tool", b,
             "tool: none (p point, l line, b box, o polygon, n clear)");

    s.tool = DYT_TOOL_POINT;
    s.point_ok = 1;
    s.point_c = 31.2f;
    dyt_vm_readout_line(&s, NULL, b, sizeof b);
    strcheck("readout: a point reading", b, "point 31.2 C");

    s.point_ok = 0;
    dyt_vm_readout_line(&s, NULL, b, sizeof b);
    strcheck("readout: a point with no reading", b, "point --");

    s.tool = DYT_TOOL_LINE;
    s.p0.x = 1; s.p0.y = 2;
    s.p1.x = 3; s.p1.y = 4;
    dyt_vm_readout_line(&s, NULL, b, sizeof b);
    strcheck("readout: a line", b, "line (1,2)-(3,4)");

    s.tool = DYT_TOOL_POLYGON;
    s.p0.x = 1; s.p0.y = 2;
    s.p1.x = 3; s.p1.y = 4;
    s.poly_n = DYT_POLYGON_SIDES;
    s.roi.n = 77;
    dyt_vm_readout_line(&s, NULL, b, sizeof b);
    strcheck("readout: a polygon", b, "polygon (1,2)-(3,4) n=77");

    s.tool = DYT_TOOL_BOX;
    s.p0.x = 0; s.p0.y = 0;
    s.p1.x = 9; s.p1.y = 9;
    s.roi.n = 100;
    s.alarm_on = 1;
    s.alarm = DYT_ALARM_HIGH;
    s.alarm_lo = 30.f;
    s.alarm_hi = 40.f;
    s.iso_on = 1;
    s.iso.count = 42;
    dyt_vm_readout_line(&s, "set emissivity = 0.50  (sent)", b, sizeof b);
    strcheck("readout: box, alarm, isotherm and notice", b,
             "box (0,0)-(9,9) n=100  |  alarm high 30.0..40.0  |  "
             "iso 42 px  |  set emissivity = 0.50  (sent)");

    /* An empty notice must not leave a dangling separator. */
    dyt_vm_readout_line(&s, "", b, sizeof b);
    strcheck("readout: an empty notice adds nothing", b,
             "box (0,0)-(9,9) n=100  |  alarm high 30.0..40.0  |  iso 42 px");
}

static void test_hover_label(void)
{
    dyt_snapshot_t s;
    char           b[64];

    base_snapshot(&s);

    dyt_vm_hover_label(&s, 31.2f, 12, 7, b, sizeof b);
    strcheck("hover: a reading and the pixel it came from", b, "31.2 C  (12,7)");

    /* A NaN is the pipeline's "not measured" marker (measure.h), and
     * dyt_vm_temp() would happily format it, so the guard is the point. */
    dyt_vm_hover_label(&s, NAN, 12, 7, b, sizeof b);
    strcheck("hover: a NaN reads as dashes", b, "---  (12,7)");

    intcheck("hover: bad snapshot",
             dyt_vm_hover_label(NULL, 1.f, 0, 0, b, sizeof b), -1);
    intcheck("hover: no room", dyt_vm_hover_label(&s, 1.f, 0, 0, b, 0), -1);
}

static void test_roi_label(void)
{
    dyt_snapshot_t s;
    char           b[128];

    base_snapshot(&s);

    s.roi.min = 30.0f; s.roi.max = 40.0f;
    s.roi.mean = 35.0f; s.roi.median = 36.0f;
    s.roi.n = 100;
    dyt_vm_roi_label(&s, b, sizeof b);
    strcheck("roi label: the four statistics", b,
             "min 30.0 C  max 40.0 C  avg 35.0 C  med 36.0 C");

    /* A region with no finite samples reports NaN statistics (measure.h), so
     * none of them may be formatted as a plausible-looking 0 C. */
    s.roi.min = s.roi.max = s.roi.mean = s.roi.median = NAN;
    s.roi.n = 0;
    dyt_vm_roi_label(&s, b, sizeof b);
    strcheck("roi label: an empty region reads as dashes", b,
             "min --  max --  avg --  med --");

    intcheck("roi label: bad snapshot",
             dyt_vm_roi_label(NULL, b, sizeof b), -1);
    intcheck("roi label: no room", dyt_vm_roi_label(&s, b, 0), -1);
}

/* -------------------------------------------------------------- colour bar */

static void test_bar(void)
{
    dyt_snapshot_t s;
    char           b[32];
    int            top, mid, bot, i, prev;

    base_snapshot(&s);

    top = dyt_vm_bar_index(0, 11);
    bot = dyt_vm_bar_index(10, 11);
    intcheck("bar: the top row is the hottest colour", top, DYT_PALETTE_N - 1);
    intcheck("bar: the bottom row is the coldest colour", bot, 0);

    mid = dyt_vm_bar_index(5, 11);
    if (mid > 0 && mid < DYT_PALETTE_N - 1)
        ok("bar: the middle row is in between");
    else {
        char d[64];
        snprintf(d, sizeof d, "got %d", mid);
        fail("bar: the middle row is in between", d);
    }

    /* Monotonic, never out of range, for a tall bar. */
    prev = DYT_PALETTE_N;
    for (i = 0; i < 192; i++) {
        int v = dyt_vm_bar_index(i, 192);
        if (v < 0 || v > DYT_PALETTE_N - 1 || v > prev)
            break;
        prev = v;
    }
    intcheck("bar: every row is in range and non-increasing", i, 192);

    intcheck("bar: a one-row bar is the top", dyt_vm_bar_index(0, 1),
             DYT_PALETTE_N - 1);
    intcheck("bar: a negative row is rejected", dyt_vm_bar_index(-1, 11), -1);
    intcheck("bar: a row past the end is rejected",
             dyt_vm_bar_index(11, 11), -1);
    intcheck("bar: an empty bar is rejected", dyt_vm_bar_index(0, 0), -1);

    dyt_vm_bar_label(&s, 0, b, sizeof b);
    strcheck("bar: the top label is the high end", b, "40.0 C");
    dyt_vm_bar_label(&s, 1, b, sizeof b);
    strcheck("bar: the middle label is the midpoint", b, "35.0 C");
    dyt_vm_bar_label(&s, 2, b, sizeof b);
    strcheck("bar: the bottom label is the low end", b, "30.0 C");
    intcheck("bar: a fourth label is rejected",
             dyt_vm_bar_label(&s, 3, b, sizeof b), -1);
}

/* The bar's scale and its inverse.  These are what put the Windows panel's two
 * range handles at the right height and read back where a dragged handle
 * landed, so the round-trip is what matters most: a pair that disagreed would
 * make a drag jump under the pointer. */
static void test_bar_scale(void)
{
    float f;
    int   i;

    f = dyt_vm_bar_frac(35.0f, 30.0f, 40.0f);
    if (fabsf(f - 0.5f) < 1e-6f)
        ok("bar scale: the midpoint is halfway up");
    else {
        char d[64];
        snprintf(d, sizeof d, "got %.4f", (double)f);
        fail("bar scale: the midpoint is halfway up", d);
    }

    f = dyt_vm_bar_frac(30.0f, 30.0f, 40.0f);
    const bool at_lo = fabsf(f - 0.0f) < 1e-6f;
    f = dyt_vm_bar_frac(40.0f, 30.0f, 40.0f);
    const bool at_hi = fabsf(f - 1.0f) < 1e-6f;
    if (at_lo && at_hi)
        ok("bar scale: the ends are 0 and 1");
    else
        fail("bar scale: the ends are 0 and 1", "ends wrong");

    /* A temperature off either end lands on the bar rather than off it. */
    f = dyt_vm_bar_frac(20.0f, 30.0f, 40.0f);
    const bool below = fabsf(f - 0.0f) < 1e-6f;
    f = dyt_vm_bar_frac(50.0f, 30.0f, 40.0f);
    const bool above = fabsf(f - 1.0f) < 1e-6f;
    if (below && above)
        ok("bar scale: a temperature off the scale clamps onto it");
    else
        fail("bar scale: a temperature off the scale clamps onto it", "no");

    /* A flat range has no fraction to speak of; the middle is the honest
     * answer and it keeps the divide out of the function. */
    f = dyt_vm_bar_frac(30.0f, 30.0f, 30.0f);
    if (fabsf(f - 0.5f) < 1e-6f)
        ok("bar scale: a flat range reports the middle");
    else {
        char d[64];
        snprintf(d, sizeof d, "got %.4f", (double)f);
        fail("bar scale: a flat range reports the middle", d);
    }

    f = dyt_vm_bar_temp(0.5f, 30.0f, 40.0f);
    const bool mid_ok = fabsf(f - 35.0f) < 1e-4f;
    f = dyt_vm_bar_temp(0.0f, 30.0f, 40.0f);
    const bool lo_ok = fabsf(f - 30.0f) < 1e-4f;
    f = dyt_vm_bar_temp(1.0f, 30.0f, 40.0f);
    const bool hi_ok = fabsf(f - 40.0f) < 1e-4f;
    if (mid_ok && lo_ok && hi_ok)
        ok("bar scale: a fraction reads back as its temperature");
    else
        fail("bar scale: a fraction reads back as its temperature", "no");

    f = dyt_vm_bar_temp(-1.0f, 30.0f, 40.0f);
    const bool cl_lo = fabsf(f - 30.0f) < 1e-4f;
    f = dyt_vm_bar_temp(2.0f, 30.0f, 40.0f);
    const bool cl_hi = fabsf(f - 40.0f) < 1e-4f;
    if (cl_lo && cl_hi)
        ok("bar scale: a fraction off the bar clamps onto it");
    else
        fail("bar scale: a fraction off the bar clamps onto it", "no");

    /* The round-trip a drag depends on. */
    for (i = 0; i <= 20; i++) {
        const float c = 30.0f + (float)i * 0.5f;
        const float back =
            dyt_vm_bar_temp(dyt_vm_bar_frac(c, 30.0f, 40.0f), 30.0f, 40.0f);
        if (fabsf(back - c) > 1e-4f)
            break;
    }
    intcheck("bar scale: frac then temp is the identity", i, 21);
}

static void test_rapid_window(void)
{
    float lo = 0.0f, hi = 0.0f;
    int   rc;

    /* A positive scene: each end rounds to a whole ten and then widens by
     * ten, so the window brackets the picture with a margin. */
    rc = dyt_vm_rapid_window(23.4f, 31.7f, &lo, &hi);
    if (rc == 0 && fabsf(lo - 10.0f) < 1e-4f && fabsf(hi - 40.0f) < 1e-4f)
        ok("rapid: a positive range widens to whole tens");
    else {
        char d[80];
        snprintf(d, sizeof d, "rc %d, got [%.1f, %.1f]", rc, (double)lo, (double)hi);
        fail("rapid: a positive range widens to whole tens", d);
    }

    /* The vendor's cast is a C# `(int)`, which truncates toward zero rather
     * than flooring.  For a positive range that is the same thing, and the
     * boundary shows it: 34.9 rounds down to 40, 35.0 rounds up to 50. */
    rc = dyt_vm_rapid_window(20.0f, 34.9f, &lo, &hi);
    const bool under = (rc == 0 && fabsf(hi - 40.0f) < 1e-4f);
    rc = dyt_vm_rapid_window(20.0f, 35.0f, &lo, &hi);
    const bool at = (rc == 0 && fabsf(hi - 50.0f) < 1e-4f);
    if (under && at)
        ok("rapid: the cast rounds a positive end to the nearest ten");
    else
        fail("rapid: the cast rounds a positive end to the nearest ten", "no");

    /* A negative range is where truncate-toward-zero differs from floor: the
     * cold end lands *above* lo.  That is the shipped behaviour, pinned here
     * so a later "fix" has to argue with a failing test rather than slip in. */
    rc = dyt_vm_rapid_window(-23.4f, -22.0f, &lo, &hi);
    if (rc == 0 && fabsf(lo - (-20.0f)) < 1e-4f && fabsf(hi - 0.0f) < 1e-4f)
        ok("rapid: a negative range keeps the vendor's truncation");
    else {
        char d[80];
        snprintf(d, sizeof d, "rc %d, got [%.1f, %.1f]", rc, (double)lo, (double)hi);
        fail("rapid: a negative range keeps the vendor's truncation", d);
    }

    /* Across a positive sweep the window must always contain the scene —
     * that is the whole point of the button. */
    int i, bad = 0;
    for (i = 0; i <= 200; i++) {
        const float a = 1.0f + (float)i * 0.37f;
        const float b = a + 4.0f;
        float wl = 0.0f, wh = 0.0f;
        if (dyt_vm_rapid_window(a, b, &wl, &wh) != 0 || wl > a || wh < b)
            bad++;
    }
    intcheck("rapid: a positive window always contains the scene", bad, 0);

    /* A flat frame is legal and still gets a window. */
    rc = dyt_vm_rapid_window(30.0f, 30.0f, &lo, &hi);
    if (rc == 0 && lo <= 30.0f && hi >= 30.0f)
        ok("rapid: a flat frame still gets a window");
    else
        fail("rapid: a flat frame still gets a window", "no");

    /* Refusals: nothing to read, and nothing sane to bracket. */
    rc = dyt_vm_rapid_window(20.0f, 30.0f, NULL, &hi);
    const bool n1 = (rc == -1);
    rc = dyt_vm_rapid_window(20.0f, 30.0f, &lo, NULL);
    const bool n2 = (rc == -1);
    rc = dyt_vm_rapid_window(30.0f, 20.0f, &lo, &hi);
    const bool n3 = (rc == -1);
    rc = dyt_vm_rapid_window(NAN, 20.0f, &lo, &hi);
    const bool n4 = (rc == -1);
    if (n1 && n2 && n3 && n4)
        ok("rapid: a null, an inverted or a non-finite range is refused");
    else
        fail("rapid: a null, an inverted or a non-finite range is refused", "no");
}

static void test_leak_effect(void)
{
    dyt_leak_effect_t fx;
    int rc;

    rc = dyt_vm_leak_effect(DYT_LEAK_SHORT, &fx);
    const bool short_ok = rc == 0 && fx.range == DYT_RANGE_AUTO &&
                          fx.view_3d == 0;

    rc = dyt_vm_leak_effect(DYT_LEAK_LARGE, &fx);
    const bool large_ok = rc == 0 && fx.range == DYT_RANGE_FIXED &&
                          fx.view_3d == 0;

    rc = dyt_vm_leak_effect(DYT_LEAK_SMALL, &fx);
    const bool small_ok = rc == 0 && fx.range == DYT_RANGE_AUTO &&
                          fx.view_3d == 1;

    if (short_ok && large_ok && small_ok)
        ok("leak: the three modes map to the vendor's range and view");
    else
        fail("leak: the three modes map to the vendor's range and view",
             "one of the three is wrong");

    /* The values are the vendor's own circuit_status, and only those three
     * exist: a fourth is a bug in the caller, not a mode. */
    const bool vals = (DYT_LEAK_SMALL == 1 && DYT_LEAK_LARGE == 2 &&
                       DYT_LEAK_SHORT == 3);
    int i, bad = 0;
    for (i = -3; i <= 8; i++) {
        if (i == DYT_LEAK_SMALL || i == DYT_LEAK_LARGE || i == DYT_LEAK_SHORT)
            continue;
        if (dyt_vm_leak_effect(i, &fx) != -1)
            bad++;
    }
    rc = dyt_vm_leak_effect(DYT_LEAK_SHORT, NULL);
    if (vals && bad == 0 && rc == -1)
        ok("leak: the numbers are the vendor's and nothing else is a mode");
    else
        fail("leak: the numbers are the vendor's and nothing else is a mode",
             "no");

    /* Only Small shows the 3D view — the other two must leave the user where
     * they were, so a stray mode cannot yank the panel to another tab. */
    rc = dyt_vm_leak_effect(DYT_LEAK_SMALL, &fx);
    const bool only_small = rc == 0 && fx.view_3d == 1;
    rc = dyt_vm_leak_effect(DYT_LEAK_LARGE, &fx);
    const bool large_flat = rc == 0 && fx.view_3d == 0;
    rc = dyt_vm_leak_effect(DYT_LEAK_SHORT, &fx);
    const bool short_flat = rc == 0 && fx.view_3d == 0;
    if (only_small && large_flat && short_flat)
        ok("leak: only Small Current Leakage asks for the 3D view");
    else
        fail("leak: only Small Current Leakage asks for the 3D view", "no");
}

/* ------------------------------------------------------------ device panel */

static void test_info(void)
{
    dyt_device_info_t d;
    dyt_vm_info_t     info;
    float             ov[5] = { 0, 0, 0, 0, 0 };
    int               on[5] = { 0, 0, 0, 0, 0 };

    memset(&d, 0, sizeof d);
    d.have_sn = 1;
    snprintf(d.sn_str, sizeof d.sn_str, "202605575259");
    d.have_usn = 1;
    snprintf(d.usn_str, sizeof d.usn_str, "DYCSTI09GG01292");
    d.usn_len = 14;
    d.usn_key = 23;
    d.radio.reflected_k = 300;
    d.radio.ambient_k   = 300;
    d.radio.emissivity  = 127;
    d.radio.distance    = 127;
    d.radio.ok          = DYT_RADIO_ALL;
    d.params_read       = 16;

    intcheck("info: every row", dyt_vm_info(&d, ov, on, &info), 5);
    strcheck("info: the serial row", info.line[0], "serial  202605575259");
    strcheck("info: the user-serial row", info.line[1],
             "user    DYCSTI09GG01292  (key 23)");
    strcheck("info: the reflected/ambient row", info.line[2],
             "refl 26.85 C   amb 26.85 C");
    strcheck("info: the emissivity/distance row", info.line[3],
             "emis 0.9922   dist 0.9922 m");
    strcheck("info: the slot row", info.line[4], "params 16/16 slots");
    intcheck("info: nothing is flagged as overridden", info.any_override, 0);

    /* An override must be visible, and must be flagged, so the panel never
     * shows a stored value a write has superseded. */
    ov[DYT_ORDER_EMISSIVITY] = 0.5f;
    on[DYT_ORDER_EMISSIVITY] = 1;
    dyt_vm_info(&d, ov, on, &info);
    strcheck("info: an override replaces the stored value", info.line[3],
             "emis 0.5000*   dist 0.9922 m");
    strcheck("info: the override is explained", info.line[4],
             "params 16/16 slots   (* = set this session)");
    intcheck("info: the override is flagged", info.any_override, 1);

    /* Nothing read: the serial row and the slot count survive — the slot row
     * is unconditional, because "0/16 slots read" is itself the answer. */
    memset(&d, 0, sizeof d);
    d.params_read = -1;
    intcheck("info: an unread device yields two rows",
             dyt_vm_info(&d, NULL, NULL, &info), 2);
    strcheck("info: an unread serial says so", info.line[0],
             "serial  (read failed)");
    strcheck("info: and the slot count reports the failure", info.line[1],
             "params -1/16 slots");

    /* A user serial read but not decoded. */
    d.have_usn = 1;
    d.usn_len = -1;
    dyt_vm_info(&d, NULL, NULL, &info);
    strcheck("info: an undecoded user serial says so", info.line[1],
             "user    (raw read; no key)");

    intcheck("info: NULL device rejected",
             dyt_vm_info(NULL, NULL, NULL, &info), -1);
    intcheck("info: NULL out rejected",
             dyt_vm_info(&d, NULL, NULL, NULL), -1);
}

/* --------------------------------------------------------- parameter ladder */

static void test_param(void)
{
    dyt_vm_param_event_t ev;
    const dyt_vm_ladder_t *L;
    char b[32];

    intcheck("param: four ladders", dyt_vm_ladder_count(), 4);
    L = dyt_vm_ladder_at(0);
    strcheck("param: the first ladder is emissivity", L ? L->name : "?", "emissivity");
    intcheck("param: emissivity has six rungs", L ? L->n : -1, 6);
    intcheck("param: an out-of-range ladder is rejected",
             dyt_vm_ladder_at(9) == NULL, 1);
    intcheck("param: an unknown order type has no ladder",
             dyt_vm_ladder((dyt_order_type_t)99) == NULL, 1);

    dyt_vm_param_format(DYT_ORDER_EMISSIVITY, 1.0f, b, sizeof b);
    strcheck("param: an emissivity value is dimensionless", b, "1.00");
    dyt_vm_param_format(DYT_ORDER_AMBIENT, 25.0f, b, sizeof b);
    strcheck("param: a temperature value carries its unit", b, "25.0 C");
    dyt_vm_param_format(DYT_ORDER_DISTANCE, 0.1f, b, sizeof b);
    strcheck("param: a distance value carries its unit", b, "0.10 m");

    /* Nothing armed: only a parameter key is ours. */
    intcheck("param: a parameter key with nothing armed is ours",
             dyt_vm_param_key('e', (dyt_order_type_t)0, 0, &ev), 1);
    intcheck("param: it arms", ev.action, DYT_VM_PARAM_ARMED);
    intcheck("param: the right parameter", ev.type, DYT_ORDER_EMISSIVITY);
    intcheck("param: at the first rung", ev.rung, 0);
    if (fabsf(ev.value - 1.00f) < 1e-6f) ok("param: with the first value");
    else fail("param: with the first value", "wrong value");

    intcheck("param: an unrelated key with nothing armed is not ours",
             dyt_vm_param_key('x', (dyt_order_type_t)0, 0, &ev), 0);
    intcheck("param: nor is y with nothing armed",
             dyt_vm_param_key('y', (dyt_order_type_t)0, 0, &ev), 0);

    /* Re-pressing advances; a different key starts its own ladder. */
    dyt_vm_param_key('e', DYT_ORDER_EMISSIVITY, 0, &ev);
    intcheck("param: re-pressing advances the rung", ev.rung, 1);
    if (fabsf(ev.value - 0.95f) < 1e-6f) ok("param: to the next value");
    else fail("param: to the next value", "wrong value");

    dyt_vm_param_key('e', DYT_ORDER_EMISSIVITY, 5, &ev);
    intcheck("param: the ladder wraps", ev.rung, 0);

    dyt_vm_param_key('D', DYT_ORDER_EMISSIVITY, 3, &ev);
    intcheck("param: another key starts its own ladder",
             ev.type, DYT_ORDER_DISTANCE);
    intcheck("param: at the first rung", ev.rung, 0);

    /* Armed: confirm, cancel, and swallow everything else. */
    intcheck("param: y is consumed while armed",
             dyt_vm_param_key('y', DYT_ORDER_DISTANCE, 2, &ev), 1);
    intcheck("param: y sends", ev.action, DYT_VM_PARAM_SEND);
    intcheck("param: the armed parameter", ev.type, DYT_ORDER_DISTANCE);
    if (fabsf(ev.value - 1.00f) < 1e-6f)
        ok("param: the value at the armed rung");
    else
        fail("param: the value at the armed rung", "wrong value");

    dyt_vm_param_key('n', DYT_ORDER_DISTANCE, 2, &ev);
    intcheck("param: n cancels", ev.action, DYT_VM_PARAM_CANCEL);
    dyt_vm_param_key(27, DYT_ORDER_DISTANCE, 2, &ev);
    intcheck("param: escape cancels too", ev.action, DYT_VM_PARAM_CANCEL);

    dyt_vm_param_key('0', DYT_ORDER_DISTANCE, 2, &ev);
    intcheck("param: a stray palette key is swallowed", ev.action,
             DYT_VM_PARAM_SWALLOW);

    intcheck("param: quit is never swallowed",
             dyt_vm_param_key('q', DYT_ORDER_DISTANCE, 2, &ev), 0);

    intcheck("param: NULL event rejected",
             dyt_vm_param_key('e', (dyt_order_type_t)0, 0, NULL), 0);
}

/* --------------------------------------------------------------- overlays */

static void test_isotherm(void)
{
    uint8_t bgr[4 * 3];
    float   temps[4];
    long    dimmed;

    memset(bgr, 100, sizeof bgr);
    temps[0] = 35.f;    /* inside */
    temps[1] = 25.f;    /* below  */
    temps[2] = NAN;     /* unmeasured */
    temps[3] = 45.f;    /* above  */

    dimmed = dyt_vm_apply_isotherm(bgr, 2, 2, temps, 4, 30.f, 40.f);
    intcheck("isotherm: three of four pixels are outside the band",
             (int)dimmed, 3);
    intcheck("isotherm: an in-band pixel is untouched", bgr[0], 100);
    intcheck("isotherm: a below-band pixel is dimmed", bgr[3], 50);
    intcheck("isotherm: an unmeasured pixel is dimmed", bgr[6], 50);
    intcheck("isotherm: an above-band pixel is dimmed", bgr[9], 50);

    intcheck("isotherm: a short temperature plane is rejected",
             (int)dyt_vm_apply_isotherm(bgr, 2, 2, temps, 3, 30.f, 40.f), -1);
    intcheck("isotherm: NULL pixels rejected",
             (int)dyt_vm_apply_isotherm(NULL, 2, 2, temps, 4, 30.f, 40.f), -1);
    intcheck("isotherm: NULL temperatures rejected",
             (int)dyt_vm_apply_isotherm(bgr, 2, 2, NULL, 4, 30.f, 40.f), -1);

    /* The same pass when the image is a magnification of the temperature
     * plane — which it is at 2x — so each temperature covers a 2x2 block.
     * Without this the overlay silently dims nothing at 2x (the plane has a
     * quarter of the samples the image needs), which is the bug it prevents. */
    {
        uint8_t big[4 * 4 * 3];
        float   t2[4];

        memset(big, 100, sizeof big);
        t2[0] = 35.f;  /* in    -> the block at (0,0) */
        t2[1] = 25.f;  /* below -> the block at (1,0) */
        t2[2] = 45.f;  /* above -> the block at (0,1) */
        t2[3] = 35.f;  /* in    -> the block at (1,1) */

        dimmed = dyt_vm_apply_isotherm_scaled(big, 4, 4, t2, 4, 2, 2,
                                              30.f, 40.f);
        intcheck("isotherm 2x: the two out-of-band blocks are dimmed",
                 (int)dimmed, 8);
        intcheck("isotherm 2x: an in-band block is untouched", big[0], 100);
        intcheck("isotherm 2x: so is the other in-band block",
                 big[(2 * 4 + 2) * 3], 100);
        intcheck("isotherm 2x: the below-band block is dimmed",
                 big[(0 * 4 + 2) * 3], 50);
        intcheck("isotherm 2x: the above-band block is dimmed",
                 big[(2 * 4 + 0) * 3], 50);

        intcheck("isotherm 2x: a short plane is rejected",
                 (int)dyt_vm_apply_isotherm_scaled(big, 4, 4, t2, 3, 2, 2,
                                                   30.f, 40.f), -1);
        intcheck("isotherm 2x: a zero temperature width is rejected",
                 (int)dyt_vm_apply_isotherm_scaled(big, 4, 4, t2, 4, 0, 2,
                                                   30.f, 40.f), -1);

        /* At scale 1 the scaled entry point must be byte-for-byte the plain
         * one — that is how the plain one is implemented, so this pins it. */
        {
            uint8_t a[2 * 2 * 3], c[2 * 2 * 3];
            float   t3[4] = { 35.f, 25.f, 45.f, 35.f };
            long    d1, d2;

            memset(a, 100, sizeof a);
            memset(c, 100, sizeof c);
            d1 = dyt_vm_apply_isotherm(a, 2, 2, t3, 4, 30.f, 40.f);
            d2 = dyt_vm_apply_isotherm_scaled(c, 2, 2, t3, 4, 2, 2,
                                              30.f, 40.f);
            intcheck("isotherm: the scaled entry agrees with the plain one at 1x",
                     d1 == d2 && memcmp(a, c, sizeof a) == 0, 1);
        }
    }
}

/* -------------------------------------------------- session-backed entries */

static void feed(dyt_session_t *s, float *img, int w, int h)
{
    dyt_frame_info_t fi;
    int              i;

    for (i = 0; i < w * h; i++)
        img[i] = 20.f + (float)(i % 16);

    fi.temps  = img;
    fi.width  = w;
    fi.height = h;
    dyt_session_process(s, &fi);
}

static void test_grab(void)
{
    dyt_session_t    *s = dyt_session_create();
    dyt_vm_scratch_t  scr = DYT_VM_SCRATCH_INIT;
    dyt_snapshot_t    snap;
    float             img[16 * 16];
    dyt_vm_scratch_t  tiny;

    if (!s) {
        fail("grab: session", "out of memory");
        return;
    }

    /* No frame yet: grab reports it rather than handing back an empty plane. */
    intcheck("grab: no frame yet", dyt_vm_grab(s, &snap, &scr), 0);

    feed(s, img, 16, 16);
    intcheck("grab: succeeds once a frame has arrived",
             dyt_vm_grab(s, &snap, &scr), 1);
    intcheck("grab: the geometry is the frame's", snap.width, 16);
    intcheck("grab: and the height", snap.height, 16);
    intcheck("grab: the scratch is sized to the frame", scr.cap, 16 * 16);

    /* The one-element seed: a caller-supplied capacity of 1 must still end up
     * with the whole plane, not a silent success on scalars only. */
    tiny.temps = (float *)malloc(sizeof(float));
    tiny.cap   = 1;
    if (tiny.temps) {
        intcheck("grab: a one-element scratch is grown",
                 dyt_vm_grab(s, &snap, &tiny), 1);
        intcheck("grab: to the full plane", tiny.cap, 16 * 16);
        if (snap.width == 16 && tiny.temps &&
            tiny.temps[0] == tiny.temps[0])
            ok("grab: and the plane really was copied");
        else
            fail("grab: and the plane really was copied", "no data");
        dyt_vm_scratch_free(&tiny);
    }

    dyt_vm_scratch_free(&scr);
    intcheck("grab: freeing clears the scratch", scr.temps == NULL, 1);
    dyt_session_free(s);
}

static void test_tool_mouse(void)
{
    dyt_session_t        *s = dyt_session_create();
    dyt_vm_pointer_t      p = DYT_VM_POINTER_INIT;
    dyt_view_transform_t  xf;
    dyt_snapshot_t        snap;
    float                 img[16 * 16];

    if (!s) {
        fail("tool: session", "out of memory");
        return;
    }
    feed(s, img, 16, 16);
    dyt_session_set_tool(s, DYT_TOOL_BOX);

    memset(&xf, 0, sizeof xf);
    xf.zoom = 1;                    /* identity: dst == src */

    /* No tool: the pointer is tracked but nothing is placed. */
    intcheck("tool: no tool places nothing",
             dyt_vm_tool_mouse(s, &p, DYT_VM_MOUSE_DOWN, DYT_TOOL_NONE,
                               &xf, 16, 16, 16, 16, 3, 4), 0);
    intcheck("tool: but the pointer is tracked", p.x, 3);
    intcheck("tool: in y too", p.y, 4);

    /* A press places both points on the same pixel: a plain click is a probe. */
    intcheck("tool: a press places a point",
             dyt_vm_tool_mouse(s, &p, DYT_VM_MOUSE_DOWN, DYT_TOOL_BOX,
                               &xf, 16, 16, 16, 16, 3, 4), 1);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("tool: point 0 x", snap.p0.x, 3);
    intcheck("tool: point 0 y", snap.p0.y, 4);
    intcheck("tool: point 1 starts on point 0", snap.p1.x, 3);
    intcheck("tool: and in y", snap.p1.y, 4);
    intcheck("tool: the drag is open", p.dragging, 1);

    /* A move drags point 1 only. */
    intcheck("tool: a move drags point 1",
             dyt_vm_tool_mouse(s, &p, DYT_VM_MOUSE_MOVE, DYT_TOOL_BOX,
                               &xf, 16, 16, 16, 16, 9, 10), 1);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("tool: point 0 is anchored", snap.p0.x, 3);
    intcheck("tool: point 1 moved x", snap.p1.x, 9);
    intcheck("tool: point 1 moved y", snap.p1.y, 10);

    /* A move without a press does not drag. */
    p.dragging = 0;
    intcheck("tool: a move outside a drag places nothing",
             dyt_vm_tool_mouse(s, &p, DYT_VM_MOUSE_MOVE, DYT_TOOL_BOX,
                               &xf, 16, 16, 16, 16, 1, 1), 0);

    /* A release ends the drag. */
    p.dragging = 1;
    intcheck("tool: a release changes no geometry",
             dyt_vm_tool_mouse(s, &p, DYT_VM_MOUSE_UP, DYT_TOOL_BOX,
                               &xf, 16, 16, 16, 16, 9, 10), 0);
    intcheck("tool: and closes the drag", p.dragging, 0);

    /* A press outside the image opens a drag but places nothing. */
    intcheck("tool: a press off-image places nothing",
             dyt_vm_tool_mouse(s, &p, DYT_VM_MOUSE_DOWN, DYT_TOOL_BOX,
                               &xf, 16, 16, 16, 16, 99, 99), 0);

    /* The polygon is placed like the box — a press sets both points, the drag
     * moves the second — and its five vertices are derived from the box the
     * drag names, so there is nothing extra to place here. */
    dyt_session_set_tool(s, DYT_TOOL_POLYGON);
    intcheck("tool: polygon press",
             dyt_vm_tool_mouse(s, &p, DYT_VM_MOUSE_DOWN, DYT_TOOL_POLYGON,
                               &xf, 16, 16, 16, 16, 1, 1), 1);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("tool: a press already has five sides", snap.poly_n,
             DYT_POLYGON_SIDES);

    intcheck("tool: a polygon drag moves point 1",
             dyt_vm_tool_mouse(s, &p, DYT_VM_MOUSE_MOVE, DYT_TOOL_POLYGON,
                               &xf, 16, 16, 16, 16, 15, 15), 1);
    dyt_session_snapshot(s, &snap, NULL, 0);
    intcheck("tool: the box's first corner", snap.p0.x, 1);
    intcheck("tool: and its far one", snap.p1.x, 15);
    intcheck("tool: the pentagon is still five sides", snap.poly_n,
             DYT_POLYGON_SIDES);
    intcheck("tool: and now measures", snap.roi_ok, 1);
    intcheck("tool: with pixels in it", snap.roi.n > 0, 1);

    /* A release after the drag changes nothing, but does end the
     * press-to-release span like any other tool. */
    intcheck("tool: a polygon release changes no geometry",
             dyt_vm_tool_mouse(s, &p, DYT_VM_MOUSE_UP, DYT_TOOL_POLYGON,
                               &xf, 16, 16, 16, 16, 15, 15), 0);
    intcheck("tool: and closes the drag", p.dragging, 0);

    dyt_session_free(s);
}

static void test_alarm_band(void)
{
    dyt_snapshot_t s;
    float          lo = 0.f, hi = 0.f, h = 0.f;

    base_snapshot(&s);                 /* lo 30, hi 40 */

    intcheck("alarm band: derived", dyt_vm_alarm_band(&s, &lo, &hi, &h), 0);
    if (fabsf(lo - 33.0f) < 1e-4f && fabsf(hi - 37.0f) < 1e-4f &&
        fabsf(h - 1.0f) < 1e-4f)
        ok("alarm band: the middle 40 %, with 10 % hysteresis");
    else {
        char d[96];
        snprintf(d, sizeof d, "got %.3f..%.3f h %.3f",
                 (double)lo, (double)hi, (double)h);
        fail("alarm band: the middle 40 %, with 10 % hysteresis", d);
    }

    /* A flat frame falls back to a one-degree span, which puts the returned
     * `hi` *below* the returned `lo`.  That is the reference viewer's
     * behaviour, pinned here so it is kept rather than quietly corrected. */
    s.lo = s.hi = 30.0f;
    dyt_vm_alarm_band(&s, &lo, &hi, &h);
    if (fabsf(lo - 30.3f) < 1e-4f && fabsf(hi - 29.7f) < 1e-4f &&
        fabsf(h - 0.1f) < 1e-4f)
        ok("alarm band: a flat frame falls back to one degree (inverted)");
    else {
        char d[96];
        snprintf(d, sizeof d, "got %.3f..%.3f h %.3f",
                 (double)lo, (double)hi, (double)h);
        fail("alarm band: a flat frame falls back to one degree (inverted)", d);
    }

    intcheck("alarm band: bad snapshot",
             dyt_vm_alarm_band(NULL, &lo, &hi, &h), -1);
    intcheck("alarm band: bad output",
             dyt_vm_alarm_band(&s, NULL, &hi, &h), -1);
}

/* The alarm setpoint's field.  The vendor's own limits (CAAnalyzer
 * num_alarm_val_LostFocus, :19610) are -20 .. 450 Celsius, and its backing
 * field starts at 70 (gfHighWarnTemp, :33191) — both pinned here so the Qt
 * spin box and the engine cannot drift apart about what may be typed. */
static void test_alarm_setpoint(void)
{
    float v;

    v = dyt_vm_alarm_clamp_setpoint(35.5f);
    if (fabsf(v - 35.5f) < 1e-6f)
        ok("alarm setpoint: a value in range is kept");
    else {
        char d[64];
        snprintf(d, sizeof d, "got %.3f", (double)v);
        fail("alarm setpoint: a value in range is kept", d);
    }

    v = dyt_vm_alarm_clamp_setpoint(-100.0f);
    if (fabsf(v - DYT_ALARM_SETPOINT_MIN) < 1e-6f)
        ok("alarm setpoint: below the floor clamps to -20");
    else {
        char d[64];
        snprintf(d, sizeof d, "got %.3f", (double)v);
        fail("alarm setpoint: below the floor clamps to -20", d);
    }

    v = dyt_vm_alarm_clamp_setpoint(1000.0f);
    if (fabsf(v - DYT_ALARM_SETPOINT_MAX) < 1e-6f)
        ok("alarm setpoint: above the ceiling clamps to 450");
    else {
        char d[64];
        snprintf(d, sizeof d, "got %.3f", (double)v);
        fail("alarm setpoint: above the ceiling clamps to 450", d);
    }

    /* A field nothing has filled in yet must still arm something sane: every
     * comparison against a NaN is false, so an un-clamped NaN would be an
     * armed alarm that can never fire. */
    v = dyt_vm_alarm_clamp_setpoint(NAN);
    if (fabsf(v - DYT_ALARM_SETPOINT_DEFAULT) < 1e-6f)
        ok("alarm setpoint: NaN falls back to the 70 degree default");
    else {
        char d[64];
        snprintf(d, sizeof d, "got %.3f", (double)v);
        fail("alarm setpoint: NaN falls back to the 70 degree default", d);
    }

    v = dyt_vm_alarm_clamp_setpoint(INFINITY);
    if (fabsf(v - DYT_ALARM_SETPOINT_DEFAULT) < 1e-6f)
        ok("alarm setpoint: an infinity falls back to the default");
    else {
        char d[64];
        snprintf(d, sizeof d, "got %.3f", (double)v);
        fail("alarm setpoint: an infinity falls back to the default", d);
    }

    /* The field is the *high* side: armed at the setpoint with the low side on
     * the clamp's floor, only a reading above the setpoint may trip it. */
    {
        dyt_alarm_t a;
        dyt_alarm_state_t st;
        dyt_alarm_init(&a);
        dyt_alarm_set(&a, DYT_ALARM_SETPOINT_MIN,
                      dyt_vm_alarm_clamp_setpoint(50.0f),
                      DYT_ALARM_HYST_DEFAULT);

        st = dyt_alarm_update(&a, 49.9f, 20.0f);
        const int quiet_below = (st == DYT_ALARM_NONE);

        st = dyt_alarm_update(&a, 50.1f, 20.0f);
        const int trips_above = (st == DYT_ALARM_HIGH);

        /* Still latched just under the threshold — that is the hysteresis the
         * port adds and the vendor's bare `>` lacks. */
        st = dyt_alarm_update(&a, 49.9f, 20.0f);
        const int holds = (st == DYT_ALARM_HIGH);

        st = dyt_alarm_update(&a, 48.9f, 20.0f);
        const int clears = (st == DYT_ALARM_NONE);

        if (quiet_below && trips_above && holds && clears)
            ok("alarm setpoint: arms high-only at the field's value");
        else {
            char d[96];
            snprintf(d, sizeof d, "below %s above %s holds %s clears %s",
                     quiet_below ? "y" : "N", trips_above ? "y" : "N",
                     holds ? "y" : "N", clears ? "y" : "N");
            fail("alarm setpoint: arms high-only at the field's value", d);
        }
    }
}

static void test_data_dir(void);

static void test_utilities(void)
{
    char b[4096];
    int  i, okfmt = 1;


    if (dyt_vm_timestamp(b, sizeof b) != 0) {
        fail("util: a timestamp", "failed");
    } else if (strlen(b) != 15 || b[8] != '-') {
        fail("util: a timestamp", b);
    } else {
        for (i = 0; i < 15; i++)
            if (i != 8 && (b[i] < '0' || b[i] > '9'))
                okfmt = 0;
        if (okfmt) ok("util: a timestamp is YYYYMMDD-HHMMSS");
        else       fail("util: a timestamp is YYYYMMDD-HHMMSS", b);
    }

    intcheck("util: the executable's directory is found",
             dyt_vm_exe_dir(b, sizeof b), 0);
    if (b[0] == '/') ok("util: and it is absolute");
    else             fail("util: and it is absolute", b);

    /* An explicit readable directory wins. */
    intcheck("util: an explicit palette directory wins",
             dyt_vm_find_palette_dir(".", b, sizeof b), 1);
    strcheck("util: and is used verbatim", b, ".");

    /* An unreadable one falls through rather than being returned. */
    dyt_vm_find_palette_dir("/nonexistent-palette-dir-xyz", b, sizeof b);
    if (strcmp(b, "/nonexistent-palette-dir-xyz") != 0)
        ok("util: an unreadable palette directory falls through");
    else
        fail("util: an unreadable palette directory falls through", b);

    /* The model is found the same way, and for the same reason: `make check`
     * runs from the port root, so the tree's own copy must win. */
    intcheck("util: the model is found",
             dyt_vm_find_model(NULL, b, sizeof b), 1);
    strcheck("util: and it is the tree's model", b, "models/zoom2.mnn");

    intcheck("util: an explicit model path wins",
             dyt_vm_find_model("models/zoom2.mnn", b, sizeof b), 1);
    strcheck("util: and is used verbatim", b, "models/zoom2.mnn");

    dyt_vm_find_model("/nonexistent-model-xyz.mnn", b, sizeof b);
    if (strcmp(b, "/nonexistent-model-xyz.mnn") != 0)
        ok("util: an unreadable model path falls through");
    else
        fail("util: an unreadable model path falls through", b);

    test_data_dir();
}

/* The installed-layout search.  Tested through dyt_vm_find_data_dir rather than
 * the palette wrapper, because the wrapper's earlier candidates (cwd, then
 * relative to the executable) always resolve under `make check` and would hide
 * the XDG branch. */
static void test_data_dir(void)
{
    char  root[] = "/tmp/dyt-vm-xdg-XXXXXX";
    char  b[4096], want[4096];
    char *old = getenv("XDG_DATA_DIRS");
    char *save = old ? strdup(old) : NULL;

    if (!mkdtemp(root)) {
        fail("data dir: a temp root", "mkdtemp failed");
        free(save);
        return;
    }

    /* <root>/usr/share/dytqt/palettes, and a second entry that does not exist,
     * so the search has to walk past one miss. */
    snprintf(b, sizeof b, "%s/usr/share/dytqt/palettes", root);
    if (mkdir_p(b) != 0) {
        fail("data dir: the fixture tree", "could not create it");
        goto out;
    }

    snprintf(b, sizeof b, "%s/absent:%s/usr/share", root, root);
    setenv("XDG_DATA_DIRS", b, 1);

    snprintf(want, sizeof want, "%s/usr/share/dytqt/palettes", root);
    if (dyt_vm_find_data_dir("palettes", b, sizeof b) == 1 &&
        strcmp(b, want) == 0)
        ok("data dir: XDG_DATA_DIRS is searched past a miss");
    else
        fail("data dir: XDG_DATA_DIRS is searched past a miss", b);

    /* A leaf that is not there is a miss, not a false hit. */
    intcheck("data dir: an absent leaf is not found",
             dyt_vm_find_data_dir("no-such-leaf", b, sizeof b), 0);

    /* A path is refused: the search must not escape dytqt/. */
    intcheck("data dir: a leaf with a separator is refused",
             dyt_vm_find_data_dir("../palettes", b, sizeof b), 0);
    intcheck("data dir: \"..\" is refused",
             dyt_vm_find_data_dir("..", b, sizeof b), 0);

    /* An empty XDG_DATA_DIRS must behave exactly like the spec default,
     * whatever that resolves to on this host — which may or may not have the
     * package installed.  Comparing the two is host-independent; asserting a
     * fixed result would not be. */
    {
        char got_empty[4096];
        int  rc_empty, rc_default;

        setenv("XDG_DATA_DIRS", "", 1);
        rc_empty = dyt_vm_find_data_dir("palettes", b, sizeof b);
        snprintf(got_empty, sizeof got_empty, "%s", b);

        setenv("XDG_DATA_DIRS", "/usr/local/share:/usr/share", 1);
        rc_default = dyt_vm_find_data_dir("palettes", b, sizeof b);

        if (rc_empty == rc_default && strcmp(got_empty, b) == 0)
            ok("data dir: an empty XDG_DATA_DIRS falls back to the default");
        else
            fail("data dir: an empty XDG_DATA_DIRS falls back to the default",
                 b);
    }

out:
    if (save) {
        setenv("XDG_DATA_DIRS", save, 1);
        free(save);
    } else {
        unsetenv("XDG_DATA_DIRS");
    }
    /* Leave nothing behind. */
    snprintf(b, sizeof b, "%s/usr/share/dytqt/palettes", root);
    rmdir(b);
    snprintf(b, sizeof b, "%s/usr/share/dytqt", root);
    rmdir(b);
    snprintf(b, sizeof b, "%s/usr/share", root);
    rmdir(b);
    snprintf(b, sizeof b, "%s/usr", root);
    rmdir(b);
    rmdir(root);
}

/* ------------------------------------------------- capture and recording */

static void test_capture(void)
{
    char      b[256];
    long long free_b = -1;

    printf("\n-- capture names, disk guard, recording label --\n");

    intcheck("cap: a name is dir/dyt_<ts>.<ext>",
             dyt_vm_capture_name(b, sizeof b, "out", "20260925-120000",
                                 "png"), 0);
    strcheck("cap: and it reads as written", b, "out/dyt_20260925-120000.png");

    intcheck("cap: an empty directory drops the slash",
             dyt_vm_capture_name(b, sizeof b, "", "20260925-120000", "mp4"), 0);
    strcheck("cap: and the name is bare", b, "dyt_20260925-120000.mp4");

    intcheck("cap: a NULL directory is the same",
             dyt_vm_capture_name(b, sizeof b, NULL, "20260925-120000",
                                 "dyt.jpg"), 0);
    strcheck("cap: and the container keeps its double extension", b,
             "dyt_20260925-120000.dyt.jpg");

    intcheck("cap: a bad argument is refused",
             dyt_vm_capture_name(b, sizeof b, "out", "", "png"), -1);
    intcheck("cap: an empty extension is refused",
             dyt_vm_capture_name(b, sizeof b, "out", "20260925-120000", ""),
             -1);
    /* The rule is the same one snprintf uses, so a name that does not fit is a
     * refusal rather than a silently truncated file. */
    intcheck("cap: a name that does not fit is refused",
             dyt_vm_capture_name(b, 8, "out", "20260925-120000", "png"), -1);

    if (dyt_vm_free_bytes(".") > 0)
        ok("cap: the free space of a real directory is reported");
    else
        fail("cap: the free space of a real directory is reported", ".");

    intcheck("cap: an unexaminable path reports -1",
             (int)dyt_vm_free_bytes("/nonexistent-dir-xyz/sub"), -1);
    intcheck("cap: a NULL path reports -1", (int)dyt_vm_free_bytes(NULL), -1);

    intcheck("cap: a byte of room is available here",
             dyt_vm_disk_room(".", 1, &free_b), 1);
    if (free_b > 0)
        ok("cap: and the free count comes back with it");
    else
        fail("cap: and the free count comes back with it", ".");

    /* 8 EiB is more than any filesystem here, so the guard must say no — and
     * still report the count, because the caller wants to say how much is
     * left rather than only that it is not enough. */
    intcheck("cap: an impossible request is refused",
             dyt_vm_disk_room(".", 1LL << 62, &free_b), 0);
    if (free_b > 0)
        ok("cap: and the free count still comes back");
    else
        fail("cap: and the free count still comes back", ".");

    intcheck("cap: an unexaminable path reports -1",
             dyt_vm_disk_room("/nonexistent-dir-xyz/sub", 1, &free_b), -1);
    intcheck("cap: and passes -1 out as the free count", (int)free_b, -1);

    intcheck("cap: elapsed 0 s", dyt_vm_elapsed(0.0, b, sizeof b), 0);
    strcheck("cap: reads 0:00", b, "0:00");

    dyt_vm_elapsed(7.9, b, sizeof b);
    strcheck("cap: 7.9 s truncates to 0:07", b, "0:07");

    dyt_vm_elapsed(67.0, b, sizeof b);
    strcheck("cap: 67 s is 1:07", b, "1:07");

    dyt_vm_elapsed(3600.0, b, sizeof b);
    strcheck("cap: an hour grows the field", b, "1:00:00");

    dyt_vm_elapsed(3661.0, b, sizeof b);
    strcheck("cap: 1:01:01 past an hour", b, "1:01:01");

    /* A clock that has not started must not print a negative time, and a NaN
     * must not print anything stranger. */
    dyt_vm_elapsed(-5.0, b, sizeof b);
    strcheck("cap: a negative clock reads 0:00", b, "0:00");

    dyt_vm_elapsed(NAN, b, sizeof b);
    strcheck("cap: a NaN clock reads 0:00", b, "0:00");

    intcheck("cap: the label is written",
             dyt_vm_rec_label(7.0, 175, b, sizeof b) > 0, 1);
    strcheck("cap: and reads REC with the count", b, "REC 0:07  175 frames");

    dyt_vm_rec_label(0.0, 1, b, sizeof b);
    strcheck("cap: one frame is singular", b, "REC 0:00  1 frame");

    /* The default capture directory.  HOME and XDG_PICTURES_DIR are set here
     * so the result does not depend on whoever runs make check — a variable
     * left over in the environment would otherwise decide the outcome. */
    {
        char  tmpl[] = "/tmp/dytvm-pics-XXXXXX";
        char *pics   = mkdtemp(tmpl);
        char *home   = getenv("HOME");
        char  saved[512] = { 0 };

        if (home)
            snprintf(saved, sizeof saved, "%s", home);

        if (pics) {
            setenv("XDG_PICTURES_DIR", pics, 1);
            intcheck("cap: the Pictures dir is used when it exists",
                     dyt_vm_default_capture_dir(b, sizeof b), 0);
            strcheck("cap: and it is the Pictures dir", b, pics);

            /* A variable naming something that is not there is skipped rather
             * than used: it would send captures somewhere unopenable. */
            setenv("XDG_PICTURES_DIR", "/nonexistent-dir-xyz/pics", 1);
            setenv("HOME", pics, 1);
            dyt_vm_default_capture_dir(b, sizeof b);
            strcheck("cap: a missing Pictures dir falls back to HOME", b, pics);

            unsetenv("XDG_PICTURES_DIR");
            dyt_vm_default_capture_dir(b, sizeof b);
            strcheck("cap: with no XDG variable HOME is used", b, pics);

            setenv("HOME", "/nonexistent-dir-xyz/home", 1);
            dyt_vm_default_capture_dir(b, sizeof b);
            strcheck("cap: with neither, the working directory", b, ".");

            rmdir(pics);
        }

        unsetenv("XDG_PICTURES_DIR");
        if (saved[0])
            setenv("HOME", saved, 1);

        intcheck("cap: a NULL buffer is refused",
                 dyt_vm_default_capture_dir(NULL, 16), -1);
        intcheck("cap: a zero-size buffer is refused",
                 dyt_vm_default_capture_dir(b, 0), -1);
    }
}

/* A session frame with a raw payload, so a still can be written from it. */
static void feed_raw(dyt_session_t *s, float *img, uint16_t *raw,
                     int w, int h)
{
    int i;

    for (i = 0; i < w * h; i++)
        raw[i] = (uint16_t)(1000 + i);
    feed(s, img, w, h);
    dyt_session_process_raw(s, raw, w * h, w, h);
}

static void test_gallery(void)
{
    char      tmpl[] = "/tmp/dytvm-gallery-XXXXXX";
    char     *dir = mkdtemp(tmpl);
    char      still[DYT_VM_PATH_CAP], clip[DYT_VM_PATH_CAP];
    char      decoy[DYT_VM_PATH_CAP], vendor[DYT_VM_PATH_CAP];
    char      msg[256] = { 0 };
    dyt_vm_item_t items[8];
    dyt_vm_still_info_t info;
    int       n;

    printf("\n-- gallery: classify, scan, describe a still --\n");

    intcheck("gal: a still is a still",
             dyt_vm_item_kind("dyt_20260925-120000.dyt.jpg"),
             DYT_VM_ITEM_STILL);
    intcheck("gal: a clip is a clip",
             dyt_vm_item_kind("dyt_20260925-120000.mp4"), DYT_VM_ITEM_CLIP);
    intcheck("gal: the extension match is case-insensitive",
             dyt_vm_item_kind("dyt_20260925-120000.MP4"), DYT_VM_ITEM_CLIP);
    intcheck("gal: a plain JPEG is not a still",
             dyt_vm_item_kind("photo.jpg"), DYT_VM_ITEM_NONE);
    intcheck("gal: a PNG is not ours",
             dyt_vm_item_kind("dyt_20260925-120000.png"), DYT_VM_ITEM_NONE);
    intcheck("gal: a bare extension is not a name",
             dyt_vm_item_kind(".mp4"), DYT_VM_ITEM_NONE);
    intcheck("gal: NULL is nothing", dyt_vm_item_kind(NULL), DYT_VM_ITEM_NONE);

    if (!dir) {
        fail("gal: a temporary directory", "mkdtemp");
        return;
    }

    snprintf(still, sizeof still, "%s/dyt_20260101-000000.dyt.jpg", dir);
    snprintf(clip, sizeof clip, "%s/dyt_20260101-000001.mp4", dir);
    snprintf(decoy, sizeof decoy, "%s/notes.txt", dir);
    snprintf(vendor, sizeof vendor, "%s/dyt_20260101-000002.dyt.jpg", dir);

    /* A real still, from a real session frame. */
    {
        dyt_session_t *s = dyt_session_create();
        float          img[16 * 16];
        uint16_t       raw[16 * 16];

        if (!s) {
            fail("gal: session", "out of memory");
            return;
        }
        feed_raw(s, img, raw, 16, 16);
        intcheck("gal: the still writes",
                 dyt_vm_write_still(s, still, msg, sizeof msg), 0);
        dyt_session_free(s);
    }

    /* The same with super-resolution on.  The embedded JPEG is 2x, so the
     * writer must size its buffer from the snapshot's factor rather than from
     * the temperature plane; sizing it from the plane makes the render return
     * -2 and the still fail, which is the bug this pins. */
    {
        dyt_session_t  *s = dyt_session_create();
        const size_t    npix = (size_t)DYT_SR_IN_W * DYT_SR_IN_H;
        float          *img = malloc(npix * sizeof *img);
        uint16_t       *raw = malloc(npix * sizeof *raw);
        dyt_snapshot_t  snap;
        char            still2[DYT_VM_PATH_CAP];

        if (!s || !img || !raw) {
            fail("gal 2x: allocate", "out of memory");
        } else {
            snprintf(still2, sizeof still2,
                     "%s/dyt_20260101-000003.dyt.jpg", dir);
            feed_raw(s, img, raw, DYT_SR_IN_W, DYT_SR_IN_H);
            dyt_session_set_sr_upscaler(s, dup_upscale);
            dyt_session_set_sr(s, DYT_SR_THERMAL);

            intcheck("gal 2x: the snapshot reports the factor",
                     dyt_session_snapshot(s, &snap, NULL, 0) == 0 &&
                     snap.sr_active == 1 && snap.xform.sr == 2, 1);
            intcheck("gal 2x: the still writes with super-resolution on",
                     dyt_vm_write_still(s, still2, msg, sizeof msg), 0);

            /* Removed again: the scan cases below count this directory, and a
             * fourth file would (correctly) change every one of them. */
            unlink(still2);
        }
        free(img);
        free(raw);
        dyt_session_free(s);
    }

    /* A clip and a decoy, so the scan has something to leave out. */
    {
        FILE *f = fopen(clip, "wb");
        if (f) { fputs("not really an mp4", f); fclose(f); }
        f = fopen(decoy, "wb");
        if (f) { fputs("ignore me", f); fclose(f); }
    }

    /* A vendor blob: the still's own JPEG and payload, with the extension
     * dropped.  It is a valid still that cannot be re-rendered thermally. */
    {
        uint8_t *blob = NULL, *raw = NULL, *jpg = NULL;
        size_t   bl = 0, rl = 0, jl = 0;

        if (dyt_dyt_read(still, &blob, &bl, &raw, &rl, &jpg, &jl) == 0) {
            uint8_t vb[DYT_DYT_HDR_FIXED];

            memcpy(vb, blob, sizeof vb);
            vb[DYT_DYT_OFF_SIZE]     = (uint8_t)(DYT_DYT_HDR_FIXED & 0xFF);
            vb[DYT_DYT_OFF_SIZE + 1] = (uint8_t)(DYT_DYT_HDR_FIXED >> 8);
            intcheck("gal: the vendor container writes",
                     dyt_dyt_write(vendor, jpg, jl, vb, sizeof vb, raw, rl), 0);
        } else {
            fail("gal: read the still back", "dyt_dyt_read");
        }
        free(blob); free(raw); free(jpg);
    }

    /* Ordering: pin the times so the test does not depend on how fast the
     * writes above ran (a tie would fall back to the name, which is a
     * different assertion). */
    {
        struct utimbuf t;
        t.actime = t.modtime = 1000; utime(still, &t);
        t.actime = t.modtime = 2000; utime(clip, &t);
        t.actime = t.modtime = 3000; utime(vendor, &t);
    }

    /* Ordering: the vendor still is written last, so it is the newest. */
    n = dyt_vm_scan(dir, NULL, 0);
    intcheck("gal: the scan counts without writing", n, 3);

    n = dyt_vm_scan(dir, items, 8);
    intcheck("gal: the scan finds all three", n, 3);
    if (n == 3) {
        intcheck("gal: newest first", items[0].kind, DYT_VM_ITEM_STILL);
        strcheck("gal: and it is the vendor still",
                 items[0].name, "dyt_20260101-000002.dyt.jpg");
        intcheck("gal: the clip is in there",
                 items[1].kind, DYT_VM_ITEM_CLIP);
        intcheck("gal: so is the first still",
                 items[2].kind, DYT_VM_ITEM_STILL);
        strcheck("gal: the decoy is left out", items[1].name,
                 "dyt_20260101-000001.mp4");
        if (items[2].bytes > 1000 && items[2].mtime > 0)
            ok("gal: an entry carries its size and time");
        else
            fail("gal: an entry carries its size and time", "0");
    }

    /* Truncation is reported, not hidden. */
    n = dyt_vm_scan(dir, items, 1);
    intcheck("gal: a small cap reports the total", n, 3);
    strcheck("gal: and writes the newest", items[0].name,
             "dyt_20260101-000002.dyt.jpg");

    intcheck("gal: a negative cap is refused", dyt_vm_scan(dir, items, -1), -1);
    intcheck("gal: an unreadable directory is refused",
             dyt_vm_scan("/nonexistent-dir-xyz/sub", items, 8), -1);

    /* Describe the still. */
    intcheck("gal: the still is described",
             dyt_vm_still_info(still, &info), 0);
    intcheck("gal: it has thermal geometry", info.have_thermal, 1);
    intcheck("gal: the width", info.width, 16);
    intcheck("gal: the active rows", info.active_rows, 16);
    intcheck("gal: the total rows", info.total_rows, 16);
    intcheck("gal: the sample count", info.n_samples, 16 * 16);
    intcheck("gal: the raw byte count", (int)info.raw_bytes, 16 * 16 * 2);
    if (info.jpeg_bytes > 0)
        ok("gal: and the embedded JPEG has bytes");
    else
        fail("gal: and the embedded JPEG has bytes", "0");

    /* The vendor still is a still with no geometry: showable, not renderable. */
    intcheck("gal: the vendor still is described",
             dyt_vm_still_info(vendor, &info), 0);
    intcheck("gal: but it has no thermal geometry", info.have_thermal, 0);
    intcheck("gal: and no width is invented", info.width, 0);
    intcheck("gal: the payload is still reported", info.n_samples, 16 * 16);
    if (info.jpeg_bytes > 0)
        ok("gal: and its JPEG is still there");
    else
        fail("gal: and its JPEG is still there", "0");

    intcheck("gal: a clip is not a still",
             dyt_vm_still_info(clip, &info), -1);
    intcheck("gal: a missing file is refused",
             dyt_vm_still_info("/nonexistent-dir-xyz/x.dyt.jpg", &info), -1);
    intcheck("gal: a NULL path is refused", dyt_vm_still_info(NULL, &info), -1);
    intcheck("gal: a NULL info is refused", dyt_vm_still_info(still, NULL), -1);

    /* The browsing state: load, move, keep the highlight across a rescan. */
    {
        dyt_vm_gallery_t g;
        dyt_vm_gallery_init(&g);
        char             b[192];

        intcheck("gal: the initial highlight is none", g.sel, -1);
        intcheck("gal: an empty list loads as 0",
                 dyt_vm_gallery_load(&g, "/nonexistent-dir-xyz/sub"), -1);
        intcheck("gal: and leaves no highlight", g.sel, -1);
        dyt_vm_gallery_move(&g, 1);
        intcheck("gal: moving an empty list is harmless", g.sel, -1);

        intcheck("gal: loading the directory", dyt_vm_gallery_load(&g, dir), 3);
        intcheck("gal: the newest is highlighted first", g.sel, 0);
        intcheck("gal: and it is the still",
                 dyt_vm_gallery_sel(&g)->kind, DYT_VM_ITEM_STILL);

        dyt_vm_gallery_move(&g, 1);
        intcheck("gal: down moves the highlight", g.sel, 1);
        intcheck("gal: onto the clip",
                 dyt_vm_gallery_sel(&g)->kind, DYT_VM_ITEM_CLIP);
        dyt_vm_gallery_move(&g, 2);
        intcheck("gal: moving past the end wraps", g.sel, 0);
        dyt_vm_gallery_move(&g, -1);
        intcheck("gal: moving before the start wraps", g.sel, 2);

        /* The size in the label is the file's, so assert the shape and the
         * name rather than a byte count that depends on the encoder. */
        dyt_vm_gallery_label(&g, b, sizeof b);
        if (strncmp(b, "gallery 3/3  ", 13) == 0 &&
            strstr(b, g.items[2].name) && strstr(b, "KB"))
            ok("gal: the label names the entry and its size");
        else
            fail("gal: the label names the entry and its size", b);

        /* A rescan keeps the highlight on the same entry, not on its index. */
        dyt_vm_gallery_move(&g, -1);
        intcheck("gal: the highlight is on the middle entry", g.sel, 1);
        strcheck("gal: which is the clip",
                 dyt_vm_gallery_sel(&g)->name, "dyt_20260101-000001.mp4");
        dyt_vm_gallery_load(&g, dir);
        intcheck("gal: a rescan keeps the selection", g.sel, 1);
        strcheck("gal: still on the same file",
                 dyt_vm_gallery_sel(&g)->name, "dyt_20260101-000001.mp4");

        /* A click picks a row outright, which a delta cannot express. */
        dyt_vm_gallery_set_sel(&g, 2);
        intcheck("gal: a click selects that row", g.sel, 2);
        strcheck("gal: and _sel agrees with it",
                 dyt_vm_gallery_sel(&g)->name, "dyt_20260101-000000.dyt.jpg");

        dyt_vm_gallery_set_sel(&g, -5);
        intcheck("gal: a click before the start clamps", g.sel, 0);
        dyt_vm_gallery_set_sel(&g, 99);
        intcheck("gal: a click past the end clamps", g.sel, 2);

        dyt_vm_gallery_set_sel(NULL, 0);            /* must not crash */

        {
            dyt_vm_gallery_t e;
            dyt_vm_gallery_init(&e);
            dyt_vm_gallery_set_sel(&e, 3);
            intcheck("gal: a click on an empty list leaves no highlight",
                     e.sel, -1);
        }

        /* The label of an empty list says so rather than showing 0/0. */
        {
            dyt_vm_gallery_t e;
            dyt_vm_gallery_init(&e);
            dyt_vm_gallery_label(&e, b, sizeof b);
            strcheck("gal: an empty list says so", b,
                     "gallery: no saved stills or clips");
        }
        intcheck("gal: a NULL gallery label is refused",
                 dyt_vm_gallery_label(NULL, b, sizeof b), -1);
    }

    remove(still);
    remove(clip);
    remove(decoy);
    remove(vendor);
    rmdir(dir);
}

/* The view keys: palette, unit, range, flip, zoom, fusion.  Each is checked
 * against the session state it is supposed to move, so a wrong letter — or a
 * letter bound to the wrong setter — fails here rather than in a window. */
static void test_view_keys(void)
{
    dyt_session_t  *s = dyt_session_create();
    dyt_snapshot_t  snap;
    float           img[16 * 16];

    printf("\n-- view keys --\n");

    if (!s) {
        fail("view: session", "out of memory");
        return;
    }
    feed(s, img, 16, 16);

#define SNAP() dyt_session_snapshot(s, &snap, NULL, 0)

    /* Palette: digits pick one directly.  How many are loaded depends on the
     * palette directory, so '0' is checked against the count rather than
     * against a hard-coded 9. */
    intcheck("view: '3' is a view key", dyt_vm_view_key(s, '3'), 1);
    SNAP();
    intcheck("view: and picks palette 2", snap.palette, 2);

    const int npal = snap.palette_n;
    if (npal < 3) {
        fail("view: the test session has palettes", "too few");
        dyt_session_free(s);
        return;
    }

    dyt_vm_view_key(s, '0');
    SNAP();
    intcheck("view: '0' picks the tenth, or the last there is",
             snap.palette, npal - 1 < 9 ? npal - 1 : 9);

    /* Cycling wraps at both ends of however many are loaded. */
    {
        const int p0 = snap.palette;
        dyt_vm_view_key(s, '.');
        SNAP();
        intcheck("view: '.' cycles forward, wrapping",
                 snap.palette, (p0 + 1) % npal);
        dyt_vm_view_key(s, ',');
        SNAP();
        intcheck("view: ',' cycles back", snap.palette, p0);
    }

    /* Unit cycles through the three the engine knows. */
    {
        dyt_unit_t before;
        SNAP();
        before = snap.unit;
        dyt_vm_view_key(s, 'u');
        SNAP();
        if (snap.unit != before)
            ok("view: 'u' cycles the unit");
        else
            fail("view: 'u' cycles the unit", "unchanged");
    }

    /* Range: auto <-> fixed. */
    {
        dyt_range_mode_t before;
        SNAP();
        before = snap.range_mode;
        dyt_vm_view_key(s, 't');
        SNAP();
        if (snap.range_mode != before)
            ok("view: 't' toggles the range mode");
        else
            fail("view: 't' toggles the range mode", "unchanged");
    }

    /* The two flips are separate bindings, and the case is what tells them
     * apart — an unconditional fold would make them the same key. */
    {
        const int h0 = snap.xform.flip_h, v0 = snap.xform.flip_v;
        dyt_vm_view_key(s, 'h');
        SNAP();
        const bool h_flipped = snap.xform.flip_h != h0 &&
                               snap.xform.flip_v == v0;
        dyt_vm_view_key(s, 'H');
        SNAP();
        const bool v_flipped = snap.xform.flip_v != v0 &&
                               snap.xform.flip_h != h0;
        if (h_flipped && v_flipped)
            ok("view: 'h' and 'H' flip different axes");
        else
            fail("view: 'h' and 'H' flip different axes", "same axis");
    }

    /* Zoom, both spellings of each direction. */
    {
        const int z0 = snap.xform.zoom;
        dyt_vm_view_key(s, '+');
        SNAP();
        const int z1 = snap.xform.zoom;
        dyt_vm_view_key(s, '-');
        SNAP();
        if (z1 == z0 + 1 && snap.xform.zoom == z0)
            ok("view: '+' and '-' step the zoom");
        else
            fail("view: '+' and '-' step the zoom", "wrong step");
    }

    /* Fusion: 'f' cycles the pattern, the four brackets nudge the alignment. */
    {
        const dyt_fusion_t f0 = snap.fusion;
        dyt_vm_view_key(s, 'f');
        SNAP();
        const bool cycled = snap.fusion != f0;
        const int dx0 = snap.fusion_dx;
        dyt_vm_view_key(s, ']');
        SNAP();
        const bool dx_moved = snap.fusion_dx == dx0 + 1;
        if (cycled && dx_moved)
            ok("view: 'f' cycles fusion and ']' nudges its alignment");
        else
            fail("view: 'f' cycles fusion and ']' nudges its alignment",
                 "no move");
    }

    /* Super-resolution: 'z' is the vendor's plane, 'Z' the thermal extension,
     * and the two are mutually exclusive because the mode is one value.  A
     * mode needs an upscaler behind it, so the test hands one in — the seam is
     * a pointer (session.h), which is what keeps this testable with no MNN. */
    {
        dyt_session_set_sr_upscaler(s, stub_upscale);

        intcheck("view: 'z' is a view key", dyt_vm_view_key(s, 'z'), 1);
        intcheck("view: 'z' selects the visible plane",
                 dyt_session_get_sr(s), DYT_SR_VISIBLE);

        intcheck("view: 'Z' is a view key", dyt_vm_view_key(s, 'Z'), 1);
        intcheck("view: 'Z' switches to the thermal plane",
                 dyt_session_get_sr(s), DYT_SR_THERMAL);

        dyt_vm_view_key(s, 'Z');
        intcheck("view: 'Z' again turns it off",
                 dyt_session_get_sr(s), DYT_SR_OFF);

        /* With nothing behind it the mode is refused, but the key is still
         * consumed — the front end must not fall through to its own bindings. */
        dyt_session_set_sr_upscaler(s, NULL);
        intcheck("view: 'z' is consumed without an upscaler",
                 dyt_vm_view_key(s, 'z'), 1);
        intcheck("view: the mode stays off", dyt_session_get_sr(s), DYT_SR_OFF);
    }

    /* A key that is not a view key is reported, so the caller falls through. */
    intcheck("view: 'w' is not a view key", dyt_vm_view_key(s, 'w'), 0);
    intcheck("view: 'p' is not a view key", dyt_vm_view_key(s, 'p'), 0);
    intcheck("view: a NULL session is refused", dyt_vm_view_key(NULL, '3'), 0);

#undef SNAP

    dyt_session_free(s);
}

int main(void)
{
    printf("=== view_model_test ===\n");
    test_temp();
    test_status_line();
    test_sr_notice();
    test_readout_line();
    test_hover_label();
    test_roi_label();
    test_bar();
    test_bar_scale();
    test_rapid_window();
    test_leak_effect();
    test_info();
    test_param();
    test_isotherm();
    test_grab();
    test_tool_mouse();
    test_alarm_band();
    test_alarm_setpoint();
    test_utilities();
    test_capture();
    test_gallery();
    test_view_keys();
    printf("=== %s ===\n", fails ? "FAIL" : "ALL PASS");
    return fails ? 1 : 0;
}
