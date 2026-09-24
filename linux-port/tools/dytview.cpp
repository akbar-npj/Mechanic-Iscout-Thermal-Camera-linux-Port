/*
 * dytview.cpp — live colour viewer for the DYT thermal camera Linux port.
 *
 * The port's answer to the vendor app's preview screen: a real window that
 * streams live temperatures, colours them with one of the six vendor
 * palettes, shows a scale, and reports the temperature under the mouse.
 *
 * This file is deliberately thin.  Everything device-independent already
 * lives in the C library and is reused verbatim:
 *
 *   src/capture.c   — opens the device, runs the verified pipeline, and
 *                     delivers temperature floats on libuvc's callback thread
 *   src/palette.c   — temperature -> RGB, palette loading, min/max
 *
 * so the viewer only does what a GUI must: marshal a frame across threads,
 * draw a scale, and read the mouse.
 *
 * Threading
 * ---------
 * libuvc delivers frames on its own thread and OpenCV insists its window
 * calls happen on the main thread, so the callback does the minimum — copy
 * the temperature image into a mutex-guarded buffer — and the main loop
 * renders.  Copying ~192 KiB per frame at 25 fps is far cheaper than
 * blocking the USB callback on a window paint.
 *
 * Usage
 * -----
 *   dytview --start-orders                 # live window (this unit)
 *   dytview --start-orders --zoom 3
 *   dytview --start-orders --palette 2     # 1..6 = vendor ramp
 *   dytview --start-orders --lo 20 --hi 40 # lock the display range
 *   dytview --start-orders --png shot.png  # headless: render one frame, exit
 *
 * Keys: 1-6 palette · r toggle auto/locked range · s snapshot PNG · q/ESC quit
 *
 * build:  via the Makefile (needs libusb + OpenCV; omitted without OpenCV)
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "capture.h"
#include "palette.h"

/* ------------------------------------------------------------------ config */

static const char *const kWin       = "dytview";
static const int         kBarW      = 22;   /* colour-bar width, px */
static const int         kBarGap    = 14;   /* image -> bar gap, px */
static const int         kLabelW    = 96;   /* room for the bar's labels */
static const int         kStatusH   = 22;   /* status strip height, px */

/* Until setTinyCOutputADValue takes effect the device streams a flat 0x8000
 * on every pixel (src/capture.c send_ad_order).  In mode 1000 that decodes to
 * a perfectly legitimate-looking 238.85 C — unlike mode 0x44c, where the
 * pipeline's 0x4000 gate rejects the filler outright — so the viewer has to
 * recognise it.  Real scenes are nowhere near this: this unit's room view
 * reads ~30 C, so a frame whose every sample sits within kFillerTol of
 * 238.85 C is the pre-bring-up output, not a measurement.  The window is
 * deliberately narrow; the filler->live transition only overshoots it by a
 * few degrees for a handful of frames. */
static const float kFillerC   = 32768.0f / 64.0f - 273.15f;   /* 238.85 */
static const float kFillerTol = 12.0f;

/* The vendor's six palettes, in the app's order (RE Docs 06 §1.4). */
static const char *const kPalFiles[DYT_PALETTE_BUILTIN_N] = {
    "01-iron-red.dat", "02-rainbow.dat",  "03-red-hot.dat",
    "04-black-hot.dat", "05-white-hot.dat", "06-cool-blue.dat"
};

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

/* ------------------------------------------------------------- shared frame */

/* The one thing that crosses the libuvc-thread / main-thread boundary. */
struct frame_shared {
    std::mutex         m;
    std::vector<float> temps;   /* pixels only — any 0x44c header stripped */
    int                w = 0, h = 0;
    long               frames = 0;
    bool               got = false;
};

struct viewer {
    frame_shared sh;

    dyt_palette_t pal[DYT_PALETTE_BUILTIN_N];
    bool          vendor[DYT_PALETTE_BUILTIN_N] = { false };  /* loaded from disk? */
    int           pal_id = 0;

    bool  auto_range = true;
    float lo = 0.f, hi = 0.f;   /* the range actually used for the last paint */

    int   zoom = 2;
    int   mouse_x = -1, mouse_y = -1;

    dyt_mode_t  mode = DYT_MODE_0;
    std::string note;           /* bring-up / waiting message */
};

