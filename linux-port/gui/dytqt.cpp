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
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "frame_source.h"
#include "palette.h"
#include "session.h"
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
    std::string fixture = "testdata/mode1000_256x384_default.raw";
    std::string palette_dir;
    std::string png;                 /* save the canvas here, then exit */
    int  width   = 256;
    int  palette = 1;
    int  zoom    = 2;
    int  frames  = 0;                /* 0 = run until closed */
    int  fps     = 25;
    bool selftest = false;
};

static void usage(const char *prog)
{
    std::fprintf(stderr,
        "usage: %s [options]\n"
        "  --fixture PATH   raw payload to replay (default %s)\n"
        "  --width N        sensor width of the fixture (default 256)\n"
        "  --palette N      1-based palette index (default 1)\n"
        "  --palette-dir D  where the *.dat ramps live (default: search)\n"
        "  --zoom N         window magnification (default 2)\n"
        "  --frames N       stop after N frames (default: run until closed)\n"
        "  --fps N          timer rate (default 25)\n"
        "  --png PATH       write the canvas here and exit\n"
        "  --selftest       headless check over the fixture; needs no display\n",
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
        else if (a == "--fixture")                 { const char *v = next("--fixture"); if (!v) return false; o.fixture = v; }
        else if (a == "--palette-dir")             { const char *v = next("--palette-dir"); if (!v) return false; o.palette_dir = v; }
        else if (a == "--png")                     { const char *v = next("--png"); if (!v) return false; o.png = v; }
        else if (a == "--width")                   { const char *v = next("--width"); if (!v) return false; o.width = std::atoi(v); }
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

/* ---------------------------------------------------------- device state
 *
 * next_live() returns WAIT both before the first frame *and* for the whole
 * start-up filler (frame_source.c:271), so the source alone cannot tell
 * "not connected yet" from "connected, still warming up".  The state is
 * therefore decided from the session's own `seq`/`ready`, which is the same
 * scalars-only snapshot next_live() takes internally and costs nothing.
 *
 * Kept a pure function of five booleans so every state is reachable in
 * `--selftest` with no device attached — which is the only way two of these
 * five can ever be tested at all. */
enum class DevState { Fixture, Connecting, NoDevice, WarmingUp, Live };

static DevState device_state(bool live, bool bringup_done, int bringup_rc,
                             bool snap_ok, bool snap_ready)
{
    if (!live)              return DevState::Fixture;
    if (!bringup_done)      return DevState::Connecting;
    if (bringup_rc != 0)    return DevState::NoDevice;
    if (!snap_ok || !snap_ready) return DevState::WarmingUp;
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
 * The rate is only shown for the two states that have painted frames: a
 * "0.0 fps" beside CONNECTING would be a claim about a stream that is not
 * running yet. */
static QString state_line(DevState s, const dyt_snapshot_t &snap, double fps)
{
    QString out = QStringLiteral("range %1   |   %2")
                      .arg(QString::fromUtf8(dyt_range_mode_name(snap.range_mode)),
                           QString::fromUtf8(state_label(s)));
    if (s == DevState::Fixture || s == DevState::Live)
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

/* ------------------------------------------------------------- the canvas */

/* Paints the engine's frame.  The only class here that knows about pixels:
 * everything above it deals in the session and the view model. */
class FrameView : public QWidget {
public:
    explicit FrameView(QWidget *parent = nullptr) : QWidget(parent)
    {
        setAutoFillBackground(true);
    }

    void set_frame(const dyt_snapshot_t &snap, const QImage &img,
                   const dyt_palette_t &pal)
    {
        snap_ = snap;
        img_  = img;
        pal_  = pal;
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

    bool  has_frame() const { return !img_.isNull(); }
    QSize imageSize() const { return img_.size(); }

    QSize sizeHint() const override
    {
        const int w = img_.isNull() ? 256 : img_.width();
        const int h = img_.isNull() ? 192 : img_.height();
        return QSize(kPad + w + kBarGap + kBarW + kLabelW + kPad, h + 2 * kPad);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(16, 16, 16));

        if (img_.isNull()) {
            p.setPen(QColor(200, 200, 200));
            p.drawText(rect(), Qt::AlignCenter, placeholder_);
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
    }

private:
    dyt_snapshot_t snap_{};
    QImage         img_;
    dyt_palette_t  pal_{};
    QString        placeholder_ = QStringLiteral("waiting for a frame");
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
    }

private:
    QString line_[3];
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
    }

    FrameView   *view()  const { return view_; }
    StatusStrip *strip() const { return strip_; }

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
        fit_to_view();
    }

    /* No frame to paint — the placeholder is up, so lines 1 and 2 keep
     * whatever they last said and only the state line moves. */
    void set_state_line(DevState st, const dyt_snapshot_t &snap, double fps)
    {
        strip_->set_lines(strip_->line(0), strip_->line(1),
                          state_line(st, snap, fps));
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

    long long ticks  = 0;   /* timer callbacks */
    long long frames = 0;   /* frames actually painted */
    int       fails  = 0;
    double    worst_ms = 0.0;  /* slowest single step, for the fps headroom claim */
    FpsMeter  fps;

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

        const DevState ds = device_state(live, bringup_done, bringup_rc,
                                        true, snap.ready != 0);

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

        /* The engine's buffer is tightly packed RGB, so QImage wraps it with
         * no conversion.  transformed() owns what it returns, because the
         * source buffer dies on the next next(). */
        const QImage wrapped(rgb, w, h, w * 3, QImage::Format_RGB888);
        win->view()->set_frame(snap, transformed(wrapped, snap.xform), pal);
        win->set_frame_status(snap, ds, fps.fps(), mode);

        frames++;
        fps.update(frames, now_s());
        return true;
    }

private:
    const std::chrono::steady_clock::time_point t_start_ =
        std::chrono::steady_clock::now();
};

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
        DYT_PLANE_BOTTOM_HALF, 25.0f, 0x82, 0, 0);
    if (!fs) {
        dyt_session_free(sess);
        return 1;
    }

    MainWindow win;
    pump       pm;
    pm.fs   = fs;
    pm.sess = sess;
    pm.win  = &win;
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
                 device_state(true, true, 0, true, fs_snap.ready != 0)
                     == DevState::WarmingUp;
            std::printf("  %-4s a filler frame is not a live frame "
                        "(ready %d, %s)\n", ok ? "ok" : "FAIL",
                        fs_snap.ready, state_label(device_state(true, true, 0,
                        true, fs_snap.ready != 0)));
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
        const DevState ds = device_state(true, true, -1, false, false);
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

    dyt_frame_source_close(fs);
    dyt_session_free(sess);

    std::printf("=== %s ===\n", fails ? "FAIL" : "ALL PASS");
    return fails ? 1 : 0;
}

