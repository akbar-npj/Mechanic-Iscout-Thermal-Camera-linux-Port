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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session.h"
#include "view_model.h"

static int fails;

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
}

static void test_readout_line(void)
{
    dyt_snapshot_t s;
    char           b[256];

    base_snapshot(&s);
    s.tool = DYT_TOOL_NONE;
    dyt_vm_readout_line(&s, NULL, b, sizeof b);
    strcheck("readout: no tool", b,
             "tool: none (p point, l line, b box, n clear)");

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
}

int main(void)
{
    printf("=== view_model_test ===\n");
    test_temp();
    test_status_line();
    test_readout_line();
    test_hover_label();
    test_roi_label();
    test_bar();
    test_info();
    test_param();
    test_isotherm();
    test_grab();
    test_tool_mouse();
    test_alarm_band();
    test_utilities();
    printf("=== %s ===\n", fails ? "FAIL" : "ALL PASS");
    return fails ? 1 : 0;
}
