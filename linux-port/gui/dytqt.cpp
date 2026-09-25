/*
 * dytqt.cpp — the Qt6 Widgets front end for the DYT thermal camera.
 *
 * The window is a *shell*: every pixel it draws and every string it shows comes
 * from libdyt.  This file owns layout and paint, and nothing else — the same
 * rule tools/dytview.cpp follows, and the reason the two front ends cannot
 * drift apart.
 *
 * Task #83 proved the shape with a spike; this is the window itself.  It runs
 * against a frozen fixture (the default, and what `--selftest` drives) or the
 * device, and it shows the device's start-up filler honestly instead of
 * painting a 238.85 C "scene" — see DevState below.
 *
 * build:  via the Makefile (needs Qt6 Widgets; see gui/README.md)
 * run:    ./build/dytqt [--fixture PATH] [--palette N] [--zoom N]
 *         ./build/dytqt --selftest
 */
#include <unistd.h>     /* usleep — the settle before the teardown read-back */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <QApplication>
#include <QCloseEvent>
#include <QColor>
#include <QEventLoop>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "capture.h"
#include "frame_source.h"
#include "palette.h"
#include "session.h"
#include "session_capture.h"
#include "view_model.h"

/* ---------------------------------------------------------------- layout */

static const int kBarW   = 22;   /* colour-bar width, px */
static const int kBarGap = 14;   /* image -> bar gap */
static const int kLabelW  = 96;  /* room for the bar's labels */
static const int kPad     = 8;

static const int kLineH   = 16;  /* one status line */
static const int kStripPad = 4;  /* the strip's own margin */

/* ---------------------------------------------------------------- options */

struct opts {
    /* Defaulted here rather than in main() so no caller can forget: parse_args
     * fills cap from the command line, and the selftest constructs bare opts. */
    opts() { dyt_capture_opts_default(&cap); }

    std::string fixture = "testdata/mode1000_256x384_default.raw";
    std::string palette_dir;
    std::string png;                 /* save the canvas here, then exit */
    int  width   = 256;
    int  palette = 1;
    int  zoom    = 2;
    int  frames  = 0;                /* 0 = run until closed */
    int  fps     = 25;
    bool selftest = false;

    bool live        = false;        /* stream from the device, not a fixture */
    bool fixture_set = false;        /* --fixture was given explicitly */
    dyt_capture_opts cap;            /* the live path's capture settings */
};

/* Why this combination of options is unusable, or NULL when it is fine.
 *
 * Split out of parse_args so the rule can be asserted on without the usage text
 * landing in `make check`.  Refusing beats resolving: silently letting --live
 * win, or silently ignoring it, would each leave the user with a window that is
 * not showing what they asked for. */
static const char *opts_conflict(const opts &o)
{
    if (!o.live)
        return nullptr;
    if (o.fixture_set)
        return "--live and --fixture ask for two different sources";
    if (o.selftest)
        return "--live and --selftest ask for two different sources";
    return nullptr;
}

static void usage(const char *prog)
{
    std::fprintf(stderr,
        "usage: %s [options]\n"
        "  --fixture PATH   raw payload to replay (default %s)\n"
        "  --width N        sensor width of the fixture (default 256); with\n"
        "                   --live, the capture width override instead\n"
        "  --palette N      1-based palette index (default 1)\n"
        "  --palette-dir D  where the *.dat ramps live (default: search)\n"
        "  --zoom N         window magnification (default 2)\n"
        "  --frames N       stop after N frames (default: run until closed)\n"
        "  --fps N          timer rate (default 25)\n"
        "  --png PATH       write the canvas here and exit\n"
        "  --selftest       headless check over the fixture; needs no display\n"
        "\n"
        "live capture (replaces the fixture):\n"
        "  --live           stream from the camera instead of replaying a file\n"
        "  --vid V --pid P  USB ids (0x0000 0x0000 = first matching device)\n"
        "  --format-index N UVC bFormatIndex (0 = auto, uncompressed 16-bpp)\n"
        "  --height N       capture height (0 = auto)\n"
        "  --t-amb C        LUT ambient for the live path (default 25.0)\n"
        "  --ad-output      send setTinyCOutputADValue and read the flat\n"
        "                   256x192 raw-AD frame; default is the device's own\n"
        "                   256x384 dual-half frame, which needs no order\n"
        "\n"
        "with --live the window reconnects on its own; press r to retry now\n",
        prog, opts{}.fixture.c_str());
}

