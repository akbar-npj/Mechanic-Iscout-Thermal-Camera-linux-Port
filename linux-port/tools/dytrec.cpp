/*
 * dytrec.cpp — record the port's rendered frames to mp4.
 *
 * Recording is muxing and codec I/O, not device logic, so it stays out of
 * libdyt: putting it in the library would drag OpenCV into everything that
 * links it and cost libdyt its no-dependency, `make check`-able property.  The
 * recorder is a *tool* that consumes dyt_session_render_rgb() — the same
 * frames the viewer draws.
 *
 * Two sources, one loop:
 *
 *   live     the device, via dyt_capture_* + the session_capture adapter
 *            (exactly the wiring dytview uses), then render.
 *   fixture  a frozen .raw replayed through the device-free pipeline
 *            (tools/frame_source.c), so a recording can be made and checked
 *            with no hardware attached.
 *
 * The codec is OpenCV's, reached through cv::VideoWriter.  That needs no new
 * dependency: opencv_videoio is already linked for the viewer, and its bundled
 * ffmpeg writes real H.264 mp4 on this host (verified — see the Phase 7 note in
 * RE Docs 09).  `--codec mp4v` is offered for players that dislike H.264.
 *
 * `--selftest` is the offline check the port's plan calls for: it replays a
 * fixture, asserts the frame count and that the frames converted to real
 * temperatures rather than the device's start-up filler, and encodes them so
 * the writer path is exercised too.  No device, no window.
 *
 * usage:  dytrec [options] OUTPUT.mp4     (see usage() below)
 * build:  via the Makefile (needs OpenCV; live mode additionally needs libusb)
 */
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "capture.h"
#include "frame_source.h"
#include "fusion.h"
#include "recorder.h"
#include "session.h"
#include "session_capture.h"

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

/* ------------------------------------------------------------------ config */

struct rec {
    std::string out;            /* the mp4 */
    std::string fixture;        /* non-empty -> offline */
    std::string palette_dir;
    std::string fusion;         /* pattern name or index */

    int   width = 0, height = 0;   /* sensor width (fixture and capture) */
    int   frames = -1;             /* -1 = not given; 0 = until signal */
    double fps = 25.0;             /* output frame rate */
    int   palette = 1;
    int   zoom = 1;
    int   timeout_s = 40;          /* live bring-up timeout */
    int   settle_s = 6;            /* live: settle after the filler clears */
    bool  have_lo = false, have_hi = false;
    float lo = 0.f, hi = 0.f;

    std::string codec = "avc1";
    bool        selftest = false;

    dyt_capture_opts cap;
};

/* --------------------------------------------------------------- utilities */

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

/* Same search dytview uses: an explicit --palette-dir wins, then the usual
 * places relative to the cwd and to the binary. */
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
    for (size_t i = 0; i < dirs.size(); i++)
        if (access(dirs[i].c_str(), R_OK) == 0)
            return dirs[i];
    return "";
}

/* Accept either the pattern's name ("ir", "edge", …) or its index. */
static bool fusion_from_name(const std::string &s, dyt_fusion_t *out)
{
    char *end = nullptr;
    long  v;

    for (int i = 0; i < DYT_FUSION_N; i++)
        if (s == dyt_fusion_name((dyt_fusion_t)i)) {
            *out = (dyt_fusion_t)i;
            return true;
        }
    v = std::strtol(s.c_str(), &end, 0);
    if (end && *end == '\0' && v >= 0 && v < DYT_FUSION_N) {
        *out = (dyt_fusion_t)v;
        return true;
    }
    return false;
}

/* Create the session and apply everything that is independent of the frame
 * source: palettes, range, fusion, zoom.  Shared by every mode so a recording
 * and a selftest cannot drift apart. */