/* ------------------------------------------------------------ frame callback */

/* Runs on libuvc's callback thread.  `n` floats, of which the last
 * width*active are pixels; mode 0x44c prefixes a 10-float header, so derive
 * the offset from the counts rather than assuming the mode. */
static void on_frame(const float *temps, int n, int width, int active, void *user)
{
    viewer *v = static_cast<viewer *>(user);
    int npix = width * active;
    int off  = n - npix;

    if (npix <= 0 || off < 0)
        return;

    std::lock_guard<std::mutex> lk(v->sh.m);
    if (static_cast<int>(v->sh.temps.size()) != npix)
        v->sh.temps.resize(npix);
    std::memcpy(v->sh.temps.data(), temps + off, sizeof(float) * npix);
    v->sh.w = width;
    v->sh.h = active;
    v->sh.frames++;
    v->sh.got = true;
}

/* Snapshot the shared buffer under the lock. */
static bool grab(viewer *v, std::vector<float> &t, int &w, int &h, long &frames)
{
    std::lock_guard<std::mutex> lk(v->sh.m);
    if (!v->sh.got || v->sh.temps.empty())
        return false;
    t      = v->sh.temps;
    w      = v->sh.w;
    h      = v->sh.h;
    frames = v->sh.frames;
    return true;
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

static const char *short_pal_name(int id)
{
    const char *n = dyt_palette_builtin_name(id);
    return n ? n : "?";
}

/* True while the frame is still the device's pre-bring-up output (see
 * kFillerC).  An all-NaN frame counts as filler too, so it is never mistaken
 * for a reading. */
static bool frame_is_filler(const std::vector<float> &t)
{
    for (float x : t) {
        if (x != x) continue;                       /* NaN: not a sample */
        if (std::fabs(x - kFillerC) > kFillerTol)
            return false;
    }
    return true;
}

/* Mean of the finite samples; false if there are none. */
static bool frame_mean(const std::vector<float> &t, float *out)
{
    double s = 0.0;
    int    n = 0;
    for (float x : t)
        if (x == x) { s += x; n++; }
    if (!n)
        return false;
    *out = static_cast<float>(s / n);
    return true;
}

/* A placeholder shown before the first frame, or while the device is still
 * streaming its pre-bring-up filler. */
static cv::Mat waiting_canvas(viewer *v, const char *msg)
{
    cv::Mat m(300, 460, CV_8UC3, cv::Scalar(24, 24, 24));
    put_text(m, "dytview", 20, 44, cv::Scalar(200, 200, 200), 0.9);
    put_text(m, msg, 20, 96, cv::Scalar(160, 200, 255), 0.55);
    if (!v->note.empty())
        put_text(m, v->note, 20, 132, cv::Scalar(170, 170, 170), 0.45);
    put_text(m, "keys: 1-6 palette   r range   s snapshot   q quit",
             20, 280, cv::Scalar(130, 130, 130), 0.45);
    return m;
}

/* Render one frame (plus its scale and readouts) into a canvas.
 * Returns the canvas; the caller owns it. */
static cv::Mat render(viewer *v)
{
    std::vector<float> t;
    int  w = 0, h = 0;
    long frames = 0;

    if (!grab(v, t, w, h, frames))
        return waiting_canvas(v, "waiting for frames...");

    /* Pre-bring-up filler: painting it as a 238.85 C scene would be a lie, and
     * its degenerate range would collapse the whole image to one colour. */
    if (frame_is_filler(t))
        return waiting_canvas(v, "warming up - waiting for live data...");

    const int npix = w * h;

    /* Range: auto fits every frame; locked holds whatever was last fitted
     * (or what --lo/--hi pinned at startup). */
    float flo = 0.f, fhi = 0.f;
    if (dyt_render_minmax(t.data(), npix, &flo, &fhi) != 0) {
        flo = 0.f;
        fhi = 1.f;
    }
    if (v->auto_range) {
        v->lo = flo;
        v->hi = fhi;
    }
    float lo = v->lo, hi = v->hi;
    if (!(hi > lo)) {           /* flat frame (e.g. the 0x8000 filler) */
        lo -= 1.f;
        hi += 1.f;
    }

    /* Temperature -> RGB, then to OpenCV's BGR. */
    std::vector<uint8_t> rgb(static_cast<size_t>(npix) * 3);
    dyt_render_rgb(t.data(), w, h, &v->pal[v->pal_id], lo, hi, rgb.data());

    cv::Mat img(h, w, CV_8UC3, rgb.data());
    cv::Mat bgr;
    cv::cvtColor(img, bgr, cv::COLOR_RGB2BGR);

    cv::Mat big;
    if (v->zoom > 1)
        cv::resize(bgr, big, cv::Size(), v->zoom, v->zoom, cv::INTER_NEAREST);
    else
        big = bgr;

    /* ---- canvas: image | gap | colour bar | labels ---- */
    const int barH = big.rows;
    const int x0   = big.cols + kBarGap;
    cv::Mat canvas(barH, x0 + kBarW + kLabelW, CV_8UC3, cv::Scalar(24, 24, 24));
    big.copyTo(canvas(cv::Rect(0, 0, big.cols, big.rows)));

    /* ---- colour bar: top = hottest, matching the palette's own order ---- */
    const dyt_palette_t *pal = &v->pal[v->pal_id];
    for (int j = 0; j < barH; j++) {
        float u   = barH > 1 ? 1.f - static_cast<float>(j) / (barH - 1) : 1.f;
        int   idx = static_cast<int>(u * (DYT_PALETTE_N - 1) + 0.5f);
        cv::rectangle(canvas, cv::Rect(x0, j, kBarW, 1), pal_bgr(pal, idx),
                      cv::FILLED);
    }
    cv::rectangle(canvas, cv::Rect(x0, 0, kBarW, barH),
                  cv::Scalar(200, 200, 200), 1);

    char buf[128];
    const int lx = x0 + kBarW + 4;
    std::snprintf(buf, sizeof buf, "%.2f C", hi);
    put_text(canvas, buf, lx, 12, cv::Scalar(255, 255, 255));
    std::snprintf(buf, sizeof buf, "%.2f C", (lo + hi) * 0.5f);
    put_text(canvas, buf, lx, barH / 2, cv::Scalar(210, 210, 210));
    std::snprintf(buf, sizeof buf, "%.2f C", lo);
    put_text(canvas, buf, lx, barH - 3, cv::Scalar(255, 255, 255));

    /* ---- hot/cold markers ---- */
    {
        int imin = -1, imax = -1;
        for (int k = 0; k < npix; k++) {
            float x = t[k];
            if (!(x == x)) continue;
            if (imin < 0 || x < t[imin]) imin = k;
            if (imax < 0 || x > t[imax]) imax = k;
        }
        auto mark = [&](int k, const cv::Scalar &col, const char *tag) {
            if (k < 0) return;
            int cx = (k % w) * v->zoom + v->zoom / 2;
            int cy = (k / w) * v->zoom + v->zoom / 2;
            cv::circle(canvas, cv::Point(cx, cy), 5, cv::Scalar(0, 0, 0), 2);
            cv::circle(canvas, cv::Point(cx, cy), 5, col, 1);
            char b[64];
            std::snprintf(b, sizeof b, "%s %.2f", tag, t[k]);
            put_text(canvas, b, cx + 8, cy - 6, col);
        };
        mark(imax, cv::Scalar(0, 0, 255), "H");   /* hottest: red */
        mark(imin, cv::Scalar(255, 120, 0), "L"); /* coldest: blue */
    }

    /* ---- hover readout: the temperature under the pointer ---- */
    if (v->mouse_x >= 0 && v->mouse_y >= 0 &&
        v->mouse_x < big.cols && v->mouse_y < big.rows) {
        int ix = v->mouse_x / v->zoom;
        int iy = v->mouse_y / v->zoom;
        if (ix >= 0 && ix < w && iy >= 0 && iy < h) {
            float x = t[iy * w + ix];
            char  b[64];
            if (x == x)
                std::snprintf(b, sizeof b, "%.2f C  (%d,%d)", x, ix, iy);
            else
                std::snprintf(b, sizeof b, "---  (%d,%d)", ix, iy);
            put_text(canvas, b, v->mouse_x + 10, v->mouse_y - 8,
                     cv::Scalar(255, 255, 255), 0.5);
            cv::line(canvas, cv::Point(v->mouse_x - 6, v->mouse_y),
                     cv::Point(v->mouse_x + 6, v->mouse_y),
                     cv::Scalar(0, 0, 0), 1);
            cv::line(canvas, cv::Point(v->mouse_x, v->mouse_y - 6),
                     cv::Point(v->mouse_x, v->mouse_y + 6),
                     cv::Scalar(0, 0, 0), 1);
        }
    }

    /* ---- status strip ---- */
    {
        cv::rectangle(canvas, cv::Rect(0, barH - kStatusH, big.cols, kStatusH),
                      cv::Scalar(16, 16, 16), cv::FILLED);
        std::snprintf(buf, sizeof buf,
                      "%s | %s | %s %.2f-%.2f C | %ld frames",
                      v->mode == DYT_MODE_44C  ? "mode 0x44c" :
                      v->mode == DYT_MODE_1000 ? "mode 1000" : "mode ?",
                      short_pal_name(v->pal_id),
                      v->auto_range ? "auto" : "lock", v->lo, v->hi, frames);
        put_text(canvas, buf, 6, barH - 6, cv::Scalar(220, 220, 220));
    }

    return canvas;
}

static void on_mouse(int event, int x, int y, int flags, void *user)
{
    (void)flags;
    viewer *v = static_cast<viewer *>(user);
    if (event == cv::EVENT_MOUSEMOVE || event == cv::EVENT_LBUTTONDOWN) {
        v->mouse_x = x;
        v->mouse_y = y;
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

/* Load the vendor ramps, preferring the real files in palettes/ over the
 * built-in approximations.  Records which came from disk so the status
 * line can be honest about it. */
static void load_palettes(viewer *v, const std::string &dir_opt)
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

    int loaded = 0;
    for (int i = 0; i < DYT_PALETTE_BUILTIN_N; i++) {
        for (size_t d = 0; d < dirs.size(); d++) {
            std::string path = dirs[d] + "/" + kPalFiles[i];
            if (dyt_palette_load(&v->pal[i], path.c_str()) == 0) {
                v->vendor[i] = true;
                loaded++;
                break;
            }
        }
        if (!v->vendor[i])
            dyt_palette_builtin(&v->pal[i], i);
    }

    std::fprintf(stderr, "dytview: %d/%d vendor palettes loaded%s\n", loaded,
                 DYT_PALETTE_BUILTIN_N,
                 loaded == DYT_PALETTE_BUILTIN_N ? "" :
                 " — using built-in approximations for the rest");
}

static std::string snapshot(viewer *v, const cv::Mat &canvas)
{
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);

    char path[128];
    std::snprintf(path, sizeof path, "dytview_%s.png", stamp);
    if (!cv::imwrite(path, canvas)) {
        std::fprintf(stderr, "dytview: cannot write %s\n", path);
        return "";
    }
    std::printf("saved %s\n", path);
    std::fflush(stdout);
    return path;
}

/* --------------------------------------------------------------------- main */

static void usage(const char *prog)
{
    std::fprintf(stderr,
        "usage: %s [options]\n"
        "  --start-orders        send setTinyCOutputADValue once streaming —\n"
        "                        required on 0bda:5840, which otherwise streams\n"
        "                        a flat 0x8000 placeholder (238.85 C)\n"
        "  --zoom N              integer upscale for the window (default 2)\n"
        "  --palette N           vendor ramp 1..6 (default 1 = iron red)\n"
        "  --lo C --hi C         lock the display range instead of auto-fitting\n"
        "  --palette-dir DIR     where the 0N-*.dat ramps live (default: search)\n"
        "  --png PATH            headless: wait for live data to settle, write\n"
        "                        one PNG, exit (no window)\n"
        "  --settle SEC          settle wait after the filler ends (default 12)\n"
        "  --timeout SEC         overall headless wait limit (default 40)\n"
        "  --frames N            quit after N painted frames\n"
        "  --vid 0xXXXX --pid 0xXXXX --format-index N\n"
        "  --width W --height H --fps F\n"
        "  --t-amb C --sensor-mode 0x82 --fix-mode 0x78\n"
        "\n"
        "keys: 1-6 palette · r auto/locked range · s snapshot · q or ESC quit\n",
        prog);
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
        if (!std::strcmp(a, "--start-orders"))
            o.send_start_orders = 1;
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

    if (zoom < 1) zoom = 1;
    if (palette < 1 || palette > DYT_PALETTE_BUILTIN_N) {
        std::fprintf(stderr, "dytview: --palette must be 1..%d\n",
                     DYT_PALETTE_BUILTIN_N);
        return 2;
    }
    v.zoom   = zoom;
    v.pal_id = palette - 1;

    load_palettes(&v, palette_dir);

    /* A pinned range must be complete to mean anything. */
    if (have_lo && have_hi && hi > lo) {
        v.auto_range = false;
        v.lo = lo;
        v.hi = hi;
    } else if (have_lo || have_hi) {
        std::fprintf(stderr, "dytview: --lo and --hi must be given together "
                            "and satisfy hi > lo; using auto range\n");
    }

    if (dyt_capture_open(&cap, &o) != 0)
        return 1;
    v.mode = dyt_capture_mode(cap);
    v.note = o.send_start_orders
                 ? "setTinyCOutputADValue sent — the sensor drifts for ~8 s "
                   "after it takes effect"
                 : "hint: 0bda:5840 needs --start-orders or it never leaves "
                   "the filler";

    /* Headless: stream, wait for the device to leave its pre-bring-up filler,
     * give the sensor a fixed settle, render one frame, save it, exit.  The
     * settle wait matters: once the AD-output order takes effect the sensor
     * drifts for ~8 s (this unit reads ~40 C and falls to ~30 C), and the
     * drift is asymptotic, so a "has it stopped moving?" test would never
     * converge — a fixed delay is both simpler and predictable.  No window is
     * ever created. */
    if (!png.empty()) {
        if (dyt_capture_start(cap, on_frame, &v) != 0) {
            dyt_capture_close(cap);
            return 1;
        }

        std::vector<float> t;
        int   w = 0, h = 0;
        long  n = 0;
        bool  live = false;
        double waited = 0.0;

        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(timeout_s);
        auto t_live   = deadline;      /* when the filler first ended */

        while (!g_stop && std::chrono::steady_clock::now() < deadline) {
            if (grab(&v, t, w, h, n) && !frame_is_filler(t)) {
                auto now = std::chrono::steady_clock::now();
                if (!live) {
                    live   = true;
                    t_live = now;
                    std::fprintf(stderr, "dytview: live data at frame %ld, "
                                         "settling %d s\n", n, settle_s);
                }
                if (now - t_live >= std::chrono::seconds(settle_s)) {
                    waited = std::chrono::duration<double>(now - t_live).count();
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        float mean = 0.f;
        bool  have_mean = grab(&v, t, w, h, n) && frame_mean(t, &mean);

        cv::Mat canvas = render(&v);
        bool ok = cv::imwrite(png, canvas);
        dyt_capture_stop(cap);
        dyt_capture_close(cap);

        if (!ok) {
            std::fprintf(stderr, "dytview: cannot write %s\n", png.c_str());
            return 1;
        }
        std::printf("%s: %ld frame(s)%s", png.c_str(), n,
                    live ? "" : " — WARNING: no live data, wrote the filler");
        if (live && have_mean)
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

    if (dyt_capture_start(cap, on_frame, &v) != 0) {
        dyt_capture_close(cap);
        return 1;
    }

    long painted = 0;
    for (;;) {
        cv::Mat canvas = render(&v);
        cv::imshow(kWin, canvas);

        int key = cv::waitKey(30) & 0xFF;
        if (key == 'q' || key == 27)
            break;
        if (key >= '1' && key <= '0' + DYT_PALETTE_BUILTIN_N) {
            v.pal_id = key - '1';
            std::printf("palette %d: %s%s\n", v.pal_id + 1,
                        short_pal_name(v.pal_id),
                        v.vendor[v.pal_id] ? "" : " (builtin)");
            std::fflush(stdout);
        } else if (key == 'r') {
            v.auto_range = !v.auto_range;
            std::printf("range: %s\n", v.auto_range ? "auto" : "locked");
            std::fflush(stdout);
        } else if (key == 's') {
            snapshot(&v, canvas);
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
    return 0;
}