static bool parse_args(int argc, char **argv, opts &o)
{
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char *what) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "dytqt: %s needs a value\n", what);
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "--help" || a == "-h")            { usage(argv[0]); return false; }
        else if (a == "--selftest")                o.selftest = true;
        else if (a == "--live")                    o.live = true;
        else if (a == "--ad-output")               o.cap.output = DYT_OUTPUT_AD;
        else if (a == "--fixture")                 { const char *v = next("--fixture"); if (!v) return false; o.fixture = v; o.fixture_set = true; }
        else if (a == "--palette-dir")             { const char *v = next("--palette-dir"); if (!v) return false; o.palette_dir = v; }
        else if (a == "--png")                     { const char *v = next("--png"); if (!v) return false; o.png = v; }
        /* --width is the sensor width in both modes: the fixture's, and the
         * capture width override when live.  One flag, because a user who says
         * "the sensor is 384 wide" means it whichever source they picked. */
        else if (a == "--width")                   { const char *v = next("--width"); if (!v) return false; o.width = std::atoi(v); o.cap.width = o.width; }
        else if (a == "--height")                  { const char *v = next("--height"); if (!v) return false; o.cap.height = std::atoi(v); }
        else if (a == "--t-amb")                   { const char *v = next("--t-amb"); if (!v) return false; o.cap.t_amb = std::strtof(v, nullptr); }
        else if (a == "--format-index")            { const char *v = next("--format-index"); if (!v) return false; o.cap.format_index = std::atoi(v); }
        else if (a == "--vid")                     { const char *v = next("--vid"); if (!v) return false; o.cap.vid = (uint16_t)std::strtoul(v, nullptr, 0); }
        else if (a == "--pid")                     { const char *v = next("--pid"); if (!v) return false; o.cap.pid = (uint16_t)std::strtoul(v, nullptr, 0); }
        else if (a == "--palette")                 { const char *v = next("--palette"); if (!v) return false; o.palette = std::atoi(v); }
        else if (a == "--zoom")                    { const char *v = next("--zoom"); if (!v) return false; o.zoom = std::atoi(v); }
        else if (a == "--frames")                  { const char *v = next("--frames"); if (!v) return false; o.frames = std::atoi(v); }
        else if (a == "--fps")                     { const char *v = next("--fps"); if (!v) return false; o.fps = std::atoi(v); }
        else {
            std::fprintf(stderr, "dytqt: unknown option '%s'\n", a.c_str());
            usage(argv[0]);
            return false;
        }
    }
    if (o.width <= 0 || o.fps <= 0) {
        std::fprintf(stderr, "dytqt: --width and --fps must be positive\n");
        return false;
    }
    if (const char *why = opts_conflict(o)) {
        std::fprintf(stderr, "dytqt: %s\n", why);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ setup */

static dyt_session_t *setup_session(const opts &o)
{
    dyt_session_t *sess = dyt_session_create();
    if (!sess) {
        std::fprintf(stderr, "dytqt: out of memory\n");
        return nullptr;
    }

    char dir[4096];
    if (dyt_vm_find_palette_dir(o.palette_dir.empty() ? nullptr
                                                      : o.palette_dir.c_str(),
                                dir, sizeof dir))
        std::fprintf(stderr, "dytqt: palettes from %s\n", dir);

    int n = dyt_session_load_palettes(sess, dir[0] ? dir : nullptr);
    if (n < 1) {
        std::fprintf(stderr, "dytqt: no palettes available\n");
        dyt_session_free(sess);
        return nullptr;
    }
    if (o.palette < 1 || o.palette > n) {
        std::fprintf(stderr, "dytqt: --palette must be 1..%d\n", n);
        dyt_session_free(sess);
        return nullptr;
    }
    dyt_session_set_palette(sess, o.palette - 1);

    int zoom = o.zoom;
    if (zoom < DYT_ZOOM_MIN) zoom = DYT_ZOOM_MIN;
    if (zoom > DYT_ZOOM_MAX) zoom = DYT_ZOOM_MAX;
    dyt_session_zoom(sess, zoom - DYT_ZOOM_MIN);

    return sess;
}

/* -------------------------------------------------------------- frame rate */

/* Painted frames per second over a short trailing window.
 *
 * The baseline is rebased once the window would span more than kWindowS, so a
 * stall reports the *new* rate instead of being averaged away by a long run of
 * earlier samples.  Deliberately not a moving average: the number on screen
 * should be the rate now, not a smoothed memory of it. */
class FpsMeter {
public:
    void update(long long frames, double t)
    {
        if (!have_) {
            have_ = true;
            f0_   = frames;
            t0_   = t;
            return;
        }
        if (t - t0_ > kWindowS) {      /* rebase: forget the old baseline */
            f0_    = frames;
            t0_    = t;
            have2_ = false;
            return;
        }
        f1_    = frames;
        t1_    = t;
        have2_ = true;
    }

    double fps() const
    {
        if (!have2_)
            return 0.0;
        const double dt = t1_ - t0_;
        return dt > 0.0 ? (double)(f1_ - f0_) / dt : 0.0;
    }

private:
    static constexpr double kWindowS = 2.0;
    bool      have_ = false, have2_ = false;
    long long f0_ = 0, f1_ = 0;
    double    t0_ = 0.0, t1_ = 0.0;
};

/* Is the stream still moving?
 *
 * The only liveness signal the session offers is `seq`, the count of frames the
 * engine has processed (session.h:73).  A disconnect and a wedge look identical
 * from here — libuvc handles LIBUSB_TRANSFER_NO_DEVICE silently, so the callback
 * simply stops and `seq` freezes — which is why the state this feeds is named
 * for the symptom, not the cause.
 *
 * `ready` gates it.  While the start-up filler is streaming, `ready` is 0, so a
 * warm-up is never a stall; and because `seq` advances during the filler too, a
 * working warm-up does not trip the counter either.  Both guards are wanted:
 * the first covers a device that connects and sends nothing, the second a
 * device that connects and sends only filler. */
struct StallWatch {
    static constexpr double kStallS = 1.5;   /* ~37 frame intervals at 25 fps */

    bool      have_ = false;
    long long last_seq_ = 0;
    double    last_change_t_ = 0.0;

    bool observe(long long seq, double t, bool ready)
    {
        if (!have_) {
            have_ = true;
            last_seq_ = seq;
            last_change_t_ = t;
            return false;
        }
        if (seq != last_seq_) {
            last_seq_ = seq;
            last_change_t_ = t;
            return false;
        }
        return ready && (t - last_change_t_ > kStallS);
    }

    void reset() { have_ = false; }
};

/* ---------------------------------------------------------- device state
 *
 * next_live() returns WAIT both before the first frame *and* for the whole
 * start-up filler (frame_source.c:271), so the source alone cannot tell
 * "not connected yet" from "connected, still warming up".  The state is
 * therefore decided from the session's own `seq`/`ready`, which is the same
 * scalars-only snapshot next_live() takes internally and costs nothing.
 *
 * Kept a pure function of six booleans so every state is reachable in
 * `--selftest` with no device attached — which is the only way two of these
 * six can ever be tested at all. */
enum class DevState { Fixture, Connecting, NoDevice, WarmingUp, Live, Stalled };

static DevState device_state(bool live, bool bringup_done, int bringup_rc,
                             bool snap_ok, bool snap_ready, bool stalled)
{
    if (!live)                   return DevState::Fixture;
    if (!bringup_done)           return DevState::Connecting;
    if (bringup_rc != 0)         return DevState::NoDevice;
    if (!snap_ok || !snap_ready) return DevState::WarmingUp;
    /* Checked last, so a stall can never mask Connecting/NoDevice/WarmingUp. */
    if (stalled)                 return DevState::Stalled;
    return DevState::Live;
}

static const char *state_label(DevState s)
{
    switch (s) {
    case DevState::Fixture:    return "FIXTURE";
    case DevState::Connecting: return "CONNECTING";
    case DevState::NoDevice:   return "NO DEVICE";
    case DevState::WarmingUp:  return "WARMING UP";
    case DevState::Live:       return "LIVE";
    case DevState::Stalled:    return "NO SIGNAL";
    }
    return "?";
}

/* What the canvas says when it has nothing real to paint. */
static const char *state_placeholder(DevState s)
{
    switch (s) {
    case DevState::Connecting: return "connecting to camera…";
    case DevState::NoDevice:   return "no camera found (see stderr)";
    case DevState::WarmingUp:  return "warming up - waiting for live data…";
    case DevState::Stalled:    return "no signal - last frame held";
    default:                   return "waiting for a frame";
    }
}

/* The third status line, and the front end's own.
 *
 * The two lines above it are the view model's, verbatim; this one carries the
 * two things the view model has no business naming — the frame rate and the
 * device state — plus the range mode, which is formatted by the engine
 * (dyt_range_mode_name) but deliberately *not* folded into
 * dyt_vm_status_line().  That shared line is already ~378 px of a 404 px
 * window at zoom 1, and dytview draws it into a strip only as wide as the
 * image, where it is already clipped; extending it would clip further and
 * would change another front end's display for no gain.  See gui/README.md.
 *
 * The rate is only shown for the states that have painted frames: a "0.0 fps"
 * beside CONNECTING would be a claim about a stream that is not running yet.
 * STALLED is included on purpose — there the 0.0 *is* the evidence, and hiding
 * it would leave the frozen picture looking healthy. */
static QString state_line(DevState s, const dyt_snapshot_t &snap, double fps)
{
    QString out = QStringLiteral("range %1   |   %2")
                      .arg(QString::fromUtf8(dyt_range_mode_name(snap.range_mode)),
                           QString::fromUtf8(state_label(s)));
    if (s == DevState::Fixture || s == DevState::Live ||
        s == DevState::Stalled)
        out += QStringLiteral("  %1 fps").arg(fps, 0, 'f', 1);
    return out;
}

/* ------------------------------------------------------------ the transform
 *
 * dyt_view_transform_map() states the contract (display.h:146): "the output is
 * the source magnified by `zoom` and then mirrored".  Transcribing it in that
 * order is what keeps this a direct expression of the contract the pointer
 * mapping inverts — and that is the property that matters, because a front end
 * that scaled or mirrored differently would put a click on the wrong pixel.
 *
 * (The two orders happen to *agree* for uniform integer magnification:
 * mirroring a z-times block-magnified image and magnifying a mirrored source
 * both send output pixel ox to source n-1-ox/z.  So this is not a fix for odd
 * zoom; it is refusing to depend on that coincidence.  Assertion 16 pins the
 * mirror actually happening.)
 *
 * Qt::FastTransformation is mandatory rather than a performance choice: it is
 * Qt's nearest-neighbour, matching map()'s integer division and the OpenCV
 * viewer's cv::INTER_NEAREST.  Smooth scaling here would put the Qt window's
 * pixels somewhere the shared pointer mapping does not agree with. */
static QImage transformed(const QImage &src, const dyt_view_transform_t &t)
{
    /* Both branches must own their pixels: `src` wraps the engine's buffer,
     * which is only valid until the next dyt_frame_source_next(). */
    QImage out = (t.zoom > 1)
        ? src.scaled(src.width() * t.zoom, src.height() * t.zoom,
                     Qt::IgnoreAspectRatio, Qt::FastTransformation)
        : src.copy();

    Qt::Orientations ori;
    if (t.flip_h) ori |= Qt::Horizontal;
    if (t.flip_v) ori |= Qt::Vertical;
    if (ori)
        out = out.flipped(ori);        /* Qt 6; mirrored() is deprecated */

    return out;
}

/* ------------------------------------------------------- measurement keys
 *
 * The measurement bindings, in one place so the window and --selftest cannot
 * diverge — both reach them through FrameView::measure_key().  Returns 1 when
 * the key was consumed.
 *
 * These are the reference viewer's bindings: one key per tool, "n" also
 * forgetting the placed points, "a" arming the derived band or disarming, "i"
 * toggling the isotherm.  The view keys (palette, unit, range, flip, zoom,
 * fusion) are deliberately not here; they are later tasks.  The
 * runtime-parameter ladder is a sibling (FrameView::param_key), because its
 * keys are case-sensitive and this function's are not.  The letters are free
 * of the device keys: retry is lowercase "r" and the panel toggle is "d". */
static int apply_measure_key(dyt_session_t *sess, int key)
{
    dyt_snapshot_t s;

    if (!sess)
        return 0;

    /* Only the alarm and isotherm keys need the current state — the range for
     * the band, and whether it is already armed. */
    if (key == 'a' || key == 'i') {
        if (dyt_session_snapshot(sess, &s, nullptr, 0) != 0)
            return 1;                 /* consumed; there is no frame yet */
    }

    switch (key) {
    case 'p': dyt_session_set_tool(sess, DYT_TOOL_POINT); return 1;
    case 'l': dyt_session_set_tool(sess, DYT_TOOL_LINE);  return 1;
    case 'b': dyt_session_set_tool(sess, DYT_TOOL_BOX);   return 1;
    case 'n':
        /* One key per tool, and "n" also forgets the placed points so the
         * next tool starts clean. */
        dyt_session_set_tool(sess, DYT_TOOL_NONE);
        dyt_session_clear_points(sess);
        return 1;
    case 'a':
        if (!s.alarm_on) {
            float lo, hi, hyst;
            if (dyt_vm_alarm_band(&s, &lo, &hi, &hyst) == 0)
                dyt_session_set_alarm(sess, lo, hi, hyst);
        } else {
            dyt_session_alarm_disable(sess);
        }
        return 1;
    case 'i':
        dyt_session_set_isotherm(sess, !s.iso_on);
        return 1;
    }
    return 0;
}

/* --------------------------------------------------- write verification
 *
 * Does the raw slot value the device returned match what `value` encodes to?
 *
 * This is the load-bearing part of verifying a runtime write, and it has to
 * compare in the *encoded* domain.  sendOrder quantises — emissivity and
 * distance to 1/128, ambient and reflected to whole kelvin — so comparing the
 * decoded float would report a mismatch for every value that does not happen
 * to land on an exact step: 0.80 encodes to 102, which decodes to 0.796875 and
 * never reads back as 0.80.
 *
 * The read itself is dyt_capture_read_param(); this decides its verdict, and
 * being pure it is pinned by --selftest with no device. */
static int param_raw_matches(dyt_order_type_t type, float value, uint16_t raw)
{
    const int expect = (type == DYT_ORDER_EMISSIVITY ||
                        type == DYT_ORDER_DISTANCE)
                           ? (int)dyt_param_encode_ratio(value)
                           : (int)dyt_param_encode_kelvin(value);
    return (int)raw == expect;
}

/* ------------------------------------------------------------- the canvas */

/* Paints the engine's frame, and owns the pointer interaction.
 *
 * It is the only class here that knows about pixels — everything above it
 * deals in the session and the view model — and that is exactly why the mouse
 * handling lives here too: a click has to be mapped back through the same
 * transform the overlay is drawn with, and the hover crosshair needs both the
 * pointer position and the temperature plane at paint time.  Splitting that
 * across a callback would duplicate the one piece of state that must not
 * drift. */
class FrameView : public QWidget {
public:
    explicit FrameView(QWidget *parent = nullptr) : QWidget(parent)
    {
        setAutoFillBackground(true);
        /* So a move with no button held still updates the hover readout. */
        setMouseTracking(true);
    }

    /* The session the pointer events act on.  Borrowed, never freed here. */
    void set_session(dyt_session_t *s) { sess_ = s; }

    void set_frame(const dyt_snapshot_t &snap, const QImage &img,
                   const dyt_palette_t &pal, const float *temps)
    {
        snap_  = snap;
        img_   = img;
        pal_   = pal;
        temps_ = temps;   /* borrowed from the pump's scratch, refreshed here
                           * in the same step() that fills it, so no paint can
                           * see a stale plane */
        /* No resize() here: the widget is laid out by MainWindow, which sizes
         * it from sizeHint().  Resizing a layout-managed widget is a no-op on
         * the next layout pass, and doing it made it look as though the canvas
         * sized itself — which is how the clipped-canvas bug stayed hidden.
         *
         * updateGeometry() is not optional, though: the first frame is the
         * first time the sensor's size is known, and with zoom applied the
         * hint changes with it. */
        updateGeometry();
        update();
    }

    /* Ignored once a frame has been painted, so a live stall keeps showing the
     * last real frame instead of flickering back to the placeholder. */
    void set_placeholder(const QString &s) { placeholder_ = s; update(); }

    /* Apply a measurement key.  Returns 1 if the key was ours, so the window
     * can fall through to whatever else it binds.  The snapshot is refreshed
     * so the overlay switches at once rather than at the next tick; only the
     * scalars are re-read, so this is cheap. */
    int measure_key(int k)
    {
        if (!apply_measure_key(sess_, k))
            return 0;
        if (sess_ && dyt_session_snapshot(sess_, &snap_, nullptr, 0) == 0)
            update();
        return 1;
    }

    /* Apply a runtime-parameter key.  `raw` is the *unfolded* character, so
     * the reference's case-sensitive bindings survive: 'e' emissivity, 'A'
     * ambient, 'R' reflected, 'D' distance, and 'y' to send.  The ladder and
     * the arming rule are the view model's (dyt_vm_param_key); what stays here
     * is the state the confirmation overlay draws from and the callback into
     * the front end that owns the device.  Returns 1 if the key was ours.
     *
     * While something is armed the view model consumes every key except 'q',
     * so a stray palette key cannot slip past a pending confirmation. */
    int param_key(int raw)
    {
        dyt_vm_param_event_t ev;

        if (!dyt_vm_param_key(raw, armed_type_, armed_rung_, &ev))
            return 0;

        switch (ev.action) {
        case DYT_VM_PARAM_ARMED:
            armed_type_  = ev.type;
            armed_rung_  = ev.rung;
            armed_value_ = ev.value;
            update();
            return 1;

        case DYT_VM_PARAM_SEND:
            /* The front end owns the device, so it decides whether the write
             * can even be attempted.  A refusal (a bring-up or teardown in
             * flight, or no device) leaves the candidate armed, so the user
             * can confirm again rather than losing it silently. */
            if (on_param_send_ && on_param_send_(ev.type, ev.value)) {
                armed_type_  = (dyt_order_type_t)0;
                armed_rung_  = 0;
                armed_value_ = 0.f;
            }
            update();
            return 1;

        case DYT_VM_PARAM_CANCEL:
            if (on_param_cancel_)
                on_param_cancel_(ev.type);
            armed_type_  = (dyt_order_type_t)0;
            armed_rung_  = 0;
            armed_value_ = 0.f;
            update();
            return 1;

        case DYT_VM_PARAM_SWALLOW:
            return 1;

        case DYT_VM_PARAM_NONE:
        default:
            return 0;
        }
    }

    /* Show or hide the device-information overlay. */
    void toggle_info()
    {
        show_info_ = !show_info_;
        update();
    }

    /* The device identity the worker read at bring-up.  `have` is false when
     * the read did not run, which is also how a failed bring-up clears it. */
    void set_info(const dyt_device_info_t &d, bool have)
    {
        info_      = d;
        have_info_ = have ? 1 : 0;
        update();
    }

    /* Forget the device: its identity, the runtime writes this session made,
     * and any armed candidate.  Called when the stream is unwound, because a
     * re-opened device's own stored values are authoritative again and the
     * candidate referred to a device that is gone. */
    void clear_info()
    {
        info_        = dyt_device_info_t{};
        have_info_   = 0;
        armed_type_  = (dyt_order_type_t)0;
        armed_rung_  = 0;
        armed_value_ = 0.f;
        for (int i = 0; i < 5; i++) {
            override_v_[i]  = 0.f;
            override_on_[i] = 0;
        }
        update();
    }

    /* Record a completed write.  Only a successful one supersedes the stored
     * value — the panel's `*` suffix must not claim a write the device
     * rejected.  `type` is a dyt_order_type_t (1..4; index 0 is unused). */
    void set_param_result(dyt_order_type_t type, float value, int rc)
    {
        if (rc == 0 && type >= DYT_ORDER_REFLECTED && type <= DYT_ORDER_DISTANCE) {
            override_v_[type]  = value;
            override_on_[type] = 1;
        }
        update();
    }

    /* Copy the runtime writes this session made, so the teardown can read the
     * slots back and confirm them.  Taken before clear_info() wipes the table.
     * `v` and `on` are 5 entries, indexed by dyt_order_type_t (1..4). */
    void param_overrides(float *v, int *on) const
    {
        for (int i = 0; i < 5; i++) {
            v[i]  = override_v_[i];
            on[i] = override_on_[i];
        }
    }

    /* The front end owns the device handle, so the write is reached through a
     * callback rather than a pointer kept here.  It returns 1 when the write
     * was accepted for sending. */
    std::function<bool(dyt_order_type_t, float)> on_param_send_;
    std::function<void(dyt_order_type_t)>        on_param_cancel_;

    /* -- what --selftest pins.  The state the overlays draw from, without a
     * canvas grab, plus the info rows the panel would show. */
    bool             param_armed() const { return armed_type_ != 0; }
    dyt_order_type_t armed_type()  const { return armed_type_; }
    int              armed_rung()  const { return armed_rung_; }
    float            armed_value() const { return armed_value_; }
    bool             info_shown()  const { return have_info_ && show_info_; }

    /* The info panel's `i`-th row as it would be drawn, or "" out of range.
     * Built on demand so the selftest sees exactly what the panel shows,
     * override suffix and all. */
    QString info_line(int i) const
    {
        dyt_vm_info_t info;
        if (dyt_vm_info(&info_, override_v_, override_on_, &info) < 0 ||
            i < 0 || i >= info.n)
            return QString();
        return QString::fromUtf8(info.line[i]);
    }

    bool  has_frame() const { return !img_.isNull(); }
    QSize imageSize() const { return img_.size(); }

    QSize sizeHint() const override
    {
        const int w = img_.isNull() ? 256 : img_.width();
        const int h = img_.isNull() ? 192 : img_.height();
        return QSize(kPad + w + kBarGap + kBarW + kLabelW + kPad, h + 2 * kPad);
    }

protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) {
            e->ignore();
            return;
        }
        pointer(DYT_VM_MOUSE_DOWN, e->position());
        e->accept();
    }

    void mouseMoveEvent(QMouseEvent *e) override
    {
        pointer(DYT_VM_MOUSE_MOVE, e->position());
        e->accept();
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) {
            e->ignore();
            return;
        }
        pointer(DYT_VM_MOUSE_UP, e->position());
        e->accept();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(16, 16, 16));

        if (img_.isNull()) {
            p.setPen(QColor(200, 200, 200));
            p.drawText(rect(), Qt::AlignCenter, placeholder_);
            /* The parameter keys do not need a frame: once bring-up finishes
             * the device is open and a write can be armed over the
             * placeholder, so its confirmation must be visible here too. */
            draw_confirm(p);
            return;
        }

        const int x0 = kPad, y0 = kPad;
        p.drawImage(QPoint(x0, y0), img_);

        /* The colour bar.  The tick rule — which palette entry belongs to
         * which row — is the view model's, so the Qt bar and the OpenCV one
         * cannot disagree about which end is hot. */
        const int bx = x0 + img_.width() + kBarGap;
        const int bh = img_.height();
        for (int j = 0; j < bh; j++) {
            const int idx = dyt_vm_bar_index(j, bh);
            if (idx < 0)
                continue;
            p.setPen(QColor(pal_.rgb[idx * 3 + 0], pal_.rgb[idx * 3 + 1],
                            pal_.rgb[idx * 3 + 2]));
            p.drawLine(bx, y0 + j, bx + kBarW - 1, y0 + j);
        }
        p.setPen(QColor(200, 200, 200));
        p.drawRect(bx, y0, kBarW - 1, bh - 1);

        /* The bar's labels, likewise from the view model. */
        const int lx = bx + kBarW + 4;
        static const int rows[3] = { 0, 1, 2 };
        static const int ys[3]   = { 12, 0, -3 };
        for (int i = 0; i < 3; i++) {
            char lbl[32];
            if (dyt_vm_bar_label(&snap_, rows[i], lbl, sizeof lbl) != 0)
                continue;
            const int yy = (i == 1) ? y0 + bh / 2 : (i == 0 ? y0 + ys[0]
                                                            : y0 + bh + ys[2]);
            p.setPen(i == 1 ? QColor(210, 210, 210) : QColor(255, 255, 255));
            p.drawText(lx, yy, QString::fromUtf8(lbl));
        }

        /* ---- the measurement overlays --------------------------------
         *
         * Everything below is projected with the view model's inverse of the
         * mapping the pointer handler uses, and with the same (src, dst) pair,
         * so a click and the marker it places cannot disagree.  `dst` is the
         * *transformed* image size; the widget offset (kPad, kPad) is added
         * only here, at draw time. */
        const int sw = snap_.width, sh = snap_.height;
        const int dw = img_.width(), dh = img_.height();
        if (sw <= 0 || sh <= 0) {
            /* Degenerate geometry: the projections below would be meaningless,
             * but the two overlays that do not depend on the frame still are
             * not — so draw those and stop. */
            draw_info_panel(p);
            draw_confirm(p);
            return;
        }

        /* Source pixel -> widget position, clamping into the frame first so a
         * box dragged partly off the image still draws, clipped at the edge,
         * rather than vanishing because one corner projects outside. */
        auto proj = [&](int sx, int sy, QPoint &out) {
            int ox = 0, oy = 0;
            sx = std::max(0, std::min(sw - 1, sx));
            sy = std::max(0, std::min(sh - 1, sy));
            if (dyt_view_transform_project(&snap_.xform, sw, sh, dw, dh,
                                           sx, sy, &ox, &oy) != 0)
                return false;
            out = QPoint(x0 + ox, y0 + oy);
            return true;
        };

        /* The frame's own extremes, marked as the reference viewer does:
         * H red for the hottest pixel, L blue for the coldest. */
        {
            auto mark = [&](int sx, int sy, float c, const QColor &col,
                            const char *tag) {
                QPoint q;
                if (sx < 0 || sy < 0 || !proj(sx, sy, q))
                    return;
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(QColor(0, 0, 0), 2));
                p.drawEllipse(q, 5, 5);
                p.setPen(col);
                p.drawEllipse(q, 5, 5);
                char t[32], lbl[64];
                dyt_vm_temp(&snap_, c, t, sizeof t);
                std::snprintf(lbl, sizeof lbl, "%s %s", tag, t);
                p.drawText(q + QPoint(8, -6), QString::fromUtf8(lbl));
            };
            mark(snap_.stats.hot_x, snap_.stats.hot_y, snap_.stats.hi,
                 QColor(255, 60, 60), "H");
            mark(snap_.stats.cold_x, snap_.stats.cold_y, snap_.stats.lo,
                 QColor(90, 160, 255), "L");
        }

        /* The hover readout: the temperature under the pointer, with a small
         * crosshair so the pixel it names is unambiguous. */
        if (temps_ && ptr_.x >= 0 && ptr_.y >= 0 && ptr_.x < dw && ptr_.y < dh) {
            int ix = 0, iy = 0;
            if (dyt_view_transform_map(&snap_.xform, sw, sh, dw, dh,
                                       ptr_.x, ptr_.y, &ix, &iy) == 0 &&
                ix >= 0 && ix < sw && iy >= 0 && iy < sh) {
                const float t = temps_[(size_t)iy * (size_t)sw + (size_t)ix];
                char lbl[64];
                dyt_vm_hover_label(&snap_, t, ix, iy, lbl, sizeof lbl);
                p.setPen(QColor(255, 255, 255));
                p.drawText(QPoint(x0 + ptr_.x + 10, y0 + ptr_.y - 8),
                           QString::fromUtf8(lbl));
                p.setPen(QColor(0, 0, 0));
                p.drawLine(x0 + ptr_.x - 6, y0 + ptr_.y,
                           x0 + ptr_.x + 6, y0 + ptr_.y);
                p.drawLine(x0 + ptr_.x, y0 + ptr_.y - 6,
                           x0 + ptr_.x, y0 + ptr_.y + 6);
            }
        }

        /* The placed tool. */
        if (snap_.tool != DYT_TOOL_NONE) {
            const QColor mark(255, 255, 0);
            QPoint a, b;
            if (snap_.tool == DYT_TOOL_POINT && snap_.point_ok &&
                proj(snap_.p0.x, snap_.p0.y, a)) {
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(QColor(0, 0, 0), 2));
                p.drawEllipse(a, 7, 7);
                p.setPen(mark);
                p.drawEllipse(a, 7, 7);
                char t[32], lbl[64];
                dyt_vm_temp(&snap_, snap_.point_c, t, sizeof t);
                std::snprintf(lbl, sizeof lbl, "P %s", t);
                p.drawText(a + QPoint(10, -8), QString::fromUtf8(lbl));
            } else if (snap_.tool == DYT_TOOL_LINE &&
                       proj(snap_.p0.x, snap_.p0.y, a) &&
                       proj(snap_.p1.x, snap_.p1.y, b)) {
                p.setPen(QPen(QColor(0, 0, 0), 3));
                p.drawLine(a, b);
                p.setPen(mark);
                p.drawLine(a, b);
                p.setBrush(mark);
                p.drawEllipse(a, 4, 4);
                p.drawEllipse(b, 4, 4);
            } else if (snap_.tool == DYT_TOOL_BOX &&
                       proj(snap_.p0.x, snap_.p0.y, a) &&
                       proj(snap_.p1.x, snap_.p1.y, b)) {
                const QRect r(QPoint(std::min(a.x(), b.x()),
                                     std::min(a.y(), b.y())),
                              QPoint(std::max(a.x(), b.x()),
                                     std::max(a.y(), b.y())));
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(QColor(0, 0, 0), 3));
                p.drawRect(r);
                p.setPen(mark);
                p.drawRect(r);
                if (snap_.roi_ok) {
                    char lbl[128];
                    dyt_vm_roi_label(&snap_, lbl, sizeof lbl);
                    p.drawText(QPoint(r.left() + 4, r.top() - 6),
                               QString::fromUtf8(lbl));
                }
            }
        }

        /* The two panels, and the confirmation last so it sits over every
         * other overlay.  The rows and the wording are the view model's; only
         * the placement is here. */
        draw_info_panel(p);
        draw_confirm(p);
    }

