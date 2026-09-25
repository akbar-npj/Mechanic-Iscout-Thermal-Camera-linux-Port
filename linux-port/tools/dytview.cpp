/*
 * dytview.cpp — live viewer for the DYT/Mechanic iScout thermal camera.
 *
 * This is deliberately a *thin proving ground*, not the product UI.  All the
 * device-independent work — the capture pipeline, the temperature→RGB render
 * core, the display range, palette selection, units, mirror/zoom, the
 * start-up-filler rule, measurement, alarms, the six fusion patterns and the
 * DYT still container — lives in libdyt (see src/session.h, src/dytjpeg.h).
 * This file drives it and draws; it should never grow a second copy of any of
 * that.
 *
 * The capture layer (src/capture.c) is the only thing that talks to the
 * device, and its callback runs on libuvc's thread.  That callback is
 * dyt_session_capture_on_frame() (src/session_capture.c) — it hands the frame
 * to the session and, in the dual-half mode, extracts the visible plane for
 * fusion.  Everything else happens on the main thread.
 *
 * usage:  dytview [options]        (see usage() below)
 * build:  via the Makefile (needs libusb + OpenCV)
 */
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "capture.h"
#include "imgwrite.h"    /* dyt_write_png, for the canvas snapshot */
#include "palette.h"     /* DYT_PALETTE_N, for the colour bar */
#include "session.h"
#include "session_capture.h"
#include "view_model.h"  /* what to show: status text, ticks, panels, ladders */

/* ------------------------------------------------------------------ config */

static const char *const kWin     = "dytview";
static const int         kBarW    = 22;   /* colour-bar width, px */
static const int         kBarGap  = 14;   /* image -> bar gap, px */
static const int         kLabelW  = 96;   /* room for the bar's labels */
static const int         kStatusH = 40;   /* status strip height, px (2 lines) */

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

/* -------------------------------------------------------------- the viewer */

struct viewer {
    dyt_session_t *sess = nullptr;

    dyt_mode_t  mode = DYT_MODE_0;
    std::string note;                 /* bring-up / waiting message */

    /* Device identity and stored parameters, read once the stream is up
     * (Phase 3).  `d` toggles the panel that shows them. */
    dyt_device_info_t info{};
    int have_info = 0;
    int show_info = 1;

    /* The per-frame temperature plane, and the snapshot scratch it grows in.
     * The grow-and-retry is the view model's now (view_model.h). */
    dyt_vm_scratch_t scr = DYT_VM_SCRATCH_INIT;

    /* The pointer, in the shape the view model wants: last position, plus
     * whether a drag is open.  The placement rules are its too. */
    dyt_vm_pointer_t ptr = DYT_VM_POINTER_INIT;

    /* Measurement.  The tool mirrors the session's, and the geometry below is
     * the *last rendered* frame, so a mouse click can be mapped from window
     * coordinates back to source pixels. */
    dyt_tool_t tool = DYT_TOOL_NONE;
    dyt_view_transform_t xform{};
    int src_w = 0, src_h = 0, dst_w = 0, dst_h = 0;

    /* The last snapshot, so the key handler can arm the alarm from the range
     * that is actually on screen. */
    dyt_snapshot_t last{};
    int have_last = 0;

    /* Phase 4: the runtime-parameter write.  A key arms a candidate value;
     * only `y` sends it.  The write runs on this (main) thread, never in
     * on_frame, and blocks ~250 ms while the device settles — so the preview
     * pauses briefly after a confirmation. */
    int         pend_type  = 0;     /* dyt_order_type_t, 0 = nothing armed */
    float       pend_value = 0.f;
    int         pend_rung  = 0;     /* position in the per-type ladder */
    std::string msg;                /* transient message (a write outcome, a
                                     * fusion change), shown in the strip */
    int         msg_ttl    = 0;     /* frames left to show `msg` */

    /* Values sent this session, indexed by order type 1..4, so the info panel
     * never shows a stored value a write has superseded. */
    float override_v[5]  = {0, 0, 0, 0, 0};
    int   override_on[5] = {0, 0, 0, 0, 0};
};

/* ------------------------------------------------------------ frame callback */

/* The frame callback is dyt_session_capture_on_frame() (session_capture.h).
 * It used to live here, but the work — strip the mode-0x44c header, feed the
 * session, extract the visible half of the dual-half payload and feed that
 * too — is identical for the Qt6 app, so it belongs in the library rather
 * than being copied into every front-end.  The viewer only creates the
 * adapter and hands it to dyt_capture_start(). */

/* Take a consistent snapshot.  This used to be a grow-and-retry loop here;
 * it is now dyt_vm_grab() (view_model.h), because the Qt6 app needs exactly
 * the same dance and the two must not drift. */
static bool grab(viewer *v, dyt_snapshot_t &snap)
{
    return dyt_vm_grab(v->sess, &snap, &v->scr) != 0;
}

/* ---------------------------------------------------------------- drawing */

/* Outline-then-fill text, so labels stay legible on any colour underneath. */
static void put_text(cv::Mat &m, const std::string &s, int x, int y,
                     const cv::Scalar &col, double scale = 0.45)
{
    cv::putText(m, s, cv::Point(x, y), cv::FONT_HERSHEY_SIMPLEX, scale,
                cv::Scalar(0, 0, 0), 3, cv::LINE_AA);
    cv::putText(m, s, cv::Point(x, y), cv::FONT_HERSHEY_SIMPLEX, scale,
                col, 1, cv::LINE_AA);
}

static cv::Scalar pal_bgr(const dyt_palette_t *p, int idx)
{
    return cv::Scalar(p->rgb[idx * 3 + 2], p->rgb[idx * 3 + 1],
                      p->rgb[idx * 3 + 0]);
}

/* A temperature in the selected unit, e.g. "31.2 C".  The conversion and the
 * failure rule are the view model's, so the viewer and the Qt6 app cannot
 * disagree about either. */
static std::string tstr(const dyt_snapshot_t &s, float celsius)
{
    char b[32];
    dyt_vm_temp(&s, celsius, b, sizeof b);
    return std::string(b);
}

/* --------------------------------------------------- runtime parameter write
 *
 * Phase 4 exposes the one device write the port implements: the vendor's
 * runtime `sendOrder` (emissivity / ambient / reflected / distance).  It is
 * volatile state, not calibration — RE Docs 04 §4.2/§4.8.
 *
 * A parameter key only *arms* a candidate; nothing is sent until `y`.  That
 * confirmation is deliberate: the device applies these immediately, so a
 * stray keypress visibly changes the reading.
 */

