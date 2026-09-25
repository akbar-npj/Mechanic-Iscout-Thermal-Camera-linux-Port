/*
 * dytqt.cpp — Qt6 Widgets spike for the DYT thermal camera.
 *
 * This is the *spike* for the GUI, not the GUI.  Its job is to settle four
 * questions with evidence rather than opinion, so that the real front-end
 * (tasks #86 onward) is built on something measured:
 *
 *   1. Does the engine's rendered RGB frame reach a widget without inventing
 *      a conversion for it?  (QImage over the engine's own buffer.)
 *   2. Is 25 fps reachable from a QTimer without the frame path stalling?
 *   3. Does the view model's status line drop straight into a Qt widget?
 *      That is the entire reason src/view_model.{h,c} exists, so it is worth
 *      proving before a window is designed around it.
 *   4. Can the whole thing be verified with no display and no device?
 *
 * It replays a frozen fixture through the device-free pipeline
 * (tools/frame_source.c), so it needs no camera — and `--selftest` runs the
 * same path headless and asserts on it, which is what makes this a check
 * rather than a screenshot.
 *
 * Everything drawn comes from libdyt.  This file owns layout and paint, and
 * nothing else — the same rule dytview.cpp follows.
 *
 * build:  via the Makefile (needs Qt6 Widgets; see gui/README.md)
 * run:    ./build/dytqt [--fixture PATH] [--palette N] [--zoom N]
 *         ./build/dytqt --selftest
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QSize>
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

/* ------------------------------------------------------------- the canvas */

/* Paints the engine's frame.  The only class here that knows about pixels:
 * everything above it deals in the session and the view model, so the real
 * front-end can replace this without touching the frame path. */
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
         * sized itself — which is how the clipped-canvas bug stayed hidden. */
        update();
    }

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
            p.drawText(rect(), Qt::AlignCenter, "waiting for a frame");
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
};

/* The window: the canvas plus a status strip.  The strip is a plain QLabel
 * fed by dyt_vm_status_line(), which is the integration this spike exists to
 * prove. */
class MainWindow : public QWidget {
public:
    explicit MainWindow(QWidget *parent = nullptr) : QWidget(parent)
    {
        view_   = new FrameView(this);
        status_ = new QLabel(this);
        status_->setTextFormat(Qt::PlainText);
        status_->setStyleSheet(
            "color:#dcdcdc; background:#101010; padding:3px;");

        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);
        lay->addWidget(view_, 1);
        lay->addWidget(status_, 0);

        setWindowTitle("dytqt — Qt6 Widgets spike");
    }

    FrameView *view() const { return view_; }
    void set_status(const QString &s) { status_->setText(s); }

private:
    FrameView *view_   = nullptr;
    QLabel    *status_ = nullptr;
};

/* ------------------------------------------------------------ the pump
 *
 * One timer at the requested rate; each tick pulls a frame from the source,
 * hands the pixels to the canvas and the text to the status label.
 *
 * NOTE for task #85: this is deliberately the *fixture* path, where the frame
 * is produced on the GUI thread.  When the source is the device, the frame
 * arrives on libuvc's callback thread and this must become a queued signal
 * rather than a direct call — which is the whole of that task.
 */
struct pump {
    dyt_frame_source_t *fs   = nullptr;
    dyt_session_t      *sess = nullptr;
    MainWindow         *win  = nullptr;
    dyt_palette_t       pal{};
    dyt_vm_scratch_t    scr  = DYT_VM_SCRATCH_INIT;

    long long ticks  = 0;   /* timer callbacks */
    long long frames = 0;   /* frames actually painted */
    int       fails  = 0;
    double    worst_ms = 0.0;  /* slowest single step, for the fps headroom claim */

    bool step()
    {
        const uint8_t *rgb = nullptr;
        int            w = 0, h = 0;

        ticks++;

        const dyt_fs_status_t st = dyt_frame_source_next(fs, &rgb, &w, &h);
        if (st == DYT_FS_END)
            return false;
        if (st != DYT_FS_FRAME || !rgb) {
            if (st == DYT_FS_ERROR)
                fails++;
            return true;                 /* WAIT is normal */
        }

        dyt_snapshot_t snap;
        if (!dyt_vm_grab(sess, &snap, &scr)) {
            fails++;
            return true;
        }

        /* The engine's buffer is tightly packed RGB, so QImage wraps it with
         * no conversion.  It is only valid until the next next(), so the
         * widget takes a copy — the one copy in this path, and the one a real
         * front-end avoids by painting inside the tick. */
        const QImage wrapped(rgb, w, h, w * 3, QImage::Format_RGB888);
        win->view()->set_frame(snap, wrapped.copy(), pal);

        char status[256];
        dyt_vm_status_line(&snap, DYT_MODE_1000, status, sizeof status);
        win->set_status(QString::fromUtf8(status));

        frames++;
        return true;
    }
};

/* ------------------------------------------------------------- selftest
 *
 * The offline check.  It runs the same path the window does, under
 * QT_QPA_PLATFORM=offscreen, and asserts on the result rather than leaving a
 * human to look at a window: the fixture really converted (not the ~238.85 C
 * start-up filler), the geometry is the sensor's, the status line is
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

    win.adjustSize();
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
    dyt_snapshot_t snap;
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

    /* 5. The status line is populated, and is the view model's. */
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

    win.adjustSize();
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