/* ------------------------------------------------------------------- main */

static int run_gui(const opts &o, QApplication &app)
{
    dyt_session_t *sess = setup_session(o);
    if (!sess)
        return 1;

    dyt_frame_source_t *fs = dyt_frame_source_open_fixture(
        sess, o.fixture.c_str(), o.width, DYT_MODE_1000,
        DYT_PLANE_BOTTOM_HALF, 25.0f, 0x82, 0, 0);
    if (!fs) {
        dyt_session_free(sess);
        return 1;
    }

    MainWindow win;
    pump       pm;
    pm.fs   = fs;
    pm.sess = sess;
    pm.win  = &win;
    if (dyt_session_get_palette(sess, o.palette - 1, &pm.pal) != 0) {
        dyt_frame_source_close(fs);
        dyt_session_free(sess);
        return 1;
    }

    win.fit_to_view();
    win.show();

    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        if (!pm.step()) {
            timer.stop();
            std::printf("dytqt: fixture exhausted after %lld frame(s)\n",
                        pm.frames);
            app.quit();
            return;
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
        const QPixmap grab = win.grab();
        if (grab.save(QString::fromUtf8(o.png.c_str())))
            std::printf("dytqt: canvas written to %s\n", o.png.c_str());
        else
            std::fprintf(stderr, "dytqt: could not write %s\n", o.png.c_str());
    }

    dyt_frame_source_close(fs);
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