/* The candidate values each key cycles through live in the view model
 * (dyt_vm_ladder, view_model.h) — short and widely spaced on purpose, because
 * this is a proving ground for "does the write reach the device", not a full
 * parameter editor.  So do the arming rules; only the wording stays here. */
static std::string param_value_str(int type, float v)
{
    char b[32];
    dyt_vm_param_format((dyt_order_type_t)type, v, b, sizeof b);
    return std::string(b);
}

/* The Phase-4 key handler.  A parameter key arms a candidate value; only a
 * following `y` sends it.  The decision is the view model's
 * (dyt_vm_param_key), since it is the same rule for any front-end; what stays
 * here is the wording and the write itself.  Returns 1 if the key was
 * consumed. */
static int handle_param_key(viewer *v, dyt_capture_t *cap, int key)
{
    dyt_vm_param_event_t ev;

    if (!dyt_vm_param_key(key, (dyt_order_type_t)v->pend_type, v->pend_rung,
                          &ev))
        return 0;

    if (ev.action == DYT_VM_PARAM_ARMED) {
        v->pend_type  = ev.type;
        v->pend_rung  = ev.rung;
        v->pend_value = ev.value;
        return 1;
    }

    if (ev.action == DYT_VM_PARAM_SEND) {
        const dyt_vm_ladder_t *L = dyt_vm_ladder(ev.type);
        int rc = dyt_capture_set_param(cap, ev.type, ev.value);
        char b[96];
        if (rc == 0) {
            uint16_t raw = (ev.type == DYT_ORDER_EMISSIVITY ||
                            ev.type == DYT_ORDER_DISTANCE)
                               ? dyt_param_encode_ratio(ev.value)
                               : dyt_param_encode_kelvin(ev.value);
            v->override_on[ev.type] = 1;
            v->override_v[ev.type]  = ev.value;
            std::snprintf(b, sizeof b, "set %s = %s  (sent)", L->name,
                          param_value_str(ev.type, ev.value).c_str());
            std::fprintf(stderr, "dytview: set %s = %s -> raw %u (type %d)\n",
                         L->name,
                         param_value_str(ev.type, ev.value).c_str(),
                         (unsigned)raw, ev.type);
        } else {
            std::snprintf(b, sizeof b, "set %s FAILED (rc %d)", L->name, rc);
            std::fprintf(stderr, "dytview: set %s FAILED (rc %d)\n",
                         L->name, rc);
        }
        v->msg = b;
        v->msg_ttl = 150;               /* ~4.5 s at the 30 ms poll */
        v->pend_type = 0;
        return 1;
    }

    if (ev.action == DYT_VM_PARAM_CANCEL) {
        v->msg = std::string("cancelled: ") + dyt_vm_ladder(ev.type)->name;
        v->msg_ttl = 90;
        v->pend_type = 0;
        return 1;
    }

    /* SWALLOW: any other key while armed is ignored, so a stray palette key
     * cannot slip past a pending confirmation. */
    return 1;
}

/* ------------------------------------------------------------ fusion keys
 *
 * Phase 5.  `f` steps through the six patterns; `[`/`]` nudge the visible
 * plane left/right and `;`/`'` nudge it up/down, in source pixels.  The
 * session clamps the offsets to ±40 — the range the vendor's own X/Y
 * coefficients are defined over (RE Docs 03 §3.5.2) — so a held key cannot
 * wind them out of range.
 *
 * The palette cycle moved from `[`/`]` to `,`/`.` to make room: `[`/`]` is
 * this phase's binding for alignment, and a bracket pair reads as
 * left/right.  `1-9/0` still select the first ten directly.
 *
 * Returns 1 if the key was consumed. */
static int handle_fusion_key(viewer *v, int key)
{
    dyt_snapshot_t snap;
    int  dx = 0, dy = 0;
    char b[96];

    switch (key) {
    case 'f':    dyt_session_cycle_fusion(v->sess, 1); break;
    case '[':    dx = -1; break;
    case ']':    dx = +1; break;
    case ';':    dy = -1; break;
    case '\'':   dy = +1; break;
    default:     return 0;
    }

    if (dx || dy)
        dyt_session_adjust_fusion_align(v->sess, dx, dy);

    /* Read the new state back from the session rather than mirroring it here:
     * the session is the single source of truth, and a stale copy in the
     * viewer is exactly the drift the library exists to prevent. */
    if (dyt_session_snapshot(v->sess, &snap, nullptr, 0) != 0)
        return 1;

    if (key == 'f') {
        /* Say plainly when the pattern cannot be honoured — the AD output
         * mode has no visible half, so anything but infrared falls back to
         * thermal and the user should not be left guessing. */
        std::snprintf(b, sizeof b, "fusion %s%s", snap.fusion_name,
                      snap.fusion_active || snap.fusion == DYT_FUSION_INFRARED
                          ? "" : "  (no visible plane in this mode)");
    } else {
        std::snprintf(b, sizeof b, "fusion align %+d,%+d", snap.fusion_dx,
                      snap.fusion_dy);
    }

    v->msg     = b;
    v->msg_ttl = 90;            /* ~2.7 s at the 30 ms poll */
    return 1;
}

/* The armed-write confirmation.  Drawn last, so it sits over every other
 * overlay. */
static void draw_confirm_prompt(cv::Mat &canvas, const viewer *v)
{
    if (!v->pend_type)
        return;

    const dyt_vm_ladder_t *L = dyt_vm_ladder((dyt_order_type_t)v->pend_type);
    if (!L)
        return;

    std::string s = std::string("SET ") + L->name + " = " +
                    param_value_str(v->pend_type, v->pend_value) +
                    "    y = send    n / esc = cancel";

    int tw = 10 + static_cast<int>(s.size()) * 9;
    int x  = std::max(6, (canvas.cols - tw) / 2);
    int y  = canvas.rows - kStatusH - 34;
    cv::rectangle(canvas, cv::Rect(x - 4, y - 4, tw + 8, 28),
                  cv::Scalar(0, 0, 150), cv::FILLED);
    cv::rectangle(canvas, cv::Rect(x - 4, y - 4, tw + 8, 28),
                  cv::Scalar(0, 220, 255), 1);
    put_text(canvas, s, x + 4, y + 16, cv::Scalar(255, 255, 255), 0.55);
}

