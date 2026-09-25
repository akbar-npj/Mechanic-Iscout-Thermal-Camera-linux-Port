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
#include "dytjpeg.h"     /* the DYT still container (Phase 6) */
#include "imgwrite.h"    /* dyt_write_png, for the canvas snapshot */
#include "jpeg.h"        /* dyt_jpeg_encode */
#include "palette.h"     /* DYT_PALETTE_N, for the colour bar */
#include "session.h"
#include "session_capture.h"

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

    std::vector<float> temps;         /* scratch for the per-frame snapshot */
    int mouse_x = -1, mouse_y = -1;

    /* Measurement.  The tool mirrors the session's, and the geometry below is
     * the *last rendered* frame, so a mouse click can be mapped from window
     * coordinates back to source pixels. */
    dyt_tool_t tool = DYT_TOOL_NONE;
    int        dragging = 0;
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

/* Take a consistent snapshot, growing the scratch buffer if the frame size
 * changed.  The session reports the new geometry on -2 precisely so this
 * retry is possible.
 *
 * The temperature plane is always requested, because the hover readout needs
 * random access to it.  Note the deliberate 1-element seed: dyt_session_
 * snapshot() only takes the too-small path when temps_out is non-NULL, and an
 * empty std::vector's data() may be NULL, which would silently skip the copy.
 * Seeding guarantees a non-NULL buffer, so the first call reports the real
 * geometry on -2 and we size it exactly. */