private:
    /* The device panel: the module serial, the decoded user serial, the four
     * stored radiometric parameters and the slot count, top-left over the
     * image — the reference viewer's placement (draw_info_panel).  A value a
     * runtime write superseded carries a `*`, which dyt_vm_info() adds. */
    void draw_info_panel(QPainter &p)
    {
        if (!have_info_ || !show_info_)
            return;

        dyt_vm_info_t info;
        if (dyt_vm_info(&info_, override_v_, override_on_, &info) < 0)
            return;

        const int pad = 6, lh = 17;
        int wmax = 0;
        for (int i = 0; i < info.n; i++)
            wmax = std::max(wmax, p.fontMetrics().horizontalAdvance(
                                      QString::fromUtf8(info.line[i])));

        const QRect box(kPad + 6, kPad + 6, wmax + 2 * pad, lh * info.n + 2 * pad);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(16, 16, 16));
        p.drawRect(box);
        p.setPen(QColor(120, 120, 120));
        p.setBrush(Qt::NoBrush);
        p.drawRect(box.adjusted(0, 0, -1, -1));

        p.setPen(QColor(190, 225, 255));
        const int ascent = p.fontMetrics().ascent();
        for (int i = 0; i < info.n; i++)
            p.drawText(box.x() + pad, box.y() + pad + lh * i + ascent,
                       QString::fromUtf8(info.line[i]));
    }

    /* The armed-write confirmation, centred over the bottom of the image.
     * Drawn last, so it sits over every other overlay. */
    void draw_confirm(QPainter &p)
    {
        if (!armed_type_)
            return;

        const dyt_vm_ladder_t *L = dyt_vm_ladder(armed_type_);
        if (!L)
            return;

        char val[32];
        dyt_vm_param_format(armed_type_, armed_value_, val, sizeof val);
        const QString s = QStringLiteral("SET ") + QString::fromUtf8(L->name) +
                          QStringLiteral(" = ") + QString::fromUtf8(val) +
                          QStringLiteral("    y = send    n / esc = cancel");

        /* The widget may be showing the placeholder, with no image to centre
         * on, so fall back to its own size. */
        const int iw = img_.isNull() ? width()  : img_.width();
        const int ih = img_.isNull() ? height() : img_.height();
        const int tw = 12 + p.fontMetrics().horizontalAdvance(s);
        const int x  = std::max(kPad + 6, kPad + (iw - tw) / 2);
        const int y  = kPad + ih - 34;
        const QRect r(x - 4, y - 4, tw + 8, 28);

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 150));
        p.drawRect(r);
        p.setPen(QColor(0, 220, 255));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r.adjusted(0, 0, -1, -1));
        p.setPen(QColor(255, 255, 255));
        p.drawText(r, Qt::AlignCenter, s);
    }

    /* The one widget -> image mapping, so a click and the marker it places
     * cannot disagree.  floor(), not a cast: (int)(-0.5) is 0, which would
     * place a point one pixel outside the image. */
    void pointer(dyt_vm_mouse_ev_t ev, const QPointF &pos)
    {
        if (!sess_ || img_.isNull())
            return;
        const int lx = (int)std::floor(pos.x() - kPad);
        const int ly = (int)std::floor(pos.y() - kPad);
        dyt_vm_tool_mouse(sess_, &ptr_, ev, snap_.tool, &snap_.xform,
                          snap_.width, snap_.height,
                          img_.width(), img_.height(), lx, ly);
        update();
    }

    dyt_snapshot_t      snap_{};
    QImage              img_;
    dyt_palette_t       pal_{};
    QString             placeholder_ = QStringLiteral("waiting for a frame");
    dyt_session_t      *sess_        = nullptr;   /* borrowed */
    dyt_vm_pointer_t    ptr_         = DYT_VM_POINTER_INIT;
    const float        *temps_       = nullptr;   /* borrowed from the pump */

    /* The armed runtime-parameter candidate.  `type` 0 means nothing armed. */
    dyt_order_type_t    armed_type_  = (dyt_order_type_t)0;
    int                 armed_rung_  = 0;
    float               armed_value_ = 0.f;

    /* The device identity and the runtime writes made this session, so the
     * panel never shows a stored value a write has superseded.  Indexed by
     * dyt_order_type_t (1..4; index 0 unused), which is what dyt_vm_info()
     * expects.  Visible by default, as in the reference viewer. */
    dyt_device_info_t   info_{};
    int                 have_info_   = 0;
    bool                show_info_   = true;
    float               override_v_[5]  = { 0.f, 0.f, 0.f, 0.f, 0.f };
    int                 override_on_[5] = { 0, 0, 0, 0, 0 };
};

/* --------------------------------------------------------------- the strip */

/* The status strip: three left-aligned lines on one background.
 *
 * A custom widget rather than three QLabels, because it maps one-to-one onto
 * the OpenCV viewer's immediate-mode strip and avoids three stylesheets
 * fighting over a shared background.  Lines 1 and 2 are the view model's
 * strings verbatim; line 3 is the front end's (state_line above). */
class StatusStrip : public QWidget {
public:
    explicit StatusStrip(QWidget *parent = nullptr) : QWidget(parent)
    {
        setAutoFillBackground(true);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }

    void set_lines(const QString &a, const QString &b, const QString &c)
    {
        line_[0] = a;
        line_[1] = b;
        line_[2] = c;
        updateGeometry();
        update();
    }

    const QString &line(int i) const { return line_[i]; }

    /* The alarm, when armed and tripped.  Kept here rather than on the canvas
     * because the first line is the one place with room to spare: the second
     * line can be arbitrarily long, and the image area is where the
     * measurement labels go. */
    void set_alarm(bool on, dyt_alarm_state_t st)
    {
        alarm_on_ = on;
        alarm_    = st;
        update();
    }

    QSize sizeHint() const override
    {
        int w = 0;
        for (int i = 0; i < 3; i++)
            w = std::max(w, fontMetrics().horizontalAdvance(line_[i]));
        return QSize(w + 2 * kStripPad, 3 * kLineH + 2 * kStripPad);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(16, 16, 16));

        static const QColor pen[3] = { QColor(0xdc, 0xdc, 0xdc),
                                       QColor(0xbe, 0xd2, 0xff),
                                       QColor(0x9a, 0xa4, 0xb4) };
        const int ascent = fontMetrics().ascent();
        for (int i = 0; i < 3; i++) {
            p.setPen(pen[i]);
            p.drawText(kStripPad, kStripPad + i * kLineH + ascent, line_[i]);
        }

        /* The alarm is the one thing worth shouting about. */
        if (alarm_on_ && alarm_ != DYT_ALARM_NONE) {
            const QString a =
                QStringLiteral("ALARM ") +
                QString::fromUtf8(dyt_alarm_name(alarm_));
            const int   bw = 12 + fontMetrics().horizontalAdvance(a);
            const QRect r(width() - bw - kStripPad, kStripPad,
                          bw, kLineH + 4);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 180));
            p.drawRect(r);
            p.setPen(QColor(255, 255, 255));
            p.drawText(r, Qt::AlignCenter, a);
        }
    }

private:
    QString line_[3];
    bool    alarm_on_ = false;
    dyt_alarm_state_t alarm_ = DYT_ALARM_NONE;
};

/* --------------------------------------------------------------- the window */

class MainWindow : public QWidget {
public:
    explicit MainWindow(QWidget *parent = nullptr) : QWidget(parent)
    {
        view_  = new FrameView(this);
        strip_ = new StatusStrip(this);

        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);
        lay->addWidget(view_, 1);
        lay->addWidget(strip_, 0);

        setWindowTitle("dytqt — DYT thermal camera");
        /* So 'R' reaches keyPressEvent rather than being dropped. */
        setFocusPolicy(Qt::StrongFocus);
    }

    FrameView   *view()  const { return view_; }
    StatusStrip *strip() const { return strip_; }

    /* The front end owns the device lifecycle, so the window only reports the
     * two events it cannot act on itself.  `on_close_` runs before the close is
     * accepted, which is what lets run_gui stop the timers and hand the device
     * back while the event loop is still alive. */
    std::function<void()> on_close_;
    std::function<void()> on_retry_;
    /* Quit is a callback rather than close() inline so --selftest can observe
     * `q` without tearing the window down.  run_gui installs it as close(). */
    std::function<void()> on_quit_;

    /* Size the window to the canvas it has to show.  Called once before the
     * window is shown, and again whenever the canvas changes size.
     *
     * This has to be asked for explicitly.  A top-level window follows its
     * layout's sizeHint only until someone calls resize()/adjustSize() on it;
     * after that Qt treats the geometry as the user's choice and leaves it
     * alone.  Without this the window would stay sized for the *pre-frame*
     * placeholder (a 256x192 default) while the canvas grew to the sensor's
     * real, zoomed size — silently clipping the image and the colour bar's
     * bottom label.  Assertion 7 in --selftest is what pins that.
     *
     * resize(), deliberately not adjustSize(): adjustSize() clamps the result
     * to two thirds of the screen, so a 512x384 canvas on the offscreen
     * platform's 800x600 screen came out 533x400 — clipped, which is the very
     * failure this is here to prevent, and it would do the same on any display
     * smaller than 1.5x the canvas.  An image window that is larger than the
     * screen is the lesser evil; a silently cropped one is not.
     *
     * Keyed off the canvas hint so it fires once when the first frame arrives
     * and again only if the zoom changes — not on every frame, which would
     * fight a user who resized the window by hand. */
    void fit_to_view()
    {
        const QSize want = view_->sizeHint();
        if (want == fitted_)
            return;
        fitted_ = want;
        resize(sizeHint());
    }

    /* A painted frame: all three lines. */
    void set_frame_status(const dyt_snapshot_t &snap, DevState st, double fps,
                          dyt_mode_t mode, const char *msg = nullptr)
    {
        char a[256], b[256];
        dyt_vm_status_line(&snap, mode, a, sizeof a);
        dyt_vm_readout_line(&snap, msg, b, sizeof b);
        strip_->set_lines(QString::fromUtf8(a), QString::fromUtf8(b),
                          state_line(st, snap, fps));
        strip_->set_alarm(snap.alarm_on != 0, snap.alarm);
        fit_to_view();
    }

    /* No frame to paint — the placeholder is up, so lines 1 and 2 keep
     * whatever they last said and only the state line moves. */
    void set_state_line(DevState st, const dyt_snapshot_t &snap, double fps)
    {
        strip_->set_lines(strip_->line(0), strip_->line(1),
                          state_line(st, snap, fps));
    }