/* The device-identity panel: the module serial plus the four runtime
 * radiometric parameters the device stores (Phase 3).  Drawn over the image
 * rather than in the status strip, because the strip is only as wide as the
 * image and already carries the measurement state. */
static void draw_info_panel(cv::Mat &canvas, const viewer *v)
{
    dyt_vm_info_t info;

    if (!v->have_info || !v->show_info)
        return;

    /* The rows and their wording are the view model's; this only draws them. */
    if (dyt_vm_info(&v->info, v->override_v, v->override_on, &info) < 0)
        return;

    const int pad = 6, lh = 17;
    int wmax = 0;
    for (int i = 0; i < info.n; i++)
        wmax = std::max(wmax,
                        10 + static_cast<int>(std::strlen(info.line[i])) * 8);

    cv::Rect box(6, 6, wmax + 2 * pad, lh * info.n + 2 * pad);
    cv::rectangle(canvas, box, cv::Scalar(16, 16, 16), cv::FILLED);
    cv::rectangle(canvas, box, cv::Scalar(120, 120, 120), 1);

    for (int i = 0; i < info.n; i++)
        put_text(canvas, info.line[i], box.x + pad, box.y + pad + lh * i + 12,
                 cv::Scalar(190, 225, 255));
}

/* A placeholder shown before the first frame, or while the device is still
 * streaming its start-up filler. */
static cv::Mat waiting_canvas(viewer *v, const char *msg){
    cv::Mat m(320, 520, CV_8UC3, cv::Scalar(24, 24, 24));
    put_text(m, "dytview", 20, 44, cv::Scalar(200, 200, 200), 0.9);
    put_text(m, msg, 20, 96, cv::Scalar(160, 200, 255), 0.55);
    if (!v->note.empty())
        put_text(m, v->note, 20, 132, cv::Scalar(170, 170, 170), 0.45);
    put_text(m, "keys: 1-9/0 palette   , . cycle   u unit   r range",
             20, 220, cv::Scalar(130, 130, 130), 0.45);
    put_text(m, "      h/v mirror   x rotate   +/- zoom   s still   w png   q quit",
             20, 244, cv::Scalar(130, 130, 130), 0.45);
    put_text(m, "      p point   l line   b box   o polygon   n clear",
             20, 268, cv::Scalar(130, 130, 130), 0.45);
    put_text(m, "      a alarm   i isotherm",
             20, 292, cv::Scalar(130, 130, 130), 0.45);
    put_text(m, "      f fusion   [ ] align X   ; ' align Y",
             20, 316, cv::Scalar(130, 130, 130), 0.45);
    put_text(m, "      d device info",
             20, 340, cv::Scalar(130, 130, 130), 0.45);
    put_text(m, "      e emis  A amb  R refl  D dist  (then y to send)",
             20, 364, cv::Scalar(130, 130, 130), 0.45);
    return m;
}

/* Render one frame (plus its scale bar and readouts) into a canvas.
 * Returns the canvas; the caller owns it. */