static dyt_session_t *setup_session(const rec &r)
{
    dyt_session_t *sess = dyt_session_create();
    if (!sess) {
        std::fprintf(stderr, "dytrec: out of memory\n");
        return nullptr;
    }

    std::string dir = find_palette_dir(r.palette_dir);
    int palette_n = dyt_session_load_palettes(sess, dir.empty() ? nullptr
                                                                : dir.c_str());
    if (palette_n < 1) {
        std::fprintf(stderr, "dytrec: no palettes available\n");
        dyt_session_free(sess);
        return nullptr;
    }
    if (r.palette < 1 || r.palette > palette_n) {
        std::fprintf(stderr, "dytrec: --palette must be 1..%d\n", palette_n);
        dyt_session_free(sess);
        return nullptr;
    }
    dyt_session_set_palette(sess, r.palette - 1);

    if (r.have_lo && r.have_hi && r.hi > r.lo)
        dyt_session_set_fixed_range(sess, r.lo, r.hi);
    else if (r.have_lo || r.have_hi)
        std::fprintf(stderr, "dytrec: --lo and --hi must be given together and "
                             "satisfy hi > lo; using auto range\n");

    if (!r.fusion.empty()) {
        dyt_fusion_t f;
        if (!fusion_from_name(r.fusion, &f)) {
            std::fprintf(stderr, "dytrec: unknown --fusion '%s'; known:",
                         r.fusion.c_str());
            for (int i = 0; i < DYT_FUSION_N; i++)
                std::fprintf(stderr, " %s", dyt_fusion_name((dyt_fusion_t)i));
            std::fprintf(stderr, "\n");
            dyt_session_free(sess);
            return nullptr;
        }
        dyt_session_set_fusion(sess, f);
    }

    /* dyt_session_zoom() steps from DYT_ZOOM_MIN, so clamp first — the same
     * conversion dytview does. */
    int zoom = r.zoom;
    if (zoom < DYT_ZOOM_MIN) zoom = DYT_ZOOM_MIN;
    if (zoom > DYT_ZOOM_MAX) zoom = DYT_ZOOM_MAX;
    dyt_session_zoom(sess, zoom - DYT_ZOOM_MIN);

    return sess;
}

/* -------------------------------------------------------------- recording */

/* Encode `want` frames from `fs` into `out`.  The writer is opened from the
 * first frame's geometry, which is the only point where the size is known on
 * the live path — that and the RGB->BGR swap both live in tools/recorder.c,
 * shared with the Qt app so the two cannot produce different containers. */
static int record_frames(dyt_frame_source_t *fs, const rec &r)
{
    const uint8_t  *rgb = nullptr;
    int             ww = 0, hh = 0, written = 0;
    dyt_recorder_t *rec = nullptr;

    if (r.codec.size() != 4) {
        std::fprintf(stderr, "dytrec: --codec must be a 4-character fourcc\n");
        return -1;
    }
    if (!(r.fps > 0.0)) {
        std::fprintf(stderr, "dytrec: --fps must be positive\n");
        return -1;
    }

    rec = dyt_recorder_open(r.out.c_str(), r.fps, r.codec.c_str());
    if (!rec) {
        std::fprintf(stderr, "dytrec: cannot start a recorder for %s\n",
                     r.out.c_str());
        return -1;
    }

    auto t0 = std::chrono::steady_clock::now();

    while ((r.frames <= 0 || written < r.frames) && !g_stop) {
        dyt_fs_status_t st = dyt_frame_source_next(fs, &rgb, &ww, &hh);

        if (st == DYT_FS_WAIT) {
            /* Normal on the live path: the device has not delivered yet, or is
             * still streaming its start-up filler. */
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (st == DYT_FS_END)
            break;
        if (st == DYT_FS_ERROR) {
            dyt_recorder_close(rec);
            return -1;
        }

        if (dyt_recorder_write(rec, rgb, ww, hh) != 0) {
            dyt_recorder_close(rec);
            return -1;
        }
        written++;

        /* Pace to the requested rate.  Without this a fixture would be encoded
         * as fast as the CPU allows, and the mp4's duration would be a
         * function of the machine rather than of --fps. */
        auto due = t0 + std::chrono::duration_cast<
                            std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(written / r.fps));
        std::this_thread::sleep_until(due);
    }

    dyt_recorder_close(rec);

    std::fprintf(stderr, "dytrec: wrote %d frame(s)%s\n", written,
                 g_stop ? " (interrupted)" : "");
    return written > 0 ? 0 : -1;
}