static bool grab(viewer *v, dyt_snapshot_t &snap)
{
    if (v->temps.empty())
        v->temps.resize(1);

    for (int attempt = 0; attempt < 4; attempt++) {
        int rc = dyt_session_snapshot(v->sess, &snap, v->temps.data(),
                                      static_cast<int>(v->temps.size()));
        if (rc == 0)
            return true;
        if (rc == -1)
            return false;                       /* no frame yet */
        v->temps.resize(static_cast<size_t>(snap.width) * snap.height);
    }
    return false;
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

/* A temperature in the selected unit, e.g. "31.2 C".  The unit conversion is
 * the library's (units.c), so the viewer and the Qt6 app cannot disagree. */
static std::string tstr(dyt_unit_t u, float celsius)
{
    char b[32];
    if (dyt_temp_format(u, celsius, b, sizeof b) < 0)
        std::snprintf(b, sizeof b, "---");
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

/* The candidate values each key cycles through.  Short and widely spaced on
 * purpose: this is a proving ground for "does the write reach the device",
 * not a full parameter editor. */
struct param_ladder {
    int         type;               /* dyt_order_type_t */
    const char *name;
    int         n;
    const float *vals;
};

static const float kEmisVals[] = { 1.00f, 0.95f, 0.90f, 0.80f, 0.50f, 0.10f };
static const float kAmbVals[]  = { 20.f, 25.f, 30.f, 40.f, 60.f };
static const float kReflVals[] = { 20.f, 25.f, 30.f, 40.f, 100.f };
static const float kDistVals[] = { 0.10f, 0.50f, 1.00f, 2.00f, 5.00f, 10.00f };

static const param_ladder kLadders[] = {
    { DYT_ORDER_EMISSIVITY, "emissivity", 6, kEmisVals },
    { DYT_ORDER_AMBIENT,    "ambient",    5, kAmbVals  },
    { DYT_ORDER_REFLECTED,  "reflected",  5, kReflVals },
    { DYT_ORDER_DISTANCE,   "distance",   6, kDistVals },
};

static const param_ladder *ladder_for(int type)
{
    for (const param_ladder &L : kLadders)
        if (L.type == type)
            return &L;
    return nullptr;
}

/* A candidate value with its unit, e.g. "0.10" / "25.0 C" / "1.00 m". */
static std::string param_value_str(int type, float v)
{
    char b[32];
    if (type == DYT_ORDER_EMISSIVITY)
        std::snprintf(b, sizeof b, "%.2f", (double)v);
    else if (type == DYT_ORDER_DISTANCE)
        std::snprintf(b, sizeof b, "%.2f m", (double)v);
    else
        std::snprintf(b, sizeof b, "%.1f C", (double)v);
    return std::string(b);
}

/* The Phase-4 key handler.  A parameter key arms a candidate value; only a
 * following `y` sends it.  While something is armed every other key is
 * swallowed (except `q`), so a stray keypress cannot slip past the
 * confirmation.  Returns 1 if the key was consumed. */
static int handle_param_key(viewer *v, dyt_capture_t *cap, int key)
{
    int type = (key == 'e') ? DYT_ORDER_EMISSIVITY :
               (key == 'A') ? DYT_ORDER_AMBIENT    :
               (key == 'R') ? DYT_ORDER_REFLECTED  :
               (key == 'D') ? DYT_ORDER_DISTANCE   : 0;

    if (type) {
        const param_ladder *L = ladder_for(type);
        /* Re-pressing the same key advances the candidate; a different key
         * starts its own ladder at the first rung. */
        v->pend_rung  = (v->pend_type == type) ? (v->pend_rung + 1) % L->n : 0;
        v->pend_type  = type;
        v->pend_value = L->vals[v->pend_rung];
        return 1;
    }

    if (!v->pend_type)
        return 0;                       /* nothing armed: not ours */

    if (key == 'q')
        return 0;                       /* never swallow quit */

    if (key == 'y' || key == 'Y') {
        const param_ladder *L = ladder_for(v->pend_type);
        int rc = dyt_capture_set_param(cap, v->pend_type, v->pend_value);
        char b[96];
        if (rc == 0) {
            uint16_t raw = (v->pend_type == DYT_ORDER_EMISSIVITY ||
                            v->pend_type == DYT_ORDER_DISTANCE)
                               ? dyt_param_encode_ratio(v->pend_value)
                               : dyt_param_encode_kelvin(v->pend_value);
            v->override_on[v->pend_type] = 1;
            v->override_v[v->pend_type]  = v->pend_value;
            std::snprintf(b, sizeof b, "set %s = %s  (sent)", L->name,
                          param_value_str(v->pend_type, v->pend_value).c_str());
            std::fprintf(stderr, "dytview: set %s = %s -> raw %u (type %d)\n",
                         L->name,
                         param_value_str(v->pend_type, v->pend_value).c_str(),
                         (unsigned)raw, v->pend_type);
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

    if (key == 'n' || key == 'N' || key == 27) {
        v->msg = std::string("cancelled: ") + ladder_for(v->pend_type)->name;
        v->msg_ttl = 90;
        v->pend_type = 0;
        return 1;
    }

    /* Any other key while armed is ignored, so a stray palette key cannot
     * slip past a pending confirmation. */
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

    const param_ladder *L = ladder_for(v->pend_type);
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
    if (!v->have_info || !v->show_info)
        return;

    const dyt_device_info_t &d = v->info;
    std::vector<std::string> lines;
    char b[96];

    lines.push_back(std::string("serial  ") +
                    (d.have_sn ? d.sn_str : "(read failed)"));

    if (d.have_usn) {
        if (d.usn_len >= 0) {
            std::snprintf(b, sizeof b, "user    %s%s  (key %u)", d.usn_str,
                          d.usn_variant ? "  (variant C)" : "",
                          (unsigned)d.usn_key);
        } else {
            std::snprintf(b, sizeof b, "user    (raw read; no key)");
        }
        lines.push_back(b);
    }

    /* A parameter's effective value: the runtime override if this session
     * sent one, else the value read at start-up.  A trailing `*` flags an
     * override, so the panel never silently shows a superseded stored value
     * (a write is volatile and cannot be re-read while streaming — §4.8). */
    auto pv = [&](int type, float stored, const char *fmt) {
        char t[40];
        std::snprintf(t, sizeof t, fmt,
                      (double)(v->override_on[type] ? v->override_v[type]
                                                    : stored));
        return std::string(t) + (v->override_on[type] ? "*" : "");
    };
    auto over = [&](int type) { return v->override_on[type] != 0; };

    bool any_over = false;
    for (int t = DYT_ORDER_REFLECTED; t <= DYT_ORDER_DISTANCE; t++)
        any_over = any_over || over(t);

    if ((d.radio.ok & DYT_RADIO_REFLECTED) || over(DYT_ORDER_REFLECTED) ||
        (d.radio.ok & DYT_RADIO_AMBIENT)    || over(DYT_ORDER_AMBIENT)) {
        lines.push_back("refl " + pv(DYT_ORDER_REFLECTED,
                                     dyt_radiometry_reflected_c(&d.radio), "%.2f") +
                        " C   amb " +
                        pv(DYT_ORDER_AMBIENT,
                           dyt_radiometry_ambient_c(&d.radio), "%.2f") + " C");
    }
    if ((d.radio.ok & DYT_RADIO_EMISSIVITY) || over(DYT_ORDER_EMISSIVITY) ||
        (d.radio.ok & DYT_RADIO_DISTANCE)   || over(DYT_ORDER_DISTANCE)) {
        lines.push_back("emis " + pv(DYT_ORDER_EMISSIVITY,
                                     dyt_radiometry_emissivity(&d.radio), "%.4f") +
                        "   dist " +
                        pv(DYT_ORDER_DISTANCE,
                           dyt_radiometry_distance_m(&d.radio), "%.4f") + " m");
    }
    std::snprintf(b, sizeof b, "params %d/%d slots%s", d.params_read, DYT_PARAM_N,
                  any_over ? "   (* = set this session)" : "");
    lines.push_back(b);

    const int pad = 6, lh = 17;
    int wmax = 0;
    for (const std::string &s : lines)
        wmax = std::max(wmax, 10 + static_cast<int>(s.size()) * 8);

    cv::Rect box(6, 6, wmax + 2 * pad, lh * static_cast<int>(lines.size()) + 2 * pad);
    cv::rectangle(canvas, box, cv::Scalar(16, 16, 16), cv::FILLED);
    cv::rectangle(canvas, box, cv::Scalar(120, 120, 120), 1);

    for (size_t i = 0; i < lines.size(); i++)
        put_text(canvas, lines[i], box.x + pad,
                 box.y + pad + lh * static_cast<int>(i) + 12,
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
    put_text(m, "      h/v mirror   +/- zoom   s still   w png   q quit",
             20, 244, cv::Scalar(130, 130, 130), 0.45);
    put_text(m, "      p point   l line   b box   n clear   a alarm   i isotherm",
             20, 268, cv::Scalar(130, 130, 130), 0.45);
    put_text(m, "      f fusion   [ ] align X   ; ' align Y",
             20, 292, cv::Scalar(130, 130, 130), 0.45);
    put_text(m, "      d device info",
             20, 316, cv::Scalar(130, 130, 130), 0.45);
    put_text(m, "      e emis  A amb  R refl  D dist  (then y to send)",
             20, 340, cv::Scalar(130, 130, 130), 0.45);
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
     * pixels that would trip the alarm stand out. ---- */
    if (snap.iso_on && v->temps.size() >= static_cast<size_t>(npix)) {
        for (int yy = 0; yy < h; yy++) {
            uint8_t *row = bgr.ptr<uint8_t>(yy);
            for (int xx = 0; xx < w; xx++) {
                float t = v->temps[static_cast<size_t>(yy) * w + xx];
                bool  inside = (t == t) && t >= snap.iso_lo && t <= snap.iso_hi;
                if (!inside) {
                    row[xx * 3 + 0] = static_cast<uint8_t>(row[xx * 3 + 0] / 2);
                    row[xx * 3 + 1] = static_cast<uint8_t>(row[xx * 3 + 1] / 2);
                    row[xx * 3 + 2] = static_cast<uint8_t>(row[xx * 3 + 2] / 2);
                }
            }
        }
    }

    /* Mirror and zoom are applied here, after the render, which is exactly the
     * order dyt_view_transform_map()/project() assume. */
    cv::Mat big;
    if (snap.xform.zoom > 1)
        cv::resize(bgr, big, cv::Size(), snap.xform.zoom, snap.xform.zoom,
                   cv::INTER_NEAREST);
    else
        big = bgr;
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
        float u   = barH > 1 ? 1.f - static_cast<float>(j) / (barH - 1) : 1.f;
        int   idx = static_cast<int>(u * (DYT_PALETTE_N - 1) + 0.5f);
        cv::rectangle(canvas, cv::Rect(x0, j, kBarW, 1), pal_bgr(&pal, idx),
                      cv::FILLED);
    }
    cv::rectangle(canvas, cv::Rect(x0, 0, kBarW, barH),
                  cv::Scalar(200, 200, 200), 1);

    const int lx = x0 + kBarW + 4;
    put_text(canvas, tstr(snap.unit, snap.hi), lx, 12, cv::Scalar(255, 255, 255));
    put_text(canvas, tstr(snap.unit, (snap.lo + snap.hi) * 0.5f), lx, barH / 2,
             cv::Scalar(210, 210, 210));
    put_text(canvas, tstr(snap.unit, snap.lo), lx, barH - 3,
             cv::Scalar(255, 255, 255));

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
            put_text(canvas, std::string(tag) + " " + tstr(snap.unit, c),
                     cx + 8, cy - 6, col);
        };
        mark(snap.stats.hot_x, snap.stats.hot_y, snap.stats.hi,
             cv::Scalar(0, 0, 255), "H");     /* hottest: red */
        mark(snap.stats.cold_x, snap.stats.cold_y, snap.stats.lo,
             cv::Scalar(255, 120, 0), "L");   /* coldest: blue */
    }

    /* ---- hover readout: the temperature under the pointer ---- */
    if (v->mouse_x >= 0 && v->mouse_y >= 0 &&
        v->mouse_x < big.cols && v->mouse_y < big.rows) {
        int ix = 0, iy = 0;
        if (dyt_view_transform_map(&snap.xform, w, h, big.cols, big.rows,
                                   v->mouse_x, v->mouse_y, &ix, &iy) == 0) {
            float t = v->temps[static_cast<size_t>(iy) * w + ix];
            std::string s = (t == t)
                ? tstr(snap.unit, t) + "  (" + std::to_string(ix) + "," +
                      std::to_string(iy) + ")"
                : std::string("---  (") + std::to_string(ix) + "," +
                      std::to_string(iy) + ")";
            put_text(canvas, s, v->mouse_x + 10, v->mouse_y - 8,
                     cv::Scalar(255, 255, 255), 0.5);
            cv::line(canvas, cv::Point(v->mouse_x - 6, v->mouse_y),
                     cv::Point(v->mouse_x + 6, v->mouse_y),
                     cv::Scalar(0, 0, 0), 1);
            cv::line(canvas, cv::Point(v->mouse_x, v->mouse_y - 6),
                     cv::Point(v->mouse_x, v->mouse_y + 6),
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
                put_text(canvas, "P " + tstr(snap.unit, snap.point_c),
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
                                  tstr(snap.unit, snap.roi.min).c_str(),
                                  tstr(snap.unit, snap.roi.max).c_str(),
                                  tstr(snap.unit, snap.roi.mean).c_str(),
                                  tstr(snap.unit, snap.roi.median).c_str());
                    put_text(canvas, buf, r.x + 4, r.y - 6,
                             cv::Scalar(0, 255, 255), 0.5);
                }
            }
        }
    }

    /* ---- device identity overlay (Phase 3) ---- */
    draw_info_panel(canvas, v);

    /* ---- pending-write confirmation (Phase 4), on top of everything ---- */
    draw_confirm_prompt(canvas, v);

    /* ---- status strip ---- */
    {
        const char *mm = snap.xform.flip_h && snap.xform.flip_v ? "HV" :
                         snap.xform.flip_h ? "H" : snap.xform.flip_v ? "V" : "-";

        /* Fusion sits on the first line because it is a view mode, like the
         * palette.  A pattern that cannot be honoured on this frame — the AD
         * output mode has no visible half — is called out rather than left to
         * look like it is working. */
        std::string fus = "fusion " + std::string(snap.fusion_name);
        if (snap.fusion_dx || snap.fusion_dy) {
            char fb[32];
            std::snprintf(fb, sizeof fb, " %+d,%+d", snap.fusion_dx,
                          snap.fusion_dy);
            fus += fb;
        }
        if (!snap.fusion_active && snap.fusion != DYT_FUSION_INFRARED)
            fus += " (no visible plane)";

        std::string status =
            std::string(v->mode == DYT_MODE_44C  ? "mode 0x44c" :
                        v->mode == DYT_MODE_1000 ? "mode 1000" : "mode ?") +
            " | " + fus +
            " | " + snap.palette_name +
            " " + std::to_string(snap.palette + 1) + "/" +
                  std::to_string(snap.palette_n) +
            " | " + (dyt_unit_suffix(snap.unit) ? dyt_unit_suffix(snap.unit) : "?") +
            " | x" + std::to_string(snap.xform.zoom) + mm +
            " | " + std::to_string(static_cast<long>(snap.seq)) + " frames";

        /* Second line: the measurement and alarm state.  Kept compact — the
         * strip is only as wide as the image, and the unit is already on the
         * line above, so the numbers here carry no suffix. */
        auto num = [&](float celsius) {
            char b[24];
            std::snprintf(b, sizeof b, "%.1f",
                          (double)dyt_temp_convert(snap.unit, celsius));
            return std::string(b);
        };

        std::string info;
        switch (snap.tool) {
        case DYT_TOOL_POINT:
            info = "point " + (snap.point_ok ? tstr(snap.unit, snap.point_c)
                                             : std::string("--"));
            break;
        case DYT_TOOL_LINE: {
            char b[64];
            std::snprintf(b, sizeof b, "line (%d,%d)-(%d,%d)",
                          snap.p0.x, snap.p0.y, snap.p1.x, snap.p1.y);
            info = b;
            break;
        }
        case DYT_TOOL_BOX: {
            char b[80];
            std::snprintf(b, sizeof b, "box (%d,%d)-(%d,%d) n=%d",
                          snap.p0.x, snap.p0.y, snap.p1.x, snap.p1.y,
                          snap.roi.n);
            info = b;
            break;
        }
        case DYT_TOOL_NONE:
        default:
            info = "tool: none (p point, l line, b box, n clear)";
            break;
        }

        if (snap.alarm_on)
            info += "  |  alarm " + std::string(dyt_alarm_name(snap.alarm)) +
                    " " + num(snap.alarm_lo) + ".." + num(snap.alarm_hi);
        if (snap.iso_on) {
            char b[48];
            std::snprintf(b, sizeof b, "  |  iso %ld px", snap.iso.count);
            info += b;
        }
        if (!v->msg.empty())
            info += "  |  " + v->msg;

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
    viewer *v = static_cast<viewer *>(user);

    v->mouse_x = x;
    v->mouse_y = y;

    if (v->tool == DYT_TOOL_NONE)
        return;

    int sx = 0, sy = 0;
    bool ok = dyt_view_transform_map(&v->xform, v->src_w, v->src_h,
                                     v->dst_w, v->dst_h, x, y, &sx, &sy) == 0;

    if (event == cv::EVENT_LBUTTONDOWN) {
        v->dragging = 1;
        if (ok) {
            dyt_session_set_point(v->sess, 0, sx, sy);
            dyt_session_set_point(v->sess, 1, sx, sy);
        }
        return;
    }
    if (event == cv::EVENT_MOUSEMOVE && v->dragging) {
        if (ok)
            dyt_session_set_point(v->sess, 1, sx, sy);
        return;
    }
    if (event == cv::EVENT_LBUTTONUP) {
        v->dragging = 0;
    }
}

/* ------------------------------------------------------------------ palettes */

static std::string exe_dir(void)
{
    char    buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0)
        return "";
    buf[n] = '\0';
    std::string s(buf);
    size_t p = s.rfind('/');
    return p == std::string::npos ? "" : s.substr(0, p);
}

/* Find the palette directory: an explicit --palette-dir wins, then the usual
 * locations relative to the cwd and to the binary. */
static std::string find_palette_dir(const std::string &dir_opt)
{
    std::vector<std::string> dirs;
    if (!dir_opt.empty())
        dirs.push_back(dir_opt);
    dirs.push_back("palettes");
    dirs.push_back("../palettes");
    std::string ed = exe_dir();
    if (!ed.empty()) {
        dirs.push_back(ed + "/palettes");
        dirs.push_back(ed + "/../palettes");
    }
    for (size_t i = 0; i < dirs.size(); i++) {
        if (access(dirs[i].c_str(), R_OK) == 0)
            return dirs[i];
    }
    return "";
}

static void stamp_now(char *out, size_t n)
{
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    std::strftime(out, n, "%Y%m%d-%H%M%S", &tm);
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
 * own raw payload and the frame geometry spliced in as APP2 segments
 * (dytjpeg.h).  A vendor tool can open it and re-render the raw data with its
 * own palette and range, which a plain PNG cannot carry.
 *
 * The raw payload comes from the session, not from dyt_capture_last_raw():
 * that pointer is only valid on the frame callback thread. */
static void save_dyt_still(viewer *v)
{
    dyt_snapshot_t snap;
    std::vector<uint8_t>  rgb;
    std::vector<uint16_t> raw;
    uint8_t *jpg = nullptr;
    size_t   jlen = 0;
    int      w = 0, h = 0, n;
    uint8_t  blob[DYT_DYT_BLOB_SIZE];
    unsigned flags;
    char     stamp[32], path[128];

    if (dyt_session_snapshot(v->sess, &snap, nullptr, 0) != 0 || !snap.ready) {
        v->msg = "still: no live frame yet"; v->msg_ttl = 120; return;
    }
    if (!snap.have_raw) {
        v->msg = "still: no raw payload"; v->msg_ttl = 120; return;
    }

    rgb.resize((size_t)snap.width * snap.height * 3);
    if (dyt_session_render_rgb(v->sess, rgb.data(), (int)rgb.size(),
                               &w, &h) != 0) {
        v->msg = "still: render failed"; v->msg_ttl = 120; return;
    }

    if (dyt_jpeg_encode(rgb.data(), w, h, 85, &jpg, &jlen) != 0) {
        v->msg = "still: JPEG encode failed"; v->msg_ttl = 120; return;
    }

    /* The geometry the still records is the *payload's*, so a reader knows how
     * to interpret the raw samples: the thermal plane is height rows of it,
     * and anything above that is the visible half. */
    flags = snap.raw_total_rows > snap.height ? DYT_DYT_FLAG_DUAL_HALF : 0u;
    if (dyt_dyt_blob_init(blob, sizeof blob, snap.width, snap.height,
                          snap.raw_total_rows, flags) != 0) {
        std::free(jpg);
        v->msg = "still: bad geometry"; v->msg_ttl = 120; return;
    }

    raw.resize((size_t)snap.raw_n);
    n = dyt_session_raw(v->sess, raw.data(), (int)raw.size());
    if (n != snap.raw_n) {
        std::free(jpg);
        v->msg = "still: raw payload changed under us"; v->msg_ttl = 120;
        return;
    }

    stamp_now(stamp, sizeof stamp);
    std::snprintf(path, sizeof path, "dytview_%s.jpg", stamp);

    if (dyt_dyt_write(path, jpg, jlen, blob, sizeof blob,
                      reinterpret_cast<const uint8_t *>(raw.data()),
                      raw.size() * sizeof(uint16_t)) != 0) {
        std::free(jpg);
        v->msg     = std::string("cannot write ") + path;
        v->msg_ttl = 120;
        return;
    }
    std::free(jpg);

    {
        char b[256];
        std::snprintf(b, sizeof b, "wrote %s  (%dx%d, %zu raw bytes)",
                      path, snap.width, snap.raw_total_rows,
                      raw.size() * sizeof(uint16_t));
        v->msg = b;
    }
    v->msg_ttl = 120;
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
        "      h/v mirror · + / - zoom · q or ESC quit\n"
        "      s save a DYT still (picture + raw payload) · w save a PNG of the\n"
        "      whole window\n"
        "      p point · l line · b box · n clear points (click/drag to place)\n"
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
        } else if (key == '+' || key == '=') {
            dyt_session_zoom(v.sess, 1);
        } else if (key == '-' || key == '_') {
            dyt_session_zoom(v.sess, -1);
        } else if (key == 'p' || key == 'l' || key == 'b' || key == 'n') {
            /* One key per tool; "n" also forgets the placed points so the
             * next tool starts clean. */
            v.tool = (key == 'p') ? DYT_TOOL_POINT :
                     (key == 'l') ? DYT_TOOL_LINE  :
                     (key == 'b') ? DYT_TOOL_BOX   : DYT_TOOL_NONE;
            dyt_session_set_tool(v.sess, v.tool);
            if (key == 'n')
                dyt_session_clear_points(v.sess);
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