static cv::Mat render(viewer *v)
{
    dyt_snapshot_t snap;

    if (!grab(v, snap))
        return waiting_canvas(v, "waiting for frames...");

    /* The start-up filler decodes to a legitimate-looking 238.85 C in mode
     * 1000, so it has to be recognised rather than painted (display.c). */
    if (!snap.ready)
        return waiting_canvas(v, "warming up - waiting for live data...");

    const int w    = snap.width;
    const int h    = snap.height;
    const int npix = w * h;

    /* Pixels come from the session rather than the snapshot copy: the session
     * renders under its own lock, so the image and the range it was rendered
     * with always agree. */
    std::vector<uint8_t> rgb(static_cast<size_t>(npix) * 3);
    int rw = 0, rh = 0;
    if (dyt_session_render_rgb(v->sess, rgb.data(),
                               static_cast<int>(rgb.size()), &rw, &rh) != 0)
        return waiting_canvas(v, "waiting for frames...");

    cv::Mat img(h, w, CV_8UC3, rgb.data());
    cv::Mat bgr;
    cv::cvtColor(img, bgr, cv::COLOR_RGB2BGR);

    /* ---- isotherm overlay: dim everything outside the alarm band, so the
     * pixels that would trip the alarm stand out.  The pass is the view
     * model's: it is a plain buffer operation, so it needs no toolkit and
     * there is no reason for a second copy of it. ---- */
    if (snap.iso_on && v->scr.cap >= npix && bgr.isContinuous())
        dyt_vm_apply_isotherm(bgr.data, w, h, v->scr.temps, v->scr.cap,
                              snap.iso_lo, snap.iso_hi);

    /* Rotation, mirror and zoom are applied here, after the render, which is
     * exactly the order dyt_view_transform_map()/project() assume — and in
     * reverse, so the mapping inverts what is on screen.  Every field of the
     * transform is honoured: rendering a subset of it would put a click on the
     * wrong pixel, which is the one failure this geometry exists to prevent. */
    cv::Mat big;
    if (snap.xform.zoom > 1)
        cv::resize(bgr, big, cv::Size(), snap.xform.zoom, snap.xform.zoom,
                   cv::INTER_NEAREST);
    else
        big = bgr;
    if (snap.xform.rot == DYT_ROT_90)
        cv::rotate(big, big, cv::ROTATE_90_CLOCKWISE);
    else if (snap.xform.rot == DYT_ROT_180)
        cv::rotate(big, big, cv::ROTATE_180);
    else if (snap.xform.rot == DYT_ROT_270)
        cv::rotate(big, big, cv::ROTATE_90_COUNTERCLOCKWISE);
    if (snap.xform.flip_h) cv::flip(big, big, 1);
    if (snap.xform.flip_v) cv::flip(big, big, 0);

    /* The mouse handler maps a click through this geometry, and the key
     * handler arms the alarm from this snapshot, so both need the frame that
     * is actually on screen. */
    v->xform  = snap.xform;
    v->src_w  = w;
    v->src_h  = h;
    v->dst_w  = big.cols;
    v->dst_h  = big.rows;
    v->last   = snap;
    v->have_last = 1;

    /* ---- canvas: image | gap | colour bar | labels ---- */
    const int barH = big.rows;
    const int x0   = big.cols + kBarGap;
    cv::Mat canvas(barH, x0 + kBarW + kLabelW, CV_8UC3, cv::Scalar(24, 24, 24));
    big.copyTo(canvas(cv::Rect(0, 0, big.cols, big.rows)));

    /* ---- colour bar: top = hottest, matching the palette's own order ---- */
    dyt_palette_t pal;
    if (dyt_session_get_palette(v->sess, snap.palette, &pal) != 0)
        dyt_palette_builtin(&pal, 0);

    for (int j = 0; j < barH; j++) {
        int idx = dyt_vm_bar_index(j, barH);
        if (idx < 0)
            continue;
        cv::rectangle(canvas, cv::Rect(x0, j, kBarW, 1), pal_bgr(&pal, idx),
                      cv::FILLED);
    }
    cv::rectangle(canvas, cv::Rect(x0, 0, kBarW, barH),
                  cv::Scalar(200, 200, 200), 1);

    const int lx = x0 + kBarW + 4;
    {
        char lbl[32];
        dyt_vm_bar_label(&snap, 0, lbl, sizeof lbl);
        put_text(canvas, lbl, lx, 12, cv::Scalar(255, 255, 255));
        dyt_vm_bar_label(&snap, 1, lbl, sizeof lbl);
        put_text(canvas, lbl, lx, barH / 2, cv::Scalar(210, 210, 210));
        dyt_vm_bar_label(&snap, 2, lbl, sizeof lbl);
        put_text(canvas, lbl, lx, barH - 3, cv::Scalar(255, 255, 255));
    }

    /* ---- hot/cold markers ---- */
    {
        auto mark = [&](int sx, int sy, float c, const cv::Scalar &col,
                        const char *tag) {
            int cx, cy;
            if (sx < 0 || sy < 0)
                return;
            if (dyt_view_transform_project(&snap.xform, w, h, big.cols,
                                           big.rows, sx, sy, &cx, &cy) != 0)
                return;
            cv::circle(canvas, cv::Point(cx, cy), 5, cv::Scalar(0, 0, 0), 2);
            cv::circle(canvas, cv::Point(cx, cy), 5, col, 1);
            put_text(canvas, std::string(tag) + " " + tstr(snap, c),
                     cx + 8, cy - 6, col);
        };
        mark(snap.stats.hot_x, snap.stats.hot_y, snap.stats.hi,
             cv::Scalar(0, 0, 255), "H");     /* hottest: red */
        mark(snap.stats.cold_x, snap.stats.cold_y, snap.stats.lo,
             cv::Scalar(255, 120, 0), "L");   /* coldest: blue */
    }

    /* ---- hover readout: the temperature under the pointer ---- */
    if (v->ptr.x >= 0 && v->ptr.y >= 0 &&
        v->ptr.x < big.cols && v->ptr.y < big.rows) {
        int ix = 0, iy = 0;
        if (dyt_view_transform_map(&snap.xform, w, h, big.cols, big.rows,
                                   v->ptr.x, v->ptr.y, &ix, &iy) == 0) {
            float t = v->scr.temps[static_cast<size_t>(iy) * w + ix];
            std::string s = (t == t)
                ? tstr(snap, t) + "  (" + std::to_string(ix) + "," +
                      std::to_string(iy) + ")"
                : std::string("---  (") + std::to_string(ix) + "," +
                      std::to_string(iy) + ")";
            put_text(canvas, s, v->ptr.x + 10, v->ptr.y - 8,
                     cv::Scalar(255, 255, 255), 0.5);
            cv::line(canvas, cv::Point(v->ptr.x - 6, v->ptr.y),
                     cv::Point(v->ptr.x + 6, v->ptr.y),
                     cv::Scalar(0, 0, 0), 1);
            cv::line(canvas, cv::Point(v->ptr.x, v->ptr.y - 6),
                     cv::Point(v->ptr.x, v->ptr.y + 6),
                     cv::Scalar(0, 0, 0), 1);
        }
    }

    /* ---- measurement overlays ---- */
    if (snap.tool != DYT_TOOL_NONE) {
        /* Clamp the source point into the frame first: a box dragged partly
         * off the image should still draw, clipped at the edge, rather than
         * vanish because one corner projects outside the canvas. */
        auto proj = [&](int sx, int sy, int &ox, int &oy) {
            sx = std::max(0, std::min(w - 1, sx));
            sy = std::max(0, std::min(h - 1, sy));
            return dyt_view_transform_project(&snap.xform, w, h,
                                              big.cols, big.rows,
                                              sx, sy, &ox, &oy) == 0;
        };

        if (snap.tool == DYT_TOOL_POINT && snap.point_ok) {
            int cx = 0, cy = 0;
            if (proj(snap.p0.x, snap.p0.y, cx, cy)) {
                cv::circle(canvas, cv::Point(cx, cy), 7, cv::Scalar(0, 0, 0), 2);
                cv::circle(canvas, cv::Point(cx, cy), 7,
                           cv::Scalar(0, 255, 255), 1);
                put_text(canvas, "P " + tstr(snap, snap.point_c),
                         cx + 10, cy - 8, cv::Scalar(0, 255, 255), 0.5);
            }
        }

        if (snap.tool == DYT_TOOL_LINE) {
            int ax = 0, ay = 0, bx = 0, by = 0;
            if (proj(snap.p0.x, snap.p0.y, ax, ay) &&
                proj(snap.p1.x, snap.p1.y, bx, by)) {
                cv::line(canvas, cv::Point(ax, ay), cv::Point(bx, by),
                         cv::Scalar(0, 0, 0), 3);
                cv::line(canvas, cv::Point(ax, ay), cv::Point(bx, by),
                         cv::Scalar(0, 255, 255), 1);
                cv::circle(canvas, cv::Point(ax, ay), 4,
                           cv::Scalar(0, 255, 255), cv::FILLED);
                cv::circle(canvas, cv::Point(bx, by), 4,
                           cv::Scalar(0, 255, 255), cv::FILLED);
            }
        }

        if (snap.tool == DYT_TOOL_BOX) {
            int ax = 0, ay = 0, bx = 0, by = 0;
            if (proj(snap.p0.x, snap.p0.y, ax, ay) &&
                proj(snap.p1.x, snap.p1.y, bx, by)) {
                cv::Rect r(cv::Point(std::min(ax, bx), std::min(ay, by)),
                           cv::Point(std::max(ax, bx), std::max(ay, by)));
                cv::rectangle(canvas, r, cv::Scalar(0, 0, 0), 3);
                cv::rectangle(canvas, r, cv::Scalar(0, 255, 255), 1);

                if (snap.roi_ok) {
                    char buf[160];
                    std::snprintf(buf, sizeof buf,
                                  "min %s  max %s  avg %s  med %s",
                                  tstr(snap, snap.roi.min).c_str(),
                                  tstr(snap, snap.roi.max).c_str(),
                                  tstr(snap, snap.roi.mean).c_str(),
                                  tstr(snap, snap.roi.median).c_str());
                    put_text(canvas, buf, r.x + 4, r.y - 6,
                             cv::Scalar(0, 255, 255), 0.5);
                }
            }
        }

        if (snap.tool == DYT_TOOL_POLYGON && snap.poly_n > 0) {
            std::vector<cv::Point> verts, outline;

            for (int i = 0; i < snap.poly_n; i++) {
                int px = 0, py = 0;
                if (proj(snap.poly[i].x, snap.poly[i].y, px, py))
                    verts.push_back(cv::Point(px, py));
            }
            outline = verts;

            /* A rubber band to the cursor while the outline is still open:
             * without it the user cannot see where the next click will land.
             * A closed outline has no next click, so it gets none. */
            if (!snap.poly_closed && v->ptr.x >= 0)
                outline.push_back(cv::Point(v->ptr.x, v->ptr.y));

            if (outline.size() >= 2) {
                const cv::Point *pp  = outline.data();
                const int        npt = (int)outline.size();
                const bool       shut = snap.poly_closed && verts.size() >= 3;

                cv::polylines(canvas, &pp, &npt, 1, shut,
                              cv::Scalar(0, 0, 0), 3);
                cv::polylines(canvas, &pp, &npt, 1, shut,
                              cv::Scalar(0, 255, 255), 1);
            }
            for (size_t i = 0; i < verts.size(); i++)
                cv::circle(canvas, verts[i], 3,
                           i == 0 ? cv::Scalar(0, 128, 255)
                                  : cv::Scalar(0, 255, 255),
                           cv::FILLED);

            if (snap.roi_ok && !verts.empty()) {
                char buf[160];
                std::snprintf(buf, sizeof buf,
                              "min %s  max %s  avg %s  med %s",
                              tstr(snap, snap.roi.min).c_str(),
                              tstr(snap, snap.roi.max).c_str(),
                              tstr(snap, snap.roi.mean).c_str(),
                              tstr(snap, snap.roi.median).c_str());
                put_text(canvas, buf, verts[0].x + 6, verts[0].y - 6,
                         cv::Scalar(0, 255, 255), 0.5);
            }
        }
    }

    /* ---- device identity overlay (Phase 3) ---- */
    draw_info_panel(canvas, v);

    /* ---- pending-write confirmation (Phase 4), on top of everything ---- */
    draw_confirm_prompt(canvas, v);

    /* ---- status strip ----
     *
     * Both lines are the view model's, so the Qt6 status bar shows exactly
     * these strings rather than a re-worded copy of them (view_model.h). */
    {
        char status[256];
        char info[512];

        dyt_vm_status_line(&snap, v->mode, status, sizeof status);
        dyt_vm_readout_line(&snap, v->msg.empty() ? nullptr : v->msg.c_str(),
                            info, sizeof info);

        cv::rectangle(canvas, cv::Rect(0, barH - kStatusH, big.cols, kStatusH),
                      cv::Scalar(16, 16, 16), cv::FILLED);
        put_text(canvas, status, 6, barH - kStatusH + 14,
                 cv::Scalar(220, 220, 220));
        put_text(canvas, info, 6, barH - 5, cv::Scalar(190, 210, 255));

        /* The alarm is the one thing worth shouting about.  It is right-aligned
         * on the *first* status line: the second line can be arbitrarily long
         * (box stats, alarm band, isotherm), and the image area is where the
         * measurement labels go, so neither is a safe place for it. */
        if (snap.alarm_on && snap.alarm != DYT_ALARM_NONE) {
            const std::string a = "ALARM " +
                                  std::string(dyt_alarm_name(snap.alarm));
            int bw = 12 + static_cast<int>(a.size()) * 10;
            int by = barH - kStatusH + 2;
            cv::rectangle(canvas, cv::Rect(big.cols - bw - 6, by, bw, 20),
                          cv::Scalar(0, 0, 180), cv::FILLED);
            put_text(canvas, a, big.cols - bw, by + 15,
                     cv::Scalar(255, 255, 255), 0.5);
        }
    }

    return canvas;
}