/* ------------------------------------------------------------- selftest */

/* The offline check.  Replays the fixture, asserts the count and that the
 * frames are real temperatures, then encodes them so the writer path is
 * exercised too.  Returns 0 on success.
 *
 * One pass, not two: the recording loop *is* the counting loop.  Pulling the
 * frames twice would need two sources, and a source that has spent its frame
 * budget produces nothing on the second pass — which is exactly the mistake
 * this test made when it was written. */
static int selftest(const rec &r)
{
    const char   *fixture = r.fixture.empty()
                                ? "testdata/mode1000_256x384_default.raw"
                                : r.fixture.c_str();
    int           width    = r.width > 0 ? r.width : 256;
    int           frames   = r.frames > 0 ? r.frames : 12;
    int           failures = 0;
    int           got = 0, ntemp = 0, ww = 0, hh = 0;
    std::vector<float> temps;
    float         tmin = 0.f, tmax = 0.f;

    rec rt = r;
    rt.frames = frames;

    std::fprintf(stderr, "dytrec --selftest: %s (%dpx, mode 1000, "
                         "bottom-half), %d frames\n", fixture, width, frames);

    dyt_session_t *sess = setup_session(rt);
    if (!sess)
        return -1;

    dyt_frame_source_t *fs = dyt_frame_source_open_fixture(
        sess, fixture, width, DYT_MODE_1000, DYT_PLANE_BOTTOM_HALF,
        25.0f, 0x82, 0, frames);
    if (!fs) {
        dyt_session_free(sess);
        return -1;
    }

    /* Encode, then ask the source how many frames it actually produced. */
    int rc  = record_frames(fs, rt);
    got     = (int)dyt_frame_source_produced(fs);

    std::printf("  %-4s the source produced the requested %d frame(s) (got %d)\n",
                got == frames ? "ok" : "FAIL", frames, got);
    if (got != frames)
        failures++;

    std::printf("  %-4s the frames encode to %s\n",
                rc == 0 ? "ok" : "FAIL", rt.out.c_str());
    if (rc != 0)
        failures++;

    /* Prove the fixture actually converted: the device's start-up filler is a
     * flat 0x8000, which mode 1000 decodes to ~238.85 C.  The frozen frame is
     * ~31.4..32.4 C, so anything near the filler means the conversion was
     * skipped.  Also pins the render geometry. */
    {
        dyt_snapshot_t snap;
        if (dyt_session_snapshot(sess, &snap, nullptr, 0) == 0) {
            ww = snap.width;
            hh = snap.height;
        }
    }
    std::printf("  %-4s the rendered frame is 256x192 (got %dx%d)\n",
                (ww == 256 && hh == 192) ? "ok" : "FAIL", ww, hh);
    if (ww != 256 || hh != 192)
        failures++;

    temps.resize((size_t)ww * hh);
    ntemp = dyt_frame_source_temps(fs, temps.data(), (int)temps.size());
    if (ntemp > 0) {
        tmin = tmax = temps[0];
        for (int i = 1; i < ntemp; i++) {
            if (temps[i] < tmin) tmin = temps[i];
            if (temps[i] > tmax) tmax = temps[i];
        }
    }
    std::printf("  %-4s the frame converted to real temperatures "
                "(min %.2f C, max %.2f C, not the filler)\n",
                (ntemp > 0 && tmin < 100.f && tmax < 100.f) ? "ok" : "FAIL",
                tmin, tmax);
    if (!(ntemp > 0 && tmin < 100.f && tmax < 100.f))
        failures++;

    std::printf("%s: %d failure%s\n", failures ? "FAIL" : "PASS", failures,
                failures == 1 ? "" : "s");

    dyt_frame_source_close(fs);
    dyt_session_free(sess);
    return failures ? 1 : 0;
}

/* ------------------------------------------------------------------ usage */