protected:
    void closeEvent(QCloseEvent *e) override
    {
        if (on_close_)
            on_close_();
        e->accept();
    }

    void keyPressEvent(QKeyEvent *e) override
    {
        /* One unfolded character, because the bindings are not all the same
         * case.  The runtime-parameter keys are case-sensitive — 'e' arms
         * emissivity but 'A'/'R'/'D' arm ambient/reflected/distance, and 'y'
         * sends — while the measurement keys are lowercase.  Qt reports a
         * letter as its uppercase code, so e->key() alone cannot tell 'r' from
         * 'R'; e->text() carries the real case.  A synthesized event (as
         * --selftest sends) has no text, so fall back to the folded key. */
        int raw;
        if (e->key() == Qt::Key_Escape)
            raw = 27;      /* the view model's cancel code; Qt::Key_Escape is not 27 */
        else if (!e->text().isEmpty() &&
                 e->text().at(0).unicode() > 0 && e->text().at(0).unicode() < 128)
            raw = e->text().at(0).unicode();
        else {
            raw = e->key();
            if (raw >= Qt::Key_A && raw <= Qt::Key_Z)
                raw += 'a' - 'A';
        }

        /* The runtime-parameter ladder first.  While a candidate is armed the
         * view model consumes every key except 'q', so a stray palette or tool
         * key cannot slip past a pending confirmation. */
        if (view_ && view_->param_key(raw))
            return;

        /* Quit.  'q' is deliberately never swallowed, armed or not. */
        if (raw == 'q') {
            if (on_quit_)
                on_quit_();
            else
                close();
            return;
        }

        /* Retry: lowercase only, because uppercase 'R' arms reflected. */
        if (raw == 'r' && on_retry_) {
            on_retry_();
            return;
        }

        /* The device panel. */
        if (raw == 'd' && view_) {
            view_->toggle_info();
            return;
        }

        /* Everything else is the measurement bindings, which are lowercase —
         * so fold the unfolded character back down before consulting them.
         * The return value decides, rather than a list of letters here, so
         * --selftest and the window cannot disagree about what is bound. */
        int k = raw;
        if (k >= 'A' && k <= 'Z')
            k += 'a' - 'A';
        if (view_ && view_->measure_key(k))
            return;
        QWidget::keyPressEvent(e);
    }

private:
    FrameView   *view_  = nullptr;
    StatusStrip *strip_ = nullptr;
    QSize        fitted_{};
};

/* ------------------------------------------------------------ the pump
 *
 * One timer at the requested rate; each tick pulls a frame from the source,
 * hands the pixels to the canvas and the text to the status strip.
 *
 * Threading.  This runs on the GUI thread and only ever reads the session
 * through its own lock (dyt_session_snapshot, via dyt_vm_grab), so no widget
 * is ever touched from the capture callback thread.  That is why the poll
 * model in session.h needs no queued signal here: a live frame is installed by
 * the adapter (session_capture.c) on libuvc's callback thread, and this side
 * picks up the latest one.  src/frame_ready.c exists to *wake* a poller on
 * demand instead of polling on a timer — an optimisation, not a correctness
 * requirement, and not wired in yet.
 */
struct pump {
    dyt_frame_source_t *fs   = nullptr;
    dyt_session_t      *sess = nullptr;
    MainWindow         *win  = nullptr;
    dyt_palette_t       pal{};
    dyt_vm_scratch_t    scr  = DYT_VM_SCRATCH_INIT;
    dyt_mode_t          mode = DYT_MODE_1000;

    /* Live only; the fixture path leaves them at their defaults, and
     * device_state() ignores them entirely when `live` is false. */
    bool live         = false;
    bool bringup_done = false;
    int  bringup_rc   = 0;

    /* Set by step() the first tick it sees NO SIGNAL, and cleared by run_gui
     * once it has started the teardown that leads to a retry.  A flag rather
     * than a callback so the pump stays a plain function of the session. */
    bool reconnect    = false;

    /* A transient notice the strip shows — the outcome of a runtime-parameter
     * write, or why one was refused.  Owned here because it is the front end's,
     * not the session's, and because dyt_vm_readout_line() already takes it as
     * an argument.  The TTL is in ticks, so it only advances while the pump
     * steps (the reference viewer's poll-driven rule). */
    std::string msg;
    int         msg_ttl = 0;

    long long ticks  = 0;   /* timer callbacks */
    long long frames = 0;   /* frames actually painted */
    int       fails  = 0;
    double    worst_ms = 0.0;  /* slowest single step, for the fps headroom claim */
    FpsMeter  fps;
    StallWatch stall;

    double now_s() const
    {
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - t_start_).count();
    }

    bool step()
    {
        const uint8_t *rgb = nullptr;
        int            w = 0, h = 0;

        ticks++;

        /* Expire the transient notice before anything can re-set it. */
        if (msg_ttl > 0 && --msg_ttl == 0)
            msg.clear();

        if (!fs) {
            /* Bring-up failed, so there is no source to pull from — but the
             * window must still say so rather than sit blank.  This is the
             * NoDevice path, and the reason a failed bring-up does not exit.
             * No source means no liveness signal, so `stalled` is false. */
            dyt_snapshot_t snap{};
            const bool have = dyt_session_snapshot(sess, &snap, nullptr, 0) == 0;
            const DevState ds = device_state(live, bringup_done, bringup_rc, have,
                                             have && snap.ready != 0, false);
            win->view()->set_placeholder(QString::fromUtf8(state_placeholder(ds)));
            win->set_state_line(ds, snap, fps.fps());
            return true;
        }

        const dyt_fs_status_t st = dyt_frame_source_next(fs, &rgb, &w, &h);
        if (st == DYT_FS_END)
            return false;                 /* the fixture's budget is spent */
        if (st == DYT_FS_ERROR)
            fails++;
        if (st != DYT_FS_FRAME || !rgb)
            return true;                  /* WAIT is normal, and so is ERROR */

        dyt_snapshot_t snap;
        if (!dyt_vm_grab(sess, &snap, &scr)) {
            fails++;
            return true;
        }

        /* Both the rate and the stall decision come from `seq` — the frames the
         * engine processed — never from the frames painted.  A stalled source
         * still returns FRAME (it re-renders the last one), so a paint-driven
         * meter would report a healthy 25 fps over a frozen picture, which is
         * the exact lie this is here to stop telling. */
        const double t     = now_s();
        const bool   ready = snap.ready != 0;
        const bool   stalled = stall.observe(snap.seq, t, ready);
        fps.update(snap.seq, t);

        const DevState ds = device_state(live, bringup_done, bringup_rc,
                                        true, ready, stalled);

        if (ds == DevState::Stalled)
            reconnect = true;         /* run_gui tears down and retries */

        if (!snap.ready) {
            /* The start-up filler decodes to a legitimate-looking 238.85 C
             * (display.h), so painting it would show a real-looking scene
             * that is not one and would collapse the colour scale onto it.
             * Hold the placeholder instead.  On the live path this is
             * defensive — next_live() already filters it — and on the fixture
             * path it cannot happen at all; it is the hook --selftest drives. */
            win->view()->set_placeholder(QString::fromUtf8(state_placeholder(ds)));
            win->set_state_line(ds, snap, fps.fps());
            return true;                  /* deliberately not a painted frame */
        }

        /* The isotherm dims the pixels outside the alarm band.  It is applied
         * to the *source-resolution* buffer and before the transform, exactly
         * as the reference viewer does, so the dimming follows the data rather
         * than the zoom.  A private copy, because the engine's buffer is not
         * ours to scribble on; and a std::vector rather than QImage::bits(),
         * because Qt may pad bytesPerLine while dyt_vm_apply_isotherm assumes
         * tightly packed w*3 rows.  The buffer is RGB, not BGR, but the pass
         * halves every channel, so the order does not matter. */
        std::vector<uint8_t> iso_buf;
        const uint8_t       *pix = rgb;
        if (snap.iso_on && scr.temps && scr.cap >= w * h) {
            iso_buf.assign(rgb, rgb + (size_t)w * (size_t)h * 3);
            dyt_vm_apply_isotherm(iso_buf.data(), w, h, scr.temps, scr.cap,
                                  snap.iso_lo, snap.iso_hi);
            pix = iso_buf.data();
        }

        /* The engine's buffer is tightly packed RGB, so QImage wraps it with
         * no conversion.  transformed() owns what it returns, because the
         * source buffer dies on the next next(). */
        const QImage wrapped(pix, w, h, w * 3, QImage::Format_RGB888);
        win->view()->set_frame(snap, transformed(wrapped, snap.xform), pal,
                               scr.temps);
        win->set_frame_status(snap, ds, fps.fps(), mode,
                              msg.empty() ? nullptr : msg.c_str());

        frames++;                        /* painted frames, for --frames/--png */
        return true;
    }

private:
    const std::chrono::steady_clock::time_point t_start_ =
        std::chrono::steady_clock::now();
};

/* ------------------------------------------------------------ retry policy */

/* Seconds to wait before retry N+1, where N is the number of retries already
 * made.  Negative means "stop trying".
 *
 * Pure, so --selftest pins it with no device — and the shape that matters
 * (double to a cap, then give up) is exactly the part a camera would not make
 * more testable.  0.5 s first, because a replug is often quick; 30 s at the
 * top, so a device that is genuinely gone does not spin forever. */
static double retry_delay_s(int attempt)
{
    static const double kBase = 0.5, kCap = 30.0;
    if (attempt < 0 || attempt >= 8) return -1.0;    /* 8 tries, then stop */
    const double d = kBase * (double)(1 << attempt); /* .5,1,2,4,8,16,30,30 */
    return d > kCap ? kCap : d;
}

/* ------------------------------------------------------------- selftest
 *
 * The offline check.  It runs the same path the window does, under
 * QT_QPA_PLATFORM=offscreen, and asserts on the result rather than leaving a
 * human to look at a window: the fixture really converted (not the ~238.85 C
 * start-up filler), the geometry is the sensor's, all three status lines are
 * populated, and the canvas actually painted more than one colour.
 */