/* Place measurement points.  A press sets point 0, dragging moves point 1, so
 * the same gesture draws a line or a box; a plain click leaves both points on
 * the same pixel (which is exactly a point probe). */
static void on_mouse(int event, int x, int y, int flags, void *user)
{
    (void)flags;
    viewer           *v = static_cast<viewer *>(user);
    dyt_vm_mouse_ev_t ev;

    switch (event) {
    case cv::EVENT_LBUTTONDOWN: ev = DYT_VM_MOUSE_DOWN; break;
    case cv::EVENT_MOUSEMOVE:   ev = DYT_VM_MOUSE_MOVE; break;
    case cv::EVENT_LBUTTONUP:   ev = DYT_VM_MOUSE_UP;   break;
    case cv::EVENT_RBUTTONDOWN:
        /* The polygon's own gesture: finish the outline.  Not routed through
         * the shared placement, which only knows presses, moves and drags. */
        if (v->tool == DYT_TOOL_POLYGON)
            dyt_session_set_polygon_closed(v->sess, 1);
        return;
    default:                    return;
    }

    /* The placement rules — a press sets both points, a drag moves point 1, a
     * release ends the drag — are the view model's, because a Qt widget needs
     * exactly the same ones (view_model.h).  All that stays here is the
     * mapping from OpenCV's event codes. */
    dyt_vm_tool_mouse(v->sess, &v->ptr, ev, v->tool, &v->xform,
                      v->src_w, v->src_h, v->dst_w, v->dst_h, x, y);
}

/* ------------------------------------------------------------------ palettes
 *
 * Both of these are the view model's: the Qt6 app has to find palettes/ and
 * stamp a filename the same way, so a second copy here would be a second
 * answer.
 */