static void usage(const char *prog)
{
    std::printf(
        "usage: %s [options] OUTPUT.mp4\n"
        "\n"
        "Records the port's rendered frames to mp4.\n"
        "\n"
        "source:\n"
        "  --fixture FILE.raw   replay a raw fixture instead of the device\n"
        "  --width N            sensor width (fixture; default 256)\n"
        "  --selftest           offline check over a fixture; writes only the\n"
        "                       temp mp4 below and no OUTPUT\n"
        "\n"
        "output:\n"
        "  --frames N           frames to record (default 250; 0 = until signal)\n"
        "  --fps F              output frame rate (default 25)\n"
        "  --codec CCCC         fourcc (default avc1/H.264; mp4v also works)\n"
        "\n"
        "display:\n"
        "  --palette N          palette index (default 1)\n"
        "  --palette-dir DIR    where the .dat palettes live\n"
        "  --lo C --hi C        fixed display range (both, hi > lo)\n"
        "  --fusion NAME|N      fusion pattern (default ir)\n"
        "  --zoom N             display zoom (default 1)\n"
        "\n"
        "live capture:\n"
        "  --ad-output          send the AD order (256x192 raw-AD mode)\n"
        "  --vid V --pid P      USB ids (default 0bda:5840)\n"
        "  --format-index N     UVC bFormatIndex (0 = auto)\n"
        "  --height N           capture height (0 = auto)\n"
        "  --timeout S          bring-up timeout (default 40)\n"
        "  --settle S           settle after the filler clears (default 6)\n"
        "\n"
        "  --help               this text\n",
        prog);
}

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    rec         r;
    std::string out;

    dyt_capture_opts_default(&r.cap);

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) {
            usage(argv[0]);
            return 0;
        }
        if (!std::strcmp(a, "--selftest"))
            r.selftest = true;
        else if (!std::strcmp(a, "--fixture") && i + 1 < argc)
            r.fixture = argv[++i];
        else if (!std::strcmp(a, "--palette-dir") && i + 1 < argc)
            r.palette_dir = argv[++i];
        else if (!std::strcmp(a, "--fusion") && i + 1 < argc)
            r.fusion = argv[++i];
        else if (!std::strcmp(a, "--codec") && i + 1 < argc)
            r.codec = argv[++i];
        else if (!std::strcmp(a, "--width") && i + 1 < argc) {
            r.width = std::atoi(argv[++i]);
            r.cap.width = r.width;
        } else if (!std::strcmp(a, "--height") && i + 1 < argc) {
            r.height = std::atoi(argv[++i]);
            r.cap.height = r.height;
        } else if (!std::strcmp(a, "--frames") && i + 1 < argc)
            r.frames = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--fps") && i + 1 < argc)
            r.fps = std::strtod(argv[++i], nullptr);
        else if (!std::strcmp(a, "--palette") && i + 1 < argc)
            r.palette = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--zoom") && i + 1 < argc)
            r.zoom = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--lo") && i + 1 < argc) {
            r.lo = std::strtof(argv[++i], nullptr); r.have_lo = true;
        } else if (!std::strcmp(a, "--hi") && i + 1 < argc) {
            r.hi = std::strtof(argv[++i], nullptr); r.have_hi = true;
        } else if (!std::strcmp(a, "--timeout") && i + 1 < argc)
            r.timeout_s = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--settle") && i + 1 < argc)
            r.settle_s = std::atoi(argv[++i]);
        else if (!std::strcmp(a, "--ad-output"))
            r.cap.output = DYT_OUTPUT_AD;
        else if (!std::strcmp(a, "--vid") && i + 1 < argc)
            r.cap.vid = static_cast<uint16_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (!std::strcmp(a, "--pid") && i + 1 < argc)
            r.cap.pid = static_cast<uint16_t>(std::strtoul(argv[++i], nullptr, 0));
        else if (!std::strcmp(a, "--format-index") && i + 1 < argc)
            r.cap.format_index = std::atoi(argv[++i]);
        else if (a[0] == '-' && a[1] == '-') {
            std::fprintf(stderr, "dytrec: unknown option %s\n", a);
            usage(argv[0]);
            return 2;
        } else if (out.empty())
            out = a;
        else {
            std::fprintf(stderr, "dytrec: unexpected argument %s\n", a);
            return 2;
        }
    }

    if (r.selftest) {
        rec st = r;
        /* Under build/ so `make clean` removes it and it never lands beside the
         * sources.  make check runs from the port root, which is also where the
         * default fixture path is relative to. */
        if (st.out.empty())
            st.out = "build/dytrec_selftest.mp4";
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        return selftest(st) == 0 ? 0 : 1;
    }

    if (out.empty()) {
        std::fprintf(stderr, "dytrec: an output file is required\n");
        usage(argv[0]);
        return 2;
    }
    r.out = out;

    /* A recording with no explicit length runs for a default 250 frames (10 s
     * at 25 fps); an explicit 0 means "until interrupted". */
    if (r.frames < 0)
        r.frames = 250;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    dyt_session_t *sess = setup_session(r);
    if (!sess)
        return 1;

    /* Offline: everything comes from the fixture, so no device is touched. */
    if (!r.fixture.empty()) {
        dyt_frame_source_t *fs = dyt_frame_source_open_fixture(
            sess, r.fixture.c_str(), r.width > 0 ? r.width : 256,
            DYT_MODE_1000, DYT_PLANE_BOTTOM_HALF, r.cap.t_amb,
            r.cap.sensor_mode, r.cap.fix_mode, 0);
        if (!fs) {
            dyt_session_free(sess);
            return 1;
        }
        int rc = record_frames(fs, r);
        dyt_frame_source_close(fs);
        dyt_session_free(sess);
        return rc == 0 ? 0 : 1;
    }

    /* Live.  Mirrors dytview's bring-up: open, read identity while the device
     * is still idle, then start the stream and let the adapter fill the
     * session. */
    dyt_capture_t *cap = nullptr;
    if (dyt_capture_open(&cap, &r.cap) != 0) {
        dyt_session_free(sess);
        return 1;
    }

    dyt_session_capture_t *sc = dyt_session_capture_create(sess);
    if (!sc || dyt_session_capture_set_capture(sc, cap) != 0) {
        std::fprintf(stderr, "dytrec: out of memory\n");
        dyt_capture_close(cap);
        dyt_session_free(sess);
        return 1;
    }

    dyt_device_info_t info;
    if (dyt_capture_read_info(cap, &info) == 0 && info.have_sn)
        std::fprintf(stderr, "dytrec: serial %s\n", info.sn_str);

    if (dyt_capture_start(cap, dyt_session_capture_on_frame, sc) != 0) {
        dyt_capture_close(cap);
        dyt_session_capture_free(sc);
        dyt_session_free(sess);
        return 1;
    }

    /* Wait out the start-up filler (~6 s), then let the sensor settle before
     * the first recorded frame — the same rule dytview's headless path uses,
     * and the same reason: the drift after the AD order is asymptotic, so a
     * "has it stopped?" test would never converge. */
    {
        dyt_snapshot_t snap;
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(r.timeout_s);
        bool live = false;
        auto t_live = deadline;

        std::fprintf(stderr, "dytrec: waiting for live data…\n");
        while (!g_stop && std::chrono::steady_clock::now() < deadline) {
            if (dyt_session_snapshot(sess, &snap, nullptr, 0) == 0 && snap.ready) {
                auto now = std::chrono::steady_clock::now();
                if (!live) {
                    live   = true;
                    t_live = now;
                    std::fprintf(stderr, "dytrec: live at frame %ld, settling "
                                         "%d s\n", snap.seq, r.settle_s);
                }
                if (now - t_live >= std::chrono::seconds(r.settle_s))
                    break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!live)
            std::fprintf(stderr, "dytrec: no live data before the timeout; "
                                 "recording whatever arrives\n");
    }

    dyt_frame_source_t *fs = dyt_frame_source_open_live(sess);
    int rc = fs ? record_frames(fs, r) : -1;
    if (fs)
        dyt_frame_source_close(fs);

    dyt_capture_stop(cap);
    dyt_capture_close(cap);
    dyt_session_capture_free(sc);
    dyt_session_free(sess);
    return rc == 0 ? 0 : 1;
}