static int selftest(const opts &o)
{
    int fails = 0;
    int want  = o.frames > 0 ? o.frames : 25;

    std::fprintf(stderr, "dytqt --selftest: %s (%dpx), %d frames at %d fps\n",
                 o.fixture.c_str(), o.width, want, o.fps);

    dyt_session_t *sess = setup_session(o);
    if (!sess)
        return 1;

    dyt_frame_source_t *fs = dyt_frame_source_open_fixture(
        sess, o.fixture.c_str(), o.width, DYT_MODE_1000,
        DYT_PLANE_BOTTOM_HALF, o.cap.t_amb, o.cap.sensor_mode,
        o.cap.fix_mode, 0);
    if (!fs) {
        dyt_session_free(sess);
        return 1;
    }

    MainWindow win;
    win.view()->set_session(sess);
    pump       pm;
    pm.fs   = fs;
    pm.sess = sess;
    pm.win  = &win;
    pm.mode = DYT_MODE_1000;
    if (dyt_session_get_palette(sess, o.palette - 1, &pm.pal) != 0) {
        dyt_frame_source_close(fs);
        dyt_session_free(sess);
        return 1;
    }

    win.fit_to_view();
    win.show();
    QApplication::processEvents();

    for (int i = 0; i < want; i++) {
        const auto t0 = std::chrono::steady_clock::now();
        if (!pm.step())
            break;
        const auto t1 = std::chrono::steady_clock::now();
        const double ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (ms > pm.worst_ms)
            pm.worst_ms = ms;
        QApplication::processEvents();
    }

    /* 1. The frames arrived. */
    std::printf("  %-4s painted the requested %d frame(s) (got %lld)\n",
                pm.frames == want ? "ok" : "FAIL", want, pm.frames);
    if (pm.frames != want)
        fails++;

    /* 2. The frame path fits the frame budget.  At 25 fps the timer gives each
     * tick 40 ms; if a step cannot finish inside that, the QTimer coalesces
     * ticks and the display silently lags the sensor.  This is the number the
     * "25 fps is reachable" claim rests on. */
    {
        const double budget = 1000.0 / (double)o.fps;
        const bool   ok     = pm.worst_ms < budget;
        std::printf("  %-4s the frame path fits the %.0f ms budget at %d fps "
                    "(worst step %.1f ms)\n", ok ? "ok" : "FAIL", budget, o.fps,
                    pm.worst_ms);
        if (!ok)
            fails++;
    }

    /* 3. The geometry is the sensor's. */
    dyt_snapshot_t snap{};
    int sw = 0, sh = 0;
    if (dyt_session_snapshot(sess, &snap, nullptr, 0) == 0) {
        sw = snap.width;
        sh = snap.height;
    }
    std::printf("  %-4s the frame is 256x192 (got %dx%d)\n",
                (sw == 256 && sh == 192) ? "ok" : "FAIL", sw, sh);
    if (sw != 256 || sh != 192)
        fails++;

    /* 4. The fixture really converted.  Mode 1000's start-up filler is a flat
     * 0x8000 -> ~238.85 C; this fixture is ~31.4..32.4 C.  Anything near the
     * filler means the conversion was skipped. */
    {
        std::vector<float> temps((size_t)sw * sh);
        const int n = dyt_frame_source_temps(fs, temps.data(), (int)temps.size());
        float lo = 0.f, hi = 0.f;
        if (n > 0) {
            lo = hi = temps[0];
            for (int i = 1; i < n; i++) {
                if (temps[i] < lo) lo = temps[i];
                if (temps[i] > hi) hi = temps[i];
            }
        }
        const bool real = n > 0 && lo > 25.f && hi < 40.f;
        std::printf("  %-4s the frame converted to real temperatures "
                    "(min %.2f C, max %.2f C)\n", real ? "ok" : "FAIL",
                    (double)lo, (double)hi);
        if (!real)
            fails++;
    }

    /* 5. The first status line is populated, and is the view model's. */
    {
        char status[256];
        dyt_vm_status_line(&snap, DYT_MODE_1000, status, sizeof status);
        const bool ok = std::strstr(status, "mode 1000") != nullptr &&
                        std::strstr(status, "frames")    != nullptr;
        std::printf("  %-4s the status line is populated (\"%s\")\n",
                    ok ? "ok" : "FAIL", status);
        if (!ok)
            fails++;
    }

    /* 6. The canvas painted.  A uniform grab means the paint path did not run
     * — which a frame count alone would not catch. */
    {
        const QPixmap pm_grab = win.grab();
        QImage        img     = pm_grab.toImage();
        int           distinct = 0;
        unsigned      seen[64];

        if (!img.isNull()) {
            for (int y = 0; y < img.height() && distinct < 64; y += 7) {
                for (int x = 0; x < img.width() && distinct < 64; x += 7) {
                    const unsigned c = img.pixel(x, y) & 0x00ffffffu;
                    int k;
                    for (k = 0; k < distinct; k++)
                        if (seen[k] == c)
                            break;
                    if (k == distinct)
                        seen[distinct++] = c;
                }
            }
        }
        const bool ok = !img.isNull() && img.width() > 256 && distinct > 8;
        std::printf("  %-4s the canvas painted (%dx%d, %d distinct colours)\n",
                    ok ? "ok" : "FAIL", img.width(), img.height(), distinct);
        if (!ok)
            fails++;

        if (!o.png.empty() && !img.isNull()) {
            if (img.save(QString::fromUtf8(o.png.c_str()))) {
                std::printf("  ok   canvas written to %s\n", o.png.c_str());
            } else {
                std::printf("  FAIL could not write %s\n", o.png.c_str());
                fails++;
            }
        }
    }

    /* 7. The canvas is not clipped.  The layout, not the view, decides the
     * view's size, so a window sized from the *view's* hint leaves the status
     * strip to steal rows from the frame — which clips the image and the
     * colour bar's bottom label while every check above still passes.  This
     * assertion is what caught that; keep it. */
    {
        const QSize hint = win.view()->sizeHint();
        const bool  ok   = win.view()->height() >= hint.height() &&
                           win.view()->width()  >= hint.width();
        std::printf("  %-4s the canvas is not clipped (%dx%d, wants %dx%d)\n",
                    ok ? "ok" : "FAIL",
                    win.view()->width(), win.view()->height(),
                    hint.width(), hint.height());
        if (!ok)
            fails++;
    }

    /* 8. The strip carries three populated lines, and the first two are the
     * view model's, unaltered. */
    {
        const QString l1 = win.strip()->line(0);
        const QString l2 = win.strip()->line(1);
        const QString l3 = win.strip()->line(2);
        const bool ok = l1.contains("mode 1000") &&
                        l2.startsWith("tool: none") &&
                        !l3.isEmpty();
        std::printf("  %-4s the strip has three populated lines\n",
                    ok ? "ok" : "FAIL");
        if (!ok)
            fails++;
        std::printf("        line 1: %s\n", l1.toUtf8().constData());
        std::printf("        line 2: %s\n", l2.toUtf8().constData());
        std::printf("        line 3: %s\n", l3.toUtf8().constData());
    }

    /* 9. Line 3 names the source.  It is checked as a *label* and not as an
     * fps value because this loop paces nothing — assertion 12 pins the
     * arithmetic instead. */
    {
        const QString l3 = win.strip()->line(2);
        const bool ok = l3.contains("FIXTURE");
        std::printf("  %-4s line 3 names the source (\"%s\")\n",
                    ok ? "ok" : "FAIL", l3.toUtf8().constData());
        if (!ok)
            fails++;
    }

    /* 10. The range mode reaches the snapshot, and comes back.  Toggling twice
     * leaves the session exactly as it was found, so this cannot perturb the
     * strip the assertions above read. */
    {
        dyt_snapshot_t s2{}, s3{};
        dyt_session_toggle_range(sess);
        const bool got_fixed = dyt_session_snapshot(sess, &s2, nullptr, 0) == 0 &&
                               s2.range_mode == DYT_RANGE_FIXED;
        dyt_session_toggle_range(sess);
        const bool back_auto = dyt_session_snapshot(sess, &s3, nullptr, 0) == 0 &&
                               s3.range_mode == DYT_RANGE_AUTO;
        const bool ok = snap.range_mode == DYT_RANGE_AUTO && got_fixed && back_auto;
        std::printf("  %-4s the range mode reaches the snapshot (%s -> %s -> %s)\n",
                    ok ? "ok" : "FAIL",
                    dyt_range_mode_name(snap.range_mode),
                    dyt_range_mode_name(s2.range_mode),
                    dyt_range_mode_name(s3.range_mode));
        if (!ok)
            fails++;
    }

    /* 11. The engine names the modes, so no front end has to re-spell them. */
    {
        const char *a = dyt_range_mode_name(DYT_RANGE_AUTO);
        const char *f = dyt_range_mode_name(DYT_RANGE_FIXED);
        const bool ok = std::strcmp(a, "auto") == 0 &&
                        std::strcmp(f, "fixed") == 0;
        std::printf("  %-4s the engine names the range modes (\"%s\", \"%s\")\n",
                    ok ? "ok" : "FAIL", a, f);
        if (!ok)
            fails++;
    }

    /* 12. The fps meter is exact: the first sample is only a baseline, so the
     * rate comes from the second alone — 25 frames in 1.0 s. */
    {
        FpsMeter fm;
        fm.update(0, 0.0);
        fm.update(25, 1.0);
        const bool ok = fm.fps() == 25.0;
        std::printf("  %-4s the fps meter is exact (%.1f fps)\n",
                    ok ? "ok" : "FAIL", fm.fps());
        if (!ok)
            fails++;
    }

    /* 13. A filler frame is not a live frame.  This is the real filler rule
     * (display.h), reached with no device and no timing: a throwaway session
     * fed a flat plane of the start-up value reports not-ready, and the state
     * machine calls that WARMING UP rather than LIVE. */
    {
        dyt_session_t *fs_sess = dyt_session_create();
        bool ok = false;
        if (fs_sess) {
            std::vector<float> filler((size_t)256 * 192, DYT_FILLER_C);
            dyt_frame_info_t fi{ filler.data(), 256, 192 };
            dyt_snapshot_t fs_snap{};
            ok = dyt_session_process(fs_sess, &fi) == 0 &&
                 dyt_session_snapshot(fs_sess, &fs_snap, nullptr, 0) == 0 &&
                 fs_snap.ready == 0 &&
                 device_state(true, true, 0, true, fs_snap.ready != 0, false)
                     == DevState::WarmingUp;
            std::printf("  %-4s a filler frame is not a live frame "
                        "(ready %d, %s)\n", ok ? "ok" : "FAIL",
                        fs_snap.ready, state_label(device_state(true, true, 0,
                        true, fs_snap.ready != 0, false)));
            dyt_session_free(fs_sess);
        } else {
            std::printf("  FAIL out of memory\n");
        }
        if (!ok)
            fails++;
    }

    /* 14. A failed bring-up is a state, not a crash.  Pure function, so this
     * is the only way the state is reachable without a camera. */
    {
        const DevState ds = device_state(true, true, -1, false, false, false);
        const bool ok = ds == DevState::NoDevice;
        std::printf("  %-4s a failed bring-up is a state, not a crash (%s)\n",
                    ok ? "ok" : "FAIL", state_label(ds));
        if (!ok)
            fails++;
    }

    /* 15. Zoom is applied to the frame, and the layout follows it.  z > 1 is
     * part of the assertion on purpose: at zoom 1 the relation below would
     * hold for an untransformed frame too, and would prove nothing. */
    {
        const int   z    = snap.xform.zoom;
        const QSize is   = win.view()->imageSize();
        const QSize hint = win.view()->sizeHint();
        const bool  ok   = z > 1 && is == QSize(256 * z, 192 * z) &&
                           hint.height() >= is.height();
        std::printf("  %-4s zoom %d is applied to the frame (%dx%d, hint %dx%d)\n",
                    ok ? "ok" : "FAIL", z, is.width(), is.height(),
                    hint.width(), hint.height());
        if (!ok)
            fails++;
    }

    /* 16. The mirror is applied, in the sense dyt_view_transform_map() inverts.
     * Driven directly, because the window has no way to reach it yet: no key
     * or option sets the flip until the interaction task, and the fixture
     * opens unmirrored.  A 3x1 source with a distinct pixel at each end, at
     * zoom 2 with flip_h, must come out 6 wide with the ends swapped. */
    {
        QImage src(3, 1, QImage::Format_RGB888);
        src.setPixelColor(0, 0, QColor(255, 0, 0));
        src.setPixelColor(1, 0, QColor(0, 255, 0));
        src.setPixelColor(2, 0, QColor(0, 0, 255));

        dyt_view_transform_t t{};
        t.zoom   = 2;
        t.flip_h = 1;
        const QImage out = transformed(src, t);

        const bool ok = out.width() == 6 && out.height() == 2 &&
                        out.pixelColor(0, 0) == QColor(0, 0, 255) &&
                        out.pixelColor(5, 0) == QColor(255, 0, 0);
        std::printf("  %-4s the mirror is applied (zoom %d, flip_h, "
                    "%dx%d, ends %s/%s)\n", ok ? "ok" : "FAIL", t.zoom,
                    out.width(), out.height(),
                    out.pixelColor(0, 0).name().toUtf8().constData(),
                    out.pixelColor(5, 0).name().toUtf8().constData());
        if (!ok)
            fails++;
    }

    /* 17. --live contradicts the fixture-only options, and the contradiction is
     * refused rather than silently resolved one way.  Checked through the pure
     * rule rather than parse_args, so the usage text it prints does not land in
     * the middle of `make check`. */
    {
        opts a;  a.live = true; a.fixture_set = true;
        opts b;  b.live = true; b.selftest    = true;
        opts c;  c.live = true;
        opts d;                              /* the fixture path, as shipped */
        const bool ok = opts_conflict(a) != nullptr &&
                        opts_conflict(b) != nullptr &&
                        opts_conflict(c) == nullptr &&
                        opts_conflict(d) == nullptr;
        std::printf("  %-4s --live contradicts --fixture/--selftest, and is "
                    "refused\n", ok ? "ok" : "FAIL");
        if (!ok)
            fails++;
    }

    /* 18. The capture options reach the struct the capture layer reads.  No
     * device is touched: this only proves the flags are wired to the right
     * fields, which is exactly the kind of thing that silently does nothing. */
    {
        opts t;
        const char *av[] = { "dytqt", "--live", "--vid", "0x1234",
                             "--pid", "0x5678", "--format-index", "3",
                             "--height", "256", "--t-amb", "21.5",
                             "--ad-output" };
        const bool ok = parse_args(13, const_cast<char **>(av), t) &&
                        t.live &&
                        t.cap.vid == 0x1234 && t.cap.pid == 0x5678 &&
                        t.cap.format_index == 3 && t.cap.height == 256 &&
                        t.cap.t_amb == 21.5f &&
                        t.cap.output == DYT_OUTPUT_AD;
        std::printf("  %-4s the capture options reach dyt_capture_opts "
                    "(%04x:%04x, fmt %d, h %d, t_amb %.1f, %s)\n",
                    ok ? "ok" : "FAIL", (unsigned)t.cap.vid,
                    (unsigned)t.cap.pid, t.cap.format_index, t.cap.height,
                    (double)t.cap.t_amb,
                    t.cap.output == DYT_OUTPUT_AD ? "AD" : "dual-half");
        if (!ok)
            fails++;
    }

    /* 19. The stall watchdog fires on a frozen counter, and only then.  The
     * negatives matter as much as the positive: motion clears it, and a warm-up
     * (ready == 0) is never a stall however long it lasts. */
    {
        StallWatch sw;
        const bool a = !sw.observe(0, 0.0, true);   /* baseline */
        const bool b = !sw.observe(0, 0.5, true);   /* frozen, under kStallS */
        const bool c =  sw.observe(0, 2.0, true);   /* frozen, over kStallS */
        const bool d = !sw.observe(1, 2.1, true);   /* moved: clears */
        const bool e = !sw.observe(1, 4.0, false);  /* frozen but warming up */
        const bool ok = a && b && c && d && e;
        std::printf("  %-4s the stall watchdog fires on a freeze, not on "
                    "motion or warm-up\n", ok ? "ok" : "FAIL");
        if (!ok)
            fails++;
    }

    /* 20. The state machine reaches NO SIGNAL, and a stall cannot mask the
     * states above it — NO DEVICE and WARMING UP still win. */
    {
        const bool a = device_state(true, true, 0, true, true, true)
                           == DevState::Stalled;
        const bool b = device_state(true, true, 0, true, false, true)
                           == DevState::WarmingUp;
        const bool c = device_state(true, true, -1, false, false, true)
                           == DevState::NoDevice;
        const bool d = device_state(true, true, 0, true, true, false)
                           == DevState::Live;
        const bool e = device_state(false, true, 0, true, true, true)
                           == DevState::Fixture;
        const bool f = std::strcmp(state_label(DevState::Stalled),
                                   "NO SIGNAL") == 0;
        const bool ok = a && b && c && d && e && f;
        std::printf("  %-4s a frozen stream is NO SIGNAL, and cannot mask the "
                    "other states (%s)\n", ok ? "ok" : "FAIL",
                    state_label(device_state(true, true, 0, true, true, true)));
        if (!ok)
            fails++;
    }

    /* 21. The fps meter reports 0.0 over a frozen counter, where a paint-driven
     * meter reported the timer rate.  Assertion 12 pins the arithmetic; this
     * pins the stall, which is the whole point of driving it from `seq`. */
    {
        FpsMeter fm;
        fm.update(100, 0.0);
        fm.update(125, 1.0);
        const double moving = fm.fps();
        fm.update(125, 1.04);
        fm.update(125, 3.3);            /* frozen past the rebase window */
        const double frozen = fm.fps();
        const bool ok = moving == 25.0 && frozen == 0.0;
        std::printf("  %-4s the fps meter reports 0.0 on a frozen counter "
                    "(%.1f -> %.1f)\n", ok ? "ok" : "FAIL", moving, frozen);
        if (!ok)
            fails++;
    }

    /* 22. The retry backoff doubles to a cap and then gives up.  Pure, so the
     * policy is pinned here rather than by watching a camera fail eight times. */
    {
        const bool ok = retry_delay_s(0) == 0.5 &&
                        retry_delay_s(1) == 1.0 &&
                        retry_delay_s(2) == 2.0 &&
                        retry_delay_s(6) == 30.0 &&
                        retry_delay_s(7) == 30.0 &&
                        retry_delay_s(8) < 0.0 &&
                        retry_delay_s(-1) < 0.0;
        std::printf("  %-4s the retry backoff doubles to a cap, then gives up "
                    "(%.1f, %.1f, ..., %.1f, %s)\n", ok ? "ok" : "FAIL",
                    retry_delay_s(0), retry_delay_s(1), retry_delay_s(6),
                    retry_delay_s(8) < 0.0 ? "stop" : "keep");
        if (!ok)
            fails++;
    }

    /* The measurement UI.  These drive the real widgets — synthesized Qt
     * events into the window and the canvas — so what is pinned is the
     * routing, not the arithmetic underneath it.  A fixture run populates
     * everything they need (a frame, its stats, and a settable tool). */
    auto send_key = [&](Qt::Key k) {
        QKeyEvent e(QEvent::KeyPress, k, Qt::NoModifier);
        QApplication::sendEvent(&win, &e);
    };

    /* 23. The tool keys reach the session.  This is also what arms the mouse
     * test below: the pointer handler reads the tool from the view's own
     * snapshot, so a tool must be selected through the key path before a drag
     * places anything — which is exactly the real order of use. */
    {
        dyt_snapshot_t s{};

        send_key(Qt::Key_L);
        dyt_session_snapshot(sess, &s, nullptr, 0);
        const bool line = s.tool == DYT_TOOL_LINE;

        send_key(Qt::Key_P);
        dyt_session_snapshot(sess, &s, nullptr, 0);
        const bool point = s.tool == DYT_TOOL_POINT;

        send_key(Qt::Key_B);
        dyt_session_snapshot(sess, &s, nullptr, 0);
        const bool box = s.tool == DYT_TOOL_BOX;

        /* "n" clears as well as deselecting, so the next tool starts clean. */
        send_key(Qt::Key_N);
        dyt_session_snapshot(sess, &s, nullptr, 0);
        const bool none = s.tool == DYT_TOOL_NONE &&
                          s.p0.x < 0 && s.p1.x < 0;

        const bool ok = line && point && box && none;
        std::printf("  %-4s the tool keys reach the session (line/point/box/"
                    "clear %s)\n", ok ? "ok" : "FAIL",
                    ok ? "all route" : "MISSED");
        if (!ok)
            fails++;
    }

    /* 24. The mouse reaches the session through the real widget: a press
     * places both points, a drag moves point 1, a release ends it.  The
     * expected source pixel comes from the view model's own map(), so this
     * pins the wiring — that the widget's coordinates reach tool_mouse at all,
     * with the right dst size — rather than the transform (assertion 16). */
    auto send_mouse = [&](QEvent::Type t, Qt::MouseButton b,
                          Qt::MouseButtons bs, int lx, int ly) {
        QMouseEvent e(t, QPointF(kPad + lx, kPad + ly),
                      QPointF(kPad + lx, kPad + ly), b, bs, Qt::NoModifier);
        QApplication::sendEvent(win.view(), &e);
    };
    {
        send_key(Qt::Key_B);            /* the view's snapshot must carry it */

        const QSize is  = win.view()->imageSize();
        const int   lx0 = 40, ly0 = 30;
        const int   lx1 = 90, ly1 = 70;

        send_mouse(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton,
                   lx0, ly0);
        dyt_snapshot_t s1{};
        dyt_session_snapshot(sess, &s1, nullptr, 0);
        int ex0 = -1, ey0 = -1;
        const bool m0 = dyt_view_transform_map(&s1.xform, s1.width, s1.height,
                                               is.width(), is.height(),
                                               lx0, ly0, &ex0, &ey0) == 0;

        send_mouse(QEvent::MouseMove, Qt::NoButton, Qt::LeftButton, lx1, ly1);
        dyt_snapshot_t s2{};
        dyt_session_snapshot(sess, &s2, nullptr, 0);
        int ex1 = -1, ey1 = -1;
        const bool m1 = dyt_view_transform_map(&s2.xform, s2.width, s2.height,
                                               is.width(), is.height(),
                                               lx1, ly1, &ex1, &ey1) == 0;

        send_mouse(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton,
                   lx1, ly1);

        const bool ok = m0 && m1 &&
                        s1.p0.x == ex0 && s1.p0.y == ey0 &&
                        s1.p1.x == ex0 && s1.p1.y == ey0 &&
                        s2.p0.x == ex0 && s2.p0.y == ey0 &&
                        s2.p1.x == ex1 && s2.p1.y == ey1;
        std::printf("  %-4s the mouse places and drags through the widget "
                    "(%d,%d -> %d,%d)\n", ok ? "ok" : "FAIL",
                    s1.p0.x, s1.p0.y, s2.p1.x, s2.p1.y);
        if (!ok)
            fails++;
    }

    /* 25. The alarm key arms the band the view model derives from the current
     * range, and a second press disarms. */
    {
        dyt_snapshot_t s{};

        send_key(Qt::Key_A);
        dyt_session_snapshot(sess, &s, nullptr, 0);
        const bool armed = s.alarm_on != 0 && s.alarm_lo < s.alarm_hi;

        float lo = 0.f, hi = 0.f, hyst = 0.f;
        dyt_vm_alarm_band(&s, &lo, &hi, &hyst);
        const bool matches = std::fabs(s.alarm_lo - lo) < 1e-4f &&
                             std::fabs(s.alarm_hi - hi) < 1e-4f;
        const float armed_lo = s.alarm_lo, armed_hi = s.alarm_hi;

        send_key(Qt::Key_A);
        dyt_session_snapshot(sess, &s, nullptr, 0);
        const bool disarmed = s.alarm_on == 0;

        const bool ok = armed && matches && disarmed;
        std::printf("  %-4s the alarm key arms the derived band, then disarms "
                    "(%.1f..%.1f)\n", ok ? "ok" : "FAIL",
                    (double)armed_lo, (double)armed_hi);
        if (!ok)
            fails++;
    }

    /* 26. The isotherm key toggles the overlay. */
    {
        dyt_snapshot_t s{};

        send_key(Qt::Key_I);
        dyt_session_snapshot(sess, &s, nullptr, 0);
        const bool on = s.iso_on != 0;

        send_key(Qt::Key_I);
        dyt_session_snapshot(sess, &s, nullptr, 0);
        const bool off = s.iso_on == 0;

        const bool ok = on && off;
        std::printf("  %-4s the isotherm key toggles the overlay\n",
                    ok ? "ok" : "FAIL");
        if (!ok)
            fails++;
    }

    /* 27. The strip the window shows carries the measurement, so the overlay
     * and the text cannot disagree.  A box is placed, then one tick runs so
     * the strip is rebuilt from a fresh snapshot the way the timer does it. */
    {
        send_key(Qt::Key_B);
        send_mouse(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton,
                   20, 20);
        send_mouse(QEvent::MouseMove, Qt::NoButton, Qt::LeftButton, 120, 100);
        send_mouse(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton,
                   120, 100);

        pm.step();                      /* the fixture replays forever */
        const QString l2 = win.strip()->line(1);
        const bool ok = l2.startsWith(QStringLiteral("box (")) &&
                        l2.contains(QStringLiteral("n="));
        std::printf("  %-4s the strip reports the measurement (\"%s\")\n",
                    ok ? "ok" : "FAIL", l2.toUtf8().constData());
        if (!ok)
            fails++;
    }

    /* ---- the runtime-parameter ladder and the device panel ----------------
     *
     * The parameter keys are case-sensitive, so these send a real character
     * rather than a bare key code: it is e->text() that carries the case, and
     * the synthesized events the measurement assertions use carry none.  The
     * write itself needs a device, so the send is intercepted here — what is
     * pinned is the routing and the contract, not libuvc. */
    auto send_char = [&](char c, Qt::KeyboardModifiers m = Qt::NoModifier) {
        const Qt::Key k = (c >= 'a' && c <= 'z')
                              ? (Qt::Key)(Qt::Key_A + (c - 'a'))
                              : (c >= 'A' && c <= 'Z')
                                    ? (Qt::Key)(Qt::Key_A + (c - 'A'))
                                    : (Qt::Key)(unsigned char)c;
        QKeyEvent e(QEvent::KeyPress, k, m, QString(QChar(c)));
        QApplication::sendEvent(&win, &e);
    };
    auto send_esc = [&]() {
        QKeyEvent e(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(&win, &e);
    };

    FrameView *fv = win.view();

    int              sends  = 0;
    dyt_order_type_t sent_t = (dyt_order_type_t)0;
    float            sent_v = 0.f;
    bool             send_ok = true;
    fv->on_param_send_ = [&](dyt_order_type_t t, float v) {
        sends++;
        sent_t = t;
        sent_v = v;
        return send_ok;
    };
    bool quit_seen  = false;
    bool retry_seen = false;
    win.on_quit_  = [&]() { quit_seen  = true; };
    win.on_retry_ = [&]() { retry_seen = true; };

    /* 28. A parameter key arms its ladder, re-pressing it advances the rung,
     * and a different parameter's key starts its own ladder at the first. */
    {
        send_char('e');
        const bool a = fv->param_armed() &&
                       fv->armed_type() == DYT_ORDER_EMISSIVITY &&
                       fv->armed_rung() == 0 &&
                       std::fabs(fv->armed_value() - 1.00f) < 1e-6;
        send_char('e');
        const bool b = fv->armed_rung() == 1 &&
                       std::fabs(fv->armed_value() - 0.95f) < 1e-6;
        send_char('A');
        const bool c = fv->armed_type() == DYT_ORDER_AMBIENT &&
                       fv->armed_rung() == 0;
        send_esc();

        const bool ok = a && b && c;
        std::printf("  %-4s a parameter key arms its ladder and advances it "
                    "(arm %s, advance %s, switch %s)\n",
                    ok ? "ok" : "FAIL", a ? "yes" : "NO", b ? "yes" : "NO",
                    c ? "yes" : "NO");
        if (!ok)
            fails++;
    }

    /* 29. The case split: 'e' is emissivity but 'A'/'R'/'D' are the other
     * three, and lowercase 'r' is still the device retry while uppercase 'R'
     * is reflected.  This is the binding the old unconditional fold destroyed. */
    {
        send_char('e');
        const bool em = fv->armed_type() == DYT_ORDER_EMISSIVITY;
        send_esc();
        send_char('A');
        const bool am = fv->armed_type() == DYT_ORDER_AMBIENT;
        send_esc();
        send_char('R');
        const bool re = fv->armed_type() == DYT_ORDER_REFLECTED;
        send_esc();
        send_char('D');
        const bool di = fv->armed_type() == DYT_ORDER_DISTANCE;
        send_esc();

        retry_seen = false;
        send_char('r');
        const bool lower = retry_seen && !fv->param_armed();

        retry_seen = false;
        send_char('R');
        const bool upper = !retry_seen &&
                           fv->armed_type() == DYT_ORDER_REFLECTED;
        send_esc();

        const bool ok = em && am && re && di && lower && upper;
        std::printf("  %-4s the case split holds (e/A/R/D %s, r=retry %s, "
                    "R=reflected %s)\n", ok ? "ok" : "FAIL",
                    (em && am && re && di) ? "yes" : "NO",
                    lower ? "yes" : "NO", upper ? "yes" : "NO");
        if (!ok)
            fails++;
    }

    /* 30. While a candidate is armed every other key is swallowed, so a stray
     * tool or view key cannot slip past a pending confirmation; ESC cancels. */
    {
        dyt_snapshot_t s{};
        dyt_session_snapshot(sess, &s, nullptr, 0);
        const dyt_tool_t tool0 = s.tool;
        const int        iso0  = s.iso_on;

        send_char('e');
        send_char('p');                 /* would be the point tool, unarmed */
        send_char('i');                 /* would toggle the isotherm */
        dyt_session_snapshot(sess, &s, nullptr, 0);
        const bool swallowed = fv->param_armed() && s.tool == tool0 &&
                               s.iso_on == iso0;
        send_esc();
        const bool cancelled = !fv->param_armed();

        const bool ok = swallowed && cancelled;
        std::printf("  %-4s a key while armed is swallowed and ESC cancels "
                    "(swallow %s, cancel %s)\n", ok ? "ok" : "FAIL",
                    swallowed ? "yes" : "NO", cancelled ? "yes" : "NO");
        if (!ok)
            fails++;
    }

    /* 31. Confirming reaches the front end with the armed value, and a refused
     * write leaves the candidate armed rather than losing it. */
    {
        sends = 0; sent_t = (dyt_order_type_t)0; sent_v = 0.f; send_ok = true;
        send_char('A');
        send_char('y');
        const bool sent = sends == 1 && sent_t == DYT_ORDER_AMBIENT &&
                          std::fabs(sent_v - 20.0f) < 1e-6 &&
                          !fv->param_armed();

        send_ok = false;
        send_char('e');
        send_char('y');
        const bool kept = fv->param_armed() &&
                          fv->armed_type() == DYT_ORDER_EMISSIVITY;
        send_ok = true;
        send_esc();

        const bool ok = sent && kept;
        std::printf("  %-4s y sends the armed value and a refusal keeps it "
                    "armed (send %s, keep %s)\n", ok ? "ok" : "FAIL",
                    sent ? "yes" : "NO", kept ? "yes" : "NO");
        if (!ok)
            fails++;
    }

    /* 32. The device panel's rows come from the view model, and a value a
     * write superseded is marked with a `*` — but only a write that succeeded. */
    {
        dyt_device_info_t d{};
        d.have_sn = 1;
        std::snprintf(d.sn_str, sizeof d.sn_str, "TESTSN");
        d.params_read = DYT_PARAM_N;
        d.radio.ok         = DYT_RADIO_EMISSIVITY | DYT_RADIO_DISTANCE;
        d.radio.emissivity = 96;        /* 96/128  = 0.75 */
        d.radio.distance   = 128;       /* 128/128 = 1.00 */
        fv->set_info(d, true);

        const QString serial = fv->info_line(0);
        const QString emis0  = fv->info_line(1);
        const bool base = serial.contains(QStringLiteral("TESTSN")) &&
                          emis0.contains(QStringLiteral("0.7500")) &&
                          !emis0.contains(QChar('*'));

        fv->set_param_result(DYT_ORDER_EMISSIVITY, 0.80f, 0);
        const bool over = fv->info_line(1).contains(QStringLiteral("0.8000*"));

        /* A failed write must not claim the stored value was superseded. */
        fv->set_param_result(DYT_ORDER_DISTANCE, 5.0f, -1);
        const QString emis2 = fv->info_line(1);
        const bool nofail = emis2.contains(QStringLiteral("1.0000")) &&
                            !emis2.contains(QStringLiteral("5.0000"));

        const bool ok = base && over && nofail;
        std::printf("  %-4s the device panel rows and the override `*` "
                    "(rows %s, override %s, failed-write %s)\n",
                    ok ? "ok" : "FAIL", base ? "yes" : "NO",
                    over ? "yes" : "NO", nofail ? "yes" : "NO");
        if (!ok)
            fails++;
    }

    /* 33. The device panel is painted.  Its background is opaque and sits at a
     * fixed spot, so an interior pixel must be the fill colour while it is up;
     * that is what pins the overlay reaching the canvas, not just the rows. */
    {
        const QImage on = fv->grab().toImage();
        const bool fill = fv->info_shown() &&
                          on.pixelColor(15, 15) == QColor(16, 16, 16) &&
                          on.pixelColor(16, 15) == QColor(16, 16, 16) &&
                          on.pixelColor(15, 16) == QColor(16, 16, 16);

        fv->toggle_info();              /* hide */
        const bool hidden = !fv->info_shown();
        fv->toggle_info();              /* show again */

        const bool ok = fill && hidden && fv->info_shown();
        std::printf("  %-4s the device panel is painted over the image "
                    "(fill %s, toggle %s)\n", ok ? "ok" : "FAIL",
                    fill ? "yes" : "NO", hidden ? "yes" : "NO");
        if (!ok)
            fails++;
    }

    /* 34. The confirmation is painted, and only while something is armed.  The
     * fill colour is counted rather than sampled, so an unlucky palette pixel
     * cannot make this pass on its own. */
    {
        auto count_fill = [](const QImage &im) {
            int n = 0;
            for (int y = 0; y < im.height(); y++)
                for (int x = 0; x < im.width(); x++)
                    if (im.pixelColor(x, y) == QColor(0, 0, 150))
                        n++;
            return n;
        };

        send_esc();                     /* nothing armed */
        const int before = count_fill(fv->grab().toImage());

        send_char('e');                 /* armed */
        const int after = count_fill(fv->grab().toImage());
        send_esc();
        const int gone = count_fill(fv->grab().toImage());

        const bool ok = after > before && after > gone;
        std::printf("  %-4s the confirmation is painted only while armed "
                    "(idle %d, armed %d, cancelled %d)\n", ok ? "ok" : "FAIL",
                    before, after, gone);
        if (!ok)
            fails++;
    }

    /* 35. `q` quits, and is never swallowed — even while a write is armed,
     * which is the view model's explicit contract. */
    {
        quit_seen = false;
        send_char('q');
        const bool idle = quit_seen;

        quit_seen = false;
        send_char('e');                 /* arm */
        send_char('q');
        const bool armed = quit_seen && fv->param_armed();
        send_esc();

        const bool ok = idle && armed;
        std::printf("  %-4s q quits and is never swallowed (idle %s, armed "
                    "%s)\n", ok ? "ok" : "FAIL", idle ? "yes" : "NO",
                    armed ? "yes" : "NO");
        if (!ok)
            fails++;
    }

    /* 36. The read-back comparison is in the encoded domain, which is the
     * whole reason a verification can pass at all: sendOrder quantises, so a
     * decoded float compare would call every write a mismatch.  The read
     * itself needs a device; this is the part that decides the verdict. */
    {
        /* 0.80 encodes to 102 and never reads back as 0.80 — 102/128 is
         * 0.796875 — so the naive compare fails and the encoded one passes. */
        const bool quantised =
            param_raw_matches(DYT_ORDER_EMISSIVITY, 0.80f, 102) &&
            !param_raw_matches(DYT_ORDER_EMISSIVITY, 0.80f, 103) &&
            std::fabs((double)dyt_param_decode_ratio(102) - 0.80) > 0.001;

        /* 1.00 is an exact step, and a one-LSB error must not pass. */
        const bool exact = param_raw_matches(DYT_ORDER_EMISSIVITY, 1.00f, 128) &&
                           !param_raw_matches(DYT_ORDER_EMISSIVITY, 1.00f, 127) &&
                           param_raw_matches(DYT_ORDER_DISTANCE, 1.00f, 128);

        /* The Celsius types encode to whole kelvin, by truncation. */
        const bool kelvin = param_raw_matches(DYT_ORDER_AMBIENT, 25.0f, 298) &&
                            !param_raw_matches(DYT_ORDER_AMBIENT, 25.0f, 299) &&
                            param_raw_matches(DYT_ORDER_REFLECTED, 20.0f, 293);

        const bool ok = quantised && exact && kelvin;
        std::printf("  %-4s the read-back compares in the encoded domain "
                    "(quantised %s, exact %s, kelvin %s)\n", ok ? "ok" : "FAIL",
                    quantised ? "yes" : "NO", exact ? "yes" : "NO",
                    kelvin ? "yes" : "NO");
        if (!ok)
            fails++;
    }

    /* Leave the view model's state as the rest of the run found it. */
    fv->clear_info();
    win.on_quit_  = nullptr;
    win.on_retry_ = nullptr;

    dyt_frame_source_close(fs);
    dyt_session_free(sess);

    std::printf("=== %s ===\n", fails ? "FAIL" : "ALL PASS");
    return fails ? 1 : 0;
}

/* ------------------------------------------------------------------- main */

/* ---------------------------------------------------------- live bring-up */

/* Everything the live path owns, so teardown can unwind exactly what was
 * created and nothing else.  `rc` is 0 only when the whole sequence succeeded;
 * `fs` is NULL until then, which is what the pump keys NoDevice off. */
struct live {
    dyt_capture_t         *cap  = nullptr;
    dyt_session_capture_t *sc   = nullptr;
    dyt_frame_source_t    *fs   = nullptr;
    int                    rc   = -1;
    dyt_mode_t             mode = DYT_MODE_1000;

    /* The identity read at bring-up.  It rides here so it crosses the worker
     * boundary in the job's `out`, and it must stay a plain C struct: `live`
     * is copied by value, so anything non-POD here would break that. */
    dyt_device_info_t      info{};
    int                    have_info = 0;
};

/* What the teardown's parameter read-back found.  See verify_writes(). */
struct VerifyOut {
    int checked = 0;   /* slots this session wrote and the device was asked for */
    int bad     = 0;   /* of those, the ones that did not confirm */
};

/* Bring the device up, or fail leaving `L` in a state tear_down_live() can
 * still unwind.  The order is load-bearing, from the canonical sequence in
 * session_capture.h:21-27:
 *
 *   - dyt_session_capture_set_capture() must precede dyt_capture_start(),
 *     because the adapter reads its capture handle on every frame;
 *   - dyt_capture_read_info() must run after open() and *before* start(),
 *     because the identity reads only answer cleanly while the device is idle
 *     (measured 2026-09-25);
 *   - the frame source is opened last, since it only renders what the adapter
 *     has already installed.
 *
 * Every failure prints its own reason to stderr — the capture layer has no
 * message string to retrieve — so the window only has to show NO DEVICE.
 *
 * dyt_capture_open() has no timeout (every libuvc control transfer in it passes
 * timeout 0, which libusb reads as "wait forever"), so a wedged device hangs
 * this call indefinitely.  That is why it runs on the worker thread and not on
 * the GUI thread — see DeviceWorker below.  It is still unbounded: a call that
 * never returns leaves a thread that is abandoned rather than joined. */
static int bring_up_live(const dyt_capture_opts &cap, dyt_session_t *sess, live &L)
{
    if (dyt_capture_open(&L.cap, &cap) != 0)
        return -1;

    L.mode = dyt_capture_mode(L.cap);

    L.sc = dyt_session_capture_create(sess);
    if (!L.sc || dyt_session_capture_set_capture(L.sc, L.cap) != 0) {
        std::fprintf(stderr, "dytqt: out of memory\n");
        return -1;
    }

    /* Retained rather than discarded: the panel shows these, and the read can
     * only be made in this idle window, so there is no second chance.  A read
     * that ran but found no serial still counts — the panel degrades row by
     * row (dyt_vm_info) rather than vanishing. */
    if (dyt_capture_read_info(L.cap, &L.info) == 0) {
        L.have_info = 1;
        if (L.info.have_sn)
            std::fprintf(stderr, "dytqt: serial %s\n", L.info.sn_str);
    }

    if (dyt_capture_start(L.cap, dyt_session_capture_on_frame, L.sc) != 0)
        return -1;

    L.fs = dyt_frame_source_open_live(sess);
    if (!L.fs) {
        std::fprintf(stderr, "dytqt: out of memory\n");
        return -1;
    }

    L.rc = 0;
    return 0;
}

/* ------------------------------------------------------- write verification
 *
 * A runtime write cannot be verified where it is made.  dyt_write_param() is
 * two OUT transfers with no status poll, so it works while the isochronous
 * stream runs; dyt_read_param() polls status register 0x0200, and the stream
 * starves that poll.  Measured 2026-09-25 on 0bda:5840: while streaming every
 * slot read fails (rc -1), and all four answer the moment the stream is
 * stopped.  So the read-back is deferred to the teardown, which is the first
 * moment the device is idle again — and the only one before the handle closes.
 *
 * It is also why the panel's `*` means "written this session, not yet
 * confirmed" rather than "verified": the confirmation can only arrive here.
 */

/* Read back every slot this session wrote, and report each.  `want`/`on` are
 * the front end's override table, indexed by dyt_order_type_t (1..4).
 *
 * Three measured behaviours shape this, all 2026-09-25 on 0bda:5840:
 *
 *  1. A runtime order is *deferred* while the stream is young.  Written at 1 s
 *     with the stream left running, it lands by ~6.7 s; written at 4.7 s it
 *     lands just the same.  So the order is not dropped, it is held until the
 *     stream has run ~6.7 s and applied then — and a stream that stops before
 *     that discards it.  That is the "single sendOrder" quirk: a harness that
 *     wrote once and stopped after a few seconds saw nothing, while the app,
 *     whose writes come after the user has been watching, saw them persist.
 *  2. The device answers a slot *before* it reflects the order.  In one run
 *     the first read, +500 ms after the stop, returned the pre-write 127, and
 *     only the +1000 ms read returned the written 128; the next four runs of
 *     the same shape were fresh at +500 ms.  So the lag is real but not fixed,
 *     and no single successful read can tell a lost order from a slow commit.
 *  3. While the device still holds an unapplied order it does not answer slot
 *     reads at all — every attempt fails at the transfer level (rc -1) — and
 *     it stays that way for ~8-9 s (measured through the app, whose stream was
 *     stopped with the write still pending).  A budget shorter than that
 *     reports "read-back failed" for a lost order instead of MISMATCH: the
 *     honest verdict, and the budget stays short so a normal quit is not held
 *     hostage to the worst case.
 *
 * The asymmetry in (2) is what makes a verdict possible: reading the *written*
 * value back cannot be faked, whereas reading anything else may still be the
 * lag.  So this polls until every armed slot has shown its written value, or
 * until the budget runs out, and then judges the last value seen.  A confirmed
 * write costs one pass; a lost one costs the budget.  A failed attempt is a
 * single errored transfer, so polling costs the sleep and little else. */
static void verify_writes(dyt_capture_t *cap, const float *want, const int *on,
                          VerifyOut &out)
{
    uint16_t last[5] = { 0, 0, 0, 0, 0 };
    int      have[5] = { 0, 0, 0, 0, 0 };
    int      good[5] = { 0, 0, 0, 0, 0 };
    int      rclast[5] = { 0, 0, 0, 0, 0 };
    int      armed   = 0;
    /* 2.25 s keeps the whole teardown inside run_gui's 3 s exit timeout. */
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(2250);

    for (int t = DYT_ORDER_REFLECTED; t <= DYT_ORDER_DISTANCE; t++)
        if (on && on[t])
            armed++;
    if (!armed)
        return;

    for (;;) {
        int pending = 0;

        for (int t = DYT_ORDER_REFLECTED; t <= DYT_ORDER_DISTANCE; t++) {
            uint16_t raw = 0;
            int      rc;

            if (!on || !on[t] || good[t])
                continue;
            pending++;                         /* still unconfirmed */
            rc = dyt_capture_read_param(cap, t, &raw);
            rclast[t] = rc;
            if (rc != 0)
                continue;                      /* not answering yet */
            last[t] = raw;
            have[t] = 1;
            if (param_raw_matches((dyt_order_type_t)t, want[t], raw)) {
                good[t] = 1;                   /* cannot be faked */
                pending--;
            }
        }
        if (!pending || std::chrono::steady_clock::now() >= deadline)
            break;
        usleep(250000);
    }

    for (int t = DYT_ORDER_REFLECTED; t <= DYT_ORDER_DISTANCE; t++) {
        const dyt_vm_ladder_t *L;
        char w[32];

        if (!on || !on[t])
            continue;

        out.checked++;
        L = dyt_vm_ladder((dyt_order_type_t)t);
        dyt_vm_param_format((dyt_order_type_t)t, want[t], w, sizeof w);

        if (!have[t]) {
            std::fprintf(stderr,
                         "dytqt: verify %s = %s -> read-back failed (rc %d)\n",
                         L ? L->name : "?", w, rclast[t]);
            out.bad++;
        } else if (good[t]) {
            std::fprintf(stderr, "dytqt: verify %s = %s -> confirmed (raw %u)\n",
                         L ? L->name : "?", w, (unsigned)last[t]);
        } else {
            std::fprintf(stderr,
                         "dytqt: verify %s = %s -> MISMATCH (device %u)\n",
                         L ? L->name : "?", w, (unsigned)last[t]);
            out.bad++;
        }
    }
}

/* Unwind in reverse.  dyt_capture_stop() comes first because it joins libuvc's
 * callback thread, and that thread is writing into the session through the
 * adapter — freeing either before it stops is a use-after-free.  Everything
 * here is NULL-safe and idempotent, so it is also the failure path.
 *
 * `want`/`on` are the writes to verify, or NULL for none: the read-back has to
 * happen after the stop (device idle) and before the close (handle gone), so
 * it lives here rather than beside the write.  A failed bring-up passes
 * neither, because a device that never came up has nothing to verify. */
static void tear_down_live(live &L, const float *want = nullptr,
                           const int *on = nullptr, VerifyOut *out = nullptr)
{
    if (L.cap) {
        dyt_capture_stop(L.cap);
        if (want && on && out)
            verify_writes(L.cap, want, on, *out);
    }
    if (L.fs)  dyt_frame_source_close(L.fs);
    if (L.cap) dyt_capture_close(L.cap);
    if (L.sc)  dyt_session_capture_free(L.sc);
    L = live{};
}

/* ------------------------------------------------------- the device worker */

/* One device operation, handed to the worker and handed back with its result.
 *
 * The capture options are *copied* rather than pointed at: a wedged job is
 * abandoned at exit, and by then run_gui's `opts` may be gone.  Nothing else is
 * owned — `sess` is borrowed (the session outlives every job), and `in`/`out`
 * are handles the GUI moves in and out so exactly one side owns them at a time. */
struct Job {
    enum Kind { BringUp, TearDown, SetParam } kind = BringUp;
    dyt_capture_opts cap{};
    dyt_session_t   *sess = nullptr;
    live             in{};    /* TearDown: the handles to unwind */
    live             out{};   /* BringUp: what was created; empty on failure */

    /* SetParam: the capture handle is *borrowed*, never owned — `in`/`out` are
     * the owned handles that tear_down_live() unwinds, and this is not one of
     * them.  It stays valid because the worker runs one job at a time and
     * every teardown is posted through the same worker. */
    dyt_capture_t   *dev = nullptr;
    dyt_order_type_t ptype = (dyt_order_type_t)0;
    float            pvalue = 0.f;

    /* TearDown: the runtime writes this session made, so the teardown can read
     * the slots back once the device is idle — the only moment a read answers
     * (see verify_writes).  Copied from the view's override table before it is
     * cleared.  `verify` is the result, filled on the worker. */
    float            want_v[5]  = { 0.f, 0.f, 0.f, 0.f, 0.f };
    int              want_on[5] = { 0, 0, 0, 0, 0 };
    VerifyOut        verify{};

    int              rc = -1;
};

/* Runs bring-up, teardown and the runtime-parameter write off the GUI thread.
 *
 * Bring-up and teardown can block without bound — dyt_capture_open() on a
 * wedged camera, dyt_capture_stop() on a stream whose transfers never
 * complete.  On the GUI thread either one freezes the window with no way out,
 * and no QTimer can rescue it because the GUI thread is the one blocked.
 *
 * The write is bounded but slow: dyt_capture_set_param() is two control
 * transfers of up to 1000 ms each plus a 250 ms settle, so ~250 ms typical and
 * ~2.25 s worst case.  It belongs here for the same reason — a user who
 * confirms a write should not watch the window stop responding — and it is
 * safe to run here because the vendor's own tool writes while streaming.
 *
 * A std::thread, detached, rather than a QThread: this file has no Q_OBJECT and
 * the build runs no moc, so a queued signal is not available either way — and
 * more to the point, a wedged worker must be *abandoned*, never joined.  A
 * QThread member aborts in its destructor while still running; a detached
 * std::thread simply dies with the process.
 *
 * One job at a time, enforced by busy(): a second bring-up while the first is
 * still inside dyt_capture_open() would fight it for the device, and a teardown
 * must never run under a write that is borrowing the capture handle.  That
 * single-job rule is what makes the borrowed handle in Job::dev safe. */
class DeviceWorker {
public:
    using Done = std::function<void(const std::shared_ptr<Job> &)>;

    bool busy() const { return busy_; }

    void post(const std::shared_ptr<Job> &job, Done done)
    {
        busy_ = true;
        std::thread([this, job, done]() {
            if (job->kind == Job::BringUp) {
                job->rc = bring_up_live(job->cap, job->sess, job->out) == 0
                              ? 0 : -1;
                /* A half-built device is unwound here, so a failed bring-up
                 * hands back an empty `live` and a retry starts from clean. */
                if (job->rc != 0)
                    tear_down_live(job->out);
            } else if (job->kind == Job::TearDown) {
                /* The read-back rides here: after the stop makes the device
                 * idle, before the close takes the handle away. */
                tear_down_live(job->in, job->want_v, job->want_on,
                               &job->verify);
            } else {
                /* SetParam.  Borrows the handle and owns nothing, so there is
                 * nothing to unwind.  This is the one device write the port
                 * implements, and it can block for a couple of seconds — which
                 * is exactly why it is here and not on the GUI thread. */
                job->rc = dyt_capture_set_param(job->dev, (int)job->ptype,
                                                job->pvalue);
            }
            /* Back to the GUI thread.  invokeMethod with a functor needs no
             * Q_OBJECT and no moc.  If the application is already gone this is
             * never delivered — which is why nothing is dereferenced until it
             * runs, and why an abandoned worker must never get here at all
             * (run_gui exits immediately instead; see the end of run_gui). */
            QMetaObject::invokeMethod(qApp, [this, job, done]() {
                busy_ = false;
                done(job);
            }, Qt::QueuedConnection);
        }).detach();
    }

private:
    bool busy_ = false;   /* written only on the GUI thread */
};

static int run_gui(const opts &o, QApplication &app)
{
    dyt_session_t *sess = setup_session(o);
    if (!sess)
        return 1;

    MainWindow win;
    win.view()->set_session(sess);
    pump       pm;
    pm.sess = sess;
    pm.win  = &win;
    pm.live = o.live;
    if (dyt_session_get_palette(sess, o.palette - 1, &pm.pal) != 0) {
        dyt_session_free(sess);
        return 1;
    }

    /* Paint before bring-up.  Bring-up now runs on a worker, so the window is
     * responsive throughout it, but it still has nothing to show until then:
     * this paints CONNECTING rather than an uninitialised window. */
    win.fit_to_view();
    win.show();
    QApplication::processEvents();

    live         L;
    DeviceWorker worker;
    bool         quitting = false;
    int          attempt  = 0;    /* retries made since the last success */

    QTimer timer;
    QTimer retry_timer;
    retry_timer.setSingleShot(true);

    /* Declared as std::functions so the retry timer, the stall handler and the
     * R key share one definition each without a Q_OBJECT (there is no moc in
     * this build). */
    std::function<void()>       post_bringup;
    std::function<void()>       schedule_retry;
    std::function<void(bool)>   reconnect;

    schedule_retry = [&]() {
        const double d = retry_delay_s(attempt);
        if (d < 0.0) {
            std::fprintf(stderr, "dytqt: giving up after %d attempt(s)\n",
                         attempt);
            return;
        }
        attempt++;
        std::fprintf(stderr, "dytqt: retrying in %.1f s\n", d);
        retry_timer.start((int)(d * 1000.0));
    };

    /* Bring the device up on the worker and act on the result.  CONNECTING
     * while the attempt runs; LIVE or NO DEVICE once it answers. */
    post_bringup = [&]() {
        if (quitting || worker.busy())
            return;
        pm.bringup_done = false;      /* CONNECTING for the duration */
        auto job  = std::make_shared<Job>();
        job->kind = Job::BringUp;
        job->cap  = o.cap;
        job->sess = sess;
        worker.post(job, [&](const std::shared_ptr<Job> &j) {
            if (quitting)
                return;
            L               = j->out;
            pm.fs           = L.fs;   /* NULL when bring-up failed */
            pm.mode         = L.mode;
            pm.bringup_rc   = j->rc;
            pm.bringup_done = true;
            /* A failed bring-up handed back an empty `out`, so this clears the
             * panel as well as filling it. */
            win.view()->set_info(L.info, L.have_info != 0);
            if (j->rc == 0) {
                attempt = 0;
                retry_timer.stop();
                pm.stall.reset();
            } else {
                schedule_retry();
            }
        });
    };

    /* The stream died: unwind what we hold, then start retrying.  The source is
     * nulled on *this* thread before the worker is asked to close it, so the
     * pump can never pull from a frame source being freed. */
    reconnect = [&](bool now) {
        if (quitting || worker.busy())
            return;
        pm.fs           = nullptr;
        pm.bringup_done = false;      /* CONNECTING, not LIVE over a dead stream */
        pm.stall.reset();
        attempt = 0;

        /* Snapshot the writes before the table is cleared: the teardown reads
         * those slots back while the device is briefly idle, which is the only
         * moment a read answers (verify_writes). */
        float want_v[5];
        int   want_on[5];
        win.view()->param_overrides(want_v, want_on);

        /* The device is about to be re-opened, so its identity and the writes
         * this session made no longer describe it — and a candidate armed
         * against it is meaningless.  The fresh bring-up re-reads both. */
        win.view()->clear_info();

        auto afterwards = [&, now]() {
            if (quitting)
                return;
            if (now) post_bringup();
            else     schedule_retry();
        };

        if (!L.cap && !L.fs && !L.sc) {
            afterwards();
            return;
        }
        auto job  = std::make_shared<Job>();
        job->kind = Job::TearDown;
        job->in   = L;                /* the worker owns them from here */
        for (int i = 0; i < 5; i++) {
            job->want_v[i]  = want_v[i];
            job->want_on[i] = want_on[i];
        }
        L = live{};
        worker.post(job, [&, afterwards](const std::shared_ptr<Job> &j) {
            /* The read-back's verdict, for anyone not watching stderr.  Only
             * when something was actually written and checked. */
            if (j->verify.checked > 0) {
                if (j->verify.bad == 0) {
                    pm.msg = "write confirmed by read-back";
                } else {
                    pm.msg = "write NOT confirmed (" +
                             std::to_string(j->verify.bad) + " of " +
                             std::to_string(j->verify.checked) + ")";
                }
                pm.msg_ttl = 150;
            }
            afterwards();
        });
    };

    win.on_retry_ = [&]() { reconnect(true); };
    win.on_quit_  = [&]() { win.close(); };

    /* The runtime-parameter write.  The window owns the arming state and the
     * device handle lives here, so the two meet through these callbacks — the
     * same split as on_retry_/on_close_, and the only option without a moc. */
    win.view()->on_param_send_ = [&](dyt_order_type_t type, float value) -> bool {
        /* Refuse rather than queue: a bring-up or teardown owns the device, and
         * the job's borrowed handle would not be safe beside one.  Returning
         * false keeps the candidate armed so the user can confirm again. */
        if (quitting || worker.busy() || !L.cap) {
            pm.msg     = "device busy; try again";
            pm.msg_ttl = 90;
            return false;
        }

        const dyt_vm_ladder_t *lad = dyt_vm_ladder(type);
        char val[32];
        dyt_vm_param_format(type, value, val, sizeof val);
        pm.msg     = std::string("set ") + (lad ? lad->name : "?") + " = " + val +
                     "  (sending)";
        pm.msg_ttl = 150;

        auto job    = std::make_shared<Job>();
        job->kind   = Job::SetParam;
        job->dev    = L.cap;      /* borrowed; the worker owns nothing */
        job->ptype  = type;
        job->pvalue = value;
        worker.post(job, [&](const std::shared_ptr<Job> &j) {
            if (quitting)
                return;
            const dyt_vm_ladder_t *l2 = dyt_vm_ladder(j->ptype);
            char v2[32];
            dyt_vm_param_format(j->ptype, j->pvalue, v2, sizeof v2);
            if (j->rc == 0) {
                win.view()->set_param_result(j->ptype, j->pvalue, 0);
                pm.msg = std::string("set ") + (l2 ? l2->name : "?") + " = " + v2 +
                         "  (sent)";
            } else {
                pm.msg = std::string("set ") + (l2 ? l2->name : "?") +
                         " FAILED (rc " + std::to_string(j->rc) + ")";
            }
            pm.msg_ttl = 150;
            /* Also to stderr, as the reference viewer does: the strip is the
             * user's view, but a live run's transcript is the evidence. */
            std::fprintf(stderr, "dytqt: set %s = %s -> rc %d\n",
                         l2 ? l2->name : "?", v2, j->rc);
        });
        return true;
    };
    win.view()->on_param_cancel_ = [&](dyt_order_type_t type) {
        const dyt_vm_ladder_t *lad = dyt_vm_ladder(type);
        pm.msg     = std::string("cancelled: ") + (lad ? lad->name : "?");
        pm.msg_ttl = 90;
    };

    win.on_close_ = [&]() {
        /* Only mark it: the device is handed back after app.exec() returns, so
         * teardown never runs re-entrantly inside a close event.
         *
         * Qt calls this on app.quit() as well as on a user close — verified,
         * not assumed — which is why nothing downstream distinguishes the two:
         * this is simply "the window is going away". */
        quitting = true;
        timer.stop();
        retry_timer.stop();
        pm.fs = nullptr;
    };

    if (!o.live) {
        pm.fs = dyt_frame_source_open_fixture(
            sess, o.fixture.c_str(), o.width, DYT_MODE_1000,
            DYT_PLANE_BOTTOM_HALF, o.cap.t_amb, o.cap.sensor_mode,
            o.cap.fix_mode, 0);
        if (!pm.fs) {
            dyt_session_free(sess);
            return 1;
        }
        pm.bringup_done = true;
    } else {
        post_bringup();               /* CONNECTING until the worker answers */
    }

    QObject::connect(&retry_timer, &QTimer::timeout, [&]() { post_bringup(); });

    QObject::connect(&timer, &QTimer::timeout, [&]() {
        if (!pm.step()) {
            timer.stop();
            std::printf("dytqt: fixture exhausted after %lld frame(s)\n",
                        pm.frames);
            app.quit();
            return;
        }
        if (pm.reconnect) {
            pm.reconnect = false;
            reconnect(false);
        }
        if (o.frames > 0 && pm.frames >= o.frames) {
            timer.stop();
            std::printf("dytqt: painted %lld frame(s) over %lld tick(s)\n",
                        pm.frames, pm.ticks);
            app.quit();
        }
    });
    timer.start(1000 / o.fps);

    const int rc = app.exec();

    if (!o.png.empty()) {
        /* Only meaningful once something has been painted.  A live source that
         * never produced a real frame would otherwise write the placeholder —
         * and a properly settled live still belongs with the capture task
         * (#90), not here. */
        if (pm.frames == 0) {
            std::fprintf(stderr, "dytqt: no frame painted; nothing written\n");
        } else {
            const QPixmap grab = win.grab();
            if (grab.save(QString::fromUtf8(o.png.c_str())))
                std::printf("dytqt: canvas written to %s\n", o.png.c_str());
            else
                std::fprintf(stderr, "dytqt: could not write %s\n", o.png.c_str());
        }
    }

    /* Hand the device back, if it can be handed back.
     *
     * A worker still running is wedged inside a libuvc call that has no timeout.
     * It cannot be joined (that would hang the exit) and it cannot be left to
     * finish (it would call back into Qt after the application is gone), so the
     * only safe thing is to leave *now*, flushing stdio by hand and skipping
     * static teardown.  The session is leaked with it, because the wedged thread
     * may still be writing into it.  This is the documented cost of not putting
     * a timeout in vendored libuvc. */
    if (worker.busy()) {
        std::fprintf(stderr,
                     "dytqt: a device operation is still running; "
                     "exiting without it\n");
        std::fflush(nullptr);
        std::_Exit(rc);
    }

    if (L.cap || L.fs || L.sc) {
        auto job  = std::make_shared<Job>();
        job->kind = Job::TearDown;
        job->in   = L;
        /* The last chance to verify this session's writes: the teardown stops
         * the stream, and a stopped device is the only one that answers a
         * parameter read.  The verdict goes to stderr, which is where an
         * exiting app's evidence lives. */
        win.view()->param_overrides(job->want_v, job->want_on);
        L = live{};
        QEventLoop loop;
        bool done = false;
        worker.post(job, [&](const std::shared_ptr<Job> &) {
            done = true;
            loop.quit();
        });
        /* 3 s, not 1: the teardown now polls the written slots for up to
         * 2.25 s (see verify_writes) before it closes the handle, and a budget
         * that no longer matches the work would report a normal close as
         * "device did not stop".  It is only paid when this session actually
         * wrote something. */
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        loop.exec();
        if (!done) {
            std::fprintf(stderr,
                         "dytqt: device did not stop; exiting without it\n");
            std::fflush(nullptr);
            std::_Exit(rc);
        }
    }

    dyt_session_free(sess);
    return rc;
}

int main(int argc, char **argv)
{
    opts o;
    if (!parse_args(argc, argv, o))
        return 2;

    /* The selftest must not need a display, and must not steal one if the
     * user has one.  This has to happen before QApplication exists. */
    if (o.selftest)
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);

    if (o.selftest)
        return selftest(o);
    return run_gui(o, app);
}