static std::string find_palette_dir(const std::string &dir_opt)
{
    char b[4096];
    if (!dyt_vm_find_palette_dir(dir_opt.empty() ? nullptr : dir_opt.c_str(),
                                 b, sizeof b))
        return "";
    return std::string(b);
}

static void stamp_now(char *out, size_t n)
{
    if (dyt_vm_timestamp(out, n) != 0 && n > 0)
        out[0] = '\0';
}

/* Write the whole canvas (overlays, bar, status strip included) as a PNG, via
 * the port's own writer rather than OpenCV's — the vendored codec is then
 * exercised on the live path, not just in the unit tests.  The canvas is BGR,
 * so it is converted first. */
static void save_canvas_png(viewer *v, const cv::Mat &canvas)
{
    char stamp[32], path[128];
    cv::Mat rgb;

    stamp_now(stamp, sizeof stamp);
    std::snprintf(path, sizeof path, "dytview_%s.png", stamp);

    cv::cvtColor(canvas, rgb, cv::COLOR_BGR2RGB);
    if (dyt_write_png(path, rgb.data, rgb.cols, rgb.rows) != 0) {
        v->msg     = std::string("cannot write ") + path;
        v->msg_ttl = 120;
        return;
    }
    v->msg     = std::string("wrote ") + path;
    v->msg_ttl = 120;               /* ~3.6 s at the 30 ms poll */
    std::printf("saved %s\n", path);
    std::fflush(stdout);
}

/* Write a DYT still (Phase 6): the rendered picture as JPEG, with the device's
 * own raw payload and the frame geometry spliced in as APP2 segments, so a
 * vendor tool can open it and re-render the raw data with its own palette and
 * range — which a plain PNG cannot carry.
 *
 * The build is the view model's (dyt_vm_write_still), because the Qt6 app's
 * capture button needs the same file; what stays here is choosing the name
 * and reporting the outcome.
 *
 * The raw payload comes from the session, not from dyt_capture_last_raw():
 * that pointer is only valid on the frame callback thread. */
static void save_dyt_still(viewer *v)
{
    char stamp[32], path[128], msg[256];

    stamp_now(stamp, sizeof stamp);
    std::snprintf(path, sizeof path, "dytview_%s.jpg", stamp);

    if (dyt_vm_write_still(v->sess, path, msg, sizeof msg) != 0) {
        v->msg     = msg[0] ? msg : "still: failed";
        v->msg_ttl = 120;
        return;
    }
    v->msg     = msg;
    v->msg_ttl = 120;               /* ~3.6 s at the 30 ms poll */
    std::printf("%s\n", v->msg.c_str());
    std::fflush(stdout);
}

/* --------------------------------------------------------------------- main */

static void usage(const char *prog)
{
    std::fprintf(stderr,
        "usage: %s [options]\n"
        "  --ad-output           send setTinyCOutputADValue once streaming and\n"
        "                        read the flat 256x192 raw-AD frame.  Default\n"
        "                        is the device's own 256x384 dual-half frame,\n"
        "                        whose bottom half is the thermal plane and which\n"
        "                        needs no vendor order\n"
        "  --zoom N              integer upscale for the window (default 2)\n"
        "  --palette N           palette index 1..%d (default 1 = iron red)\n"
        "  --lo C --hi C         lock the display range instead of auto-fitting\n"
        "  --palette-dir DIR     where the *.dat ramps live (default: search)\n"
        "  --png PATH            headless: wait for live data to settle, write\n"
        "                        one PNG, exit (no window)\n"
        "  --settle SEC          settle wait after the filler ends (default 12)\n"
        "  --timeout SEC         overall headless wait limit (default 40)\n"
        "  --frames N            quit after N painted frames\n"
        "  --vid 0xXXXX --pid 0xXXXX --format-index N\n"
        "  --width W --height H --fps F\n"
        "  --t-amb C --sensor-mode 0x82 --fix-mode 0x78\n"
        "\n"
        "keys: 1-9/0 palette · , . cycle palette · u unit · r auto/locked range\n"
        "      h/v mirror · x rotate a quarter turn · + / - zoom · q or ESC quit\n"
        "      s save a DYT still (picture + raw payload) · w save a PNG of the\n"
        "      whole window\n"
        "      p point · l line · b box · o polygon (click per vertex,\n"
        "      right-click or Enter to close, Backspace to undo a vertex)\n"
        "      n clear (click/drag to place)\n"
        "      a alarm on/off · i isotherm (alarm band) overlay\n"
        "      f cycle fusion · [ ] align visible X · ; ' align visible Y\n"
        "      d device-info panel (serial + stored parameters)\n"
        "      e/A/R/D arm emissivity/ambient/reflected/distance, then y to\n"
        "      send (runtime write — volatile, never calibration)\n",
        prog, DYT_PALETTE_MAX);
}

int main(int argc, char **argv)
{
    dyt_capture_opts o;
    viewer           v;
    dyt_capture_t   *cap = NULL;

    int         zoom      = 2;
    int         palette   = 1;
    bool        have_lo   = false, have_hi   = false;
    float       lo = 0.f, hi = 0.f;
    std::string png, palette_dir;
    int         timeout_s = 40;
    int         settle_s  = 12;
    int         frames    = 0;

    dyt_capture_opts_default(&o);

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) {
            usage(argv[0]);
            return 0;
        }
        if (!std::strcmp(a, "--ad-output"))
            o.output = DYT_OUTPUT_AD;
        else if (!std::strcmp(a, "--zoom") && i + 1 < argc)
            zoom = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--palette") && i + 1 < argc)
            palette = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--lo") && i + 1 < argc) {
            lo = std::strtof(argv[++i], nullptr); have_lo = true;
        } else if (!std::strcmp(a, "--hi") && i + 1 < argc) {
            hi = std::strtof(argv[++i], nullptr); have_hi = true;
        } else if (!std::strcmp(a, "--palette-dir") && i + 1 < argc)
            palette_dir = argv[++i];
        else if (!std::strcmp(a, "--png") && i + 1 < argc)
            png = argv[++i];
        else if (!std::strcmp(a, "--timeout") && i + 1 < argc)
            timeout_s = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--settle") && i + 1 < argc)
            settle_s = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--frames") && i + 1 < argc)
            frames = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--vid") && i + 1 < argc)
            o.vid = static_cast<uint16_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (!std::strcmp(a, "--pid") && i + 1 < argc)
            o.pid = static_cast<uint16_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (!std::strcmp(a, "--format-index") && i + 1 < argc)
            o.format_index = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--width") && i + 1 < argc)
            o.width = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--height") && i + 1 < argc)
            o.height = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--fps") && i + 1 < argc)
            o.fps = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--t-amb") && i + 1 < argc)
            o.t_amb = std::strtof(argv[++i], nullptr);
        else if (!std::strcmp(a, "--sensor-mode") && i + 1 < argc)
            o.sensor_mode = static_cast<int>(std::strtol(argv[++i], nullptr, 0));
        else if (!std::strcmp(a, "--fix-mode") && i + 1 < argc)
            o.fix_mode = static_cast<int>(std::strtol(argv[++i], nullptr, 0));
        else {
            usage(argv[0]);
            return 2;
        }
    }

    v.sess = dyt_session_create();
    if (!v.sess) {
        std::fprintf(stderr, "dytview: out of memory\n");
        return 1;
    }

    /* Palettes first, so --palette can be validated against what actually
     * loaded rather than against a hard-coded count. */
    int palette_n;
    {
        std::string dir = find_palette_dir(palette_dir);
        palette_n = dyt_session_load_palettes(v.sess, dir.empty() ? nullptr
                                                                  : dir.c_str());
        if (palette_n < 1) {
            std::fprintf(stderr, "dytview: no palettes available\n");
            dyt_session_free(v.sess);
            return 1;
        }
        std::fprintf(stderr, "dytview: %d palettes loaded%s\n", palette_n,
                     dir.empty() ? " (built-in approximations)" :
                                   (" from " + dir).c_str());
    }
    if (palette < 1 || palette > palette_n) {
        std::fprintf(stderr, "dytview: --palette must be 1..%d\n", palette_n);
        dyt_session_free(v.sess);
        return 2;
    }
    dyt_session_set_palette(v.sess, palette - 1);

    if (zoom < DYT_ZOOM_MIN) zoom = DYT_ZOOM_MIN;
    if (zoom > DYT_ZOOM_MAX) zoom = DYT_ZOOM_MAX;
    dyt_session_zoom(v.sess, zoom - DYT_ZOOM_MIN);

    /* A pinned range must be complete to mean anything. */
    if (have_lo && have_hi && hi > lo) {
        dyt_session_set_fixed_range(v.sess, lo, hi);
    } else if (have_lo || have_hi) {
        std::fprintf(stderr, "dytview: --lo and --hi must be given together "
                            "and satisfy hi > lo; using auto range\n");
    }

    if (dyt_capture_open(&cap, &o) != 0) {
        dyt_session_free(v.sess);
        return 1;
    }
    v.mode = dyt_capture_mode(cap);

    /* The capture -> session adapter (Phase 5).  It owns the frame callback
     * and, in the default dual-half mode, extracts the visible plane the
     * fusion patterns need; in the AD mode it feeds temperatures only. */
    dyt_session_capture_t *sc = dyt_session_capture_create(v.sess);
    if (!sc || dyt_session_capture_set_capture(sc, cap) != 0) {
        std::fprintf(stderr, "dytview: out of memory\n");
        dyt_capture_close(cap);
        dyt_session_free(v.sess);
        return 1;
    }

    /* Both output modes stream a flat 0x8000 filler for ~6 s, and both then
     * drift for a few more as the sensor settles. */
    v.note = o.output == DYT_OUTPUT_AD
                 ? "setTinyCOutputADValue sent — the sensor drifts for ~8 s "
                   "after it takes effect"
                 : "dual-half mode — no vendor order sent";

    /* Read the serial and stored parameters.  Done straight after
     * dyt_capture_open() and *before* the stream starts: the vendor's
     * control transfers compete with the isochronous stream, and measured
     * 2026-09-25 the identity reads succeed cleanly only while the device
     * is idle.  dyt_capture_open() has already claimed the control
     * interface, so nothing further is needed.  ~18 control transfers, so
     * once. */
    auto read_device_info = [&]() {
        if (dyt_capture_read_info(cap, &v.info) != 0)
            return;
        v.have_info = 1;
        std::fprintf(stderr, "dytview: serial %s",
                     v.info.have_sn ? v.info.sn_str : "(read failed)");
        if (v.info.usn_len >= 0)
            std::fprintf(stderr, ", user %s (key %u)", v.info.usn_str,
                         (unsigned)v.info.usn_key);
        std::fprintf(stderr, ", %d/%d params",
                     v.info.params_read, DYT_PARAM_N);
        if (v.info.radio.ok & DYT_RADIO_EMISSIVITY)
            std::fprintf(stderr, ", emissivity %.4f, distance %.4f m",
                         (double)dyt_radiometry_emissivity(&v.info.radio),
                         (double)dyt_radiometry_distance_m(&v.info.radio));
        std::fprintf(stderr, "\n");
    };
    read_device_info();

    /* Headless: stream, wait for the device to leave its filler,
     * give the sensor a fixed settle, render one frame, save it, exit.  The
     * settle wait matters: once the AD-output order takes effect the sensor
     * drifts for ~8 s (this unit reads ~40 C and falls to ~30 C), and the
     * drift is asymptotic, so a "has it stopped moving?" test would never
     * converge — a fixed delay is both simpler and predictable.  No window is
     * ever created. */
    if (!png.empty()) {
        if (dyt_capture_start(cap, dyt_session_capture_on_frame, sc) != 0) {
            dyt_capture_close(cap);
            dyt_session_capture_free(sc);
            dyt_session_free(v.sess);
            return 1;
        }

        dyt_snapshot_t snap;
        bool   live = false;
        double waited = 0.0;

        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(timeout_s);
        auto t_live   = deadline;      /* when the filler first ended */

        while (!g_stop && std::chrono::steady_clock::now() < deadline) {
            if (grab(&v, snap) && snap.ready) {
                auto now = std::chrono::steady_clock::now();
                if (!live) {
                    live   = true;
                    t_live = now;
                    std::fprintf(stderr, "dytview: live data at frame %ld, "
                                         "settling %d s\n", snap.seq, settle_s);
                }
                if (now - t_live >= std::chrono::seconds(settle_s)) {
                    waited = std::chrono::duration<double>(now - t_live).count();
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        bool  have_snap = grab(&v, snap);
        float mean      = have_snap ? snap.stats.mean : 0.f;

        /* Through the port's own writer (the vendored codec), so this headless
         * path exercises the same code the still writer does rather than
         * depending on OpenCV being able to write the format. */
        cv::Mat canvas = render(&v), rgb;
        cv::cvtColor(canvas, rgb, cv::COLOR_BGR2RGB);
        bool ok = dyt_write_png(png.c_str(), rgb.data, rgb.cols, rgb.rows) == 0;
        dyt_capture_stop(cap);
        dyt_capture_close(cap);
        dyt_session_capture_free(sc);
        dyt_session_free(v.sess);

        if (!ok) {
            std::fprintf(stderr, "dytview: cannot write %s\n", png.c_str());
            return 1;
        }
        std::printf("%s: %ld frame(s)%s", png.c_str(),
                    have_snap ? snap.seq : 0L,
                    live ? "" : " — WARNING: no live data, wrote the filler");
        if (live)
            std::printf(", settled %.1f s, mean %.2f C", waited, mean);
        std::printf("\n");
        return live ? 0 : 1;
    }

    /* Interactive. */
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    cv::namedWindow(kWin, cv::WINDOW_AUTOSIZE);
    cv::setMouseCallback(kWin, on_mouse, &v);

    /* Paint the placeholder before bring-up.  dyt_capture_start() blocks for a
     * few seconds waiting for the first frame before it sends the AD order, and
     * an unpainted window for that long looks like a hang. */
    cv::imshow(kWin, render(&v));
    cv::waitKey(1);

    if (dyt_capture_start(cap, dyt_session_capture_on_frame, sc) != 0) {
        dyt_capture_close(cap);
        dyt_session_capture_free(sc);
        dyt_session_free(v.sess);
        return 1;
    }

    long painted = 0;
    for (;;) {
        cv::Mat canvas = render(&v);
        cv::imshow(kWin, canvas);

        int key = cv::waitKey(30) & 0xFF;

        /* Expire the transient write-result message. */
        if (v.msg_ttl > 0 && --v.msg_ttl == 0)
            v.msg.clear();

        /* Phase 4: the runtime-parameter keys come first.  While a write is
         * armed this consumes every key except `q`, so a stray palette key
         * cannot slip past the confirmation. */
        int consumed = handle_param_key(&v, cap, key);

        /* Phase 5: fusion and its alignment. */
        if (!consumed)
            consumed = handle_fusion_key(&v, key);

        if (consumed) {
            /* handled by the phase handlers above */
        } else if (key == 'q' || key == 27) {
            break;
        } else if (key >= '1' && key <= '9') {
            dyt_session_set_palette(v.sess, key - '1');
        } else if (key == '0') {
            dyt_session_set_palette(v.sess, 9);
        } else if (key == '.' || key == ',') {
            dyt_session_cycle_palette(v.sess, key == '.' ? 1 : -1);
        } else if (key == 'u') {
            dyt_session_cycle_unit(v.sess);
        } else if (key == 'r') {
            dyt_session_toggle_range(v.sess);
        } else if (key == 'h') {
            dyt_session_toggle_flip_h(v.sess);
        } else if (key == 'v') {
            dyt_session_toggle_flip_v(v.sess);
        } else if (key == 'x') {
            /* A quarter turn clockwise per press, the step the Qt shell's rail
             * takes.  The picture and the pointer mapping both follow the
             * session's transform, so there is nothing to keep in step here. */
            dyt_session_rotate(v.sess, DYT_ROT_90);
        } else if (key == '+' || key == '=') {
            dyt_session_zoom(v.sess, 1);
        } else if (key == '-' || key == '_') {
            dyt_session_zoom(v.sess, -1);
        } else if (key == 'p' || key == 'l' || key == 'b' || key == 'o' ||
                   key == 'n') {
            /* One key per tool; "n" also forgets the placed points so the
             * next tool starts clean. */
            v.tool = (key == 'p') ? DYT_TOOL_POINT   :
                     (key == 'l') ? DYT_TOOL_LINE    :
                     (key == 'b') ? DYT_TOOL_BOX     :
                     (key == 'o') ? DYT_TOOL_POLYGON : DYT_TOOL_NONE;
            dyt_session_set_tool(v.sess, v.tool);
            if (key == 'n')
                dyt_session_clear_points(v.sess);
        } else if ((key == 13 || key == 10) && v.tool == DYT_TOOL_POLYGON) {
            /* Enter closes the outline, so the next click starts a new one
             * instead of extending the shape just finished.  A polygon
             * gesture rather than a tool key, which is why it is not in the
             * chain above. */
            dyt_session_set_polygon_closed(v.sess, 1);
        } else if (key == 8 && v.tool == DYT_TOOL_POLYGON) {
            /* Backspace takes the last vertex back.  Not "z": that is the
             * super-resolution key, and a polygon tool that shadowed it would
             * be the only way to lose the binding. */
            dyt_session_polygon_undo(v.sess);
        } else if (key == 'a') {
            /* Arm the alarm across the middle of whatever range is on
             * screen, so the hottest and coldest parts of the scene trip it.
             * A proving-ground choice — the real thresholds are the Phase-4
             * runtime parameters. */
            if (v.have_last && !v.last.alarm_on) {
                float span = v.last.hi - v.last.lo;
                if (!(span > 0.0f)) span = 1.0f;
                dyt_session_set_alarm(v.sess,
                                      v.last.lo + 0.30f * span,
                                      v.last.hi - 0.30f * span,
                                      0.10f * span);
            } else {
                dyt_session_alarm_disable(v.sess);
            }
        } else if (key == 'i') {
            dyt_session_set_isotherm(v.sess, v.have_last ? !v.last.iso_on : 1);
        } else if (key == 'd') {
            v.show_info = !v.show_info;
        } else if (key == 's') {
            /* The DYT still: picture + raw payload, so it can be re-rendered. */
            save_dyt_still(&v);
        } else if (key == 'w') {
            /* The whole window as a PNG — what the UI actually looks like. */
            save_canvas_png(&v, canvas);
        }

        painted++;
        if (frames > 0 && painted >= frames)
            break;

        /* Closing the window is the other way out. */
        try {
            if (cv::getWindowProperty(kWin, cv::WND_PROP_VISIBLE) < 1)
                break;
        } catch (const cv::Exception &) {
            break;
        }
    }

    dyt_capture_stop(cap);
    dyt_capture_close(cap);
    cv::destroyAllWindows();
    dyt_session_capture_free(sc);
    dyt_session_free(v.sess);
    return 0;
}
