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
#include <dirent.h>     /* the selftest counts what a capture actually wrote */
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
/* The GL renderer is optional: without Qt6OpenGLWidgets the tab still works
 * through its QPainter fallback, so these headers — and the class that needs
 * them — are compiled only when the module was found (see the Makefile). */
#ifdef DYT_HAVE_QT6_OPENGL
#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QVector3D>
#endif
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygon>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSize>
#include <QSlider>
#include <QSpinBox>
#include <QString>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>

#include "capture.h"
#include "compare.h"
#include "frame_source.h"
#include "fusion.h"      /* dyt_fusion_clamp_align — the layout align's clamp */
#include "imgwrite.h"    /* dyt_write_png — the still's shareable half */
#include "mnn.h"         /* the optional super-resolution model */
#include "palette.h"
#include "session.h"
#include "session_capture.h"
#include "surface.h"     /* the 3D Analysis mesh + view transform */
#include "view_model.h"

#ifdef DYT_HAVE_OPENCV
#include "player.h"      /* the shared mp4 reader, the writer's mirror */
#include "recorder.h"    /* the shared mp4 writer, when OpenCV is present */
#endif

/* ---------------------------------------------------------------- layout */

/* The port's version, shown in the About box and named by the packages.  The
 * Makefile passes the version the package is built as (VERSION -> the
 * -DDYT_VERSION macro), so the About box and the packages cannot disagree; the
 * literal below is the fallback for a hand compile without the Makefile. */
#ifndef DYT_VERSION
#define DYT_VERSION "0.1.0"
#endif
static const char kAppVersion[] = DYT_VERSION;

/* The name a person reads: the window title, the About box, and the desktop
 * entry's Name — which packaging/check.sh ties back to this string.  The
 * binary and the package stay `dytqt`; only what the user sees is the product
 * name. */
static const char kAppName[] = "Mechanic iScout Thermal Camera";

static const int kBarW   = 22;   /* colour-bar width, px */
static const int kBarGap = 14;   /* image -> bar gap */
static const int kLabelW  = 96;  /* room for the bar's labels */
static const int kPad     = 8;

static const int kLineH   = 16;  /* one status line */
static const int kStripPad = 4;  /* the strip's own margin */
static const int kDotW    = 9;   /* the connection dot's diameter */
static const int kDotGap  = 6;   /* dot -> line 3's text */

/* The Mark tool's kinds.  The vendor's DRAW_CODE carries Label and Arrow
 * (CAAnalyzer.decompiled.cs:7159, :7169) beside the shape and point editors;
 * the port's Mark is the two *annotations*, which are the two with no engine
 * measurement behind them — nothing about a label is measured or written to
 * the device, so the whole feature lives in the front end.  A front-end enum
 * rather than a DYT_TOOL_*: the engine has no notion of a label, and adding
 * one it can never act on would be a lie in the session's state. */
enum MarkKind { MarkNone = 0, MarkText, MarkArrow };

/* How far a Mark's chip is kept inside the picture.  A label placed on the
 * last row or column would otherwise have its plate drawn half off the canvas
 * — unreadable, and unreachable by a drag — so the plate is pulled back from
 * the edge while its leader still points at the pixel it names. */
static const int kMarkInset = 4;

/* ------------------------------------------------------------------ theme
 *
 * The Windows counterpart (ThermalAnalysisSystem.exe; RE Docs 07 and the manual
 * rendered at /tmp/pdfx/w-08.png) is a dark blue-grey shell with a cyan canvas
 * border and white text.  One stylesheet reproduces it; the colour names below
 * are the ones the manual's screenshot reads as, so a future reader can match
 * them against the reference without grepping for hex. */
static const char kDarkQss[] =
    "QWidget { background: #1e1e1e; color: #e6e6e6; }"
    "QTabWidget::pane, QGroupBox, QFrame { "
        "background: #2b2b2b; }"
    "QGroupBox { border: 1px solid #3a3a3a; border-radius: 2px; "
        "margin-top: 10px; padding-top: 6px; }"
    "QGroupBox::title { subcontrol-origin: margin; left: 8px; "
        "color: #9aa0a6; }"
    "QTabBar::tab { background: #2b2b2b; color: #9aa0a6; "
        "padding: 6px 10px; font-size: 9px; border: 1px solid #3a3a3a; "
        "border-bottom: none; border-top-left-radius: 2px; "
        "border-top-right-radius: 2px; }"
    "QTabBar::tab:selected { background: #1e1e1e; color: #e6e6e6; "
        "border-color: #00b4d8; }"
    "QTabWidget::pane { border: 1px solid #3a3a3a; }"
    "QPushButton { background: #2b2b2b; color: #e6e6e6; border: 1px solid "
        "#3a3a3a; padding: 4px 10px; }"
    "QPushButton:checked, QPushButton:pressed { background: #003644; "
        "border-color: #00b4d8; }"
    "QPushButton#rail { border: none; border-radius: 0; text-align: center; "
        "padding: 6px 2px; font-size: 9px; }"
    "QPushButton#rail:checked { background: #003644; }"
    "QPushButton#row { text-align: left; padding: 4px 6px; font-size: 9px; }"
    "QPushButton#row:checked { background: #003644; border-color: #00b4d8; }"
    "QLineEdit, QSpinBox, QDoubleSpinBox { background: #2b2b2b; color: #e6e6e6; "
        "border: 1px solid #3a3a3a; padding: 2px; }"
    "QListWidget { background: #1e1e1e; color: #e6e6e6; border: 1px solid "
        "#3a3a3a; }"
    "QScrollBar:vertical { background: #1e1e1e; width: 10px; }"
    "QScrollBar::handle:vertical { background: #3a3a3a; border-radius: 4px; }"
    "QStatusBar { background: #2b2b2b; color: #9aa0a6; }"
    "QLabel#srstatus { color: #00b4d8; font-size: 9px; }"
    "QLabel#srhint { color: #7a8085; font-size: 8px; }"
    "QPushButton#setting { min-width: 64px; }"
    "QLabel#settingstatus { color: #9aa0a6; font-size: 9px; }"
    "QTextBrowser#contact { background: #2b2b2b; border: 1px solid #3a3a3a; }"
    "QFrame[frameShape=\"6\"] { border: 1px solid #00b4d8; }"  /* canvas border */
    ;

/* The rail's fixed width — narrow enough that the canvas keeps the room, wide
 * enough for a stacked 24-px icon and a 9-pt label ("Tutorials" is the longest
 * at ~45 px).  The Windows rail is a slim vertical strip; this matches it
 * without crowding the picture. */
static const int kRailW = 72;

/* The right panel's width.
 *
 * Sized for the tab bar, not for the rows: the Windows panel carries four
 * horizontal tabs (Troubleshoot | 3D Analysis | Comparison | Circuit Design)
 * and ours carries those plus Super Resolution, and a QTabWidget whose tabs do
 * not fit hides the overflow behind scroll arrows — a control the user cannot
 * reach.  Measured, with the tab style above: one tab wants 80 px, two 155,
 * three 231, four 316, five 415.  Five plus the pane's 2-px border is 417, so
 * 440 leaves ~5% for a different platform's font metrics.
 *
 * The vendor's own panel is ~400 px by the same measure, so this is close to
 * the reference rather than a departure from it. */
static const int kPanelW = 440;

/* ---------------------------------------------------------------- options */

struct opts {
    /* Defaulted here rather than in main() so no caller can forget: parse_args
     * fills cap from the command line, and the selftest constructs bare opts. */
    opts() { dyt_capture_opts_default(&cap); }

    std::string fixture = "testdata/mode1000_256x384_default.raw";
    std::string palette_dir;
    std::string png;                 /* save the canvas here, then exit */
    std::string shot;                /* save the whole window here, then exit */
    std::string capture_dir = ".";   /* where 's' and 'v' put their files */
    std::string model;               /* --model: the super-resolution model */
    std::string reference;           /* --reference: a saved still to diff against */
    std::string layout_image;        /* --layout-image: a board layout to overlay */
    int  width   = 256;
    int  palette = 1;
    int  zoom    = 2;
    int  unit    = -1;               /* -1 = leave the engine's default (C) */
    int  fusion  = -1;               /* -1 = leave the engine's default (ir) */
    int  sr      = -1;               /* -1 = leave the engine's default (off) */
    int  frames  = 0;                /* 0 = run until closed */
    int  fps     = 25;
    bool selftest = false;

    bool live        = false;        /* stream from the device, not a fixture */
    bool fixture_set = false;        /* --fixture was given explicitly */
    /* Which view options the command line actually named.  A saved preference
     * fills in only what it did not, so an explicit flag always wins. */
    bool palette_set     = false;
    bool zoom_set        = false;
    bool capture_dir_set = false;
    /* Preferences: `no_prefs` skips both loading and saving, and `prefs_path`
     * overrides where they live (empty = $DYT_PREFS, else Qt's default). */
    bool        no_prefs = false;
    std::string prefs_path;
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

/* ------------------------------------------------------------ preferences
 *
 * The view state a user expects to survive a restart: which palette, unit,
 * zoom and fusion they last chose, and where captures go.  QSettings is used
 * rather than a hand-rolled file because it already does the atomic write and
 * the XDG location.
 *
 * What is deliberately *not* saved is anything about the device — its identity
 * and stored parameters belong to the camera, not the user — or the
 * measurement, because a box drawn over one scene is about that scene.
 *
 * The path is overridable so a test and a portable run do not touch the real
 * config: `--prefs PATH` names a file, else `$DYT_PREFS`, else Qt's default.
 */
struct prefs {
    int         palette = -1;      /* -1 = unset, use the built-in default */
    int         unit    = -1;
    int         zoom    = -1;
    int         fusion  = -1;
    int         sr      = -1;
    std::string capture_dir;
};

static std::string prefs_path(const opts &o)
{
    if (!o.prefs_path.empty())
        return o.prefs_path;
    const char *e = std::getenv("DYT_PREFS");
    return e ? std::string(e) : std::string();
}

static std::unique_ptr<QSettings> prefs_open(const std::string &path)
{
    if (path.empty())
        return std::unique_ptr<QSettings>(new QSettings());
    return std::unique_ptr<QSettings>(
        new QSettings(QString::fromStdString(path), QSettings::IniFormat));
}

static void prefs_load(const std::string &path, prefs &p)
{
    auto s = prefs_open(path);
    p.palette = s->value(QStringLiteral("view/palette"), -1).toInt();
    p.unit    = s->value(QStringLiteral("view/unit"),    -1).toInt();
    p.zoom    = s->value(QStringLiteral("view/zoom"),    -1).toInt();
    p.fusion  = s->value(QStringLiteral("view/fusion"),  -1).toInt();
    p.sr      = s->value(QStringLiteral("view/sr"),      -1).toInt();
    p.capture_dir =
        s->value(QStringLiteral("capture/dir")).toString().toStdString();
}

static void prefs_save(const std::string &path, const prefs &p)
{
    auto s = prefs_open(path);
    s->setValue(QStringLiteral("view/palette"), p.palette);
    s->setValue(QStringLiteral("view/unit"),    p.unit);
    s->setValue(QStringLiteral("view/zoom"),    p.zoom);
    s->setValue(QStringLiteral("view/fusion"),  p.fusion);
    s->setValue(QStringLiteral("view/sr"),      p.sr);
    s->setValue(QStringLiteral("capture/dir"),
                QString::fromStdString(p.capture_dir));
    s->sync();
}

/* Fill in what the command line did not name, so an explicit flag always wins
 * over a saved preference.  Mutates a *copy* of the options; the caller's stay
 * as parsed, which is what --selftest asserts on. */
static void opts_apply_prefs(opts &eff, const prefs &p)
{
    if (!eff.palette_set && p.palette >= 0)
        eff.palette = p.palette + 1;         /* prefs are 0-based, CLI 1-based */
    if (!eff.zoom_set && p.zoom >= 0)
        eff.zoom = p.zoom;
    if (!eff.capture_dir_set && !p.capture_dir.empty())
        eff.capture_dir = p.capture_dir;
    if (eff.unit < 0)
        eff.unit = p.unit;
    if (eff.fusion < 0)
        eff.fusion = p.fusion;
    if (eff.sr < 0)
        eff.sr = p.sr;
}

/* The preferences as the session currently stands, ready to be saved. */
static prefs prefs_from_session(dyt_session_t *sess, const std::string &dir)
{
    prefs          p;
    dyt_snapshot_t snap{};

    if (sess && dyt_session_snapshot(sess, &snap, nullptr, 0) == 0) {
        p.palette = snap.palette;
        p.unit    = (int)snap.unit;
        p.zoom    = snap.xform.zoom;
        p.fusion  = (int)snap.fusion;
        p.sr      = (int)snap.sr;
    }
    p.capture_dir = dir;
    return p;
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
        "  --unit N         0=C, 1=F, 2=K (default C)\n"
        "  --fusion N       fusion pattern index (default 0 = ir)\n"
        "  --model PATH     super-resolution model (default: search for\n"
        "                   zoom2.mnn in the tree or beside the installed app)\n"
        "  --reference PATH a saved still (.dyt.jpg) to diff against in the\n"
        "                   Comparison tab (default: none)\n"
        "  --layout-image P a board layout image to overlay in the Circuit\n"
        "                   Design tab (default: none)\n"
        "  --sr MODE        super-resolution: off|visible|thermal (default off)\n"
        "  --frames N       stop after N frames (default: run until closed)\n"
        "  --fps N          timer rate (default 25)\n"
        "  --png PATH       write the canvas here and exit\n"
        "  --shot PATH      write the whole window here and exit (the shell,\n"
        "                   rail and panel included — use --png for the picture\n"
        "                   alone).  Either implies --frames 1 unless one is\n"
        "                   given, so the app exits by itself\n"
        "  --capture-dir D  where 's' (still) and 'v' (clip) write, and the\n"
        "                   folder the gallery lists (default: the Pictures\n"
        "                   folder, else $HOME; a saved preference wins)\n"
        "  --prefs PATH     preferences file (default: $DYT_PREFS, else the\n"
        "                   standard config location)\n"
        "  --no-prefs       do not load or save preferences\n"
        "  --selftest       headless check over the fixture; needs no display\n"
        "\n"
        "live capture (replaces the fixture):\n"
        "  --live           stream from the camera instead of replaying a file;\n"
        "                   automatic when the default fixture is not present\n"
        "  --vid V --pid P  USB ids (0x0000 0x0000 = first matching device)\n"
        "  --format-index N UVC bFormatIndex (0 = auto, uncompressed 16-bpp)\n"
        "  --height N       capture height (0 = auto)\n"
        "  --t-amb C        LUT ambient for the live path (default 25.0)\n"
        "  --ad-output      send setTinyCOutputADValue and read the flat\n"
        "                   256x192 raw-AD frame; default is the device's own\n"
        "                   256x384 dual-half frame, which needs no order\n"
        "\n"
        "with --live the window reconnects on its own; press r to retry now\n"
        "\n"
        "in the window: F11 is full screen; the left rail's Tutorials opens the\n"
        "full key guide, and its Setting opens the parameters and the view\n"
        "options.  Everything the keys do is on the rail or in the right panel,\n"
        "and quit is the window's close button or 'q'\n",
        prog, opts{}.fixture.c_str());
}

/* "off" / "visible" / "thermal" -> the engine's mode, or -1 when the word is
 * not a mode.  The names are dyt_sr_name()'s, so the flag, the status line and
 * the About box all say the same words. */
static int sr_mode_arg(const char *s)
{
    if (!std::strcmp(s, "off"))     return DYT_SR_OFF;
    if (!std::strcmp(s, "visible")) return DYT_SR_VISIBLE;
    if (!std::strcmp(s, "thermal")) return DYT_SR_THERMAL;
    return -1;
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
        else if (a == "--shot")                    { const char *v = next("--shot"); if (!v) return false; o.shot = v; }
        else if (a == "--capture-dir")             { const char *v = next("--capture-dir"); if (!v) return false; o.capture_dir = v; o.capture_dir_set = true; }
        else if (a == "--prefs")                   { const char *v = next("--prefs"); if (!v) return false; o.prefs_path = v; }
        else if (a == "--no-prefs")                o.no_prefs = true;
        /* --width is the sensor width in both modes: the fixture's, and the
         * capture width override when live.  One flag, because a user who says
         * "the sensor is 384 wide" means it whichever source they picked. */
        else if (a == "--width")                   { const char *v = next("--width"); if (!v) return false; o.width = std::atoi(v); o.cap.width = o.width; }
        else if (a == "--height")                  { const char *v = next("--height"); if (!v) return false; o.cap.height = std::atoi(v); }
        else if (a == "--t-amb")                   { const char *v = next("--t-amb"); if (!v) return false; o.cap.t_amb = std::strtof(v, nullptr); }
        else if (a == "--format-index")            { const char *v = next("--format-index"); if (!v) return false; o.cap.format_index = std::atoi(v); }
        else if (a == "--vid")                     { const char *v = next("--vid"); if (!v) return false; o.cap.vid = (uint16_t)std::strtoul(v, nullptr, 0); }
        else if (a == "--pid")                     { const char *v = next("--pid"); if (!v) return false; o.cap.pid = (uint16_t)std::strtoul(v, nullptr, 0); }
        else if (a == "--palette")                 { const char *v = next("--palette"); if (!v) return false; o.palette = std::atoi(v); o.palette_set = true; }
        else if (a == "--zoom")                    { const char *v = next("--zoom"); if (!v) return false; o.zoom = std::atoi(v); o.zoom_set = true; }
        else if (a == "--unit")                    { const char *v = next("--unit"); if (!v) return false; o.unit = std::atoi(v); }
        else if (a == "--fusion")                  { const char *v = next("--fusion"); if (!v) return false; o.fusion = std::atoi(v); }
        else if (a == "--model")                   { const char *v = next("--model"); if (!v) return false; o.model = v; }
        else if (a == "--reference")               { const char *v = next("--reference"); if (!v) return false; o.reference = v; }
        else if (a == "--layout-image")            { const char *v = next("--layout-image"); if (!v) return false; o.layout_image = v; }
        else if (a == "--sr") {
            const char *v = next("--sr");
            if (!v) return false;
            o.sr = sr_mode_arg(v);
            if (o.sr < 0) {
                std::fprintf(stderr, "dytqt: --sr must be off, visible or "
                                     "thermal (got '%s')\n", v);
                return false;
            }
        }
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

    /* A unit or fusion pattern that came from the command line or a saved
     * preference; -1 means "leave the engine's default". */
    if (o.unit >= 0)
        dyt_session_set_unit(sess, (dyt_unit_t)o.unit);
    if (o.fusion >= 0)
        dyt_session_set_fusion(sess, (dyt_fusion_t)o.fusion);

    return sess;
}

/* Install the optional super-resolution upscaler, then apply the mode the
 * command line or a saved preference asked for.
 *
 * Deliberately *not* part of setup_session(): that function is "the view state
 * from the options" and is also used for the throwaway session that re-renders
 * a saved still, where an upscaler would be pointless and the start-up message
 * would be printed on every open.  This is a front-end capability step, so the
 * two callers that want it (run_gui and --selftest) ask for it.
 *
 * It is optional at every layer — this build may have no MNN runtime, and the
 * model may not be installed — so both failures are reported rather than
 * silently ignored, and either way the session simply has no upscaler behind
 * it.  That is what makes the SR keys refuse instead of pretending, and it is
 * what the About box reports. */
static void setup_super_resolution(dyt_session_t *sess, const opts &o)
{
    const char *opt = o.model.empty() ? nullptr : o.model.c_str();
    char        model[4096];

    /* An explicit --model that cannot be read is refused here rather than
     * falling through to the search.  dyt_vm_find_model() does fall through —
     * it mirrors the palette-directory search, where a directory of
     * interchangeable ramps makes that the right answer — but a model is not
     * interchangeable: loading a different one than the user named would make
     * the flag a lie.  The refusal belongs here, where the flag is known. */
    if (opt && access(opt, R_OK) != 0) {
        std::fprintf(stderr, "dytqt: --model %s is not readable; no "
                             "super-resolution model loaded\n", opt);
        return;
    }

    if (dyt_vm_find_model(opt, model, sizeof model)) {
        if (dyt_mnn_load(model) == 0) {
            dyt_session_set_sr_upscaler(sess, dyt_mnn_zoom2);
            std::fprintf(stderr, "dytqt: super-resolution model %s\n", model);
        } else {
            std::fprintf(stderr, "dytqt: cannot load the model at %s\n", model);
        }
    } else {
        std::fprintf(stderr, "dytqt: no super-resolution model found\n");
    }

    /* A mode with no upscaler behind it is refused by the session, so this is
     * safe to apply either way. */
    if (o.sr >= 0)
        dyt_session_set_sr(sess, (dyt_sr_t)o.sr);
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

/* The connection dot's colour, one per state.  The Windows app's status bar
 * carries a green dot before its "Camera connected" word, and the port's is the
 * same idea with the states it actually has:
 *
 *   green  Live      — up and streaming
 *   amber  Connecting / WarmingUp / Stalled — up but not delivering (or not
 *          yet), so the dot says "not healthy" without claiming "not there"
 *   red    NoDevice  — the device is not there at all
 *   grey   Fixture   — no device is expected; a replay is not a connection
 *
 * The grey matters: painting the fixture path green would claim a camera that
 * is not attached, which is exactly the lie the state word already avoids. */
static QColor conn_colour(DevState s)
{
    switch (s) {
    case DevState::Live:       return QColor(0x2e, 0xcc, 0x40);  /* green */
    case DevState::Connecting:
    case DevState::WarmingUp:
    case DevState::Stalled:    return QColor(0xe0, 0xa0, 0x20);  /* amber */
    case DevState::NoDevice:   return QColor(0xd0, 0x30, 0x30);  /* red */
    case DevState::Fixture:    break;
    }
    return QColor(0x70, 0x78, 0x88);                             /* grey */
}

/* The third status line, and the front end's own.
 *
 * The two lines above it are the view model's, verbatim; this one carries the
 * things the view model has no business naming — the range mode, the device
 * state, the camera's serial number and the frame rate.  The range mode is
 * formatted by the engine (dyt_range_mode_name) but deliberately *not* folded
 * into dyt_vm_status_line().  That shared line is already ~378 px of a 404 px
 * window at zoom 1, and dytview draws it into a strip only as wide as the
 * image, where it is already clipped; extending it would clip further and
 * would change another front end's display for no gain.  See gui/README.md.
 *
 * The rate is only shown for the states that have painted frames: a "0.0 fps"
 * beside CONNECTING would be a claim about a stream that is not running yet.
 * STALLED is included on purpose — there the 0.0 *is* the evidence, and hiding
 * it would leave the frozen picture looking healthy.
 *
 * `sn` is the module serial from the bring-up read, or NULL when there is no
 * device (the fixture path) or the read failed — in which case the segment is
 * omitted rather than shown empty, so a blank field cannot be mistaken for a
 * serial of spaces.  The Windows app's own status bar carries exactly this
 * pair — a connection word and "Camera SN: <serial>" — and the serial is the
 * *module* serial (dyt_sn_str's output), not the decoded user serial, which
 * the info panel shows on its own row. */
static QString state_line(DevState s, const dyt_snapshot_t &snap, double fps,
                          const char *sn)
{
    QString out = QStringLiteral("range %1   |   %2")
                      .arg(QString::fromUtf8(dyt_range_mode_name(snap.range_mode)),
                           QString::fromUtf8(state_label(s)));
    if (sn && *sn)
        out += QStringLiteral("   Camera SN: %1").arg(QString::fromUtf8(sn));
    if (s == DevState::Fixture || s == DevState::Live ||
        s == DevState::Stalled)
        out += QStringLiteral("  %1 fps").arg(fps, 0, 'f', 1);
    return out;
}

/* ------------------------------------------------------------ the transform
 *
 * dyt_view_transform_map() states the contract (display.h:146): "the output is
 * the source rotated clockwise, magnified by `zoom` and then mirrored".
 * Transcribing it in that order is what keeps this a direct expression of the
 * contract the pointer mapping inverts — and that is the property that matters,
 * because a front end that scaled, turned or mirrored differently would put a
 * click on the wrong pixel.
 *
 * (The magnification and the mirror happen to *commute* with each other for
 * uniform integer magnification: mirroring a z-times block-magnified image and
 * magnifying a mirrored source both send output pixel ox to source n-1-ox/z.  So
 * this is not a fix for odd zoom; it is refusing to depend on that coincidence.
 * The rotation does not commute with either — which is why it goes first, in
 * source space, and why map() undoes it last.  Assertion 16 pins the mirror
 * actually happening, and 53g the rotation.)
 *
 * Qt::FastTransformation is mandatory rather than a performance choice: it is
 * Qt's nearest-neighbour, matching map()'s integer division and the OpenCV
 * viewer's cv::INTER_NEAREST.  Smooth scaling here would put the Qt window's
 * pixels somewhere the shared pointer mapping does not agree with. */
static QImage transformed(const QImage &src, const dyt_view_transform_t &t)
{
    /* Own the pixels from the start: `src` wraps the engine's buffer, which is
     * only valid until the next dyt_frame_source_next(), and a branch below
     * that did nothing would otherwise hand that buffer straight back. */
    QImage out = src.copy();

    /* A quarter turn first, in source space.  QTransform::rotate() turns
     * clockwise for a positive angle in Qt's y-down coordinates, which is the
     * direction dyt_view_transform_rotate() counts in. */
    if (t.rot)
        out = out.transformed(QTransform().rotate(t.rot),
                              Qt::FastTransformation);

    if (t.zoom > 1)
        out = out.scaled(out.width() * t.zoom, out.height() * t.zoom,
                         Qt::IgnoreAspectRatio, Qt::FastTransformation);

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
 * These are the reference viewer's bindings: one key per tool ("o" is the
 * polygon), "n" also forgetting the placed points, "a" arming the derived band
 * or disarming, "i" toggling the isotherm.  The polygon adds two gestures of
 * its own — Enter closes the outline and Backspace undoes a vertex — which are
 * answered only while the polygon tool is up, so neither steals a key from
 * anywhere else.  The view keys (palette, unit, range, flip, zoom, fusion) are
 * deliberately not here; they are later tasks.  The runtime-parameter ladder is
 * a sibling (FrameView::param_key), because its keys are case-sensitive and
 * this function's are not.  The letters are free of the device keys: retry is
 * lowercase "r" and the panel toggle is "d". */
static int apply_measure_key(dyt_session_t *sess, int key, float alarm_setpoint)
{
    dyt_snapshot_t s;

    if (!sess)
        return 0;

    /* The alarm and isotherm keys both need the current state: whether the
     * alarm is already armed (this key is a toggle) and whether the isotherm is
     * on.  The alarm's *threshold* is not among them — it comes from the
     * panel's field, handed in by the caller. */
    if (key == 'a' || key == 'i') {
        if (dyt_session_snapshot(sess, &s, nullptr, 0) != 0)
            return 1;                 /* consumed; there is no frame yet */
    }

    switch (key) {
    case 'p': dyt_session_set_tool(sess, DYT_TOOL_POINT);   return 1;
    case 'l': dyt_session_set_tool(sess, DYT_TOOL_LINE);    return 1;
    case 'b': dyt_session_set_tool(sess, DYT_TOOL_BOX);     return 1;
    case 'o': dyt_session_set_tool(sess, DYT_TOOL_POLYGON); return 1;
    case 'n':
        /* One key per tool, and "n" also forgets the placed points so the
         * next tool starts clean. */
        dyt_session_set_tool(sess, DYT_TOOL_NONE);
        dyt_session_clear_points(sess);
        return 1;
    case 'a':
        /* The Windows panel's High TEMP. Alarm is a *threshold*, not a band:
         * the user types the temperature in the field beside the row and the
         * alarm fires above it.  So arm the high side at the field's value and
         * put the low side on the clamp's floor, where nothing a sensor can
         * report will ever reach it — that is what makes this a high alarm and
         * not a window alarm.  The hysteresis is the port's own addition (the
         * vendor tests a bare `>`), so a scene sitting on the threshold does
         * not chatter. */
        if (!s.alarm_on)
            dyt_session_set_alarm(sess, DYT_ALARM_SETPOINT_MIN,
                                  dyt_vm_alarm_clamp_setpoint(alarm_setpoint),
                                  DYT_ALARM_HYST_DEFAULT);
        else
            dyt_session_alarm_disable(sess);
        return 1;
    case 'i':
        dyt_session_set_isotherm(sess, !s.iso_on);
        return 1;
    }

    /* The polygon's own gestures.  Enter finishes the outline so the next
     * click starts a new one; Backspace takes the last vertex back, which is
     * the only way to correct a misclick without losing the whole shape.
     *
     * Both are ours only while the polygon tool is up.  Enter belongs to the
     * gallery otherwise, and Backspace must fall through to Qt rather than be
     * swallowed, so this answers 0 for every other tool. */
    if (key == 13 || key == Qt::Key_Return || key == Qt::Key_Enter ||
        key == 8 || key == Qt::Key_Backspace) {
        if (dyt_session_snapshot(sess, &s, nullptr, 0) != 0)
            return 0;                  /* no frame: not a key we own */
        if (s.tool != DYT_TOOL_POLYGON)
            return 0;
        if (key == 8 || key == Qt::Key_Backspace)
            dyt_session_polygon_undo(sess);
        else
            dyt_session_set_polygon_closed(sess, 1);
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

/* ------------------------------------------------- capture and recording
 *
 * The still writer and the disk guard are the view model's; what a *window*
 * adds is the state between keypresses — which directory, and the one clip
 * that can be running at a time.  That is all this is.
 *
 * Two decisions worth stating, because both could reasonably go the other way:
 *
 *  - A still writes *two* files: the DYT container, which keeps the device's
 *    raw payload so a vendor tool can re-render it, and a PNG of the same
 *    render, which is what anything else can actually display.  Both are the
 *    clean source-resolution render, not the zoomed canvas with its overlays,
 *    so the picture and the data always agree.
 *  - A clip records the frame *as displayed* — isotherm dimming included when
 *    it is on — because that is what a record button is understood to do.
 *
 * The disk guard is checked before a clip starts and then periodically while
 * one runs, because the two failures are different: refusing to start loses
 * nothing, while a clip that fills the disk has to be stopped and its frames
 * finalised rather than left as a truncated file.
 */

static const long long kRecStartMin  = 64LL << 20;  /* refuse to start below */
static const long long kRecStopMin   = 16LL << 20;  /* stop a running clip below */
static const int       kRecDiskEvery = 25;          /* frames between checks */

struct CaptureCtl {
    std::string dir = ".";       /* where stills and clips go */
    std::string path;            /* the clip being written, for the notices */
    bool        recording = false;
    long long   frames    = 0;
    std::chrono::steady_clock::time_point t0{};

#ifdef DYT_HAVE_OPENCV
    dyt_recorder_t *writer = nullptr;
#endif

    double elapsed_s() const
    {
        if (!recording)
            return 0.0;
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - t0).count();
    }

    /* The strip's recording indicator, empty when nothing is running. */
    std::string label() const
    {
        char b[64];

        if (!recording)
            return std::string();
        dyt_vm_rec_label(elapsed_s(), frames, b, sizeof b);
        return b;
    }

    /* Start a clip.  Returns false and fills `why` when it cannot. */
    bool start(double fps, std::string &why)
    {
        char      name[512];
        char      ts[32];
        long long free_b = -1;

        if (recording)
            return true;                     /* already running */

        if (dyt_vm_disk_room(dir.c_str(), kRecStartMin, &free_b) != 1) {
            char b[192];
            if (free_b < 0)
                snprintf(b, sizeof b, "cannot record: %s is not writable",
                         dir.c_str());
            else
                snprintf(b, sizeof b,
                         "cannot record: only %lld MB free, need %lld MB",
                         free_b >> 20, kRecStartMin >> 20);
            why = b;
            return false;
        }

        if (dyt_vm_timestamp(ts, sizeof ts) != 0 ||
            dyt_vm_capture_name(name, sizeof name, dir.c_str(), ts, "mp4") != 0) {
            why = "cannot record: no usable file name";
            return false;
        }

#ifdef DYT_HAVE_OPENCV
        writer = dyt_recorder_open(name, fps, "avc1");
        if (!writer) {
            why = "cannot record: the mp4 writer did not start";
            return false;
        }
        why = std::string("recording ") + name;
#else
        (void)name;
        why = "cannot record: built without OpenCV";
        return false;
#endif
        path      = name;
        recording = true;
        frames    = 0;
        t0        = std::chrono::steady_clock::now();
        return true;
    }

    /* Feed one displayed frame.  Returns false when the clip stopped itself
     * (the disk filled), with `why` set; true otherwise, recording or not. */
    bool feed(const uint8_t *rgb, int w, int h, std::string &why)
    {
        if (!recording)
            return true;

#ifdef DYT_HAVE_OPENCV
        if (dyt_recorder_write(writer, rgb, w, h) != 0) {
            stop();
            why = "recording stopped: the writer refused a frame";
            return false;
        }
#else
        (void)rgb;
        (void)w;
        (void)h;
#endif
        frames++;

        if (frames % kRecDiskEvery == 0) {
            long long free_b = -1;
            if (dyt_vm_disk_room(dir.c_str(), kRecStopMin, &free_b) == 0) {
                char b[192];
                snprintf(b, sizeof b,
                         "recording stopped: disk full (%lld MB free)",
                         free_b >> 20);
                stop();
                why = b;
                return false;
            }
        }
        return true;
    }

    /* Stop and finalise.  Returns the number of frames the clip holds. */
    long long stop()
    {
        const long long n = frames;

        if (!recording)
            return 0;
        recording = false;
#ifdef DYT_HAVE_OPENCV
        if (writer)
            dyt_recorder_close(writer);
        writer = nullptr;
#endif
        frames = 0;
        return n;
    }

    ~CaptureCtl() { stop(); }
};

/* --------------------------------------------------------- clip playback
 *
 * The clip being played, the mirror of CaptureCtl: where that one owns the
 * writer and is fed by the pump, this one owns the reader and feeds the canvas.
 * One clip at a time, and the state the badge and the keys read.
 *
 * Frames are pulled at the pump's per-tick hook rather than from the frame
 * path, so a clip keeps playing on the WAIT and no-frame ticks — which is
 * exactly when a live stream has nothing to show and a recorded one does.  It
 * also means browsing works with no camera attached, which is the whole point
 * of having saved the clip.
 *
 * It holds no Qt: advance() decodes into a plain RGB buffer and says whether
 * the frame is new, and the pump is what wraps it in a QImage.  That keeps the
 * reader's contract identical to the one tools/player.h documents and testable
 * without a window.
 */
static std::string base_name(const std::string &path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

struct PlaybackCtl {
    std::string name;             /* the file's base name, for the labels */
    bool        active = false;   /* a clip is open */
    bool        paused = false;
    long long   pos    = 0;       /* the last frame delivered, 0-based */
    long long   count  = 0;       /* the clip's frame count, 0 when unknown */
    std::vector<uint8_t> buf;     /* the frame just decoded, tightly-packed RGB */
    int         w = 0, h = 0;

#ifdef DYT_HAVE_OPENCV
    dyt_player_t *reader = nullptr;
#endif

    /* Line 3 while a clip is up.  Constant for the clip's life, so the line
     * does not churn; the moving position is the badge's job. */
    std::string view_label() const { return "viewing " + name; }

    /* The strip's badge — "playing <name> 12/50", or "paused" — and empty when
     * nothing is playing.  Its own badge rather than part of a line, for the
     * same reason the recording one is: it must stay visible while the
     * transient notices come and go. */
    std::string label() const
    {
        char b[300];

        if (!active)
            return std::string();
        if (count > 0)
            snprintf(b, sizeof b, "%s %s  %lld/%lld",
                     paused ? "paused" : "playing", name.c_str(), pos + 1,
                     count);
        else
            snprintf(b, sizeof b, "%s %s  frame %lld",
                     paused ? "paused" : "playing", name.c_str(), pos + 1);
        return b;
    }

    /* Open a clip to play.  Returns false and fills `why` when it cannot —
     * a still, a missing file, or a build with no OpenCV. */
    bool open(const std::string &path, std::string &why)
    {
        stop();                             /* one clip at a time */

#ifdef DYT_HAVE_OPENCV
        reader = dyt_player_open(path.c_str());
        if (!reader) {
            why = "cannot play " + base_name(path) + ": not a readable clip";
            return false;
        }
        if (dyt_player_size(reader, &w, &h) != 0 || w <= 0 || h <= 0) {
            dyt_player_close(reader);
            reader = nullptr;
            why = "cannot play " + base_name(path) + ": no frame size";
            return false;
        }
        buf.assign((size_t)w * (size_t)h * 3, 0);
        count = dyt_player_count(reader);
        name  = base_name(path);
        pos   = 0;
        paused = false;
        active = true;
        return true;
#else
        (void)path;
        why = "cannot play " + base_name(path) + ": built without OpenCV";
        return false;
#endif
    }

    /* Decode the next frame into `buf`.  Returns true when `buf` holds a fresh
     * frame, false when nothing new is shown (paused, or the clip failed and
     * was stopped).  A failure leaves its reason in `why`. */
    bool advance(std::string &why)
    {
        if (!active || paused)
            return false;

#ifdef DYT_HAVE_OPENCV
        if (dyt_player_next(reader, buf.data(), (int)buf.size(), &pos) != 0) {
            stop();
            why = "playback stopped: the clip could not be decoded";
            return false;
        }
        return true;
#else
        return false;
#endif
    }

    void stop()
    {
        active = false;
        paused = false;
        pos    = 0;
#ifdef DYT_HAVE_OPENCV
        if (reader)
            dyt_player_close(reader);
        reader = nullptr;
#endif
    }

    ~PlaybackCtl() { stop(); }
};

/* Write a still: the container and the PNG.  Returns true on success; on
 * failure `msg` says why. */
static bool save_still(dyt_session_t *sess, const std::string &dir,
                       std::string &msg)
{
    char           ts[32], dyt_path[512], png_path[512], why[256] = { 0 };
    dyt_snapshot_t snap;
    int            w = 0, h = 0;
    const char    *base;

    if (!sess) {
        msg = "still: no session";
        return false;
    }

    /* Render before writing anything, so a frame that cannot be rendered does
     * not leave half a still behind. */
    if (dyt_session_snapshot(sess, &snap, nullptr, 0) != 0 || !snap.ready) {
        msg = "still: no live frame yet";
        return false;
    }
    {
        /* Size from the factor the *next* render will use, which is what the
         * snapshot reports (session.h): with super-resolution on the PNG is 2x
         * the temperature plane, and a buffer sized from the native geometry
         * would make render_rgb() refuse with -2. */
        const int f = snap.xform.sr >= 1 ? snap.xform.sr : 1;
        std::vector<uint8_t> rgb((size_t)snap.width * f *
                                 (size_t)snap.height * f * 3);
        if (dyt_session_render_rgb(sess, rgb.data(), (int)rgb.size(), &w, &h)
                != 0) {
            msg = "still: render failed";
            return false;
        }
        if (dyt_vm_timestamp(ts, sizeof ts) != 0 ||
            dyt_vm_capture_name(dyt_path, sizeof dyt_path, dir.c_str(), ts,
                                "dyt.jpg") != 0 ||
            dyt_vm_capture_name(png_path, sizeof png_path, dir.c_str(), ts,
                                "png") != 0) {
            msg = "still: no usable file name";
            return false;
        }
        if (dyt_vm_write_still(sess, dyt_path, why, sizeof why) != 0) {
            msg = why[0] ? why : "still: the container was not written";
            return false;
        }
        if (dyt_write_png(png_path, rgb.data(), w, h) != 0) {
            msg = "still: the PNG was not written";
            return false;
        }
    }

    base = strrchr(dyt_path, '/');
    base = base ? base + 1 : dyt_path;
    msg  = std::string("saved ") + base + " + .png";
    return true;
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

    /* A saved still being viewed.  The pump keeps painting the live frame
     * underneath — live keeps streaming, the fixture keeps ticking — so
     * clearing this restores the stream with nothing to unwind.
     *
     * updateGeometry(), not just update(): sizeHint() is sized from the
     * override when one is set, so the layout has to re-ask for it — otherwise
     * fit_to_view() would resize the window against a stale hint and a clip
     * bigger than the live view would be cropped. */
    void set_override(const QImage &img)
    {
        override_ = img;
        updateGeometry();
        update();
    }

    /* ---- the circuit-layout overlay -------------------------------------
     *
     * The Windows "Circuit Design" tab composites a board layout over the
     * thermal picture, so a hot component can be read off the layout rather
     * than off a bare blob.  There is no gerber/CAD parser in scope: the
     * layout is an image the user supplies (a PNG), drawn over whichever
     * picture the canvas is showing at that picture's own size — the live
     * frame, or a saved still/clip the gallery put up.
     *
     * The align offset is in *drawn-image* pixels, not sensor pixels.  The
     * layout is scaled to the drawn picture, so an offset in sensor pixels
     * would silently change size when the user zoomed, which no other control
     * in the app does; in drawn pixels "X +5" always moves the layout five
     * pixels of the picture as it is shown, and the selftest can pin the
     * composite without a sensor.  The ±40 range is the vendor's fusion
     * alignment range (DYT_FUSION_ALIGN_MAX) and the clamp is
     * dyt_fusion_clamp_align(), so the two alignment controls cannot disagree
     * about what "in range" means — a shared limit, not a shared unit. */
    void set_layout(const QImage &img)
    {
        layout_ = img;
        update();
    }
    void clear_layout()
    {
        layout_ = QImage();
        update();
    }
    bool  has_layout() const { return !layout_.isNull(); }
    QSize layoutSize() const { return layout_.size(); }

    void set_layout_align(int dx, int dy)
    {
        layout_dx_ = dyt_fusion_clamp_align(dx);
        layout_dy_ = dyt_fusion_clamp_align(dy);
        update();
    }
    int layout_dx() const { return layout_dx_; }
    int layout_dy() const { return layout_dy_; }

    void set_layout_opacity(int pct)
    {
        layout_opacity_ = std::max(0, std::min(100, pct));
        update();
    }
    int layout_opacity() const { return layout_opacity_; }

    void set_layout_shown(bool on) { layout_shown_ = on; update(); }
    bool layout_shown() const { return layout_shown_; }

    /* Apply a measurement key.  Returns 1 if the key was ours, so the window
     * can fall through to whatever else it binds.  The snapshot is refreshed
     * so the overlay switches at once rather than at the next tick; only the
     * scalars are re-read, so this is cheap. */
    int measure_key(int k)
    {
        if (!apply_measure_key(sess_, k, alarm_setpoint_))
            return 0;
        /* One tool at a time: picking a measurement tool puts the Mark tool
         * away, exactly as the vendor's single `cursor_code` does.  The alarm
         * and isotherm keys are not tools, so they leave it alone. */
        if (k != 'a' && k != 'i')
            mark_mode_ = MarkNone;
        resnap();
        return 1;
    }

    /* The threshold the High TEMP. Alarm row arms at, in Celsius.  The panel
     * owns the field and pushes the value here (push_alarm, the same
     * panel-is-the-view split push_layout uses), and a change while the alarm
     * is armed re-arms it — so the number on screen and the number the session
     * compares against cannot drift apart.  Clamped on the way in, so every
     * path into this — the field, --selftest, a preference — meets the same
     * limit. */
    void set_alarm_setpoint(float c)
    {
        alarm_setpoint_ = dyt_vm_alarm_clamp_setpoint(c);
        if (sess_ && snap_.alarm_on) {
            dyt_session_set_alarm(sess_, DYT_ALARM_SETPOINT_MIN,
                                  alarm_setpoint_, DYT_ALARM_HYST_DEFAULT);
            resnap();
        }
    }
    float alarm_setpoint() const { return alarm_setpoint_; }

    /* Re-read the scalars — and the polygon outline — so an overlay change
     * shows at once rather than at the next tick.  The measurements are
     * recomputed with it, so this is for one-off gestures (a key, a
     * right-click), not for a drag. */
    void resnap()
    {
        if (sess_ && dyt_session_snapshot(sess_, &snap_, nullptr, 0) == 0)
            update();
    }

    /* Apply a view key — palette, unit, range, flip, zoom, fusion,
     * super-resolution.  The bindings are the view model's, so the two front
     * ends cannot disagree about which letter does what; the frame itself is
     * re-rendered by the pump on its next tick.  Returns 1 if the key was one
     * of these.
     *
     * A super-resolution key can be refused or take no effect — no model, a
     * frame the model cannot take — and the status line says nothing while the
     * mode is off, so a key that appeared to do nothing would be the only
     * feedback.  dyt_vm_sr_notice() builds the wording from the fresh snapshot
     * and reports 0 for every other key, so nothing here duplicates the
     * binding. */
    int view_key(int k)
    {
        if (!dyt_vm_view_key(sess_, k))
            return 0;
        if (sess_ && dyt_session_snapshot(sess_, &snap_, nullptr, 0) == 0) {
            char why[128];
            if (on_notice_ && dyt_vm_sr_notice(k, &snap_, why, sizeof why) == 1)
                on_notice_(why);
            update();
        }
        return 1;
    }

    /* Rapid Diagnostics: the Windows panel's button of that name (its icon is
     * the `fixed_temp` glyph, and it sits in the LUT group — the port's Image
     * Enhancement).  It reads the *frame's* own extremes and latches a fixed
     * window that brackets them with a margin, so a scene with poor contrast
     * gets a range that uses the whole palette without the user hunting for
     * one.  The arithmetic is dyt_vm_rapid_window()'s, so the vendor's
     * rounding is pinned by the view-model test and not re-derived here.
     *
     * Applied to the session directly, the way view_key() does: the view owns
     * the session, and the panel row reaches this through handle_key('F')
     * exactly as the keyboard does. */
    void rapid_diagnostics()
    {
        float lo = 0.0f, hi = 0.0f;

        if (!sess_ || dyt_session_snapshot(sess_, &snap_, nullptr, 0) != 0)
            return;
        if (dyt_vm_rapid_window(snap_.stats.lo, snap_.stats.hi,
                                &lo, &hi) != 0)
            return;
        dyt_session_set_fixed_range(sess_, lo, hi);
        resnap();
    }

    /* Put the display range into `m` and refresh the scalars.  Small, but it
     * is the seam the circuit modes need: they set the range from a policy
     * (dyt_vm_leak_effect) rather than from a key or a drag. */
    void set_range_mode(dyt_range_mode_t m)
    {
        if (!sess_)
            return;
        dyt_session_set_range_mode(sess_, m);
        resnap();
    }

    /* ---- the Mark annotations -------------------------------------------
     *
     * A mark is a source pixel and the text placed there, held in source
     * pixels so a zoom, a flip or a rotation carries it with the pixel it
     * names — the same rule the measurement overlay follows, and the vendor's
     * too (it stores `ShowToBit(location)` and draws at `rawPointToShow`).
     * The list is the front end's because nothing here is measured: there is
     * no engine call behind a label, which is why the tool is not a
     * DYT_TOOL_*.
     */
    struct MarkLabel {
        float   x = 0.0f, y = 0.0f;   /* source pixel */
        QString text;
    };

    /* The Mark tool, and the one-tool-at-a-time rule.  Picking a mark puts
     * the measurement tool away and vice versa (measure_key() does the other
     * half): the vendor has a single `cursor_code`, and a click that placed a
     * label *and* a point would be two tools at once. */
    void set_mark_mode(int m)
    {
        mark_mode_ = m;
        if (m != MarkNone && sess_) {
            dyt_session_set_tool(sess_, DYT_TOOL_NONE);
            resnap();
        }
        update();
    }
    int mark_mode() const { return mark_mode_; }

    /* Place a label at a source pixel.  Empty text is refused rather than
     * stored: a chip with nothing in it is a mark the user cannot read or
     * find again.  The pixel is clamped into the frame so a label can never
     * be stored somewhere project() would refuse to draw it. */
    bool add_mark(const QPointF &src, const QString &text)
    {
        if (text.isEmpty() || snap_.width <= 0 || snap_.height <= 0)
            return false;
        MarkLabel m;
        m.x = (float)std::max(0.0, std::min((double)snap_.width - 1,
                                            (double)src.x()));
        m.y = (float)std::max(0.0, std::min((double)snap_.height - 1,
                                            (double)src.y()));
        m.text = text;
        marks_.push_back(m);
        update();
        return true;
    }

    void clear_marks() { marks_.clear(); update(); }
    size_t mark_count() const { return marks_.size(); }
    const MarkLabel &mark_at(size_t i) const { return marks_[i]; }

    /* Asked for a label's text at a source pixel.  A callback rather than the
     * dialog inline, so --selftest can place a label without a modal window —
     * the same reason on_about_ is one.  A null callback, or an empty answer,
     * refuses the click. */
    std::function<QString(const QPointF &)> on_mark_text_;

    /* The widget position of a source pixel, and its inverse.  The inverse is
     * pointer()'s own mapping, factored out so a click that places a label
     * and a click that places a point cannot disagree about which pixel they
     * landed on. */
    bool widget_to_source(const QPointF &pos, QPointF *src) const
    {
        if (img_.isNull() || snap_.width <= 0 || snap_.height <= 0 || !src)
            return false;
        const double  s   = display_scale();
        const QPointF org = display_origin();
        const int lx = (int)std::floor((pos.x() - org.x()) / s - kPad);
        const int ly = (int)std::floor((pos.y() - org.y()) / s - kPad);
        int sx = 0, sy = 0;
        if (dyt_view_transform_map(&snap_.xform, snap_.width, snap_.height,
                                   img_.width(), img_.height(), lx, ly,
                                   &sx, &sy) != 0)
            return false;
        if (sx < 0 || sy < 0 || sx >= snap_.width || sy >= snap_.height)
            return false;
        *src = QPointF(sx, sy);
        return true;
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

    /* The hottest/coldest markers.  On by default, which is the reference
     * viewer's behaviour (it always marks the extremes); the Windows panel
     * exposes a Tracking switch, so this is what that switch drives.  A canvas
     * flag rather than a session one: it changes what is drawn, not what is
     * measured, and the frame's extremes are recomputed every frame anyway. */
    void toggle_hot()
    {
        show_hot_ = !show_hot_;
        update();
    }
    bool hot_shown() const { return show_hot_; }

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

    /* The device identity read at bring-up, so the Settings dialog can show
     * each parameter's *stored* value as its starting point.  The four
     * decoders return NaN for a slot the device did not answer, which is how
     * the dialog tells "the device says 0.95" from "the device did not say" —
     * the same distinction the info panel's row count makes. */
    const dyt_device_info_t &device_info() const { return info_; }

    /* The front end owns the device handle, so the write is reached through a
     * callback rather than a pointer kept here.  It returns 1 when the write
     * was accepted for sending. */
    std::function<bool(dyt_order_type_t, float)> on_param_send_;
    std::function<void(dyt_order_type_t)>        on_param_cancel_;

    /* A transient notice a view key produced — the super-resolution reasons,
     * which the status line cannot carry because it stays silent while the
     * mode is off.  Owned by the front end, like the parameter notices. */
    std::function<void(const std::string &)>     on_notice_;

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
        /* The override is what is actually drawn when one is set — a saved
         * still or a playing clip — so the canvas sizes to it, not to the live
         * frame underneath.  Without this a clip played with no camera
         * attached would be laid out against the placeholder's 256x192 and
         * scaled against the wrong natural size. */
        const QImage &shown = override_.isNull() ? img_ : override_;
        const int w = shown.isNull() ? 256 : shown.width();
        const int h = shown.isNull() ? 192 : shown.height();
        return QSize(kPad + w + kBarGap + kBarW + kLabelW + kPad, h + 2 * kPad);
    }

    /* ---- the display layer ------------------------------------------------
     *
     * The canvas has a *natural* size — the picture, the colour bar and the
     * labels, with the padding around them (sizeHint) — and it is drawn at
     * that size whenever the widget is no bigger.  When the widget is bigger,
     * which is what a full screen window or a hand-resized one is, the whole
     * canvas is scaled up and centred in it.
     *
     * This is deliberately *not* the engine's zoom.  That magnifies the
     * source, so the frame arrives here already zoom*sr times the sensor and
     * every measurement, marker and pointer mapping is done in those
     * coordinates.  Scaling here is a display transform applied after all of
     * that, so the mapping never sees it and a click stays exact — which is
     * why pointer() is its inverse and nothing else in this file knows about
     * it.
     *
     * Scaling by a real factor rather than an integer is the point: the sensor
     * is 256x384, so the engine's largest integer zoom still leaves most of a
     * 1080p screen empty.  Aspect ratio is preserved (one factor for both axes,
     * the smaller of the two ratios), so the picture cannot be stretched. */
    double display_scale() const
    {
        const QSize nat = sizeHint();
        if (nat.width() <= 0 || nat.height() <= 0)
            return 1.0;
        const double s = std::min((double)width()  / (double)nat.width(),
                                  (double)height() / (double)nat.height());
        /* 1.0 exactly at the natural size — not 0.99 from a rounding, which
         * would resample every pixel of the common case for nothing. */
        return s > 1.0 ? s : 1.0;
    }

    /* Where canvas-local (0,0) lands in the widget.  Floored to whole pixels:
     * at scale 1 a half-pixel offset would blur the picture and move the
     * overlay samples by one, and the canvas is centred often enough that the
     * odd width is not a corner case.  pointer() uses this same origin, so the
     * flooring cancels. */
    QPointF display_origin() const
    {
        const QSize  nat = sizeHint();
        const double s   = display_scale();
        return QPointF(std::floor((width()  - nat.width()  * s) / 2.0),
                       std::floor((height() - nat.height() * s) / 2.0));
    }

    /* The canvas drawn at its own size, in canvas coordinates — the same
     * content paintEvent draws, minus the widget-space fill, the placeholder
     * and the display transform.  --selftest samples the overlays through this
     * rather than through grab(), so an assertion is about what the canvas
     * paints and not about how big the window happens to be: once the icon rail
     * and the control panel take their columns, the window can be narrower than
     * the canvas on a small screen, and a grab would crop the very pixels the
     * assertions look at.  The transform itself is pinned separately (the
     * scaled-click assertion). */
    QImage render_canvas()
    {
        QImage im(sizeHint(), QImage::Format_RGB888);
        im.fill(QColor(16, 16, 16));
        QPainter p(&im);
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        if (img_.isNull() && override_.isNull()) {
            p.setPen(QColor(200, 200, 200));
            p.drawText(im.rect(), Qt::AlignCenter, placeholder_);
            draw_confirm(p);
            return im;
        }
        draw_content(p);
        return im;
    }

    /* ---- the colour bar's range handles --------------------------------
     *
     * The Windows panel's temperature bar carries a pair of arrows —
     * `panel_lut_max_but` at the top and `panel_lut_min_but` at the bottom
     * (CAAnalyzer.decompiled.cs:18577/18517) — that drag the *display window*
     * narrower or wider inside the bar's scale, with `label_lut_adjust_max`
     * and `label_lut_adjust_min` showing the values.  This is that pair.
     *
     * The view reports and the window sets: a drag calls on_bar_range_ with
     * the new window, and MainWindow turns it into a fixed range on the
     * session.  FrameView has no session to set a range on and no business
     * owning one — the same split push_layout and push_alarm take. */

    /* The scale the bar is drawn over: the frame's own extremes.  A frame with
     * no spread (or none yet) falls back to the display range, so the
     * fractions below stay finite rather than dividing by zero. */
    void bar_scale(float *lo, float *hi) const
    {
        if (snap_.stats.hi > snap_.stats.lo) {
            *lo = snap_.stats.lo;
            *hi = snap_.stats.hi;
        } else {
            *lo = snap_.lo;
            *hi = snap_.hi;
        }
    }

    /* The canvas y of the bar row a temperature sits on, and its widget-space
     * twin.  The widget one goes through the display transform, so a hit test
     * and the drawing cannot disagree about where the handle is. */
    int bar_row_canvas(float c) const
    {
        float bl = 0.f, bhi = 0.f;
        bar_scale(&bl, &bhi);
        const int h = img_.height();
        if (h <= 1)
            return kPad;
        const double u = (double)dyt_vm_bar_frac(c, bl, bhi);
        return kPad + (int)std::lround((1.0 - u) * (h - 1));
    }

    int bar_row_widget(float c) const
    {
        const double  s   = display_scale();
        const QPointF org = display_origin();
        return (int)std::lround(org.y() + s * bar_row_canvas(c));
    }

    /* The temperature a widget-space y on the bar means — the inverse of the
     * above, and what a dragged handle's new position reads back as. */
    float bar_temp_at_widget(int wy) const
    {
        float bl = 0.f, bhi = 0.f;
        bar_scale(&bl, &bhi);
        const double  s   = display_scale();
        const QPointF org = display_origin();
        const int     h   = img_.height() > 1 ? img_.height() - 1 : 1;
        const double  cy  = (wy - org.y()) / s - kPad;   /* canvas y */
        return dyt_vm_bar_temp((float)(1.0 - cy / h), bl, bhi);
    }

    /* The bar's column, in widget space, with room for the two triangles that
     * stick out past its ends.  Deliberately not widened sideways: the strip
     * to its left is the picture, and a measurement placed there must not be
     * stolen by a handle. */
    QRect bar_hit_rect_widget() const
    {
        const double  s   = display_scale();
        const QPointF org = display_origin();
        const int bx = (int)std::floor(org.x() + s * (kPad + img_.width()
                                                      + kBarGap));
        const int by = (int)std::floor(org.y() + s * kPad);
        const int bw = (int)std::ceil(s * kBarW);
        const int bh = (int)std::ceil(s * img_.height());
        return QRect(bx, by - 10, bw, bh + 20);
    }

    /* Which handle a widget-space point grabs — the window end whose row is
     * nearer — or -1 when the point is not on the bar at all. */
    int bar_handle_at(const QPoint &wp) const
    {
        if (img_.isNull() || snap_.width <= 0)
            return -1;
        if (!bar_hit_rect_widget().contains(wp))
            return -1;
        const int dy = std::abs(wp.y() - bar_row_widget(snap_.hi));
        const int dl = std::abs(wp.y() - bar_row_widget(snap_.lo));
        return dy <= dl ? 0 : 1;
    }

    /* Move the dragged handle to the pointer and report the new window.  The
     * two ends may not cross: a quarter of a degree of daylight is kept
     * between them, so the handles cannot be dragged onto one row and become
     * impossible to tell apart again. */
    void bar_drag_apply(const QPoint &wp)
    {
        const float v   = bar_temp_at_widget(wp.y());
        const float gap = 0.25f;
        float lo = snap_.lo, hi = snap_.hi;

        if (bar_drag_ == 0)
            hi = std::max(v, lo + gap);
        else
            lo = std::min(v, hi - gap);

        if (on_bar_range_)
            on_bar_range_(lo, hi);
        update();
    }

    /* The two handles, at the display window's ends inside the bar's scale.
     * A triangle just outside the bar points at the row it marks, in the
     * hot/cold colours the extremes already use, with the window's value
     * beside it — so a drag reads its own number without a second widget. */
    void draw_bar_handles(QPainter &p)
    {
        if (img_.isNull() || snap_.width <= 0 || snap_.height <= 0)
            return;

        const int bx = kPad + img_.width() + kBarGap;
        const int cx = bx + kBarW / 2;
        const int lx = bx + kBarW + 4;

        for (int which = 0; which < 2; which++) {
            const bool  hot = (which == 0);
            const float c   = hot ? snap_.hi : snap_.lo;
            const int   y   = bar_row_canvas(c);

            p.setPen(Qt::NoPen);
            p.setBrush(hot ? QColor(255, 90, 60) : QColor(90, 180, 255));

            QPolygon tri;
            if (hot)
                tri << QPoint(cx - 5, y - 8) << QPoint(cx + 5, y - 8)
                    << QPoint(cx, y - 1);
            else
                tri << QPoint(cx - 5, y + 8) << QPoint(cx + 5, y + 8)
                    << QPoint(cx, y + 1);
            p.drawPolygon(tri);

            char lbl[32];
            if (dyt_vm_temp(&snap_, c, lbl, sizeof lbl) != 0)
                continue;
            p.setPen(hot ? QColor(255, 150, 120) : QColor(150, 205, 255));
            p.drawText(lx, y + 4, QString::fromUtf8(lbl));
        }
    }

public:
    /* A drag on either handle: the new display window, in Celsius.  The window
     * owns the session, so it is what turns this into a fixed range. */
    std::function<void(float lo, float hi)> on_bar_range_;
    /* A double-click on the bar: hand the range back to the frame. */
    std::function<void()>                  on_bar_auto_;

    /* The bar's column in canvas coordinates, the canvas row a temperature's
     * handle sits on, and the canvas -> widget transform.  Exposed so
     * --selftest can aim a synthetic drag at the very pixel the canvas drew,
     * rather than re-deriving the layout constants and the display layer. */
    QRect bar_rect() const
    {
        if (img_.isNull())
            return QRect();
        return QRect(kPad + img_.width() + kBarGap, kPad, kBarW,
                     img_.height());
    }
    int bar_handle_row(float c) const { return bar_row_canvas(c); }
    QPoint canvas_to_widget(const QPointF &cp) const
    {
        const double  s   = display_scale();
        const QPointF org = display_origin();
        return QPoint((int)std::lround(org.x() + s * cp.x()),
                      (int)std::lround(org.y() + s * cp.y()));
    }

protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        /* The polygon's finish gesture.  Only the polygon has one, so every
         * other tool keeps the right button free — and an unhandled
         * right-click is passed on, which is what lets a future context menu
         * exist without this having to change. */
        if (e->button() == Qt::RightButton) {
            if (snap_.tool == DYT_TOOL_POLYGON) {
                dyt_session_set_polygon_closed(sess_, 1);
                resnap();
                e->accept();
                return;
            }
            e->ignore();
            return;
        }
        if (e->button() != Qt::LeftButton) {
            e->ignore();
            return;
        }
        /* The colour bar's range handles come first: the bar is not part of
         * the picture, so a press on it is never a measurement. */
        const int h = bar_handle_at(e->pos());
        if (h >= 0) {
            bar_drag_ = h;
            bar_drag_apply(e->pos());
            e->accept();
            return;
        }
        /* Then the Mark tool, which is the front end's own — a click in the
         * picture asks for the text and places it.  The vendor refuses a click
         * outside the image and puts the tool away (:10250), and so does this:
         * a label stored off the frame could never be drawn.  A placed label
         * disarms the tool (one label per click, as the vendor's
         * `cursor_code = None` does); a cancelled one leaves it armed so the
         * next click can try again. */
        if (mark_mode_ == MarkText) {
            QPointF src;
            if (!widget_to_source(e->position(), &src)) {
                set_mark_mode(MarkNone);
            } else {
                const QString t = on_mark_text_ ? on_mark_text_(src)
                                                : QString();
                if (add_mark(src, t))
                    set_mark_mode(MarkNone);
            }
            e->accept();
            return;
        }
        pointer(DYT_VM_MOUSE_DOWN, e->position());
        e->accept();
    }

    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (bar_drag_ >= 0) {
            bar_drag_apply(e->pos());
            e->accept();
            return;
        }
        pointer(DYT_VM_MOUSE_MOVE, e->position());
        e->accept();
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) {
            e->ignore();
            return;
        }
        if (bar_drag_ >= 0) {
            bar_drag_ = -1;
            e->accept();
            return;
        }
        pointer(DYT_VM_MOUSE_UP, e->position());
        e->accept();
    }

    /* A double-click on the bar hands the range back to the frame — the same
     * thing 't' does from the keyboard, so there is a way out of a window the
     * user has narrowed to nothing. */
    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && !img_.isNull() &&
            bar_hit_rect_widget().contains(e->pos())) {
            bar_drag_ = -1;
            if (on_bar_auto_)
                on_bar_auto_();
            e->accept();
            return;
        }
        QWidget::mouseDoubleClickEvent(e);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(16, 16, 16));

        /* The placeholder is the only thing left when there is neither a live
         * frame nor an override, and it is drawn in *widget* space — centred
         * in the window — which is why it comes before the transform below. */
        if (img_.isNull() && override_.isNull()) {
            p.setPen(QColor(200, 200, 200));
            p.drawText(rect(), Qt::AlignCenter, placeholder_);
            /* The parameter keys do not need a frame: once bring-up finishes
             * the device is open and a write can be armed over the
             * placeholder, so its confirmation must be visible here too. */
            draw_confirm(p);
            return;
        }

        /* Everything below draws in *canvas* coordinates — the ones this
         * widget used before the display layer existed, with the padding at
         * (kPad, kPad) — and the painter's transform is what puts them on the
         * screen.  At the natural size the origin is (0,0) and the scale is 1,
         * so this is pixel-for-pixel the drawing it always was; the fill above
         * and the placeholder above that stay in widget space, which is why
         * they are before this point.
         *
         * Nearest-neighbour, like the engine's own zoom: a thermal picture
         * magnified smoothly invents gradients that are not in the data, and a
         * measurement is read off the pixel it names. */
        const double s = display_scale();
        p.translate(display_origin());
        if (s != 1.0)
            p.scale(s, s);
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);

        draw_content(p);
    }

    /* Everything the canvas draws in *canvas* coordinates — the image, the
     * colour bar, the overlays.  Split out of paintEvent so --selftest can ask
     * for the same drawing in canvas space (render_canvas()), where an overlay
     * can be sampled at its canvas pixel no matter how the window is sized or
     * cropped.  The widget-space fill, the placeholder and the display
     * transform stay in paintEvent: they are about the widget, not the canvas. */
    void draw_content(QPainter &p)
    {
        const int x0 = kPad, y0 = kPad;

        /* A saved still or a playing clip takes the canvas.  It is drawn at
         * the source's own size, so no scaling is needed, and the confirm
         * overlay stays because the parameter keys are still live over it.
         *
         * This is checked before the live frame is drawn, so the override
         * covers it completely — and, with the check above, so it appears even
         * with no camera attached, which is exactly when a saved clip is the
         * only thing there is to look at. */
        if (!override_.isNull()) {
            p.drawImage(QPoint(x0, y0), override_);
            draw_layout(p);
            draw_confirm(p);
            return;
        }

        p.drawImage(QPoint(x0, y0), img_);

        /* The circuit layout sits over the picture and under everything else,
         * so the colour bar, the markers and the labels all stay on top. */
        draw_layout(p);

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

        /* The bar's end labels are the frame's own extremes, because that is
         * the range the bar is a *scale* over.  The Windows panel labels it the
         * same way — the outer pair beside the bar — and puts the display
         * window's values on the two arrows, so narrowing the window moves the
         * arrows without moving the ends. */
        float bl = 0.f, bhi = 0.f;
        bar_scale(&bl, &bhi);
        const int lx = bx + kBarW + 4;
        for (int i = 0; i < 3; i++) {
            /* An end label earns its place only once its handle has left the
             * end: at AUTO the window *is* the frame, so drawing both would
             * print the same number twice, one line apart.  The mid label has
             * no handle to collide with and is always drawn. */
            if (i == 0 && bar_row_canvas(snap_.hi) <= y0 + 2)
                continue;
            if (i == 2 && bar_row_canvas(snap_.lo) >= y0 + bh - 2)
                continue;

            const float c = (i == 0) ? bhi
                                     : (i == 1 ? (bl + bhi) * 0.5f : bl);
            char lbl[32];
            if (dyt_vm_temp(&snap_, c, lbl, sizeof lbl) != 0)
                continue;
            const int yy = (i == 1) ? y0 + bh / 2 : (i == 0 ? y0 + 12
                                                            : y0 + bh - 3);
            p.setPen(i == 1 ? QColor(170, 170, 170) : QColor(210, 210, 210));
            p.drawText(lx, yy, QString::fromUtf8(lbl));
        }

        /* The two range handles, over the scale and the labels. */
        draw_bar_handles(p);

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
         * H red for the hottest pixel, L blue for the coldest.  The Tracking
         * switch hides them; the measurement they report is unaffected. */
        if (show_hot_) {
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
            } else if (snap_.tool == DYT_TOOL_POLYGON && snap_.poly_n > 0) {
                /* The outline through the placed vertices.  While it is still
                 * open a rubber band runs from the last vertex to the pointer,
                 * so the user can see where the next click will land; a closed
                 * outline has no next click and is drawn shut instead. */
                QPolygon poly;
                for (int i = 0; i < snap_.poly_n; i++) {
                    QPoint q;
                    if (proj(snap_.poly[i].x, snap_.poly[i].y, q))
                        poly << q;
                }
                if (!snap_.poly_closed && ptr_.x >= 0)
                    poly << QPoint(x0 + ptr_.x, y0 + ptr_.y);

                const bool shut = snap_.poly_closed && poly.size() >= 3;

                if (poly.size() >= 2) {
                    p.setBrush(Qt::NoBrush);
                    /* A dark stroke under the bright one, as the other tools
                     * draw, so the shape reads on any picture. */
                    for (int pass = 0; pass < 2; pass++) {
                        p.setPen(pass == 0 ? QPen(QColor(0, 0, 0), 3) : mark);
                        if (shut)
                            p.drawPolygon(poly);
                        else
                            p.drawPolyline(poly);
                    }
                }

                /* The vertices, the first one larger: it is where the shape
                 * started, and Backspace undoes from the other end.  Only the
                 * placed ones — the rubber band's end is not a vertex. */
                p.setBrush(mark);
                p.setPen(mark);
                for (int i = 0; i < snap_.poly_n && i < poly.size(); i++)
                    p.drawEllipse(poly[i], i == 0 ? 4 : 3, i == 0 ? 4 : 3);

                if (snap_.roi_ok && !poly.isEmpty()) {
                    char lbl[128];
                    dyt_vm_roi_label(&snap_, lbl, sizeof lbl);
                    p.drawText(poly[0] + QPoint(8, -8), QString::fromUtf8(lbl));
                }
            }
        }

        /* The Mark labels.  Over the measurement shapes — a label is the
         * user's own note, and hiding it behind a box it happens to sit in
         * would be the wrong way round — and under the two panels.  Each is
         * projected with the same lambda the measurements use, so a label and
         * a marker placed on the same pixel cannot land apart, and the chip is
         * clamped into the picture so a label at the edge stays readable. */
        if (!marks_.empty()) {
            const QRect bounds(x0, y0, dw, dh);
            for (const MarkLabel &m : marks_) {
                QPoint q;
                if (!proj((int)std::lround(m.x), (int)std::lround(m.y), q))
                    continue;
                draw_mark_chip(p, q, m.text, bounds);
            }
        }

        /* The two panels, and the confirmation last so it sits over every
         * other overlay.  The rows and the wording are the view model's; only
         * the placement is here. */
        draw_info_panel(p);
        draw_confirm(p);
    }

private:
    /* The circuit-layout overlay: the loaded layout scaled to the picture it
     * sits over and offset by the align control, alpha-composited at the
     * opacity the slider sets.  Drawn over the base image and *under* every
     * other overlay, so the measurement markers and the info panel stay
     * readable on top of it — the layout is context, not a measurement.
     *
     * The picture it covers is the override when one is up, else the live
     * frame; with neither there is nothing to align a layout to, so it is
     * skipped (and the placeholder path never reaches here). */
    void draw_layout(QPainter &p)
    {
        if (!layout_shown_ || layout_.isNull() || layout_opacity_ <= 0)
            return;
        const QImage &under = override_.isNull() ? img_ : override_;
        if (under.isNull())
            return;

        /* On the live frame the layout gets the *same* turn and mirrors the
         * picture got, so it moves with the thermal content instead of staying
         * put while the picture turns under it.  The dst rect below does the
         * scaling (the layout is stretched to cover the picture whatever its
         * own size), so only the rotation and the flips are applied here —
         * the same QTransform primitives, in the same order, that
         * transformed() uses for the frame itself.  A saved still or clip
         * already has its transform baked into the image, so its layout is
         * drawn as loaded. */
        QImage lay = layout_;
        if (override_.isNull()) {
            if (snap_.xform.rot)
                lay = lay.transformed(QTransform().rotate(snap_.xform.rot),
                                      Qt::FastTransformation);
            Qt::Orientations ori;
            if (snap_.xform.flip_h) ori |= Qt::Horizontal;
            if (snap_.xform.flip_v) ori |= Qt::Vertical;
            if (ori)
                lay = lay.flipped(ori);
        }

        const QRect dst(kPad + layout_dx_, kPad + layout_dy_,
                        under.width(), under.height());
        /* Clipped to the picture's rect: an aligned layout is meant to shift
         * *within* the picture, and without this a positive offset would
         * bleed over the colour bar. */
        p.save();
        p.setClipRect(QRect(kPad, kPad, under.width(), under.height()));
        p.setOpacity(layout_opacity_ / 100.0);
        p.drawImage(dst, lay);
        p.restore();
    }

    /* One Mark label: a small plate holding the text, joined to the pixel it
     * names by a leader, with a dot on the pixel itself.  The plate is what
     * makes a label readable over any picture — the vendor's own label
     * drawing fills a rectangle behind the string (M_DarwString, :14002) —
     * and the leader is what keeps the text and the pixel from being confused
     * when several labels sit close together.
     *
     * The plate is clamped into the picture by kMarkInset, so a label placed
     * on the last row or column is pulled back inside rather than drawn half
     * off the canvas.  `bounds` is the picture's canvas rectangle. */
    void draw_mark_chip(QPainter &p, const QPoint &anchor, const QString &text,
                        const QRect &bounds)
    {
        p.save();

        /* One point smaller than the canvas font, so a chip reads as a note
         * beside the picture rather than as part of it. */
        QFont f = p.font();
        if (f.pointSize() > 8)
            f.setPointSize(f.pointSize() - 1);
        p.setFont(f);
        const QFontMetrics fm(f);

        const int pad = 3;
        const int tw  = fm.horizontalAdvance(text) + 2 * pad;
        const int th  = fm.height() + 2 * pad;

        QRect in = bounds.adjusted(kMarkInset, kMarkInset,
                                   -kMarkInset, -kMarkInset);
        /* A picture narrower than the plate (or than the two insets) cannot
         * hold it with the margin, so the whole picture is used instead —
         * the clamp still keeps the plate inside the canvas. */
        if (in.width() < tw || in.height() < th)
            in = bounds;

        /* To the upper right of the pixel, as the measurement labels are. */
        int x = anchor.x() + 9;
        int y = anchor.y() - th - 3;
        x = std::max(in.left(), std::min(in.right()  - tw + 1, x));
        y = std::max(in.top(),  std::min(in.bottom() - th + 1, y));
        const QRect r(x, y, tw, th);

        /* The leader first, so the plate and the dot cover its two ends. */
        const QPoint to(x, y + th / 2);
        p.setPen(QPen(QColor(0, 0, 0), 3));
        p.drawLine(anchor, to);
        p.setPen(QPen(QColor(255, 255, 0), 1));
        p.drawLine(anchor, to);

        /* The plate, then the text on it. */
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 200));
        p.drawRect(r);
        p.setBrush(Qt::NoBrush);
        p.setPen(QColor(255, 255, 0));
        p.drawRect(r.adjusted(0, 0, -1, -1));
        p.setPen(QColor(255, 255, 255));
        p.drawText(r.adjusted(pad, 0, -pad, 0),
                   Qt::AlignLeft | Qt::AlignVCenter, text);

        /* The pixel itself, over the leader's end. */
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 0));
        p.drawEllipse(anchor, 2, 2);

        p.restore();
    }

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
     * place a point one pixel outside the image.
     *
     * The first two lines are the exact inverse of the transform paintEvent
     * applies — the same origin, the same scale — so the display layer is
     * invisible to everything downstream.  At the natural size they reduce to
     * the bare `pos - kPad` they have always been. */
    void pointer(dyt_vm_mouse_ev_t ev, const QPointF &pos)
    {
        if (!sess_ || img_.isNull())
            return;
        const double  s   = display_scale();
        const QPointF org = display_origin();
        const int lx = (int)std::floor((pos.x() - org.x()) / s - kPad);
        const int ly = (int)std::floor((pos.y() - org.y()) / s - kPad);
        const int placed =
            dyt_vm_tool_mouse(sess_, &ptr_, ev, snap_.tool, &snap_.xform,
                              snap_.width, snap_.height,
                              img_.width(), img_.height(), lx, ly);
        /* The polygon is placed a click at a time and its outline lives in the
         * snapshot, so a new vertex has to be read back before it can be drawn
         * — and the fill with it.  The drag tools only move point 1, which the
         * next tick picks up anyway, and a polygon *move* places nothing, so
         * this costs one snapshot per click rather than one per mouse event. */
        if (placed && snap_.tool == DYT_TOOL_POLYGON && sess_)
            dyt_session_snapshot(sess_, &snap_, nullptr, 0);
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
     * expects.
     *
     * Hidden at start-up.  The Windows app opens on the bare picture — the
     * info panel is something the user asks for (its 'd', or the Settings
     * box), not a block of text over the first frame they see. */
    dyt_device_info_t   info_{};
    int                 have_info_   = 0;
    bool                show_info_   = false;
    /* The hottest/coldest markers, on by default (the reference viewer always
     * marks them).  The Windows panel's Tracking switch drives this. */
    bool                show_hot_    = true;
    /* The High TEMP. Alarm threshold, in Celsius.  The panel's field pushes it
     * here (set_alarm_setpoint); this is only the value 'a' arms at. */
    float               alarm_setpoint_ = DYT_ALARM_SETPOINT_DEFAULT;
    /* Which colour-bar handle a drag is moving: 0 the top (the window's high
     * end), 1 the bottom, -1 nothing. */
    int                 bar_drag_ = -1;
    float               override_v_[5]  = { 0.f, 0.f, 0.f, 0.f, 0.f };
    int                 override_on_[5] = { 0, 0, 0, 0, 0 };

    /* A saved still or clip frame being viewed.  Owned here; pushed by
     * MainWindow, which is also where the gallery lives. */
    QImage                    override_;

    /* The circuit-layout overlay (see set_layout).  `layout_` empty means none
     * is loaded; whether a loaded one is drawn is `layout_shown_`, which the
     * page's toggle owns. */
    QImage                    layout_;
    int                       layout_dx_ = 0, layout_dy_ = 0;
    int                       layout_opacity_ = 100;
    bool                      layout_shown_ = false;

    /* The Mark annotations and the tool that places them (see set_mark_mode).
     * `mark_mode_` is a MarkKind; the list is in source pixels. */
    std::vector<MarkLabel>    marks_;
    int                       mark_mode_ = MarkNone;
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
        /* The width is Ignored, the height Fixed.  The picture decides how wide
         * the window is — never the status text: a long line (a saved file's
         * full path, a device's notice) would otherwise widen the window the
         * moment it appeared, and the window would jump as notices came and
         * went.  The paint already elides each line to whatever width it is
         * given, so nothing is lost, only shortened. */
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
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

    /* The recording indicator — "REC 0:07  175 frames" — or empty when
     * nothing is running.  Its own badge rather than part of a line, because a
     * recording must stay visible while the transient notices come and go. */
    void set_recording(const QString &s)
    {
        rec_ = s;
        update();
    }

    const QString &recording_label() const { return rec_; }

    /* The playback indicator — "playing clip.mp4  12/50" or "paused" — or
     * empty when no clip is up.  A badge of its own, like the recording one,
     * and on the middle line so the two cannot collide: the alarm owns line 1
     * and the recording badge owns line 3. */
    void set_playback(const QString &s)
    {
        play_ = s;
        update();
    }

    const QString &playback_label() const { return play_; }

    /* The connection dot, at the left of the third line and coloured by the
     * device state (conn_colour).  A dot rather than a word because line 3
     * already names the state; the dot is what makes it readable at a glance,
     * and it is what the Windows app's status bar carries before its
     * "Camera connected".  Kept separate from set_lines() so a caller cannot
     * set the state word without the dot that qualifies it. */
    void set_conn(DevState st)
    {
        conn_      = st;
        have_conn_ = true;
        update();
    }

    /* For the selftest: the colour the dot is painted in, or an invalid colour
     * before any state has been set.  Exposed rather than read back from a
     * grab because a 9-px circle in a 1172-px window is a needle, and the
     * interesting failure (the wrong state reaching the strip) is not a
     * painting bug. */
    QColor conn_dot_colour() const
    {
        return have_conn_ ? conn_colour(conn_) : QColor();
    }
    bool conn_dot_shown() const { return have_conn_; }

    QSize sizeHint() const override
    {
        int w = 0;
        for (int i = 0; i < 3; i++)
            w = std::max(w, fontMetrics().horizontalAdvance(line_[i]) +
                                (i == 2 && have_conn_ ? kDotW + kDotGap : 0));
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

        /* A badge shares its line with the text, so the text is elided to stop
         * before it — otherwise a long status or readout line simply vanishes
         * under the badge.  One inset per line: the alarm is on line 1,
         * playback on line 2, recording on line 3.  The badges themselves are
         * drawn below, after the text. */
        const QString alarm_txt =
            (alarm_on_ && alarm_ != DYT_ALARM_NONE)
                ? QStringLiteral("ALARM ") +
                      QString::fromUtf8(dyt_alarm_name(alarm_))
                : QString();
        const int inset[3] = {
            alarm_txt.isEmpty() ? 0 : 12 + fontMetrics().horizontalAdvance(alarm_txt),
            play_.isEmpty()     ? 0 : 12 + fontMetrics().horizontalAdvance(play_),
            rec_.isEmpty()      ? 0 : 12 + fontMetrics().horizontalAdvance(rec_),
        };

        /* The third line is indented past the connection dot, so its text
         * cannot sit under it. */
        const int left[3] = { 0, 0, have_conn_ ? kDotW + kDotGap : 0 };

        for (int i = 0; i < 3; i++) {
            p.setPen(pen[i]);
            const int avail = width() - 2 * kStripPad - inset[i] - left[i];
            p.drawText(kStripPad + left[i], kStripPad + i * kLineH + ascent,
                       fontMetrics().elidedText(line_[i], Qt::ElideRight,
                                                std::max(0, avail)));
        }

        /* The connection dot: a filled circle on the third line's centre, in
         * the state's colour.  Drawn after the text so it cannot be covered by
         * it, though the inset above already keeps them apart. */
        if (have_conn_) {
            p.setPen(Qt::NoPen);
            p.setBrush(conn_colour(conn_));
            const int cy = kStripPad + 2 * kLineH + kLineH / 2;
            p.drawEllipse(QRect(kStripPad, cy - kDotW / 2, kDotW, kDotW));
        }

        /* The alarm is the one thing worth shouting about. */
        if (!alarm_txt.isEmpty()) {
            const int   bw = 12 + fontMetrics().horizontalAdvance(alarm_txt);
            const QRect r(width() - bw - kStripPad, kStripPad,
                          bw, kLineH + 4);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 180));
            p.drawRect(r);
            p.setPen(QColor(255, 255, 255));
            p.drawText(r, Qt::AlignCenter, alarm_txt);
        }

        /* Playback, on the middle line so it cannot collide with the alarm
         * above it or the recording badge below.  Amber rather than the
         * recording red: a clip playing is not something being written. */
        if (!play_.isEmpty()) {
            const int   bw = 12 + fontMetrics().horizontalAdvance(play_);
            const QRect r(width() - bw - kStripPad,
                          kStripPad + kLineH, bw, kLineH + 4);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0x9a, 0x66, 0x00));
            p.drawRect(r);
            p.setPen(QColor(255, 255, 255));
            p.drawText(r, Qt::AlignCenter, play_);
        }

        /* Recording, on the last line so the two badges cannot collide. */
        if (!rec_.isEmpty()) {
            const int   bw = 12 + fontMetrics().horizontalAdvance(rec_);
            const QRect r(width() - bw - kStripPad,
                          kStripPad + 2 * kLineH, bw, kLineH + 4);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0xb0, 0x10, 0x10));
            p.drawRect(r);
            p.setPen(QColor(255, 255, 255));
            p.drawText(r, Qt::AlignCenter, rec_);
        }
    }

private:
    QString line_[3];
    QString rec_;
    QString play_;
    bool    alarm_on_ = false;
    dyt_alarm_state_t alarm_ = DYT_ALARM_NONE;
    /* The connection dot.  `have_conn_` is false until a state arrives, so the
     * strip built before the first frame has no dot rather than a grey one
     * that would read as a state. */
    DevState conn_      = DevState::Fixture;
    bool     have_conn_ = false;
};

/* ------------------------------------------------------------- the gallery */

/* The list of saved stills and clips, as a real widget.
 *
 * It used to be painted inside FrameView::paintEvent, *under* the canvas's
 * display transform — so it scaled with the picture, and the two early returns
 * that skip the overlays (no frame yet, degenerate geometry) skipped it too.
 * A child widget is composited in *widget* space instead: the transform cannot
 * move it and no paint path can hide it, which is what makes it appear in
 * fullscreen as well as windowed.  It also brings mouse selection, double-click
 * and scrolling with it, none of which the painted version had.
 *
 * Everything here is Qt::NoFocus.  The keys belong to MainWindow's single
 * dispatch, and a focusable list would swallow them the moment it was clicked;
 * mouse events need no focus, so clicking a row still works.
 *
 * The entries and the highlight are dyt_vm_gallery_*'s, never the widget's: the
 * list is a view of that state, so the keyboard and the mouse cannot disagree
 * about which row is selected. */
class GalleryPanel : public QWidget {
public:
    GalleryPanel(const dyt_vm_gallery_t *g, QWidget *parent)
        : QWidget(parent), gal_(g)
    {
        setFocusPolicy(Qt::NoFocus);
        setAutoFillBackground(false);

        head_   = new QLabel(this);
        folder_ = new QLabel(this);
        head_->setFocusPolicy(Qt::NoFocus);
        folder_->setFocusPolicy(Qt::NoFocus);
        /* Both labels need an explicit colour: the default palette's text is
         * dark, which is invisible on this panel — a mistake a pixel check
         * cannot make for you, and one only a render shows. */
        head_->setStyleSheet(QStringLiteral("color: #e6e6e6;"));
        folder_->setStyleSheet(QStringLiteral("color: #9aa0a6;"));

        pick_ = new QPushButton(QStringLiteral("Folder\u2026"), this);
        pick_->setFocusPolicy(Qt::NoFocus);
        pick_->setCursor(Qt::PointingHandCursor);
        pick_->setStyleSheet(QStringLiteral(
            "QPushButton { color:#dcdcdc; background:#303030;"
            "  border:1px solid #666; padding:2px 8px; }"
            "QPushButton:hover { background:#3a3a3a; }"));
        connect(pick_, &QPushButton::clicked, this, [this]() {
            if (on_choose_folder)
                on_choose_folder();
        });

        list_ = new QListWidget(this);
        list_->setFocusPolicy(Qt::NoFocus);
        list_->setSelectionMode(QAbstractItemView::SingleSelection);
        list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list_->setStyleSheet(QStringLiteral(
            "QListWidget { background: transparent; border: none;"
            "  color: #f0f0f0; outline: none; }"
            "QListWidget::item { padding: 1px 3px; }"
            "QListWidget::item:selected { background: #005aa0;"
            "  color: #ffffff; }"));
        connect(list_, &QListWidget::itemClicked, this,
                [this](QListWidgetItem *it) {
                    if (on_select)
                        on_select(list_->row(it));
                });
        connect(list_, &QListWidget::itemDoubleClicked, this,
                [this](QListWidgetItem *) {
                    if (on_open)
                        on_open();
                });

        auto *top   = new QHBoxLayout();
        auto *texts = new QVBoxLayout();
        texts->setSpacing(0);
        texts->setContentsMargins(0, 0, 0, 0);
        texts->addWidget(head_);
        texts->addWidget(folder_);
        top->addLayout(texts, 1);
        top->addWidget(pick_, 0, Qt::AlignTop);

        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(8, 6, 8, 8);
        lay->setSpacing(4);
        lay->addLayout(top, 0);
        lay->addWidget(list_, 1);

        /* The parent's resize is what moves the panel: it is a free-floating
         * overlay, not a row of the layout, so nothing else would reposition
         * it. */
        parent->installEventFilter(this);
    }

    /* The row clicked, the row opened (double-click), and the Folder button. */
    std::function<void(int)> on_select;
    std::function<void()>    on_open;
    std::function<void()>    on_choose_folder;

    void set_folder(const QString &dir)
    {
        dir_ = dir;
        folder_->setText(dir_);
        folder_->setToolTip(dir_);
    }

    /* Rebuild the rows from the gallery state.  Called after a scan, so the
     * widget and dyt_vm_gallery_t are never showing different things. */
    void reload()
    {
        list_->clear();
        for (int i = 0; i < gal_->n; i++) {
            const dyt_vm_item_t &it = gal_->items[i];
            list_->addItem(QStringLiteral("%1  %2")
                               .arg(QString::fromLatin1(
                                        it.kind == DYT_VM_ITEM_STILL ? "still"
                                                                     : "clip"),
                                    QString::fromUtf8(it.name)));
        }
        sync();
    }

    /* Follow the gallery's own highlight, which is what a key moves. */
    void sync()
    {
        char lbl[192];
        dyt_vm_gallery_label(gal_, lbl, sizeof lbl);
        head_->setText(QString::fromUtf8(lbl));
        list_->setCurrentRow(gal_->sel >= 0 && gal_->sel < list_->count()
                                 ? gal_->sel
                                 : -1);
        reposition();
    }

    /* The row the panel is showing as current, for --selftest. */
    int current_row() const { return list_->currentRow(); }
    QString folder_text() const { return folder_->text(); }
    QListWidget *list() const { return list_; }

protected:
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (o == parentWidget() && e->type() == QEvent::Resize)
            reposition();
        return QWidget::eventFilter(o, e);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setPen(QColor(120, 120, 120));
        /* Nearly opaque: the device panel is painted on the canvas underneath,
         * and at 215 the two overlays' text showed through each other. */
        p.setBrush(QColor(12, 12, 12, 242));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }

private:
    /* Fit the rows, but never taller than the canvas — past that the list
     * scrolls, which is what the QListWidget is here for. */
    void reposition()
    {
        QWidget *par = parentWidget();
        if (!par)
            return;

        const int row_h = list_->count() ? list_->sizeHintForRow(0) : 18;
        const int rows  = std::max(1, std::min(list_->count(), 14));
        list_->setFixedHeight(rows * row_h + 8);

        const int w = std::min(560, std::max(240, par->width() - 2 * kPad));
        const int h = std::min(std::max(120, par->height() - 2 * kPad),
                               sizeHint().height());
        setGeometry(kPad, kPad, w, h);
    }

    const dyt_vm_gallery_t *gal_ = nullptr;   /* borrowed */
    QLabel       *head_   = nullptr;
    QLabel       *folder_ = nullptr;
    QPushButton  *pick_   = nullptr;
    QListWidget  *list_   = nullptr;
    QString       dir_;
};

/* ----------------------------------------------------------- the icon rail
 *
 * The Windows counterpart's left rail (manual p.5, /tmp/pdfx/w-08.png) is a
 * slim vertical strip of stacked icon+label buttons.  This is its Qt analogue.
 *
 * Every button is `Qt::NoFocus` for the same reason the control panel's are: a
 * focused button swallows the keys before `keyPressEvent` sees them, which
 * would silently break every binding the moment someone clicked the rail.
 * `checkable` buttons are set from the session, not from their own toggle, so
 * `handle_key` can drive them and `sync_actions()` can set their state.  Each
 * button's `clicked` runs the same `handle_key` the keyboard does, so the rail
 * is a second route to the same actions — never a second set.
 *
 * The eight icons are QPainter-drawn vectors: the package ships no icon assets,
 * and a hand-drawn glyph is the closest a no-asset build can come to the
 * Windows rail's look.  Each is a small delegate painted into a 24x24 pixmap.
 */
class MainWindow;   /* forward: IconRail reports clicks to the window */
class IconRail : public QWidget {
public:
    enum RailItem {
        Palette = 0, Mark, Rotate, Compare, ResetImage,
        Tutorials, ContactUs, Setting,
        N_RailItems
    };

    explicit IconRail(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedWidth(kRailW);
        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, kPad, 0, kPad);
        lay->setSpacing(6);

        /* The top group — palette, mark, rotate, compare, reset. */
        for (int i = 0; i < ResetImage + 1; i++)
            lay->addWidget(make_button((RailItem)i));
        lay->addStretch(1);
        /* The bottom group — tutorials, contact, setting. */
        for (int i = Tutorials; i < N_RailItems; i++)
            lay->addWidget(make_button((RailItem)i));

        /* Compare is now live — the two-board comparison engine (src/compare.c)
         * has landed.  Clicking it switches the control panel to the Comparison
         * tab, the same way the rail's other items route to their tab or
         * dialog. */
    }

    /* A click on `item`.  Set by MainWindow, which owns the session and the
     * one handle_key dispatch — the same callback split every other control in
     * this file uses (on_help_, on_still_, …), and what keeps IconRail from
     * needing MainWindow's complete definition here. */
    std::function<void(RailItem)> on_action;

    /* A button the front end can re-parent into a popup (Palette) or drive
     * the same way a QAction would.  Returns nullptr for an out-of-range item. */
    QPushButton *button(RailItem i) const
    {
        if (i < 0 || i >= N_RailItems)
            return nullptr;
        return btn_[i];
    }

    /* The icons, painted into 24x24 device-pixel pixmaps.  Each is a delegate
     * so the painting stays in one place and the button just sets the pixmap. */
    static void paint_icon(QPainter &p, RailItem i, const QRect &r)
    {
        p.setRenderHint(QPainter::Antialiasing, true);
        const QColor cyan(QStringLiteral("#00b4d8"));
        const QColor white(QStringLiteral("#e6e6e6"));
        QPen pen(cyan, 1.4);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        const int cx = r.center().x(), cy = r.center().y();

        switch (i) {
        case Palette: {
            /* a colour wheel: six wedges in the palette's accent hues. */
            const QColor hues[] = {
                QColor(QStringLiteral("#e6194b")),
                QColor(QStringLiteral("#f58231")),
                QColor(QStringLiteral("#ffe119")),
                QColor(QStringLiteral("#3cb44b")),
                QColor(QStringLiteral("#00b4d8")),
                QColor(QStringLiteral("#911eb4")) };
            const int R = std::min(r.width(), r.height()) / 2 - 2;
            QRectF circ(cx - R, cy - R, 2 * R, 2 * R);
            for (int k = 0; k < 6; k++) {
                p.setBrush(hues[k]);
                p.setPen(Qt::NoPen);
                p.drawPie(circ, 60 * k * 16, 60 * 16);
            }
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(white, 1.0));
            p.drawEllipse(circ);
            break;
        }
        case Mark:
            /* a thermometer + a plus: the measurement "mark" glyph. */
            p.drawRoundedRect(cx - 3, cy - 9, 6, 14, 3, 3);
            p.setBrush(cyan);
            p.drawEllipse(QRectF(cx - 5, cy + 3, 10, 10));
            p.setBrush(Qt::NoBrush);
            p.drawLine(cx + 6, cy - 8, cx + 11, cy - 8);
            p.drawLine(cx + 8, cy - 10, cx + 8, cy - 6);
            break;
        case Rotate: {
            /* a circular arrow, the rotate glyph. */
            const int R = 8;
            QRectF circ(cx - R, cy - R, 2 * R, 2 * R);
            p.drawArc(circ, 30 * 16, 270 * 16);
            /* arrowhead at the open end. */
            QTransform old = p.transform();
            p.translate(cx + R - 1, cy + 4);
            p.rotate(120);
            p.setBrush(cyan);
                QPolygon tri; tri << QPoint(0, 0) << QPoint(-5, -3)
                                  << QPoint(-5, 3);
                p.drawPolygon(tri);
            p.setBrush(Qt::NoBrush);
            p.setTransform(old);
            break;
        }
        case Compare: {
            /* two opposing arrows — the compare glyph. */
            p.drawLine(cx - 9, cy - 4, cx + 9, cy - 4);
            p.drawLine(cx + 9, cy - 4, cx + 5, cy - 7);
            p.drawLine(cx + 9, cy - 4, cx + 5, cy - 1);
            p.drawLine(cx - 9, cy + 4, cx + 9, cy + 4);
            p.drawLine(cx - 9, cy + 4, cx - 5, cy + 1);
            p.drawLine(cx - 9, cy + 4, cx - 5, cy + 7);
            break;
        }
        case ResetImage: {
            /* a circular arrow with a tail — the reset glyph. */
            const int R = 8;
            QRectF circ(cx - R, cy - R, 2 * R, 2 * R);
            p.drawArc(circ, 45 * 16, 250 * 16);
            QTransform old = p.transform();
            p.translate(cx + R - 1, cy - 4);
            p.rotate(-30);
            p.setBrush(cyan);
                QPolygon tri; tri << QPoint(0, 0) << QPoint(-5, -3)
                                  << QPoint(-5, 3);
                p.drawPolygon(tri);
            p.setBrush(Qt::NoBrush);
            p.setTransform(old);
            break;
        }
        case Tutorials:
            /* an open book. */
            p.drawRect(cx - 9, cy - 6, 18, 12);
            p.drawLine(cx, cy - 6, cx, cy + 6);
            p.drawLine(cx - 7, cy - 2, cx - 2, cy - 2);
            p.drawLine(cx - 7, cy + 1, cx - 2, cy + 1);
            p.drawLine(cx + 2, cy - 2, cx + 7, cy - 2);
            p.drawLine(cx + 2, cy + 1, cx + 7, cy + 1);
            break;
        case ContactUs:
            /* an envelope. */
            p.drawRect(cx - 9, cy - 6, 18, 12);
            p.drawLine(cx - 9, cy - 6, cx, cy + 2);
            p.drawLine(cx + 9, cy - 6, cx, cy + 2);
            break;
        case Setting: {
            /* a gear — the setting glyph. */
            const int R = 9, r = 5, teeth = 8;
            QPainterPath gear;
            for (int k = 0; k < teeth * 2; k++) {
                const double a = (k / 2.0) * 2 * M_PI / teeth;
                const int rad = (k % 2 == 0) ? R : r;
                const QPointF pt(cx + rad * std::cos(a),
                                 cy + rad * std::sin(a));
                if (k == 0) gear.moveTo(pt); else gear.lineTo(pt);
            }
            gear.closeSubpath();
            p.drawPath(gear);
            p.setBrush(QColor(QStringLiteral("#1e1e1e")));
            p.drawEllipse(QRectF(cx - 3, cy - 3, 6, 6));
            p.setBrush(Qt::NoBrush);
            break;
        }
        case N_RailItems: break;
        }
    }

private:
    QPushButton *make_button(RailItem i)
    {
        static const char *const kLabels[N_RailItems] = {
            "Palette", "Mark", "Rotate", "Compare", "Reset",
            "Tutorials", "Contact", "Setting" };
        auto *b = new QPushButton(this);
        b->setObjectName(QStringLiteral("rail"));
        b->setFocusPolicy(Qt::NoFocus);   /* see the class comment */
        b->setCheckable(true);
        b->setText(QString::fromUtf8(kLabels[i]));
        b->setIconSize(QSize(24, 24));
        b->setMinimumHeight(48);
        /* Paint the icon into a pixmap once — the glyph never changes. */
        QPixmap pm(24, 24);
        pm.fill(Qt::transparent);
        pm.fill(QColor(QStringLiteral("#2b2b2b")));
        {   QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing, true);
            paint_icon(p, i, QRect(0, 0, 24, 24)); }
        b->setIcon(QIcon(pm));
        b->setIconSize(QSize(24, 24));
        /* A QPushButton lays its icon *beside* the text, not above it (that is
         * a QToolButton style, and setToolButtonStyle is not a QPushButton
         * API).  The label therefore shares the 72-px rail with a 24-px icon,
         * which is why the labels are one short word each and the rail's QSS
         * sets a 9-px font: "Compare" and "Contact" are the longest, and at the
         * default font they would elide.  The glyph carries the meaning; the
         * label disambiguates it. */
        /* Report the click; MainWindow decides what it means.  Every rail item
         * is wired this way from the start, so a button is never a silent
         * no-op — the handler is what grows, step by step. */
        b->connect(b, &QPushButton::clicked, [this, i]() {
            if (on_action)
                on_action((RailItem)i);
        });
        btn_[i] = b;
        return b;
    }

    QPushButton *btn_[N_RailItems] = {};
};

/* ------------------------------------------------------- the analysis chart
 *
 * The Analysis group's plot.  The reference's group has two rows over one
 * chart, and their labels are the reading: **Line** is the line itself — the
 * temperature along the placed line, from dyt_measure_line, a slice through
 * space — and **Chart analysis** is the analysis *of* that curve: the same
 * samples with the statistics drawn on them (the mean and median rules, and
 * the peak picked out with its value).  Neither is a second data source, which
 * is why one widget carries both.
 *
 * Hand-painted rather than Qt Charts: that is a separate module for one plot,
 * and the drawing is a polyline and a few labels.
 *
 * The samples arrive from the window rather than being read here, so this
 * widget holds no session and no lock.  It is fed once per painted frame, and
 * the statistics are computed there rather than in paintEvent — a sort inside a
 * paint is a sort per repaint, and repaints do not happen at the frame rate.
 */
static const int kPlotH     = 76;   /* the curve and one row of labels */
static const int kPlotAxisW = 44;   /* the value labels' gutter */
static const int kPlotW     = 300;  /* a preference; the panel sets the width */

class ProfilePlot : public QWidget {
public:
    enum Mode { Line = 0, Analysis };

    explicit ProfilePlot(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedHeight(kPlotH);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setFocusPolicy(Qt::NoFocus);   /* see ControlPanel's comment */
        setToolTip(QStringLiteral(
            "x: pixels along the line · y: temperature"));
    }

    /* Install a profile: `n` samples in Celsius, NaN where the sample was off
     * the image (measure.h), which is drawn as a gap rather than a zero.
     *
     * n <= 0, or a profile with no finite sample, clears back to the
     * placeholder — a curve of nothing is not a curve, and drawing a flat line
     * at 0 C would be inventing a reading.  `unit` is only for the labels; the
     * samples stay Celsius, as everywhere else. */
    void set_profile(const float *v, int n, dyt_unit_t unit);

    void set_mode(Mode m) { mode_ = m; update(); }
    Mode mode() const { return mode_; }

    /* The index of the highest finite sample, or -1 when there is none: the
     * peak the chart marks, exposed for --selftest. */
    int peak_bin() const { return peak_; }

    QSize sizeHint() const override { return QSize(kPlotW, kPlotH); }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QVector<float> v_;
    int            peak_   = -1;
    float          lo_     = 0.f, hi_ = 0.f;
    float          mean_   = 0.f, median_ = 0.f;
    dyt_unit_t     unit_   = DYT_UNIT_C;
    Mode           mode_   = Line;
};

void ProfilePlot::set_profile(const float *v, int n, dyt_unit_t unit)
{
    v_.clear();
    peak_ = -1;
    mean_ = median_ = 0.f;
    unit_ = unit;

    if (v && n > 0) {
        QVector<float> sorted;
        double         sum  = 0.0;
        float          mn   = 0.f, mx = 0.f;
        int            seen = 0;

        sorted.reserve(n);
        for (int i = 0; i < n; i++) {
            if (!std::isfinite(v[i]))
                continue;               /* off the image: a gap, not a zero */
            /* Strict > keeps the *first* maximum, the same tie-break
             * dyt_frame_stats() uses — so the peak this marks is the pixel the
             * H marker does, not a second opinion about it. */
            if (seen == 0 || v[i] < mn) mn = v[i];
            if (seen == 0 || v[i] > mx) { mx = v[i]; peak_ = i; }
            sum += v[i];
            sorted.append(v[i]);
            seen++;
        }

        if (seen > 0) {
            /* Every sample is kept, NaN included, so a gap stays where it is
             * on the x-axis instead of the line closing up around it. */
            v_.resize(n);
            for (int i = 0; i < n; i++)
                v_[i] = v[i];

            mean_ = (float)(sum / (double)seen);
            std::sort(sorted.begin(), sorted.end());
            median_ = (seen & 1)
                          ? sorted[seen / 2]
                          : 0.5f * (sorted[seen / 2 - 1] + sorted[seen / 2]);

            /* A flat profile would divide by zero, and a nearly-flat one would
             * fill the plot with rounding noise; give it half a degree either
             * side so the line sits across the middle. */
            if (mx - mn < 0.05f) { mn -= 0.5f; mx += 0.5f; }
            lo_ = mn;
            hi_ = mx;
        }
    }
    update();
}

void ProfilePlot::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const QRect r = rect().adjusted(0, 0, -1, -1);

    p.fillRect(rect(), QColor(QStringLiteral("#0d161d")));
    p.setPen(QColor(QStringLiteral("#2a3a45")));
    p.drawRect(r);

    if (v_.isEmpty()) {
        p.setPen(QColor(120, 132, 142));
        p.drawText(rect(), Qt::AlignCenter,
                   QStringLiteral("place a line to see its profile"));
        return;
    }

    /* The plot area: a gutter on the left for the value labels and a row at
     * the bottom for the sample positions. */
    const QRect plot(r.left() + kPlotAxisW, r.top() + 4,
                     r.width() - kPlotAxisW - 6, r.height() - 20);

    const double span = (double)(hi_ - lo_) > 0 ? (double)(hi_ - lo_) : 1.0;
    const int    n    = (int)v_.size();

    auto xmap = [&](int i) {
        return plot.left() + (n <= 1
                   ? 0
                   : (int)std::lround((double)i * (plot.width() - 1)
                                      / (double)(n - 1)));
    };
    auto ymap = [&](float c) {
        return plot.bottom() - (int)std::lround(
            ((double)c - (double)lo_) / span * (plot.height() - 1));
    };

    /* Three rules: the axis's top, middle and bottom.  The curve's own range
     * *is* the axis, so the top and bottom rules are its min and max. */
    p.setPen(QColor(QStringLiteral("#233039")));
    p.drawLine(plot.left(), plot.top(), plot.right(), plot.top());
    p.drawLine(plot.left(), plot.center().y(), plot.right(), plot.center().y());
    p.drawLine(plot.left(), plot.bottom(), plot.right(), plot.bottom());

    /* The curve.  A NaN sample breaks the path rather than being interpolated
     * across, so a line that leaves the image shows as a gap. */
    QPainterPath path;
    bool         open = false;
    for (int i = 0; i < n; i++) {
        if (!std::isfinite(v_[i])) { open = false; continue; }
        const QPointF q(xmap(i), ymap(v_[i]));
        if (open) path.lineTo(q);
        else    { path.moveTo(q); open = true; }
    }
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(QStringLiteral("#00b4d8")), 1.4));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    p.setRenderHint(QPainter::Antialiasing, false);

    char b[32];

    if (mode_ == Analysis) {
        /* The statistics as rules across the curve — the mean solid and the
         * median dotted, so a skewed profile shows as the gap between them —
         * and the peak marked where it is. */
        p.setPen(QPen(QColor(QStringLiteral("#e0a020")), 1, Qt::DashLine));
        p.drawLine(plot.left(), ymap(mean_), plot.right(), ymap(mean_));
        p.setPen(QPen(QColor(QStringLiteral("#7ac0a0")), 1, Qt::DotLine));
        p.drawLine(plot.left(), ymap(median_), plot.right(), ymap(median_));

        if (peak_ >= 0) {
            const QPoint q(xmap(peak_), ymap(v_[peak_]));
            p.setPen(QPen(QColor(0, 0, 0), 3));
            p.drawEllipse(q, 4, 4);
            p.setPen(QColor(QStringLiteral("#ffd166")));
            p.drawEllipse(q, 4, 4);
            dyt_temp_format(unit_, v_[peak_], b, sizeof b);
            p.drawText(q + QPoint(7, -5), QString::fromUtf8(b));
        }
    }

    /* The axis labels: the range in the gutter, the sample positions along the
     * bottom.  Left-aligned in the gutter so a long value cannot push the plot
     * around. */
    p.setPen(QColor(140, 152, 162));
    const int gw = kPlotAxisW - 8;
    dyt_temp_format(unit_, hi_, b, sizeof b);
    p.drawText(QRect(r.left() + 3, plot.top() - 7, gw, 14),
               Qt::AlignLeft | Qt::AlignVCenter, QString::fromUtf8(b));
    dyt_temp_format(unit_, lo_, b, sizeof b);
    p.drawText(QRect(r.left() + 3, plot.bottom() - 7, gw, 14),
               Qt::AlignLeft | Qt::AlignVCenter, QString::fromUtf8(b));

    const QRect foot(plot.left(), plot.bottom() + 3, plot.width(), 14);
    p.drawText(foot, Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("0"));
    p.drawText(foot, Qt::AlignRight | Qt::AlignVCenter,
               QString::number(n - 1));
}

/* ------------------------------------------------------------ the 3D surface
 *
 * The Windows app's "3D Analysis" tab (manual p.5) draws the thermal grid as
 * a height-mapped landscape.  Two renderers share one model: a GL one for the
 * ordinary case, and a QPainter one for a machine where QOpenGLWidget cannot
 * make a context — so the tab is never dead, and `make check` can exercise the
 * software path on any host.  They draw the same vertices, the same palette
 * colours and the same matrix (src/surface.c), so they cannot drift apart in
 * what they show.
 *
 * The model is deliberately not a widget: it holds the decimated grid, the
 * mesh, the colours and the orbit camera, and both renderers borrow it.  That
 * is also what the selftest reads, because the interesting failures — a wrong
 * height, a colour off the palette, a camera that will not orbit — are all in
 * the model, not in the GL calls.
 */

/* The decimation cap.  The engine builds a mesh for whatever grid it is
 * handed, but 256x192 is ~97k triangles, which the software path cannot
 * repaint at an interactive rate — and a 3D overview of a sensor does not
 * need every pixel.  Capping both renderers at the same grid keeps them
 * showing the same surface. */
static const int kSurfaceMaxDim = 96;

/* The camera a fresh view starts from: turned a little off-axis so the grid
 * reads as a landscape rather than a flat sheet, and tilted so the near edge
 * is low. */
static const float kSurfaceYaw0   = 0.6f;
static const float kSurfacePitch0 = 0.5f;
static const float kSurfaceZoom0  = 1.0f;

struct SurfaceModel {
    int   w = 0, h = 0;                        /* the display grid, decimated */
    std::vector<dyt_surface_vertex_t> verts;   /* z == temperature, + normals */
    std::vector<float>    pos;                 /* display x,y,height per vertex */
    std::vector<uint32_t> idx;                 /* (w-1)*(h-1)*6 */
    std::vector<uint8_t>  col;                 /* palette RGB per vertex */

    /* The orbit camera.  Kept on the model, not a renderer, so switching
     * backend or resizing does not move the view. */
    float yaw = kSurfaceYaw0, pitch = kSurfacePitch0, zoom = kSurfaceZoom0;

    /* The Windows 3D page's height mode: Morphological Change (the window)
     * or Color Changes (the grid).  Held here rather than in the widget
     * because it is an input to the mesh, not to the drawing. */
    int mode_ = DYT_SURFACE_SHAPE;

    void set_mode(int m) { mode_ = m; }
    int  mode() const { return mode_; }

    bool empty() const { return verts.empty() || idx.empty(); }
    long tri_count() const { return (long)(idx.size() / 3); }
    long vertex_count() const { return (long)verts.size(); }

    void reset_camera()
    {
        yaw = kSurfaceYaw0; pitch = kSurfacePitch0; zoom = kSurfaceZoom0;
    }

    void clear()
    {
        w = h = 0;
        verts.clear(); pos.clear(); idx.clear(); col.clear();
    }

    /* Decimate `src` to at most kSurfaceMaxDim on either axis, build the mesh,
     * and colour it from the palette.  `lo`/`hi` are the same display range
     * the 2D view uses, so a vertex's colour is the colour that pixel would
     * have in the canvas.  A grid below 2x2 has no cells, so the model is
     * left empty rather than handed a degenerate mesh. */
    void set_grid(const float *src, int sw, int sh,
                  const dyt_palette_t &pal, float lo, float hi)
    {
        clear();
        if (!src || sw < 2 || sh < 2)
            return;

        int dw = std::max(2, std::min(sw, kSurfaceMaxDim));
        int dh = std::max(2, std::min(sh, kSurfaceMaxDim));

        /* Stride-subsample, pinning both ends, so the extremes survive and the
         * grid keeps its aspect. */
        std::vector<float> small((size_t)dw * dh);
        for (int j = 0; j < dh; j++) {
            const int sj = (int)((long)j * (sh - 1) / (dh - 1));
            for (int i = 0; i < dw; i++) {
                const int si = (int)((long)i * (sw - 1) / (dw - 1));
                small[(size_t)j * dw + i] = src[(size_t)sj * sw + si];
            }
        }

        w = dw;
        h = dh;

        /* The grid's own finite extremes, read off the decimated copy — the
         * same numbers dyt_surface_vertices() would compute, but needed before
         * the mesh exists so the height range can be chosen first. */
        float gzlo = 0.0f, gzhi = 0.0f;
        bool  seen = false;
        for (size_t k = 0; k < small.size(); k++) {
            const float z = small[k];
            if (!std::isfinite(z))
                continue;
            if (!seen || z < gzlo) gzlo = z;
            if (!seen || z > gzhi) gzhi = z;
            seen = true;
        }

        /* Which range the height is normalised across is the Windows page's
         * two modes: the display window (Morphological Change, its default)
         * or the grid's own extremes (Color Changes).  The colour is the
         * window's either way, so only the shape moves. */
        float zlo = gzlo, zhi = gzhi;
        if (seen && dyt_surface_z_range(mode_, gzlo, gzhi, lo, hi,
                                        &zlo, &zhi) != 0) {
            zlo = gzlo;
            zhi = gzhi;
        }

        verts.resize((size_t)w * h);
        idx.resize((size_t)dyt_surface_index_count(w, h));
        if (dyt_surface_vertices_range(small.data(), w, h, zlo, zhi - zlo,
                                       verts.data()) != 0 ||
            dyt_surface_indices(w, h, idx.data()) != 0) {
            clear();
            return;
        }

        /* Display positions: x,y are already in [-1, 1]; the height is the
         * temperature normalised to [0, 1] over the same range the engine's
         * normals were computed from, so the shading describes the shape that
         * is actually drawn.  dyt_surface_hnorm() is the one definition of
         * that height — re-deriving it here would let the two clamp
         * differently. */
        const float zrange = zhi - zlo;

        pos.resize(verts.size() * 3);
        col.resize(verts.size() * 3);
        for (size_t k = 0; k < verts.size(); k++) {
            pos[k * 3 + 0] = verts[k].x;
            pos[k * 3 + 1] = verts[k].y;
            pos[k * 3 + 2] = dyt_surface_hnorm(verts[k].z, zlo, zrange);

            /* The same lookup the canvas uses, so the two views agree about
             * what a temperature looks like. */
            const int pi = dyt_palette_index(verts[k].z, lo, hi);
            col[k * 3 + 0] = pal.rgb[pi * 3 + 0];
            col[k * 3 + 1] = pal.rgb[pi * 3 + 1];
            col[k * 3 + 2] = pal.rgb[pi * 3 + 2];
        }
    }
};

/* The light the surface is shaded with, in mesh space.  Mesh space, not view
 * space, on purpose: the shading then describes the shape (peaks lit, slopes
 * turned away) and does not change as the user orbits — and both renderers get
 * it without either having to reconstruct a rotation matrix. */
static const float kSurfaceLight[3] = { 0.35f, 0.35f, 0.87f };

/* The software renderer: project the mesh, sort the triangles back to front,
 * and fill each with its palette colour shaded by its normal.  Slow enough
 * that the model is decimated, fast enough to orbit, and it needs no context
 * at all — which is the whole point of it existing. */
class SoftSurface : public QWidget {
public:
    explicit SoftSurface(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAutoFillBackground(false);
    }

    void set_model(const SurfaceModel *m) { m_ = m; update(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(0x0f, 0x12, 0x18));

        if (!m_ || m_->empty() || width() < 2 || height() < 2) {
            p.setPen(QColor(0xc8, 0xc8, 0xc8));
            p.drawText(rect(), Qt::AlignCenter,
                       QStringLiteral("No thermal frame yet."));
            return;
        }

        const float aspect = (float)width() / (float)height();
        float mvp[16];
        dyt_surface_mvp(m_->yaw, m_->pitch, m_->zoom, aspect, mvp);

        const int nv = (int)m_->verts.size();
        std::vector<float> sx(nv), sy(nv), sz(nv);
        for (int k = 0; k < nv; k++)
            dyt_surface_project(mvp, m_->pos[k * 3 + 0], m_->pos[k * 3 + 1],
                                m_->pos[k * 3 + 2], &sx[k], &sy[k], &sz[k]);

        /* Painter's algorithm: the camera looks down -z, so the most negative
         * clip z is the farthest triangle and goes first. */
        const int nt = (int)m_->tri_count();
        std::vector<int>   order(nt);
        std::vector<float> depth(nt);
        for (int t = 0; t < nt; t++) {
            order[t] = t;
            const uint32_t a = m_->idx[t * 3 + 0], b = m_->idx[t * 3 + 1],
                           c = m_->idx[t * 3 + 2];
            depth[t] = (sz[a] + sz[b] + sz[c]) / 3.0f;
        }
        std::sort(order.begin(), order.end(),
                  [&](int a, int b) { return depth[a] < depth[b]; });

        const auto to_x = [&](float n) { return (n * 0.5f + 0.5f) * width(); };
        const auto to_y = [&](float n) { return (1.0f - (n * 0.5f + 0.5f)) * height(); };

        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        for (int oi = 0; oi < nt; oi++) {
            const int      t  = order[oi];
            const uint32_t ia = m_->idx[t * 3 + 0], ib = m_->idx[t * 3 + 1],
                           ic = m_->idx[t * 3 + 2];

            /* Flat shade from the face's averaged vertex normal. */
            const float nx = m_->verts[ia].nx + m_->verts[ib].nx + m_->verts[ic].nx;
            const float ny = m_->verts[ia].ny + m_->verts[ib].ny + m_->verts[ic].ny;
            const float nz = m_->verts[ia].nz + m_->verts[ib].nz + m_->verts[ic].nz;
            const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
            float lam = 0.0f;
            if (nl > 0.0f)
                lam = (nx * kSurfaceLight[0] + ny * kSurfaceLight[1] +
                       nz * kSurfaceLight[2]) / nl;
            lam = std::max(0.0f, lam);
            const float shade = 0.35f + 0.65f * lam;

            const int r = (int)((m_->col[ia * 3 + 0] + m_->col[ib * 3 + 0] +
                                 m_->col[ic * 3 + 0]) / 3.0f * shade);
            const int g = (int)((m_->col[ia * 3 + 1] + m_->col[ib * 3 + 1] +
                                 m_->col[ic * 3 + 1]) / 3.0f * shade);
            const int b = (int)((m_->col[ia * 3 + 2] + m_->col[ib * 3 + 2] +
                                 m_->col[ic * 3 + 2]) / 3.0f * shade);

            QPolygonF tri;
            tri << QPointF(to_x(sx[ia]), to_y(sy[ia]))
                << QPointF(to_x(sx[ib]), to_y(sy[ib]))
                << QPointF(to_x(sx[ic]), to_y(sy[ic]));
            p.setBrush(QColor(std::min(255, r), std::min(255, g), std::min(255, b)));
            p.drawPolygon(tri);
        }
    }

private:
    const SurfaceModel *m_ = nullptr;
};

/* The GL renderer.  It uploads the model's mesh once per change and draws it
 * with the same matrix the software path uses.  A shader that will not compile
 * or link is reported through on_gl_status so the container can fall back,
 * rather than leaving a blank widget behind.
 *
 * Compiled only when Qt6OpenGLWidgets was found; without it the container
 * simply has no GL renderer and always uses the software one. */
#ifdef DYT_HAVE_QT6_OPENGL
class GLSurface : public QOpenGLWidget, protected QOpenGLFunctions {
public:
    explicit GLSurface(QWidget *parent = nullptr) : QOpenGLWidget(parent)
    {
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    std::function<void(bool)> on_gl_status;

    void set_model(const SurfaceModel *m) { m_ = m; dirty_ = true; update(); }

protected:
    void initializeGL() override
    {
        initializeOpenGLFunctions();
        glEnable(GL_DEPTH_TEST);
        glClearColor(0.06f, 0.07f, 0.09f, 1.0f);

        /* GLSL 1.10 (the default Qt compiles without a #version), so this
         * works on the compatibility profile a plain desktop context gives
         * and on ES2.  A core-profile context would reject attribute/varying,
         * which is one of the ways ok_ ends up false and the container falls
         * back to the software renderer. */
        static const char *kVert =
            "attribute vec3 a_pos;\n"
            "attribute vec3 a_nrm;\n"
            "attribute vec3 a_col;\n"
            "uniform mat4 u_mvp;\n"
            "uniform vec3 u_light;\n"
            "varying vec3 v_col;\n"
            "void main() {\n"
            "    float lam = max(dot(normalize(a_nrm), u_light), 0.0);\n"
            "    v_col = a_col * (0.35 + 0.65 * lam);\n"
            "    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
            "}\n";
        static const char *kFrag =
            "varying vec3 v_col;\n"
            "void main() { gl_FragColor = vec4(v_col, 1.0); }\n";

        ok_ = prog_.addShaderFromSourceCode(QOpenGLShader::Vertex, kVert) &&
              prog_.addShaderFromSourceCode(QOpenGLShader::Fragment, kFrag) &&
              prog_.link();
        if (ok_) {
            vbo_ = new QOpenGLBuffer(QOpenGLBuffer::VertexBuffer);
            ibo_ = new QOpenGLBuffer(QOpenGLBuffer::IndexBuffer);
            vao_ = new QOpenGLVertexArrayObject;
            vbo_->create();
            ibo_->create();
            vao_->create();
        }
        dirty_ = true;
        if (on_gl_status)
            on_gl_status(ok_);
    }

    void resizeGL(int w, int h) override { glViewport(0, 0, w, h); }

    void paintGL() override
    {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (!ok_ || !m_ || m_->empty())
            return;
        if (dirty_)
            upload();

        const float aspect =
            height() > 0 ? (float)width() / (float)height() : 1.0f;
        float mvp[16];
        dyt_surface_mvp(m_->yaw, m_->pitch, m_->zoom, aspect, mvp);
        QMatrix4x4 qm;
        std::memcpy(qm.data(), mvp, sizeof mvp);

        prog_.bind();
        prog_.setUniformValue("u_mvp", qm);
        prog_.setUniformValue("u_light",
                              QVector3D(kSurfaceLight[0], kSurfaceLight[1],
                                        kSurfaceLight[2]));
        vao_->bind();
        glDrawElements(GL_TRIANGLES, (GLsizei)m_->idx.size(),
                       GL_UNSIGNED_INT, nullptr);
        vao_->release();
        prog_.release();
    }

private:
    /* Interleaved pos(3) + normal(3) + colour(3), rebuilt only when the model
     * changes.  The element buffer must stay bound while the VAO is bound —
     * releasing it first would clear the VAO's element binding. */
    void upload()
    {
        dirty_ = false;
        const int n = (int)m_->verts.size();
        std::vector<float> buf((size_t)n * 9);
        for (int k = 0; k < n; k++) {
            float *d = &buf[(size_t)k * 9];
            d[0] = m_->pos[k * 3 + 0];
            d[1] = m_->pos[k * 3 + 1];
            d[2] = m_->pos[k * 3 + 2];
            d[3] = m_->verts[k].nx;
            d[4] = m_->verts[k].ny;
            d[5] = m_->verts[k].nz;
            d[6] = m_->col[k * 3 + 0] / 255.0f;
            d[7] = m_->col[k * 3 + 1] / 255.0f;
            d[8] = m_->col[k * 3 + 2] / 255.0f;
        }

        const int stride = 9 * (int)sizeof(float);
        vao_->bind();
        vbo_->bind();
        vbo_->setUsagePattern(QOpenGLBuffer::StaticDraw);
        vbo_->allocate(buf.data(), (int)(buf.size() * sizeof(float)));
        prog_.enableAttributeArray("a_pos");
        prog_.setAttributeBuffer("a_pos", GL_FLOAT, 0, 3, stride);
        prog_.enableAttributeArray("a_nrm");
        prog_.setAttributeBuffer("a_nrm", GL_FLOAT, 3 * (int)sizeof(float), 3, stride);
        prog_.enableAttributeArray("a_col");
        prog_.setAttributeBuffer("a_col", GL_FLOAT, 6 * (int)sizeof(float), 3, stride);
        vbo_->release();
        ibo_->bind();
        ibo_->setUsagePattern(QOpenGLBuffer::StaticDraw);
        ibo_->allocate(m_->idx.data(),
                       (int)(m_->idx.size() * sizeof(uint32_t)));
        vao_->release();
    }

    const SurfaceModel *m_ = nullptr;
    QOpenGLShaderProgram prog_;
    QOpenGLBuffer *vbo_ = nullptr;
    QOpenGLBuffer *ibo_ = nullptr;
    QOpenGLVertexArrayObject *vao_ = nullptr;
    bool ok_ = false;
    bool dirty_ = true;
};
#endif /* DYT_HAVE_QT6_OPENGL */

/* The container: one model, both renderers, and whichever of them is usable.
 * It owns the mouse (the children are transparent to it) so a drag orbits and
 * the wheel zooms no matter which backend is live. */
class SurfaceView : public QWidget {
public:
    explicit SurfaceView(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFocusPolicy(Qt::NoFocus);
        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);

        /* The software renderer is always built: it is the fallback, and on a
         * host without Qt6OpenGLWidgets it is the only one there is. */
        sw_ = new SoftSurface(this);
        lay->addWidget(sw_);
        sw_->set_model(&model_);

#ifdef DYT_HAVE_QT6_OPENGL
        /* GL is the first choice, laid over the software renderer, which is
         * already there — so a failed context is a hide, not a rebuild. */
        gl_ = new GLSurface(this);
        lay->addWidget(gl_);
        sw_->setVisible(false);
        gl_->set_model(&model_);
        gl_->on_gl_status = [this](bool ok) {
            use_gl_ = ok;
            gl_->setVisible(use_gl_);
            sw_->setVisible(!use_gl_);
            update();
        };
#else
        use_gl_ = false;
#endif
    }

    /* Feed a new frame.  `lo`/`hi` are the display range the canvas is using,
     * so a vertex's colour matches the pixel it came from. */
    void set_grid(const float *temps, int w, int h,
                  const dyt_palette_t &pal, float lo, float hi)
    {
        model_.set_grid(temps, w, h, pal, lo, hi);
        sw_->set_model(&model_);
        gl_set_model();
    }

    /* The height mode.  The normalisation is baked into the mesh's positions,
     * so a change needs the next frame to rebuild it — which is the pump's
     * job, exactly as it is for a range change on the canvas.  The update()
     * here is only so the widget repaints promptly. */
    void set_mode(int m)
    {
        if (model_.mode() == m)
            return;
        model_.set_mode(m);
        sw_->update();
    }
    int mode() const { return model_.mode(); }

    void set_camera(float yaw, float pitch, float zoom)
    {
        model_.yaw = yaw;
        model_.pitch = pitch;
        model_.zoom = zoom;
        sw_->update();
        gl_update();
    }

    void reset_camera()
    {
        model_.reset_camera();
        sw_->update();
        gl_update();
    }

    const SurfaceModel &model() const { return model_; }
    bool using_gl() const { return use_gl_; }

    /* Whether a GL context actually came up.  False when Qt6OpenGLWidgets was
     * absent, the context could not be created, or the shader would not link —
     * the exact conditions the software renderer exists for. */
    bool gl_ready() const
    {
#ifdef DYT_HAVE_QT6_OPENGL
        return gl_ && gl_->isValid();
#else
        return false;
#endif
    }

    /* The software renderer, for the selftest to paint and read back.  It is
     * always present — it is the fallback — so a check on it holds on any
     * host, whether or not GL is available. */
    QWidget *software_view() const { return sw_; }

protected:
    void mousePressEvent(QMouseEvent *e) override { last_ = e->pos(); }

    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (!(e->buttons() & Qt::LeftButton))
            return;
        const float dx = (float)(e->pos().x() - last_.x()) * 0.01f;
        const float dy = (float)(e->pos().y() - last_.y()) * 0.01f;
        last_ = e->pos();
        set_camera(model_.yaw + dx,
                   std::max(-1.4f, std::min(1.4f, model_.pitch + dy)),
                   model_.zoom);
    }

    void wheelEvent(QWheelEvent *e) override
    {
        const float step = e->angleDelta().y() > 0 ? 1.1f : 1.0f / 1.1f;
        set_camera(model_.yaw, model_.pitch,
                   std::max(0.3f, std::min(6.0f, model_.zoom * step)));
    }

private:
    /* The GL renderer may not exist at all (no Qt6OpenGLWidgets), so every
     * call to it goes through these — one #ifdef each instead of one per call
     * site. */
    void gl_set_model()
    {
#ifdef DYT_HAVE_QT6_OPENGL
        if (gl_)
            gl_->set_model(&model_);
#endif
    }
    void gl_update()
    {
#ifdef DYT_HAVE_QT6_OPENGL
        if (gl_)
            gl_->update();
#endif
    }

    SurfaceModel model_;
    SoftSurface *sw_ = nullptr;
#ifdef DYT_HAVE_QT6_OPENGL
    GLSurface   *gl_ = nullptr;
#endif
    bool         use_gl_ = true;
    QPoint       last_;
};

/* ---------------------------------------------------------- the control panel
 *
 * The Windows counterpart's right panel (manual p.5, /tmp/pdfx/w-08.png): a
 * narrow tabbed column of grouped controls.  Only the groups the engine backs
 * are present, so nothing on screen is a control that does nothing.
 *
 * Every row runs the same handle_key the keyboard does (through on_key), so a
 * button is a second route to the same action, never a second implementation.
 * The checked state comes from the snapshot in sync(), never from the button's
 * own toggle, for the same reason MainWindow::sync_actions() does it: a key the
 * session refused must not leave a button lit.
 *
 * No control takes focus (Qt::NoFocus): a focused button swallows the keys
 * before keyPressEvent sees them, which would break every binding the moment
 * someone clicked one — including the armed-parameter swallow.
 *
 * The row glyphs are QPainter vectors, like the rail's: the package ships no
 * icon assets.
 */
class ControlPanel : public QWidget {
public:
    enum Id {
        Spot = 0, Line, Rect, Poly, ToolNone,
        AnalysisLine, AnalysisChart,
        Tracking, Alarm, Highlight,
        FlipH, FlipV, FixedRange,
        RapidDiag,
        LeakShort, LeakLarge, LeakSmall,
        SurfShape, SurfColor,
        MarkTextRow,
        Still, Record, Gallery,
        SrOff, SrVisible, SrThermal,
        N_Ids
    };

    /* The tabs, in the Windows panel's order (Troubleshoot | 3D Analysis |
     * Comparison | Circuit Design), with the port's own Super Resolution last
     * so the vendor's order is preserved and the addition is visibly an
     * addition.  Named rather than numbered: the rail's Compare item and the
     * selftest both switch to a tab, and a bare index would drift the moment a
     * tab is inserted ahead of them — which is exactly what happened when 3D
     * Analysis landed. */
    enum Tab {
        TabTroubleshoot = 0, TabAnalysis3D, TabComparison, TabCircuitDesign,
        TabSuperResolution,
        TabCount
    };

    explicit ControlPanel(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedWidth(kPanelW);
        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);

        tabs_ = new QTabWidget(this);
        tabs_->setFocusPolicy(Qt::NoFocus);
        outer->addWidget(tabs_);
        tabs_->addTab(build_troubleshoot(), QStringLiteral("Troubleshoot"));
        tabs_->addTab(build_analysis_3d(), QStringLiteral("3D Analysis"));
        tabs_->addTab(build_comparison(), QStringLiteral("Comparison"));
        tabs_->addTab(build_circuit_design(), QStringLiteral("Circuit Design"));
        tabs_->addTab(build_super_resolution(),
                      QStringLiteral("Super Resolution"));

        /* The circuit mode's, the height mode's and the Mark tool's checkmarks
         * are this panel's own state, so they have to be lit from the same
         * sources sync() uses — a frame will not arrive before the first paint
         * on the fixture path. */
        sync_leak_marks();
        sync_surface_marks();
        sync_mark_marks(MarkNone);
    }

    /* A click on the control whose key is `key`.  Set by MainWindow, which owns
     * the session and the one handle_key dispatch. */
    std::function<void(int)> on_key;

    QPushButton *button(Id i) const
    {
        return (i >= 0 && i < N_Ids) ? btn_[i] : nullptr;
    }
    QTabWidget *tabs() const { return tabs_; }

    /* The 3D Analysis view.  The pump owns the session and the per-frame
     * Celsius plane, so it is the pump that feeds the grid; the panel just
     * holds the widget.  A NULL return means the page is not built, which the
     * pump guards for the way it guards every other optional widget. */
    SurfaceView *surface() const { return surface_; }

    /* The Comparison tab's hooks.  The pump (which owns the session and the
     * reference grid) installs these: Load opens a still, Clear drops it,
     * and Threshold is read live on each frame.  The panel is the view; the
     * pump is the model — the same split on_key takes. */
    std::function<void()> on_load_reference_;
    std::function<void()> on_clear_reference_;
    std::function<void()> on_threshold_changed_;

    /* Push the reference path and the pre-computed difference stats into the
     * page.  The text is the pump's to format: it owns the session and the
     * reference grid, so it runs dyt_compare_stats() and hands the result in
     * as a string, exactly the way sync_sr() takes a mode the session holds. */
    void sync_compare(const QString &path, bool have_ref, const QString &stats)
    {
        if (ref_path_)
            ref_path_->setText(path.isEmpty() ? QStringLiteral("(none)")
                                             : path);
        if (ref_stats_)
            ref_stats_->setText(stats);
        /* The threshold spin is always live — the pump recomputes on change —
         * but the stats box is empty until a reference is loaded, so a user
         * sees the controls are there, not a page that looks broken. */
        (void)have_ref;
    }

    double compare_threshold() const
    {
        return ref_thresh_ ? ref_thresh_->value() : 2.0;
    }

    /* The Circuit Design tab's hooks.  The pump/window owns the canvas, so it
     * is what loads the image and pushes the alignment into FrameView; the
     * panel is the view, exactly the split the Comparison hooks take.
     * `on_layout_changed_` fires for the align spins, the opacity slider and
     * the show toggle alike, so the window has one push to make. */
    std::function<void()> on_load_layout_;
    std::function<void()> on_clear_layout_;
    std::function<void()> on_layout_changed_;

    /* What the layout controls are set to, as the push reads them.  Public so
     * the window (and the selftest) can drive the panel→canvas contract
     * without reaching into private members. */
    int  layout_dx() const { return layout_spin_x_ ? layout_spin_x_->value() : 0; }
    int  layout_dy() const { return layout_spin_y_ ? layout_spin_y_->value() : 0; }
    int  layout_opacity() const
    {
        return layout_opacity_ ? layout_opacity_->value() : 100;
    }
    bool layout_shown() const
    {
        return layout_show_ ? layout_show_->isChecked() : false;
    }
    /* The align fields, so the selftest can pin their ±DYT_FUSION_ALIGN_MAX
     * clamp — the same limit the vendor's fusion alignment uses. */
    QSpinBox *layout_spin_x() const { return layout_spin_x_; }
    QSpinBox *layout_spin_y() const { return layout_spin_y_; }

    /* The High TEMP. Alarm threshold.  The panel owns the field and reports
     * changes through `on_alarm_setpoint_changed_`; the window pushes the value
     * into the canvas, which is what arms at it (push_alarm). */
    std::function<void()> on_alarm_setpoint_changed_;
    double alarm_setpoint() const
    {
        return alarm_spin_ ? alarm_spin_->value()
                           : (double)DYT_ALARM_SETPOINT_DEFAULT;
    }
    /* The field itself, so the selftest can pin its clamp — the same
     * DYT_ALARM_SETPOINT_MIN/MAX the engine's helper enforces. */
    QDoubleSpinBox *alarm_spin() const { return alarm_spin_; }

    /* Push the loaded layout's path (or "(none)") into the page, the way
     * sync_compare() pushes the reference path. */
    void sync_layout(const QString &path)
    {
        if (layout_path_)
            layout_path_->setText(path.isEmpty() ? QStringLiteral("(none)")
                                                 : path);
    }

    /* Turn the page's show toggle on or off.  Public because the window is
     * what loads an image (a dialog or --layout-image), and loading one that
     * stayed hidden because the toggle happened to be off would look like the
     * load failed.  Signals are blocked: the caller pushes afterwards, and a
     * signal here would push twice. */
    void set_layout_shown(bool on)
    {
        if (!layout_show_)
            return;
        const QSignalBlocker block(layout_show_);
        layout_show_->setChecked(on);
    }

    /* The panel is as tall as its current page's content.
     *
     * QScrollArea::sizeHint() with widgetResizable() reports a small default
     * rather than the widget's own, so the window would come up short — and
     * because widgetResizable() resizes the page to the viewport, a page taller
     * than that is neither scrolled nor fitted: it is silently clipped, and the
     * last group becomes a control the user cannot reach.  Asking the page's
     * own layout for what it needs is the fix; the scroll area then saves the
     * user when the window is deliberately made smaller than that.
     *
     * The *current* page, not the tallest: the window is sized once, when the
     * first frame arrives, and never re-fits on a tab switch, so the height
     * stays put and only the opening tab has to fit.  Troubleshoot is both
     * first and tallest, so nothing is hidden at start-up either way. */
    QSize sizeHint() const override
    {
        QSize s(kPanelW, 0);
        if (tabs_ && tabs_->currentWidget()) {
            QWidget *page = tabs_->currentWidget();
            if (auto *sa = qobject_cast<QScrollArea *>(page))
                if (sa->widget())
                    page = sa->widget();
            s.setHeight(tabs_->tabBar()->sizeHint().height() +
                        page->sizeHint().height());
        }
        return s;
    }

    /* …but it may be squeezed.  The height hint above is a preference, not a
     * floor: without this the panel's minimum would be its content's, the
     * window could not be made shorter than the tallest page, and the scroll
     * area would be inert — a window that cannot be shrunk small enough for the
     * screen is its own trap.  The tab bar plus one row is enough to see that
     * the panel is there; the scroll area is what keeps every group reachable
     * below that. */
    QSize minimumSizeHint() const override
    {
        return QSize(kPanelW, (tabs_ ? tabs_->tabBar()->sizeHint().height() : 0)
                              + 48);
    }

    /* Check a row without letting the button's own signal fire: these are
     * derived from state, not a user gesture, so a checkmark that ran the row's
     * action would re-enter the mode it is reporting. */
    static void set_checked(QPushButton *b, bool on)
    {
        if (!b)
            return;
        const QSignalBlocker block(b);
        b->setChecked(on);
    }

    void sync_leak_marks()
    {
        set_checked(btn_[LeakShort], leak_mode_ == DYT_LEAK_SHORT);
        set_checked(btn_[LeakLarge], leak_mode_ == DYT_LEAK_LARGE);
        set_checked(btn_[LeakSmall], leak_mode_ == DYT_LEAK_SMALL);
    }

    void sync_surface_marks()
    {
        const int m = surface_ ? surface_->mode() : DYT_SURFACE_SHAPE;
        set_checked(btn_[SurfShape], m == DYT_SURFACE_SHAPE);
        set_checked(btn_[SurfColor], m == DYT_SURFACE_COLOR);
    }

    /* The Mark tool's row.  `mode` is a MarkKind, held by the canvas (the
     * marks are the front end's, so the canvas owns the tool that places
     * them); this only mirrors it onto the row.  It shares the tools' button
     * group, so checking it clears whichever tool row was lit. */
    void sync_mark_marks(int mode)
    {
        mark_mode_ = mode;
        set_checked(btn_[MarkTextRow], mode == MarkText);
    }

    /* The 3D page's height mode.  Held by the SurfaceView (it is an input to
     * the mesh), so this only forwards and re-lights the rows; the pump
     * rebuilds the grid on its next frame. */
    void set_surface_mode(int m)
    {
        if (surface_)
            surface_->set_mode(m);
        sync_surface_marks();
    }

    /* Bring every checkmark up to date with the frame just painted.  The
     * session-derived states come from `snap`; the five that live on the
     * canvas, the window or this panel (the marker toggle, a running clip, an
     * open gallery, the circuit mode, the Mark tool) are passed in or held
     * here, because they are not in the snapshot. */
    void sync(const dyt_snapshot_t &snap, bool hot_shown, bool recording,
              bool gallery_open, int mark_mode)
    {
        sync_mark_marks(mark_mode);
        set_checked(btn_[Spot],      snap.tool == DYT_TOOL_POINT);
        set_checked(btn_[Line],      snap.tool == DYT_TOOL_LINE);
        set_checked(btn_[Rect],      snap.tool == DYT_TOOL_BOX);
        set_checked(btn_[Poly],      snap.tool == DYT_TOOL_POLYGON);
        /* A Mark tool is a tool: arming one puts the session's tool to NONE
         * (the vendor's single `cursor_code` does the same), so the Mark row
         * and the Tool None row must never both be lit.  Both are in the one
         * exclusive group, and the Mark row was set above, so this only has to
         * decline to light Tool None. */
        set_checked(btn_[ToolNone],  snap.tool == DYT_TOOL_NONE &&
                                     mark_mode_ == MarkNone);
        /* The Analysis pair tracks the chart's mode, not the tool: "Chart
         * analysis" leaves the line tool selected, so a tool-derived checkmark
         * would light the wrong row. */
        set_checked(btn_[AnalysisLine],  plot_ && plot_->mode() == ProfilePlot::Line);
        set_checked(btn_[AnalysisChart], plot_ && plot_->mode() == ProfilePlot::Analysis);
        set_checked(btn_[Tracking],  hot_shown);
        set_checked(btn_[Alarm],     snap.alarm_on != 0);
        set_checked(btn_[Highlight], snap.iso_on != 0);
        set_checked(btn_[FlipH],     snap.xform.flip_h != 0);
        set_checked(btn_[FlipV],     snap.xform.flip_v != 0);
        set_checked(btn_[FixedRange], snap.range_mode != DYT_RANGE_AUTO);
        set_checked(btn_[Record],    recording);
        set_checked(btn_[Gallery],   gallery_open);
        sync_leak_marks();
        sync_surface_marks();
    }

    /* The circuit mode is this panel's own state — nothing in the snapshot
     * says which of the three rows is lit — so it is held here and the rows
     * are driven from it.  MainWindow::apply_leak_mode() is the one caller;
     * it also applies the mode's effect to the session. */
    void set_leak_mode(int mode)
    {
        leak_mode_ = mode;
        sync_leak_marks();
    }
    int leak_mode() const { return leak_mode_; }


    /* The Super Resolution page.  Both facts it shows are session state, not
     * frame state, so it takes them directly rather than a snapshot: the
     * capability comes from the session and is known before the first frame,
     * and the mode is what the session is holding even if the render has not
     * had a chance to apply it yet.  That is what lets the page be correct
     * while the canvas still shows the placeholder. */
    void sync_sr(bool model_loaded, dyt_sr_t mode)
    {
        sr_mode_ = mode;
        auto set = [](QPushButton *b, bool on) {
            if (!b)
                return;
            const QSignalBlocker block(b);
            b->setChecked(on);
        };
        set(btn_[SrOff],     mode == DYT_SR_OFF);
        set(btn_[SrVisible], mode == DYT_SR_VISIBLE);
        set(btn_[SrThermal], mode == DYT_SR_THERMAL);

        /* A mode with no model behind it cannot be held — the session refuses
         * it — so the three radios are disabled rather than left clickable and
         * silently ineffective. */
        for (Id i : { SrOff, SrVisible, SrThermal })
            if (btn_[i])
                btn_[i]->setEnabled(model_loaded);

        if (!sr_status_)
            return;
        if (!model_loaded) {
            sr_status_->setText(
                QStringLiteral("No model loaded — super-resolution is "
                               "unavailable.\nInstall zoom2.mnn beside the app "
                               "or pass --model PATH."));
        } else {
            sr_status_->setText(QStringLiteral("Model loaded · mode: %1%2")
                .arg(QString::fromUtf8(dyt_sr_name(mode)),
                     mode == DYT_SR_OFF ? QString()
                                        : QStringLiteral(" (2x)")));
        }
    }

    /* The page's live status line, for the selftest to read back. */
    QLabel *sr_status() const { return sr_status_; }

    /* ---- the Analysis chart ---------------------------------------------
     * The mode is a *panel* flag rather than a session one: the chart is a way
     * of looking at the measurement, not a measurement, and the two rows that
     * select it are the only things that read it.  The window sets it from the
     * same keys the rows send, so a click and a key cannot disagree. */
    ProfilePlot *plot() const { return plot_; }

    void set_chart_mode(ProfilePlot::Mode m)
    {
        if (plot_)
            plot_->set_mode(m);
    }

    /* Feed the chart: the profile (n samples, NaN for an off-image point, or
     * NULL for none) and the unit its labels are drawn in.  Called once per
     * painted frame with the profile the window just read from the session, so
     * the widget itself holds no session and takes no lock. */
    void sync_analysis(const dyt_snapshot_t &snap, const float *line, int n)
    {
        if (plot_)
            plot_->set_profile(line, n, snap.unit);
    }

    /* The key that turns super-resolution off from the mode the session is
     * holding.  Each of 'z' and 'Z' toggles *its own* plane, so neither alone
     * means "off" from every mode — from thermal, 'z' would select visible
     * rather than clear.  Off therefore presses the key of the mode it is
     * leaving, which is also why it needs a callback rather than a fixed key. */
    int sr_off_key() const
    {
        return sr_mode_ == DYT_SR_THERMAL ? 'Z' : 'z';
    }

private:
    QGroupBox *group(const QString &title)
    {
        auto *g = new QGroupBox(title, this);
        auto *lay = new QVBoxLayout(g);
        lay->setContentsMargins(6, 4, 6, 6);
        lay->setSpacing(3);
        return g;
    }

    /* A row on a panel page.  Normally the click runs the same dispatch as the
     * key named in `key`, which is the rule that keeps the panel from becoming
     * a second implementation.  `act` is for the rare control whose key cannot
     * express it: 'z' and 'Z' each *toggle their own plane*, so neither alone
     * means "off" from every mode, and the Off row has to ask the session
     * which key means "off" from where it is. */
    QPushButton *row(QGroupBox *g, Id id, const QString &text, int key,
                     bool checkable, std::function<void()> act = {},
                     QBoxLayout *into = nullptr)
    {
        auto *b = new QPushButton(text, g);
        b->setObjectName(QStringLiteral("row"));
        b->setFocusPolicy(Qt::NoFocus);
        b->setCheckable(checkable);
        b->setIconSize(QSize(18, 18));
        QPixmap pm(18, 18);
        pm.fill(Qt::transparent);
        {   QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing, true);
            paint_icon(p, id, QRect(0, 0, 18, 18)); }
        b->setIcon(QIcon(pm));
        b->setToolTip(QStringLiteral("key: %1")
                          .arg(QChar((char)key).toUpper()));
        connect(b, &QPushButton::clicked, [this, key, act]() {
            if (act) {
                act();
                return;
            }
            if (on_key)
                on_key(key);
        });
        /* `into` is for the one row that shares its line with a field (the
         * alarm and its threshold); everything else goes straight down the
         * group's column. */
        if (into)
            into->addWidget(b);
        else
            qobject_cast<QVBoxLayout *>(g->layout())->addWidget(b);
        btn_[id] = b;
        return b;
    }

    QWidget *build_troubleshoot()
    {
        auto *page = new QWidget;
        auto *lay  = new QVBoxLayout(page);
        lay->setContentsMargins(6, 6, 6, 6);
        lay->setSpacing(8);

        /* Circuit mode.  First on the page because it governs everything below
         * it: the vendor's radios sit with the tool buttons for the same
         * reason, and one of the three (Small Current Leakage) turns the
         * measurement tools off entirely (:22345).  The labels and their order
         * are the shipped ones — "Short-circuit", "Large Current Leakage",
         * "Small Current Leakage" — and the keys are the shift forms, because
         * 's', 'l' and 'm' are already still/line/tracking. */
        QGroupBox *cir = group(QStringLiteral("Circuit Mode"));
        auto *cgrp = new QButtonGroup(this);
        cgrp->setExclusive(true);
        cgrp->addButton(row(cir, LeakShort,
                            QStringLiteral("Short-circuit"), 'S', true));
        cgrp->addButton(row(cir, LeakLarge,
                            QStringLiteral("Large Current Leakage"), 'L', true));
        cgrp->addButton(row(cir, LeakSmall,
                            QStringLiteral("Small Current Leakage"), 'M', true));
        lay->addWidget(cir);

        /* Temperature Measurement — one tool at a time, so an exclusive group
         * keeps the checkmarks consistent with the session's single `tool`. */
        QGroupBox *meas = group(QStringLiteral("Temperature Measurement"));
        auto *mgrp = new QButtonGroup(this);
        mgrp->setExclusive(true);
        for (Id i : { Spot, Line, Rect, Poly, ToolNone })
            mgrp->addButton(row(meas, i, tool_label(i), tool_key(i), true));
        lay->addWidget(meas);

        /* Mark — the annotations, which the vendor's DRAW_CODE carries beside
         * the shape and point editors (:7159).  Its own *group box*, because
         * nothing about a label is measured or written to the device, but the
         * same exclusive button group as the tools above: the vendor has a
         * single `cursor_code`, so exactly one of the six rows is lit and
         * arming a Mark clears the measurement tool (see set_mark_mode).  Two
         * groups would have needed the Tool None row *unchecked* while the Mark
         * row was checked, and Qt refuses to uncheck the checked button of an
         * exclusive group — which is why this is one group and not two. */
        QGroupBox *mark = group(QStringLiteral("Mark"));
        mgrp->addButton(row(mark, MarkTextRow, QStringLiteral("Text"),
                            'T', true));
        lay->addWidget(mark);

        /* Analysis — one chart, two presentations of it, in the reference's
         * order and with its labels.  The reference's manual never describes
         * the panel, so the labels are read literally: "Line" is the line,
         * "Chart analysis" is the analysis *of* it.  Both select the line tool
         * as well, because a line chart with no line has nothing to show, and
         * 'l' is the tool key — so the row and the key do exactly the same
         * thing, which is the rule every other row follows. */
        QGroupBox *ana = group(QStringLiteral("Analysis"));
        auto *agrp = new QButtonGroup(this);
        agrp->setExclusive(true);
        agrp->addButton(row(ana, AnalysisLine, QStringLiteral("Line"),
                            'l', true));
        agrp->addButton(row(ana, AnalysisChart, QStringLiteral("Chart analysis"),
                            'c', true));
        plot_ = new ProfilePlot(ana);
        qobject_cast<QVBoxLayout *>(ana->layout())->addWidget(plot_);
        lay->addWidget(ana);

        QGroupBox *hot = group(QStringLiteral("High Temperature"));
        row(hot, Tracking,  QStringLiteral("High TEMP. Tracking"), 'm', true);

        /* The alarm row shares its line with the threshold it arms at.  The
         * vendor puts the field beside the row (`num_alarm_val` next to
         * `label_alert`), and the port needs it for the same reason: a row
         * that arms an alarm with no way to say *at what temperature* is the
         * gap the Windows panel does not have.  Always enabled rather than
         * enabled-on-arm as the vendor has it — a field you must arm before
         * you can type in is a control the user cannot reach, and the arm uses
         * whatever it holds either way. */
        {
            auto *line = new QHBoxLayout;
            line->setContentsMargins(0, 0, 0, 0);
            line->setSpacing(6);
            row(hot, Alarm, QStringLiteral("High TEMP. Alarm"), 'a', true,
                {}, line);

            alarm_spin_ = new QDoubleSpinBox(hot);
            alarm_spin_->setObjectName(QStringLiteral("row"));
            alarm_spin_->setFocusPolicy(Qt::NoFocus);
            alarm_spin_->setDecimals(1);
            alarm_spin_->setRange((double)DYT_ALARM_SETPOINT_MIN,
                                  (double)DYT_ALARM_SETPOINT_MAX);
            alarm_spin_->setSingleStep(1.0);
            alarm_spin_->setSuffix(QStringLiteral(" \u00b0C"));
            alarm_spin_->setValue((double)DYT_ALARM_SETPOINT_DEFAULT);
            /* Narrow enough that the row keeps most of the line — the vendor's
             * field is a small box beside the row, not a second row. */
            alarm_spin_->setMaximumWidth(96);
            alarm_spin_->setToolTip(
                QStringLiteral("the temperature the alarm trips above "
                               "(%1 to %2 \u00b0C)")
                    .arg((double)DYT_ALARM_SETPOINT_MIN, 0, 'f', 0)
                    .arg((double)DYT_ALARM_SETPOINT_MAX, 0, 'f', 0));
            QObject::connect(alarm_spin_,
                             QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                             this, [this](double) {
                                 if (on_alarm_setpoint_changed_)
                                     on_alarm_setpoint_changed_();
                             });
            line->addWidget(alarm_spin_, 1);
            qobject_cast<QVBoxLayout *>(hot->layout())->addLayout(line);
        }

        row(hot, Highlight, QStringLiteral("Highlight High TEMP. Area"),
            'i', true);
        lay->addWidget(hot);

        QGroupBox *enh = group(QStringLiteral("Image Enhancement"));
        row(enh, FlipH,      QStringLiteral("Flip horizontally"), 'h', true);
        row(enh, FlipV,      QStringLiteral("Flip vertically"),   'H', true);
        row(enh, FixedRange, QStringLiteral("Fixed range"),       't', true);
        /* The vendor's Rapid Diagnostics lives in the LUT group
         * (panel_tool_img_lut holds panel_tool_temp_w, :24729), which is this
         * one, so it belongs beside the range control it moves.  Its key is
         * 'F' — 'f' is fusion and 't' already toggles the range; the shift
         * form is what the port uses for a variant of a lowercase binding
         * ('H' beside 'h'), and "Fast" is the mnemonic. */
        row(enh, RapidDiag,  QStringLiteral("Rapid Diagnostics"), 'F', false);
        lay->addWidget(enh);

        QGroupBox *cap = group(QStringLiteral("Capture"));
        row(cap, Still,   QStringLiteral("Still"),       's', false);
        row(cap, Record,  QStringLiteral("Record clip"), 'v', true);
        row(cap, Gallery, QStringLiteral("Gallery"),     'g', true);
        lay->addWidget(cap);

        lay->addStretch(1);

        /* Scrollable: the panel is taller than a short window, and a control
         * the user cannot reach is the failure the whole on-screen-controls
         * rule exists to prevent. */
        auto *scroll = new QScrollArea(this);
        scroll->setWidget(page);
        scroll->setWidgetResizable(true);
        scroll->setFocusPolicy(Qt::NoFocus);
        scroll->setFrameShape(QFrame::NoFrame);
        return scroll;
    }

    /* The 3D Analysis tab.  The centre is the SurfaceView — a GL renderer
     * with a QPainter fallback, fed the live Celsius grid by the pump.  The
     * page is deliberately *not* a QScrollArea: a scroll area would eat the
     * wheel that zooms the view, and the view wants the whole page anyway.
     * The panel's own sizeHint() already handles a page that is not a scroll
     * area (it only unwraps one when it is). */
    QWidget *build_analysis_3d()
    {
        auto *page = new QWidget;
        auto *lay  = new QVBoxLayout(page);
        lay->setContentsMargins(6, 6, 6, 6);
        lay->setSpacing(8);

        QGroupBox *view = group(QStringLiteral("Surface"));
        auto *vlay = qobject_cast<QVBoxLayout *>(view->layout());
        surface_ = new SurfaceView(view);
        surface_->setObjectName(QStringLiteral("surface3d"));
        surface_->setMinimumHeight(240);
        vlay->addWidget(surface_, 1);
        lay->addWidget(view, 1);

        QGroupBox *cam = group(QStringLiteral("Camera"));
        auto *clay = qobject_cast<QVBoxLayout *>(cam->layout());
        auto *reset = new QPushButton(QStringLiteral("Reset view"), cam);
        reset->setFocusPolicy(Qt::NoFocus);
        reset->setObjectName(QStringLiteral("reset3d"));
        connect(reset, &QPushButton::clicked, [this]() {
            if (surface_)
                surface_->reset_camera();
        });
        clay->addWidget(reset);
        lay->addWidget(cam);

        /* The Windows page's pair of height buttons, with its own labels.  The
         * keys are the shift forms: 'p' is the point tool and 'c' is chart
         * analysis, and handle_key reads them above the fold so neither can
         * fall through. */
        QGroupBox *ht = group(QStringLiteral("Height"));
        auto *hgrp = new QButtonGroup(this);
        hgrp->setExclusive(true);
        hgrp->addButton(row(ht, SurfShape,
                            QStringLiteral("Morphological Change"), 'P', true));
        hgrp->addButton(row(ht, SurfColor,
                            QStringLiteral("Color Changes"), 'C', true));
        lay->addWidget(ht);

        auto *hint = new QLabel(
            QStringLiteral("Drag to rotate, scroll to zoom.  Height is the "
                           "temperature and colour is the active palette, so "
                           "the surface and the canvas agree.  Morphological "
                           "Change scales the height to the display range; "
                           "Color Changes scales it to the frame's own "
                           "extremes and leaves the window to the colour."),
            page);
        hint->setObjectName(QStringLiteral("hint3d"));
        hint->setWordWrap(true);
        hint->setFocusPolicy(Qt::NoFocus);
        lay->addWidget(hint);

        return page;
    }

    /* The Comparison tab.  Two thermal boards side by side: a saved reference
     * still and the live frame, a difference grid between them, and the stats
     * that name where they differ most.  The engine is src/compare.c; this
     * page is the view, the pump (which owns the session) computes the stats
     * and hands them in as text — the same split the Super Resolution page
     * takes (it reports, it does not act on the engine directly). */
    QWidget *build_comparison()
    {
        auto *page = new QWidget;
        auto *lay  = new QVBoxLayout(page);
        lay->setContentsMargins(6, 6, 6, 6);
        lay->setSpacing(8);

        QGroupBox *ref = group(QStringLiteral("Reference"));
        ref_path_ = new QLabel(QStringLiteral("(none)"), ref);
        ref_path_->setWordWrap(true);
        ref_path_->setFocusPolicy(Qt::NoFocus);
        ref_path_->setObjectName(QStringLiteral("refpath"));
        auto *ref_lay = qobject_cast<QVBoxLayout *>(ref->layout());
        ref_lay->addWidget(ref_path_);
        auto *load = new QPushButton(QStringLiteral("Load reference…"), ref);
        load->setFocusPolicy(Qt::NoFocus);
        load->setObjectName(QStringLiteral("loadref"));
        connect(load, &QPushButton::clicked, [this]() {
            if (on_load_reference_)
                on_load_reference_();
        });
        ref_lay->addWidget(load);
        auto *clear = new QPushButton(QStringLiteral("Clear reference"), ref);
        clear->setFocusPolicy(Qt::NoFocus);
        clear->setObjectName(QStringLiteral("clearref"));
        connect(clear, &QPushButton::clicked, [this]() {
            if (on_clear_reference_)
                on_clear_reference_();
        });
        ref_lay->addWidget(clear);
        lay->addWidget(ref);

        QGroupBox *diff = group(QStringLiteral("Difference"));
        auto *diff_lay = qobject_cast<QVBoxLayout *>(diff->layout());
        auto *th_lay = new QHBoxLayout;
        th_lay->setContentsMargins(0, 0, 0, 0);
        th_lay->setSpacing(6);
        auto *th_lbl = new QLabel(QStringLiteral("Threshold (°C):"), diff);
        th_lbl->setFocusPolicy(Qt::NoFocus);
        th_lay->addWidget(th_lbl);
        ref_thresh_ = new QDoubleSpinBox(diff);
        ref_thresh_->setFocusPolicy(Qt::NoFocus);
        ref_thresh_->setRange(0.0, 100.0);
        ref_thresh_->setSingleStep(0.5);
        ref_thresh_->setDecimals(1);
        ref_thresh_->setValue(2.0);
        ref_thresh_->setObjectName(QStringLiteral("refthresh"));
        connect(ref_thresh_,
                qOverload<double>(&QDoubleSpinBox::valueChanged),
                [this](double) {
                    if (on_threshold_changed_)
                        on_threshold_changed_();
                });
        th_lay->addWidget(ref_thresh_);
        diff_lay->addLayout(th_lay);
        ref_stats_ = new QLabel(diff);
        ref_stats_->setWordWrap(true);
        ref_stats_->setFocusPolicy(Qt::NoFocus);
        ref_stats_->setObjectName(QStringLiteral("refstats"));
        diff_lay->addWidget(ref_stats_);
        lay->addWidget(diff);

        auto *hint = new QLabel(
            QStringLiteral("The reference is a saved still (.dyt.jpg).  "
                           "Load one with the button above or --reference PATH. "
                           " The stats name the largest absolute change and "
                           "how many pixels exceed the threshold."), this);
        hint->setObjectName(QStringLiteral("cmphint"));
        hint->setWordWrap(true);
        hint->setFocusPolicy(Qt::NoFocus);
        lay->addWidget(hint);

        lay->addStretch(1);

        auto *scroll = new QScrollArea(this);
        scroll->setWidget(page);
        scroll->setWidgetResizable(true);
        scroll->setFocusPolicy(Qt::NoFocus);
        scroll->setFrameShape(QFrame::NoFrame);
        return scroll;
    }

    /* The Circuit Design tab.  The Windows panel overlays a board layout on
     * the thermal picture so a hot component can be named; with no
     * gerber/CAD parser in scope, the layout is an image the user loads.  The
     * page only *drives* the overlay — the image lives on FrameView and the
     * compositing happens in its draw path — so this page is the same
     * "controls that push, not a second renderer" shape the other tabs use. */
    QWidget *build_circuit_design()
    {
        auto *page = new QWidget;
        auto *lay  = new QVBoxLayout(page);
        lay->setContentsMargins(6, 6, 6, 6);
        lay->setSpacing(8);

        QGroupBox *src = group(QStringLiteral("Layout image"));
        auto *slay = qobject_cast<QVBoxLayout *>(src->layout());
        layout_path_ = new QLabel(QStringLiteral("(none)"), src);
        layout_path_->setWordWrap(true);
        layout_path_->setFocusPolicy(Qt::NoFocus);
        layout_path_->setObjectName(QStringLiteral("layoutpath"));
        slay->addWidget(layout_path_);

        auto *load = new QPushButton(QStringLiteral("Load layout…"), src);
        load->setFocusPolicy(Qt::NoFocus);
        load->setObjectName(QStringLiteral("loadlayout"));
        connect(load, &QPushButton::clicked, [this]() {
            if (on_load_layout_)
                on_load_layout_();
        });
        slay->addWidget(load);

        auto *clear = new QPushButton(QStringLiteral("Clear layout"), src);
        clear->setFocusPolicy(Qt::NoFocus);
        clear->setObjectName(QStringLiteral("clearlayout"));
        connect(clear, &QPushButton::clicked, [this]() {
            if (on_clear_layout_)
                on_clear_layout_();
        });
        slay->addWidget(clear);
        lay->addWidget(src);

        /* Alignment and opacity.  The ±range is the vendor's fusion alignment
         * limit, so the two alignment controls share one definition of "in
         * range" — dyt_fusion_clamp_align() is the clamp FrameView applies,
         * and the spin's own range is the same constant. */
        QGroupBox *adj = group(QStringLiteral("Alignment"));
        auto *alay = qobject_cast<QVBoxLayout *>(adj->layout());

        auto *xrow = new QHBoxLayout;
        xrow->setContentsMargins(0, 0, 0, 0);
        xrow->setSpacing(6);
        auto *xlbl = new QLabel(QStringLiteral("X offset:"), adj);
        xlbl->setFocusPolicy(Qt::NoFocus);
        xrow->addWidget(xlbl);
        layout_spin_x_ = new QSpinBox(adj);
        layout_spin_x_->setFocusPolicy(Qt::NoFocus);
        layout_spin_x_->setRange(-DYT_FUSION_ALIGN_MAX, DYT_FUSION_ALIGN_MAX);
        layout_spin_x_->setSuffix(QStringLiteral(" px"));
        layout_spin_x_->setObjectName(QStringLiteral("layoutx"));
        connect(layout_spin_x_, qOverload<int>(&QSpinBox::valueChanged),
                [this](int) { layout_changed(); });
        xrow->addWidget(layout_spin_x_);
        alay->addLayout(xrow);

        auto *yrow = new QHBoxLayout;
        yrow->setContentsMargins(0, 0, 0, 0);
        yrow->setSpacing(6);
        auto *ylbl = new QLabel(QStringLiteral("Y offset:"), adj);
        ylbl->setFocusPolicy(Qt::NoFocus);
        yrow->addWidget(ylbl);
        layout_spin_y_ = new QSpinBox(adj);
        layout_spin_y_->setFocusPolicy(Qt::NoFocus);
        layout_spin_y_->setRange(-DYT_FUSION_ALIGN_MAX, DYT_FUSION_ALIGN_MAX);
        layout_spin_y_->setSuffix(QStringLiteral(" px"));
        layout_spin_y_->setObjectName(QStringLiteral("layouty"));
        connect(layout_spin_y_, qOverload<int>(&QSpinBox::valueChanged),
                [this](int) { layout_changed(); });
        yrow->addWidget(layout_spin_y_);
        alay->addLayout(yrow);

        auto *orow = new QHBoxLayout;
        orow->setContentsMargins(0, 0, 0, 0);
        orow->setSpacing(6);
        auto *olbl = new QLabel(QStringLiteral("Opacity:"), adj);
        olbl->setFocusPolicy(Qt::NoFocus);
        orow->addWidget(olbl);
        layout_opacity_ = new QSlider(Qt::Horizontal, adj);
        layout_opacity_->setFocusPolicy(Qt::NoFocus);
        layout_opacity_->setRange(0, 100);
        layout_opacity_->setValue(100);
        layout_opacity_->setObjectName(QStringLiteral("layoutopacity"));
        connect(layout_opacity_, &QSlider::valueChanged,
                [this](int) { layout_changed(); });
        orow->addWidget(layout_opacity_, 1);
        alay->addLayout(orow);

        layout_show_ = new QCheckBox(QStringLiteral("Show layout"), adj);
        layout_show_->setFocusPolicy(Qt::NoFocus);
        layout_show_->setChecked(true);
        layout_show_->setObjectName(QStringLiteral("layoutshow"));
        connect(layout_show_, &QCheckBox::toggled,
                [this](bool) { layout_changed(); });
        alay->addWidget(layout_show_);
        lay->addWidget(adj);

        auto *hint = new QLabel(
            QStringLiteral("Load a board layout (a PNG or other image) to "
                           "composite it over the picture.  The offset is in "
                           "pixels of the displayed picture; the range matches "
                           "the fusion alignment (±%1).").arg(
                               DYT_FUSION_ALIGN_MAX), page);
        hint->setObjectName(QStringLiteral("layouthint"));
        hint->setWordWrap(true);
        hint->setFocusPolicy(Qt::NoFocus);
        lay->addWidget(hint);

        lay->addStretch(1);

        auto *scroll = new QScrollArea(this);
        scroll->setWidget(page);
        scroll->setWidgetResizable(true);
        scroll->setFocusPolicy(Qt::NoFocus);
        scroll->setFrameShape(QFrame::NoFrame);
        return scroll;
    }

    /* One place for the three controls' change to reach the window, so a new
     * control cannot forget to push. */
    void layout_changed()
    {
        if (on_layout_changed_)
            on_layout_changed_();
    }

    /* The Super Resolution tab.  Off / Visible plane (2x) / Thermal plane
     * (2x), one at a time, each routed through the same dispatch as the 'z'
     * and 'Z' keys.  The page is ours, not the vendor's: super-resolution is a
     * recovered capability (src/sr.h), so it gets its own tab rather than
     * being wedged into the Troubleshoot groups the manual lays out. */
    QWidget *build_super_resolution()
    {
        auto *page = new QWidget;
        auto *lay  = new QVBoxLayout(page);
        lay->setContentsMargins(6, 6, 6, 6);
        lay->setSpacing(8);

        QGroupBox *mode = group(QStringLiteral("Upscale plane (2x)"));
        auto *mgrp = new QButtonGroup(this);
        mgrp->setExclusive(true);
        /* Off presses whichever key means "off" from the mode the session
         * holds; the other two are their own keys. */
        QPushButton *off = row(mode, SrOff, QStringLiteral("Off"), 'z', true,
                               [this]() {
                                   if (on_key)
                                       on_key(sr_off_key());
                               });
        off->setToolTip(QStringLiteral("key: z / Z"));
        mgrp->addButton(off);
        mgrp->addButton(row(mode, SrVisible,
                            QStringLiteral("Visible plane (2x)"), 'z', true));
        mgrp->addButton(row(mode, SrThermal,
                            QStringLiteral("Thermal plane (2x)"), 'Z', true));
        lay->addWidget(mode);

        sr_status_ = new QLabel(this);
        sr_status_->setObjectName(QStringLiteral("srstatus"));
        sr_status_->setWordWrap(true);
        sr_status_->setFocusPolicy(Qt::NoFocus);
        lay->addWidget(sr_status_);

        auto *hint = new QLabel(
            QStringLiteral("The model is the recovered 256x192 → 512x384 2x "
                           "upscaler (models/zoom2.mnn).  \"Visible\" upscales "
                           "the vendor's grey plane; \"Thermal\" upscales the "
                           "picture through the display range."), this);
        hint->setObjectName(QStringLiteral("srhint"));
        hint->setWordWrap(true);
        hint->setFocusPolicy(Qt::NoFocus);
        lay->addWidget(hint);

        lay->addStretch(1);

        auto *scroll = new QScrollArea(this);
        scroll->setWidget(page);
        scroll->setWidgetResizable(true);
        scroll->setFocusPolicy(Qt::NoFocus);
        scroll->setFrameShape(QFrame::NoFrame);
        return scroll;
    }

    static QString tool_label(Id i)
    {
        switch (i) {
        case Spot:     return QStringLiteral("Spot");
        case Line:     return QStringLiteral("Line");
        case Rect:     return QStringLiteral("Rectangle");
        case Poly:     return QStringLiteral("Polygon");
        case ToolNone: return QStringLiteral("None");
        default:       return QString();
        }
    }
    static int tool_key(Id i)
    {
        switch (i) {
        case Spot:     return 'p';
        case Line:     return 'l';
        case Rect:     return 'b';
        case Poly:     return 'o';
        case ToolNone: return 'n';
        default:       return 0;
        }
    }

    /* The row glyphs, 18x18.  Cyan strokes, the same accent as the rail. */
    static void paint_icon(QPainter &p, Id id, const QRect &r)
    {
        const QColor cyan(QStringLiteral("#00b4d8"));
        const QColor white(QStringLiteral("#e6e6e6"));
        p.setPen(QPen(cyan, 1.3));
        p.setBrush(Qt::NoBrush);
        const int cx = r.center().x(), cy = r.center().y();

        switch (id) {
        case Spot:
            /* a thermometer */
            p.drawRoundedRect(cx - 2, cy - 7, 4, 9, 2, 2);
            p.setBrush(cyan);
            p.drawEllipse(QRectF(cx - 3.5, cy + 1, 7, 7));
            break;
        case Line:
            p.drawLine(cx - 6, cy + 5, cx + 6, cy - 5);
            p.setBrush(cyan);
            p.drawEllipse(QPointF(cx - 6, cy + 5), 2, 2);
            p.drawEllipse(QPointF(cx + 6, cy - 5), 2, 2);
            break;
        case Rect:
            p.drawRect(cx - 6, cy - 5, 12, 10);
            break;
        case Poly: {
            /* a pentagon, with the first vertex marked like the canvas does */
            QPolygon poly;
            poly << QPoint(cx,     cy - 7) << QPoint(cx + 6, cy - 2)
                 << QPoint(cx + 4, cy + 6) << QPoint(cx - 4, cy + 6)
                 << QPoint(cx - 6, cy - 2);
            p.drawPolygon(poly);
            p.setBrush(cyan);
            p.drawEllipse(QPointF(cx, cy - 7), 2, 2);
            break;
        }
        case ToolNone:
            p.drawEllipse(QPointF(cx, cy), 6, 6);
            p.drawLine(cx - 5, cy + 5, cx + 5, cy - 5);
            break;
        case AnalysisLine: {
            /* a chart frame with a profile in it */
            p.drawRect(cx - 7, cy - 6, 14, 12);
            p.drawLine(cx - 7, cy + 2, cx - 3, cy - 2);
            p.drawLine(cx - 3, cy - 2, cx + 1, cy + 2);
            p.drawLine(cx + 1, cy + 2, cx + 6, cy - 4);
            break;
        }
        case AnalysisChart: {
            /* the same frame, with the statistics drawn across it */
            p.drawRect(cx - 7, cy - 6, 14, 12);
            p.drawLine(cx - 7, cy - 2, cx + 6, cy - 2);
            p.setPen(QPen(cyan, 1.3, Qt::DashLine));
            p.drawLine(cx - 7, cy + 2, cx + 6, cy + 2);
            break;
        }
        case Tracking:
            p.drawEllipse(QPointF(cx, cy), 4, 4);
            p.drawLine(cx - 7, cy, cx - 2, cy);
            p.drawLine(cx + 2, cy, cx + 7, cy);
            p.drawLine(cx, cy - 7, cx, cy - 2);
            p.drawLine(cx, cy + 2, cx, cy + 7);
            break;
        case Alarm:
            /* a bell */
            p.drawArc(QRectF(cx - 5, cy - 6, 10, 10), 0, 180 * 16);
            p.drawLine(cx - 5, cy, cx - 5, cy + 3);
            p.drawLine(cx + 5, cy, cx + 5, cy + 3);
            p.drawLine(cx - 7, cy + 3, cx + 7, cy + 3);
            p.setBrush(cyan);
            p.drawEllipse(QPointF(cx, cy + 5), 1.6, 1.6);
            break;
        case Highlight:
            /* a filled band inside a rectangle */
            p.drawRect(cx - 7, cy - 5, 14, 10);
            p.setBrush(cyan);
            p.setPen(Qt::NoPen);
            p.drawRect(cx - 7, cy - 1, 14, 3);
            break;
        case FlipH:
            p.drawLine(cx, cy - 6, cx, cy + 6);
            p.drawLine(cx - 7, cy - 3, cx - 2, cy);
            p.drawLine(cx - 7, cy + 3, cx - 2, cy);
            p.drawLine(cx + 7, cy - 3, cx + 2, cy);
            p.drawLine(cx + 7, cy + 3, cx + 2, cy);
            break;
        case FlipV:
            p.drawLine(cx - 6, cy, cx + 6, cy);
            p.drawLine(cx - 3, cy - 7, cx, cy - 2);
            p.drawLine(cx + 3, cy - 7, cx, cy - 2);
            p.drawLine(cx - 3, cy + 7, cx, cy + 2);
            p.drawLine(cx + 3, cy + 7, cx, cy + 2);
            break;
        case FixedRange:
            p.drawLine(cx - 5, cy - 6, cx - 5, cy + 6);
            p.drawLine(cx - 5, cy - 6, cx - 2, cy - 6);
            p.drawLine(cx - 5, cy + 6, cx - 2, cy + 6);
            p.drawLine(cx + 5, cy - 6, cx + 5, cy + 6);
            p.drawLine(cx + 5, cy - 6, cx + 2, cy - 6);
            p.drawLine(cx + 5, cy + 6, cx + 2, cy + 6);
            break;
        case RapidDiag:
            /* a gauge bracketed by two range ends: the frame's own scale,
             * narrowed to a window.  Deliberately unlike FixedRange's two
             * bars — the two rows sit on the same line of the panel and must
             * not read as the same control. */
            p.drawArc(QRectF(cx - 7, cy - 7, 14, 12), 0, 180 * 16);
            p.drawLine(cx - 7, cy - 1, cx - 7, cy + 5);
            p.drawLine(cx + 7, cy - 1, cx + 7, cy + 5);
            p.setBrush(cyan);
            p.setPen(Qt::NoPen);
            p.drawPolygon(QPolygonF()
                          << QPointF(cx, cy + 2) << QPointF(cx - 3, cy + 6)
                          << QPointF(cx + 3, cy + 6));
            break;
        case LeakShort:
            /* a solid bolt: a short circuit.  Filled, because it is the
             * whole-conductor fault the other two are partial versions of. */
            p.setBrush(cyan);
            p.setPen(Qt::NoPen);
            p.drawPolygon(QPolygonF()
                          << QPointF(cx + 2, cy - 7) << QPointF(cx - 5, cy + 1)
                          << QPointF(cx - 1, cy + 1) << QPointF(cx - 2, cy + 7)
                          << QPointF(cx + 5, cy - 1) << QPointF(cx + 1, cy - 1));
            break;
        case LeakLarge:
            /* a bolt with a wide gap beside it: most of the current leaking
             * away.  The gap's width is what distinguishes the two leakage
             * rows at a glance. */
            p.setPen(QPen(white, 1.3));
            p.drawPolygon(QPolygonF()
                          << QPointF(cx - 6, cy - 7) << QPointF(cx - 2, cy - 1)
                          << QPointF(cx - 5, cy - 1) << QPointF(cx - 3, cy + 7));
            p.setPen(QPen(cyan, 1.3));
            p.drawLine(cx + 1, cy - 6, cx + 1, cy + 6);
            p.drawLine(cx + 5, cy - 6, cx + 5, cy + 6);
            break;
        case LeakSmall:
            /* the same bolt, but the leak is a thin thread. */
            p.setPen(QPen(white, 1.3));
            p.drawPolygon(QPolygonF()
                          << QPointF(cx - 6, cy - 7) << QPointF(cx - 2, cy - 1)
                          << QPointF(cx - 5, cy - 1) << QPointF(cx - 3, cy + 7));
            p.setPen(QPen(cyan, 1.3));
            p.drawLine(cx + 3, cy - 6, cx + 3, cy + 6);
            break;
        case SurfShape:
            /* a landscape: two ridges and a valley, the shape the height
             * carries. */
            p.drawPolyline(QPolygonF()
                           << QPointF(cx - 8, cy + 5) << QPointF(cx - 4, cy - 4)
                           << QPointF(cx - 1, cy + 2) << QPointF(cx + 2, cy - 6)
                           << QPointF(cx + 8, cy + 5));
            p.setPen(QPen(cyan, 1.0, Qt::DotLine));
            p.drawLine(cx - 8, cy + 6, cx + 8, cy + 6);
            break;
        case SurfColor:
            /* three swatches: the palette doing the talking. */
            p.setPen(Qt::NoPen);
            for (int s = 0; s < 3; s++) {
                p.setBrush(s == 0 ? QColor(40, 90, 200)
                          : s == 1 ? QColor(60, 200, 120)
                                   : QColor(230, 160, 40));
                p.drawRect(cx - 7 + s * 5, cy - 6, 4, 12);
            }
            p.setBrush(Qt::NoBrush);
            break;
        case MarkTextRow:
            /* a note pinned to a pixel: the plate, the leader and the dot the
             * canvas draws, so the row and what it places look alike. */
            p.drawRect(cx - 7, cy - 7, 13, 8);
            p.drawLine(cx - 3, cy + 1, cx - 3, cy + 4);
            p.setBrush(cyan);
            p.setPen(Qt::NoPen);
            p.drawEllipse(QPointF(cx - 3, cy + 6), 1.6, 1.6);
            break;
        case Still:
            /* a camera */
            p.drawRect(cx - 7, cy - 4, 14, 9);
            p.drawRect(cx - 3, cy - 6, 6, 3);
            p.drawEllipse(QPointF(cx, cy + 0.5), 3, 3);
            break;
        case Record: {
            /* a video camera */
            p.drawRect(cx - 7, cy - 4, 9, 8);
            QPolygon tri;
            tri << QPoint(cx + 3, cy - 2) << QPoint(cx + 7, cy - 4)
                << QPoint(cx + 7, cy + 4) << QPoint(cx + 3, cy + 2);
            p.drawPolygon(tri);
            break;
        }
        case Gallery:
            /* stacked frames */
            p.drawRect(cx - 7, cy - 5, 10, 8);
            p.drawRect(cx - 4, cy - 7, 10, 8);
            break;
        case SrOff:
            /* one pixel, struck through: native resolution */
            p.setBrush(cyan);
            p.setPen(Qt::NoPen);
            p.drawRect(cx - 2, cy - 2, 4, 4);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(white, 1.4));
            p.drawLine(cx - 6, cy + 6, cx + 6, cy - 6);
            break;
        case SrVisible:
        case SrThermal:
            /* a pixel becoming four: the 2x upscale.  The thermal variant
             * marks the centre, so the two are distinguishable at a glance. */
            p.setPen(QPen(cyan, 1.2));
            for (int gx = 0; gx < 2; gx++)
                for (int gy = 0; gy < 2; gy++)
                    p.drawRect(cx - 6 + gx * 7, cy - 6 + gy * 7, 5, 5);
            if (id == SrThermal) {
                p.setBrush(cyan);
                p.setPen(Qt::NoPen);
                p.drawEllipse(QPointF(cx, cy), 1.8, 1.8);
            }
            break;
        case N_Ids:
            break;
        }
    }

    QTabWidget  *tabs_ = nullptr;
    QPushButton *btn_[N_Ids] = {};
    ProfilePlot *plot_ = nullptr;        /* the Analysis group's chart */
    int          leak_mode_ = DYT_LEAK_SHORT;  /* the circuit-mode rows */
    /* The Mark tool the canvas is holding, as the last sync reported it.  A
     * MarkKind; it decides the Tool None row, because arming a Mark puts the
     * session's tool to NONE. */
    int          mark_mode_ = MarkNone;
    QLabel      *sr_status_ = nullptr;   /* the Super Resolution tab's readout */
    QLabel      *ref_path_  = nullptr;   /* the Comparison tab's reference path */
    QLabel      *ref_stats_ = nullptr;   /* the Comparison tab's difference stats */
    QDoubleSpinBox *ref_thresh_ = nullptr; /* the Comparison tab's threshold */
    SurfaceView *surface_   = nullptr;   /* the 3D Analysis tab's view */
    /* The Circuit Design tab's controls.  The image itself lives on FrameView;
     * these only drive it. */
    QLabel    *layout_path_    = nullptr;
    QSpinBox  *layout_spin_x_  = nullptr;
    QSpinBox  *layout_spin_y_  = nullptr;
    QSlider   *layout_opacity_ = nullptr;
    QCheckBox *layout_show_    = nullptr;
    /* The High TEMP. Alarm threshold field.  The canvas is what arms at it, so
     * this pushes through push_alarm() on every change — the same split the
     * layout controls take. */
    QDoubleSpinBox *alarm_spin_ = nullptr;
    /* The mode the last sync reported, so the Off row knows which key means
     * "off" from where the session is.  Kept as the last *synced* mode rather
     * than read live because the panel has no session handle — it reports, it
     * does not act on the engine directly. */
    dyt_sr_t     sr_mode_ = DYT_SR_OFF;
};

/* Push the Circuit Design page's controls into the canvas.  A free function
 * rather than a lambda inside run_gui so the selftest can drive the same
 * contract: the panel is the view and FrameView is the canvas, and this is the
 * one place the two are joined.  Called on every control change and once at
 * start-up, so the two cannot drift. */
static void push_layout(ControlPanel *panel, FrameView *view)
{
    if (!panel || !view)
        return;
    view->set_layout_align(panel->layout_dx(), panel->layout_dy());
    view->set_layout_opacity(panel->layout_opacity());
    view->set_layout_shown(panel->layout_shown());
}

/* Push the High TEMP. Alarm threshold field into the canvas, which is what
 * arms at it.  The same free-function contract as push_layout: one place joins
 * the panel to the canvas, so run_gui and --selftest exercise the same path. */
static void push_alarm(ControlPanel *panel, FrameView *view)
{
    if (!panel || !view)
        return;
    view->set_alarm_setpoint((float)panel->alarm_setpoint());
}

/* --------------------------------------------------------- settings dialog */

/* The rail's Setting item — the Windows app's own Setting button.
 *
 * It presents the four runtime radiometric parameters the `e`/`A`/`R`/`D`
 * ladder walks as numeric fields, so a value the ladder cannot *step* to is
 * still reachable.  That is not a widening of what the device accepts: the
 * encoder (params.c) truncates any value in range, and the reference's own
 * panel uses editable numeric fields for its thresholds.  The spin boxes are
 * bounded by the ladder's own range, so the dialog can offer more values than
 * the keyboard but never one outside what the keyboard could reach.
 *
 * What it deliberately does not do is write anything itself.  Each row's Send
 * goes through FrameView::on_param_send_, the same callback the ladder's 'y'
 * uses, so there is one write path and the two cannot diverge in what they
 * send — or in what they say when the device refuses.  The dialog adds an
 * input method, nothing else.
 *
 * One Send per row rather than one for the dialog: a parameter is armed and
 * confirmed on its own, never as a batch, and a batch would need an
 * all-or-nothing semantics the device does not have.
 *
 * A parameter the device never reported starts at the ladder's first rung and
 * says "not read" rather than showing a plausible zero. */
class SettingsDialog : public QDialog {
public:
    explicit SettingsDialog(std::function<bool(dyt_order_type_t, float)> send,
                            QWidget *parent = nullptr)
        : QDialog(parent), send_(std::move(send))
    {
        setWindowTitle(QStringLiteral("Settings"));

        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(12, 12, 12, 12);
        outer->setSpacing(10);

        /* ---- Device: the four runtime parameters, and the one device
         * action that is not a parameter. ---- */
        auto *dev = new QGroupBox(QStringLiteral("Device"), this);
        auto *devlay = new QVBoxLayout(dev);
        devlay->setContentsMargins(8, 6, 8, 8);
        devlay->setSpacing(8);

        auto *grid = new QGridLayout;
        grid->setHorizontalSpacing(10);
        grid->setVerticalSpacing(6);

        for (int i = 0; i < dyt_vm_ladder_count(); i++) {
            const dyt_vm_ladder_t *L = dyt_vm_ladder_at(i);
            if (!L)
                continue;

            /* The ladder's own extent and its finest step, so a spin box
             * cannot offer a value the ladder's range excludes. */
            float lo = L->vals[0], hi = L->vals[0], step = 0.f;
            for (int k = 1; k < L->n; k++) {
                lo = std::min(lo, L->vals[k]);
                hi = std::max(hi, L->vals[k]);
                const float gap = std::fabs(L->vals[k] - L->vals[k - 1]);
                if (gap > 0.f && (step == 0.f || gap < step))
                    step = gap;
            }
            if (step <= 0.f)
                step = (hi - lo) / 10.f;

            QString name = QString::fromUtf8(L->name);
            if (!name.isEmpty())
                name[0] = name[0].toUpper();

            auto *lab = new QLabel(name, dev);

            auto *spin = new QDoubleSpinBox(dev);
            spin->setObjectName(QStringLiteral("setting"));
            spin->setDecimals(decimals(L->type));
            spin->setRange(lo, hi);
            spin->setSingleStep(step);
            spin->setSuffix(suffix(L->type));
            spin->setKeyboardTracking(false);
            spin->setValue(L->vals[0]);

            auto *btn = new QPushButton(QStringLiteral("Send"), dev);
            btn->setObjectName(QStringLiteral("setting"));
            QObject::connect(btn, &QPushButton::clicked, this, [this, L, spin]() {
                const float v = (float)spin->value();
                const bool  ok = send_ && send_(L->type, v);
                char shown[32];
                dyt_vm_param_format(L->type, v, shown, sizeof shown);
                status_->setText(
                    QStringLiteral("%1 = %2 — %3")
                        .arg(QString::fromUtf8(L->name),
                             QString::fromUtf8(shown),
                             ok ? QStringLiteral("sent")
                                : QStringLiteral("refused; see the status bar")));
            });

            const int row = grid->rowCount();
            grid->addWidget(lab,  row, 0);
            grid->addWidget(spin, row, 1);
            grid->addWidget(btn,  row, 2);
            labs_[L->type]  = lab;
            spins_[L->type] = spin;
            sends_[L->type] = btn;
        }
        grid->setColumnStretch(1, 1);
        devlay->addLayout(grid);

        /* Retry is a device action with a key, so it runs that key — the same
         * rule every other control here follows. */
        retry_ = new QPushButton(QStringLiteral("Retry connection"), dev);
        retry_->setObjectName(QStringLiteral("setting"));
        QObject::connect(retry_, &QPushButton::clicked, this,
                         [this]() { if (on_key) on_key('r'); });
        devlay->addWidget(retry_, 0, Qt::AlignLeft);
        outer->addWidget(dev);

        /* ---- Display: the view options the retired menu bar carried.  They
         * are here rather than in the Troubleshoot tab because the reference's
         * tab has exactly four groups and this dialog is the reference's own
         * catch-all Setting panel.  Palette is deliberately absent: the rail's
         * Palette item is its popup picker, and two routes to one list would
         * be one more than the reference has. ---- */
        auto *disp = new QGroupBox(QStringLiteral("Display"), this);
        auto *dlay = new QGridLayout(disp);
        dlay->setContentsMargins(8, 6, 8, 8);
        dlay->setHorizontalSpacing(10);
        dlay->setVerticalSpacing(6);

        /* Unit — an absolute choice, so no key can express it; the menu made
         * the same call. */
        dlay->addWidget(new QLabel(QStringLiteral("Unit"), disp), 0, 0);
        auto *ubox = new QWidget(disp);
        auto *ulay = new QHBoxLayout(ubox);
        ulay->setContentsMargins(0, 0, 0, 0);
        ulay->setSpacing(6);
        units_ = new QButtonGroup(this);
        for (int i = 0; i < DYT_UNIT_N; i++) {
            const char *nm = dyt_unit_name((dyt_unit_t)i);
            auto *b = new QPushButton(QString::fromUtf8(nm ? nm : "?"), ubox);
            b->setObjectName(QStringLiteral("setting"));
            b->setCheckable(true);
            b->setFocusPolicy(Qt::NoFocus);
            units_->addButton(b, i);
            ulay->addWidget(b);
        }
        QObject::connect(units_, &QButtonGroup::idClicked, this,
                         [this](int id) {
                             if (on_unit)
                                 on_unit((dyt_unit_t)id);
                         });
        dlay->addWidget(ubox, 0, 1);

        /* Fusion — likewise absolute. */
        dlay->addWidget(new QLabel(QStringLiteral("Fusion"), disp), 1, 0);
        fusion_ = new QComboBox(disp);
        fusion_->setObjectName(QStringLiteral("setting"));
        fusion_->setFocusPolicy(Qt::NoFocus);
        for (int i = 0; i < DYT_FUSION_N; i++) {
            const char *nm = dyt_fusion_name((dyt_fusion_t)i);
            fusion_->addItem(QString::fromUtf8(nm ? nm : "?"));
        }
        QObject::connect(fusion_, &QComboBox::currentIndexChanged, this,
                         [this](int i) {
                             if (on_fusion)
                                 on_fusion((dyt_fusion_t)i);
                         });
        dlay->addWidget(fusion_, 1, 1);

        /* Zoom — a delta, so it has keys and they are what the buttons run. */
        dlay->addWidget(new QLabel(QStringLiteral("Zoom"), disp), 2, 0);
        auto *zbox = new QWidget(disp);
        auto *zlay = new QHBoxLayout(zbox);
        zlay->setContentsMargins(0, 0, 0, 0);
        zlay->setSpacing(6);
        for (const auto &z : { std::make_pair(QStringLiteral("Zoom out"), '-'),
                               std::make_pair(QStringLiteral("Zoom in"), '+') }) {
            auto *b = new QPushButton(z.first, zbox);
            b->setObjectName(QStringLiteral("setting"));
            b->setFocusPolicy(Qt::NoFocus);
            const int k = z.second;
            QObject::connect(b, &QPushButton::clicked, this,
                             [this, k]() { if (on_key) on_key(k); });
            zlay->addWidget(b);
            zoom_[k == '+' ? 1 : 0] = b;
        }
        zlay->addStretch(1);
        dlay->addWidget(zbox, 2, 1);

        fullscreen_ = new QCheckBox(QStringLiteral("Full screen"), disp);
        fullscreen_->setObjectName(QStringLiteral("setting"));
        fullscreen_->setFocusPolicy(Qt::NoFocus);
        QObject::connect(fullscreen_, &QCheckBox::clicked, this,
                         [this]() { if (on_key) on_key(Qt::Key_F11); });
        dlay->addWidget(fullscreen_, 3, 0, 1, 2);

        info_ = new QCheckBox(QStringLiteral("Device panel"), disp);
        info_->setObjectName(QStringLiteral("setting"));
        info_->setFocusPolicy(Qt::NoFocus);
        QObject::connect(info_, &QCheckBox::clicked, this,
                         [this]() { if (on_key) on_key('d'); });
        dlay->addWidget(info_, 4, 0, 1, 2);

        dlay->setColumnStretch(1, 1);
        outer->addWidget(disp);

        status_ = new QLabel(this);
        status_->setObjectName(QStringLiteral("settingstatus"));
        status_->setWordWrap(true);
        outer->addWidget(status_);

        auto *buttons = new QHBoxLayout;
        about_ = new QPushButton(QStringLiteral("About…"), this);
        about_->setObjectName(QStringLiteral("setting"));
        about_->setFocusPolicy(Qt::NoFocus);
        QObject::connect(about_, &QPushButton::clicked, this,
                         [this]() { if (on_key) on_key('?'); });
        buttons->addWidget(about_);
        buttons->addStretch(1);
        auto *close = new QPushButton(QStringLiteral("Close"), this);
        close->setObjectName(QStringLiteral("setting"));
        QObject::connect(close, &QPushButton::clicked, this, &QDialog::accept);
        buttons->addWidget(close);
        outer->addLayout(buttons);

        seed(nullptr);
        sync_display(DYT_UNIT_C, DYT_FUSION_INFRARED, false, false);
    }

    /* Point every row at the value the session knows, and label the ones the
     * device never reported.  Called on every open, not just at construction:
     * the values are the device's, and a write made since the last open — or a
     * device that has come and gone — must not leave the fields showing a
     * value nothing holds.  `current` is indexed by dyt_order_type_t, with NaN
     * for "not known"; NULL means every row is unknown. */
    void seed(const float *current)
    {
        for (int i = 0; i < dyt_vm_ladder_count(); i++) {
            const dyt_vm_ladder_t *L = dyt_vm_ladder_at(i);
            if (!L)
                continue;
            const bool known = current && std::isfinite(current[L->type]);

            QString name = QString::fromUtf8(L->name);
            if (!name.isEmpty())
                name[0] = name[0].toUpper();
            if (labs_[L->type])
                labs_[L->type]->setText(
                    known ? name : name + QStringLiteral(" (not read)"));
            if (spins_[L->type])
                spins_[L->type]->setValue(known ? current[L->type]
                                                : L->vals[0]);
        }

        if (status_)
            status_->setText(
                QStringLiteral("A parameter is written to the device the moment "
                               "it is sent, and the reading changes with it."));
    }

    /* Point the Display controls at the state the session and the window are
     * in.  Called on every open beside seed(), for the same reason: a unit
     * changed from the keyboard, or a full screen toggled with F11, must not
     * leave the dialog showing the other one. */
    void sync_display(dyt_unit_t unit, dyt_fusion_t fusion, bool fullscreen,
                      bool info)
    {
        if (units_) {
            const QSignalBlocker block(units_);
            if (QAbstractButton *b = units_->button((int)unit))
                b->setChecked(true);
        }
        if (fusion_) {
            const QSignalBlocker block(fusion_);
            fusion_->setCurrentIndex((int)fusion);
        }
        if (fullscreen_) {
            const QSignalBlocker block(fullscreen_);
            fullscreen_->setChecked(fullscreen);
        }
        if (info_) {
            const QSignalBlocker block(info_);
            info_->setChecked(info);
        }
    }

    /* The two Display controls that are window/canvas state rather than session
     * state.  Set here — with the signals blocked — because this is the window
     * telling the box what happened, not the box asking: a user pressing F11 or
     * 'd' while the dialog is open must not leave it showing the other state,
     * and a stalled stream paints no frame for the per-frame sync to catch. */
    void sync_window(bool fullscreen, bool info)
    {
        if (fullscreen_) {
            const QSignalBlocker block(fullscreen_);
            fullscreen_->setChecked(fullscreen);
        }
        if (info_) {
            const QSignalBlocker block(info_);
            info_->setChecked(info);
        }
    }

    /* -- what --selftest drives.  The rows are indexed by dyt_order_type_t, so
     * a test asks for the parameter by the same name the ladder uses. */
    QDoubleSpinBox *spin(dyt_order_type_t t) const
    {
        return t >= 0 && t <= DYT_ORDER_DISTANCE ? spins_[t] : nullptr;
    }
    QPushButton *send_button(dyt_order_type_t t) const
    {
        return t >= 0 && t <= DYT_ORDER_DISTANCE ? sends_[t] : nullptr;
    }
    QPushButton *unit_button(dyt_unit_t u) const
    {
        return units_ ? qobject_cast<QPushButton *>(units_->button((int)u))
                      : nullptr;
    }
    QComboBox *fusion_box() const { return fusion_; }
    QPushButton *retry_button() const { return retry_; }
    QPushButton *zoom_button(bool in) const { return zoom_[in ? 1 : 0]; }
    QPushButton *about_button() const { return about_; }
    QCheckBox   *fullscreen_box() const { return fullscreen_; }
    QCheckBox   *info_box() const { return info_; }
    QString status_text() const { return status_ ? status_->text() : QString(); }

    /* Every action a control here takes goes out through one of these, so the
     * dialog holds no session and cannot become a second front end.  `on_key`
     * is MainWindow's handle_key, which is why the buttons that have keys press
     * them rather than calling the engine. */
    std::function<void(int)>           on_key;
    std::function<void(dyt_unit_t)>    on_unit;
    std::function<void(dyt_fusion_t)>  on_fusion;

private:
    static int decimals(dyt_order_type_t t)
    {
        return t == DYT_ORDER_EMISSIVITY ? 2
             : t == DYT_ORDER_DISTANCE   ? 2 : 1;
    }
    static QString suffix(dyt_order_type_t t)
    {
        if (t == DYT_ORDER_EMISSIVITY)
            return QString();                       /* dimensionless */
        if (t == DYT_ORDER_DISTANCE)
            return QStringLiteral(" m");
        return QStringLiteral(" \u00b0C");          /* ambient, reflected */
    }

    std::function<bool(dyt_order_type_t, float)> send_;
    QLabel *status_ = nullptr;
    /* Indexed by dyt_order_type_t (1..4; 0 unused), like the override tables
     * FrameView keeps — the same indexing the wire protocol uses. */
    QLabel         *labs_[DYT_ORDER_DISTANCE + 1]  = {};
    QDoubleSpinBox *spins_[DYT_ORDER_DISTANCE + 1] = {};
    QPushButton    *sends_[DYT_ORDER_DISTANCE + 1] = {};
    /* Display. */
    QButtonGroup *units_      = nullptr;
    QComboBox    *fusion_     = nullptr;
    QPushButton  *retry_      = nullptr;
    QPushButton  *zoom_[2]    = {};   /* [0] out, [1] in */
    QPushButton  *about_      = nullptr;
    QCheckBox    *fullscreen_ = nullptr;
    QCheckBox    *info_       = nullptr;
};

/* ---------------------------------------------------------- contact dialog */

/* The rail's Contact item.  Static vendor contact details — the Windows app's
 * "Contact us" is a plain information panel with nothing to act on, so this is
 * a read-only dialog rather than a message box, which would let the user copy
 * an address out of it. */
class ContactDialog : public QDialog {
public:
    explicit ContactDialog(QWidget *parent = nullptr) : QDialog(parent)
    {
        setWindowTitle(QStringLiteral("Contact us"));

        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(12, 12, 12, 12);
        outer->setSpacing(10);

        auto *text = new QTextBrowser(this);
        text->setObjectName(QStringLiteral("contact"));
        text->setReadOnly(true);
        text->setOpenExternalLinks(false);
        text->setPlainText(QStringLiteral(
            "DYT / Mechanic-Ti thermal camera\n"
            "\n"
            "This is an independent Linux port of the vendor's Windows and "
            "Android clients, built from the recovered USB protocol.  It is "
            "not affiliated with or supported by the vendor.\n"
            "\n"
            "For the camera hardware, its calibration and its warranty, "
            "contact the vendor you bought the unit from.\n"
            "\n"
            "For this port, see the project's own issue tracker.  Please "
            "include:\n"
            "  - the output of  dytqt --version\n"
            "  - the camera serial shown on the status bar\n"
            "  - what you did, and what happened instead"));
        outer->addWidget(text, 1);

        auto *close = new QPushButton(QStringLiteral("Close"), this);
        QObject::connect(close, &QPushButton::clicked, this, &QDialog::accept);
        outer->addWidget(close, 0, Qt::AlignRight);
    }
};

/* --------------------------------------------------------------- the window */

class MainWindow : public QWidget {
public:
    explicit MainWindow(QWidget *parent = nullptr) : QWidget(parent)
    {
        view_  = new FrameView(this);
        strip_ = new StatusStrip(this);

        /* The shell is the three-column Windows layout: a left icon rail, a
         * centre column holding the canvas and its status bar, and a right
         * tabbed control panel.  There is no menu bar and no toolbar — the
         * reference has neither, and every action they carried now has a home
         * on the rail, in the panel, or in the Settings dialog.
         *
         * MainWindow stays a plain QWidget rather than becoming a QMainWindow.
         * QMainWindow::sizeHint() does not account for its menu and tool bar
         * heights, so fit_to_view()'s resize(sizeHint()) would size the window
         * for the central widget alone and let the bars steal rows from the
         * canvas — clipping the picture.  The centre column's QVBoxLayout rows
         * count towards QWidget::sizeHint() automatically, and the outer
         * QHBoxLayout that holds rail | centre | panel is itself a row of that
         * same QWidget::sizeHint(). */
        rail_  = new IconRail(this);
        panel_ = new ControlPanel(this);
        /* Every panel control runs the one dispatch, exactly as the rail does
         * and as the retired menu items did. */
        panel_->on_key = [this](int k) { handle_key(k); };

        /* The rail's items.  Setting and Contact open a dialog; Palette opens
         * its picker; Mark and Reset act on the session; Rotate turns the
         * picture; Tutorials is the guide.  Compare stays a no-op until the
         * engine behind it exists (the two-board comparison), so no rail button
         * is ever a control that does nothing *silently* — the one that waits is
         * documented here and in the README. */
        rail_->on_action = [this](IconRail::RailItem i) {
            switch (i) {
            case IconRail::Palette:
                show_palette_popup();
                break;
            case IconRail::Mark:
                /* Back to the tool the user last had.  The panel's Temperature
                 * Measurement radio is the selector; this is the rail's way
                 * back to it after 'n' cleared it, which is what the reference
                 * Mark button does.  Spot is the default — it is the tool a
                 * first-time user gets, and the cheapest to place. */
                handle_key(last_tool_ == DYT_TOOL_LINE ? 'l'
                         : last_tool_ == DYT_TOOL_BOX  ? 'b' : 'p');
                break;
            case IconRail::ResetImage:
                /* One session call rather than four GUI ones: the reset reads
                 * the state under the session's own lock, so it cannot act on
                 * a frame that has already been superseded, and it cannot
                 * forget one of the four things it undoes. */
                if (sess_)
                    dyt_session_reset_view(sess_);
                break;
            case IconRail::Rotate:
                /* A quarter turn clockwise per click, the step the reference's
                 * own Rotate button takes.  It is a session call, not a GUI
                 * flag, because the rotation is part of the transform the
                 * pointer mapping inverts — a copy kept here would let the
                 * picture and the clicks disagree.  Reset Image clears it. */
                if (sess_)
                    dyt_session_rotate(sess_, DYT_ROT_90);
                break;
            case IconRail::Tutorials:
                if (on_help_)
                    on_help_();
                break;
            case IconRail::Compare:
                /* Switch the control panel to the Comparison tab.  The tab is
                 * where the reference still, the threshold, and the diff stats
                 * live — the same routing the rail's other items do for their
                 * own tab or dialog.  Named, not numbered: inserting the 3D
                 * Analysis tab ahead of it is exactly the change that would
                 * have silently pointed this at the wrong page. */
                if (panel_ && panel_->tabs())
                    panel_->tabs()->setCurrentIndex(ControlPanel::TabComparison);
                break;
            case IconRail::ContactUs: {
                ContactDialog d(this);
                d.exec();
                break;
            }
            case IconRail::Setting: {
                SettingsDialog *d = settings_dialog();
                d->show();
                d->raise();
                d->activateWindow();
                break;
            }
            case IconRail::N_RailItems:
                break;
            }
        };

        auto *centre = new QWidget(this);
        auto *clay = new QVBoxLayout(centre);
        clay->setContentsMargins(0, 0, 0, 0);
        clay->setSpacing(0);
        clay->addWidget(view_, 1);
        clay->addWidget(strip_, 0);

        auto *lay = new QHBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);
        lay->addWidget(rail_, 0);
        lay->addWidget(centre, 1);
        lay->addWidget(panel_, 0);

        setWindowTitle(kAppName);
        /* So 'R' reaches keyPressEvent rather than being dropped. */
        setFocusPolicy(Qt::StrongFocus);

        dyt_vm_gallery_init(&gal_);

        /* The gallery is an overlay on the canvas, not a row of the layout:
         * it floats over the top-left of the picture and is hidden until 'g'.
         * Parented to the canvas so it is composited in widget space, above
         * the image, and never sees the display transform. */
        gal_panel_ = new GalleryPanel(&gal_, view_);
        gal_panel_->on_select = [this](int row) {
            dyt_vm_gallery_set_sel(&gal_, row);
            gal_panel_->sync();
        };
        gal_panel_->on_open = [this]() { open_gallery_sel(); };
        gal_panel_->on_choose_folder = [this]() {
            if (on_choose_folder_)
                on_choose_folder_();
        };
        gal_panel_->hide();
    }

    IconRail *rail() const { return rail_; }
    ControlPanel *panel() const { return panel_; }

    /* Whether the session has a super-resolution model behind it.  Asked of
     * the session, not read from snap_: the control panel shows this before
     * the first frame arrives, when there is no snapshot to read it from, and
     * the panel must not claim "no model" for a session that has one. */
    bool sr_model_loaded() const
    {
        return sess_ && dyt_session_sr_capable(sess_) != 0;
    }

    /* The Settings dialog, created on first use and then reused.  Shown rather
     * than exec()'d: the whole point of sending a parameter is watching the
     * reading change, and a modal dialog would put the reading behind it.
     * Public, like rail() and panel(), so --selftest can drive it without
     * entering a nested event loop. */
    SettingsDialog *settings_dialog()
    {
        if (!settings_) {
            /* The write path is the ladder's own: on_param_send_ is what the
             * 'y' key calls, so the dialog and the keyboard cannot send
             * different values or disagree about a refusal. */
            settings_ = new SettingsDialog(
                [this](dyt_order_type_t t, float v) {
                    return view_ && view_->on_param_send_ &&
                           view_->on_param_send_(t, v);
                }, this);

            /* The Display controls route the same two ways every other
             * control does: through handle_key when the action has a key, and
             * through the session when it is an absolute choice no key can
             * express (unit, fusion — the same split the retired menu bar
             * made). */
            settings_->on_key = [this](int k) { handle_key(k); };
            settings_->on_unit = [this](dyt_unit_t u) {
                if (sess_)
                    dyt_session_set_unit(sess_, u);
            };
            settings_->on_fusion = [this](dyt_fusion_t f) {
                if (sess_)
                    dyt_session_set_fusion(sess_, f);
            };
        }
        settings_->seed(settings_current());
        settings_->sync_display(
            (dyt_unit_t)(snap_.unit < DYT_UNIT_N ? snap_.unit : DYT_UNIT_C),
            (dyt_fusion_t)(snap_.fusion < DYT_FUSION_N ? snap_.fusion
                                                       : DYT_FUSION_INFRARED),
            fullscreen_, view_ && view_->info_shown());
        return settings_;
    }

    /* The current value of each runtime parameter as the session knows it: a
     * write this session made, else what the device reported at bring-up, else
     * NaN.  The override wins because the device cannot be re-read while
     * streaming (RE Docs 04 §4.8) — the table is the only place the new value
     * exists. */
    const float *settings_current()
    {
        for (int t = 0; t <= DYT_ORDER_DISTANCE; t++)
            settings_cur_[t] = NAN;

        if (view_) {
            float ov[5];
            int   ov_on[5];
            view_->param_overrides(ov, ov_on);
            const dyt_device_info_t &d = view_->device_info();

            for (int t = DYT_ORDER_REFLECTED; t <= DYT_ORDER_DISTANCE; t++) {
                if (ov_on[t]) {
                    settings_cur_[t] = ov[t];
                    continue;
                }
                switch (t) {
                case DYT_ORDER_REFLECTED:
                    settings_cur_[t] = dyt_radiometry_reflected_c(&d.radio);
                    break;
                case DYT_ORDER_AMBIENT:
                    settings_cur_[t] = dyt_radiometry_ambient_c(&d.radio);
                    break;
                case DYT_ORDER_EMISSIVITY:
                    settings_cur_[t] = dyt_radiometry_emissivity(&d.radio);
                    break;
                case DYT_ORDER_DISTANCE:
                    settings_cur_[t] = dyt_radiometry_distance_m(&d.radio);
                    break;
                default:
                    break;
                }
            }
        }
        return settings_cur_;
    }

    /* The rail's Palette picker.  A popup under the button rather than a tab or
     * a dialog, because the reference's rail item is exactly that: a picker you
     * open, choose from and dismiss without the picture moving.
     *
     * The first ten entries carry their digit key and therefore *run* it — the
     * same rule the retired menu bar followed, so the popup is no more capable
     * than the keyboard for those.  Past the tenth there is no key, so those
     * entries set the palette directly; that is the one place the picker
     * reaches further than the keyboard, exactly as the menu's Next/Previous
     * items did, and it is what makes the entries past the tenth reachable.
     *
     * Public, like rail()/panel()/settings_dialog(), so --selftest can inspect
     * the picker without entering the nested loop that exec() would run.  The
     * menu is rebuilt on each call and owned by its caller, because the checked
     * entry has to be the session's *current* palette, not the one it was when
     * the menu was first built. */
    QMenu *palette_menu()
    {
        if (!sess_)
            return nullptr;

        auto *menu = new QMenu(this);
        auto *grp  = new QActionGroup(menu);
        grp->setExclusive(true);

        for (int i = 0; i < snap_.palette_n; i++) {
            dyt_palette_t p{};
            if (dyt_session_get_palette(sess_, i, &p) != 0)
                continue;
            QAction *a = nullptr;
            if (i < 10) {
                const int  key    = (i == 9) ? '0' : ('1' + i);
                const char hint[] = { (char)key, '\0' };
                a = key_action(QString::fromUtf8(p.name), key, hint, true);
            } else {
                a = plain_action(QString::fromUtf8(p.name),
                                 [this, i]() {
                                     if (sess_)
                                         dyt_session_set_palette(sess_, i);
                                 }, true);
            }
            a->setChecked(snap_.palette == i);
            grp->addAction(a);
            menu->addAction(a);
        }

        menu->addSeparator();
        menu->addAction(key_action(QStringLiteral("Next palette"), '.', "."));
        menu->addAction(key_action(QStringLiteral("Previous palette"), ',', ","));
        return menu;
    }

    FrameView   *view()  const { return view_; }
    StatusStrip *strip() const { return strip_; }
    GalleryPanel *gallery_panel() const { return gal_panel_; }

    /* The session the on-screen controls act on.  The canvas borrows it too;
     * the window needs its own handle because a few controls (unit, fusion,
     * the palette popup's higher entries, Reset Image) select an absolute
     * value or an operation, which no key expresses.  Borrowed, never freed
     * here. */
    void set_session(dyt_session_t *s)
    {
        sess_ = s;
        view_->set_session(s);
    }

    /* The front end owns the device lifecycle, so the window only reports the
     * two events it cannot act on itself.  `on_close_` runs before the close is
     * accepted, which is what lets run_gui stop the timers and hand the device
     * back while the event loop is still alive. */
    std::function<void()> on_close_;
    std::function<void()> on_retry_;
    /* Quit is a callback rather than close() inline so --selftest can observe
     * `q` without tearing the window down.  run_gui installs it as close(). */
    std::function<void()> on_quit_;

    /* Capture and recording, callbacks for the same reason quit is one:
     * --selftest drives the real keys and then looks at what changed. */
    std::function<void()> on_still_;
    std::function<void()> on_record_;

    /* The gallery.  `on_gallery_open_` is handed the highlighted entry (NULL
     * when there is none) and `on_gallery_export_` likewise; both are the front
     * end's, because one needs the pipeline and the other the disk.  Open
     * returns false when the entry could not be opened, which keeps the list up
     * rather than hiding it over nothing. */
    std::function<bool(const dyt_vm_item_t *)> on_gallery_open_;
    std::function<void(const dyt_vm_item_t *)> on_gallery_export_;
    /* Called when the list is opened, so the front end can rescan and pick up
     * anything saved since it was last looked at. */
    std::function<void()> on_gallery_refresh_;

    /* The gallery's Folder… button (the File menu's item went with the menu).
     * The front end owns the dialog and the directory, so the window only
     * reports that it was asked for — the same split as every other callback
     * here. */
    std::function<void()> on_choose_folder_;

    /* The About box.  A callback so --selftest can see the key without a modal
     * dialog blocking the event loop. */
    std::function<void()> on_about_;

    /* The usage guide.  A callback for the same reason About is one: the modal
     * lives in the front end, so --selftest can observe the route without a
     * dialog.  It has no key of its own — F1 is About, and F sits next to 'f'
     * (fusion), where a mis-shift would open a modal.  The rail's Tutorials
     * item is how it is reached. */
    std::function<void()> on_help_;

    /* The browsing state, owned here and read by the canvas at paint time. */
    dyt_vm_gallery_t *gallery() { return &gal_; }

    /* Borrow the clip reader from the pump.  The window reads its state (is a
     * clip up? paused?) and steers it (pause, stop), but the pump owns it and
     * is what advances it — the same borrow as sess_. */
    void set_playback(PlaybackCtl *p) { play_ = p; }

    /* Show a saved still: the canvas draws it and line 3 names it.  The pump
     * keeps painting the live frame underneath, so clear_viewing() restores the
     * stream with no source to swap back. */
    void set_viewing(const QImage &img, const QString &label)
    {
        viewing_label_ = label;
        view_->set_override(img);
        strip_->set_lines(strip_->line(0), strip_->line(1), label);
        fit_to_view();
    }

    void clear_viewing()
    {
        viewing_label_.clear();
        view_->set_override(QImage());
        /* Back to the live view's size.  set_viewing() re-fits the window to
         * what it is showing, so dismissing a still or a clip has to undo
         * that — a 64x48 clip played over a 512x384 stream would otherwise
         * leave the window sized for the clip.  Guarded inside fit_to_view(),
         * so the common case of nothing having changed resizes nothing. */
        fit_to_view();
    }

    bool viewing() const { return !viewing_label_.isEmpty(); }

    /* The directory the list is showing, for the panel's own header — the one
     * place a user can see where the files they are looking at actually are. */
    void set_folder(const QString &dir)
    {
        if (gal_panel_)
            gal_panel_->set_folder(dir);
    }

    /* Rescan and refill the panel in one step, so nothing can rescan the state
     * and forget to refresh what is on screen.  The scan itself is the front
     * end's (it owns the directory). */
    void rescan_gallery()
    {
        if (on_gallery_refresh_)
            on_gallery_refresh_();
        if (gal_panel_)
            gal_panel_->reload();
    }

    /* Stop any playing clip and return the canvas to the live view.  One place,
     * so the badge and the override cannot be cleared without stopping the
     * reader (or the other way round). */
    void stop_playback()
    {
        if (play_)
            play_->stop();
        if (strip_)
            strip_->set_playback(QString());
        clear_viewing();
    }

    /* Open the highlighted entry, then get out of the way: the panel hides so
     * the still or the playing clip fills the canvas, and 'g' brings the list
     * back.  Doing it here rather than in the front end keeps the keyboard and
     * the mouse on one path.
     *
     * A failure leaves the list up.  Hiding it over a clip that could not be
     * decoded would leave the user looking at the live view with no idea why —
     * the notice on the strip is the answer, and it needs the list to stay put
     * so another entry can be tried. */
    void open_gallery_sel()
    {
        const dyt_vm_item_t *it = dyt_vm_gallery_sel(&gal_);
        if (!it || !on_gallery_open_)
            return;                 /* nothing highlighted: leave the list up */
        if (!on_gallery_open_(it))
            return;
        gal_.open = 0;
        if (gal_panel_)
            gal_panel_->hide();
        view_->update();
    }

    /* Show or hide the list, keeping the panel and the state in step.  Closing
     * returns to the live view, which is the contract 'g' and Esc share — and
     * that includes stopping a clip, since a clip plays with the panel hidden
     * and closing is how a user says "back to the camera". */
    void set_gallery_open(bool open)
    {
        gal_.open = open ? 1 : 0;
        if (!gal_panel_)
            return;
        if (gal_.open) {
            rescan_gallery();
            gal_panel_->show();
            gal_panel_->raise();
        } else {
            gal_panel_->hide();
            stop_playback();
        }
        view_->update();
    }

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
     * fight a user who resized the window by hand.
     *
     * Nothing here while fullscreen is on.  This runs on every painted frame
     * (set_frame_status), and resize() on a fullscreen window is not harmless:
     * the window manager owns that geometry and may drop the fullscreen hint,
     * and on the offscreen platform resize() really does change the size — so
     * a zoom during fullscreen would resize the window out from under
     * showFullScreen().  The guard sits *before* the cache, so `fitted_` is not
     * advanced while fullscreen either and leaving it re-fits if the canvas
     * changed size in the meantime. */
    void fit_to_view()
    {
        if (fullscreen_)
            return;

        const QSize want = view_->sizeHint();
        if (want == fitted_)
            return;
        fitted_ = want;
        /* The window's hint can lag the view's fresh one by one event-loop
         * turn.  The canvas's updateGeometry() only invalidates its *immediate*
         * parent's layout and *posts* a LayoutRequest; the outer layout's
         * cached hint for the centre column is not recomputed until that event
         * runs.  Reading sizeHint() without flushing would size the window from
         * the previous layout — invisible while the chrome is small, and it
         * clips the canvas as soon as it is not (the icon rail's fixed width is
         * what exposed it).  Flush the posted requests so sizeHint() below is
         * the one that holds this canvas. */
        QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        if (QLayout *l = layout())
            l->activate();
        resize(sizeHint());
    }

    /* Full screen, the vendor viewer's F11.  A window state, not a canvas one:
     * the canvas draws the same picture either way and scales it to whatever
     * room it is given, so there is nothing to tell FrameView here beyond a
     * repaint.
     *
     * showNormal() restores the geometry the window had before, but `fitted_`
     * is deliberately cleared: if the canvas grew while fullscreen (a zoom, or
     * a super-resolved frame) the window would otherwise come back too small
     * and clip the picture.  fit_to_view() then re-fits to the new hint. */
    void toggle_fullscreen()
    {
        fullscreen_ = !fullscreen_;
        if (fullscreen_) {
            showFullScreen();
        } else {
            showNormal();
            fitted_ = QSize();
            fit_to_view();
        }
        /* Told to the Settings dialog here as well as in sync_actions(),
         * because the fullscreen state does not come from a frame: a stalled
         * stream paints no frame, so the per-frame sync would not run and the
         * box would keep claiming the old state. */
        if (settings_)
            settings_->sync_window(fullscreen_, view_ && view_->info_shown());
        if (view_)
            view_->update();
    }

    bool fullscreen() const { return fullscreen_; }

    /* A painted frame: all three lines. */
    void set_frame_status(const dyt_snapshot_t &snap, DevState st, double fps,
                          dyt_mode_t mode, const char *msg = nullptr)
    {
        char a[256], b[256];
        dyt_vm_status_line(&snap, mode, a, sizeof a);
        dyt_vm_readout_line(&snap, msg, b, sizeof b);
        strip_->set_lines(QString::fromUtf8(a), QString::fromUtf8(b),
                          viewing() ? viewing_label_
                                    : state_line(st, snap, fps, serial()));
        strip_->set_conn(st);
        strip_->set_alarm(snap.alarm_on != 0, snap.alarm);
        snap_ = snap;               /* for the action lambdas */
        fit_to_view();
        sync_actions();
    }

    /* No frame to paint — the placeholder is up, so lines 1 and 2 keep
     * whatever they last said and only the state line moves. */
    void set_state_line(DevState st, const dyt_snapshot_t &snap, double fps)
    {
        strip_->set_lines(strip_->line(0), strip_->line(1),
                          viewing() ? viewing_label_
                                    : state_line(st, snap, fps, serial()));
        strip_->set_conn(st);
        /* The panel's Super Resolution page is session state, not frame state,
         * so it stays right while the canvas is still a placeholder — which is
         * exactly when a user wondering why the radios are dead needs it. */
        sync_sr_panel();
    }

    /* The module serial the bring-up read found, or NULL when there is no
     * device (the fixture path) or the read failed.
     *
     * Read from FrameView, which holds the one dyt_device_info_t the info
     * panel and the Settings dialog also read — the device identity is the
     * front end's, not the session's (the session never touches the capture
     * layer; the front end does the idle-window read itself).  One source
     * means the status line and the info panel cannot disagree about which
     * camera is attached, and a teardown that clears the info clears both. */
    const char *serial() const
    {
        if (!view_)
            return nullptr;
        const dyt_device_info_t &d = view_->device_info();
        return d.have_sn ? d.sn_str : nullptr;
    }

    /* The single key dispatch.  Every key the app understands is interpreted
     * here and nowhere else, so the keyboard and the on-screen controls cannot
     * disagree: a rail button, a panel row or a palette entry is just a call to
     * this with the character its key would produce.  That is also why no
     * QAction carries a
     * shortcut — Qt's shortcut map runs *before* keyPressEvent, so a shortcut
     * would fire while a runtime-parameter candidate is armed and break the
     * ladder's swallow contract (assertion 30).  The key's name belongs in the
     * item's text, not in its shortcut.
     *
     * `raw` is the *unfolded* character.  Returns 1 when the key was consumed;
     * 0 lets the caller fall through to the base class.
     *
     * Order matters.  The runtime-parameter ladder goes first, because while a
     * candidate is armed the view model consumes every key except 'q', so a
     * stray palette or tool key cannot slip past a pending confirmation. */
    /* The Troubleshoot panel's circuit modes.  One function, reached by both
     * the row (through handle_key) and the keyboard, so the checkmark, the
     * range and the view cannot disagree: the policy is dyt_vm_leak_effect()'s,
     * the range goes to the session through the view, and the 3D half switches
     * the panel's own tab the way the rail's Compare item does. */
    void apply_leak_mode(int mode)
    {
        dyt_leak_effect_t fx;

        if (dyt_vm_leak_effect(mode, &fx) != 0)
            return;
        if (view_)
            view_->set_range_mode(fx.range);
        if (panel_) {
            panel_->set_leak_mode(mode);
            if (fx.view_3d)
                panel_->tabs()->setCurrentIndex(ControlPanel::TabAnalysis3D);
        }
    }

    int handle_key(int raw)
    {
        if (view_ && view_->param_key(raw))
            return 1;

        /* Full screen.  Placed after the ladder, so it is swallowed while a
         * write is armed like every other key but 'q'.  Qt::Key_F11, not a
         * character: an event for it carries no text, so keyPressEvent's
         * unfold leaves the key code in `raw` untouched. */
        if (raw == Qt::Key_F11) {
            toggle_fullscreen();
            return 1;
        }

        /* Quit.  'q' is deliberately never swallowed, armed or not. */
        if (raw == 'q') {
            if (on_quit_)
                on_quit_();
            else
                close();
            return 1;
        }

        /* Retry: lowercase only, because uppercase 'R' arms reflected. */
        if (raw == 'r' && on_retry_) {
            on_retry_();
            return 1;
        }

        /* The device panel. */
        if (raw == 'd' && view_) {
            view_->toggle_info();
            return 1;
        }

        /* The hottest/coldest markers, the Windows panel's Tracking switch.
         * 'm' is free of every other binding (the tools are p/l/b/n, capture
         * s/v, gallery g, retry r, panel d, quit q, and the ladders e/A/R/D/y
         * and the view keys). */
        if (raw == 'm' && view_) {
            view_->toggle_hot();
            return 1;
        }

        /* About / the key list.  A callback rather than the dialog inline so
         * --selftest can observe the key without a modal window. */
        if ((raw == '?' || raw == Qt::Key_F1) && on_about_) {
            on_about_();
            return 1;
        }

        /* The gallery.  'g' toggles the list; while it is open the arrows (and
         * j/k) move the highlight, 'o' or Enter opens the entry and 'x' exports
         * it, and Esc closes.  While the list is open it swallows the other
         * bindings, because a key that moved the highlight must not also change
         * the tool or arm a parameter. */
        if (raw == 'g' && view_) {
            set_gallery_open(!gal_.open);
            return 1;
        }
        if (gal_.open) {
            if (raw == Qt::Key_Up || raw == 'k') {
                dyt_vm_gallery_move(&gal_, -1);
                if (gal_panel_)
                    gal_panel_->sync();
                return 1;
            }
            if (raw == Qt::Key_Down || raw == 'j') {
                dyt_vm_gallery_move(&gal_, +1);
                if (gal_panel_)
                    gal_panel_->sync();
                return 1;
            }
            /* Return arrives as 13 when the event carries text and as
             * Qt::Key_Return when it does not (a synthesized event), so both
             * are matched.  Opening hides the panel, so it is not a key that
             * leaves the list up. */
            if (raw == 13 || raw == Qt::Key_Return || raw == Qt::Key_Enter ||
                raw == 'o') {
                open_gallery_sel();
                return 1;
            }
            if (raw == 'x' && on_gallery_export_) {
                on_gallery_export_(dyt_vm_gallery_sel(&gal_));
                return 1;
            }
            if (raw == 27) {                    /* Esc closes, back to live */
                set_gallery_open(false);
                return 1;
            }
            /* Anything else is swallowed rather than acted on: the list has
             * the keyboard while it is up. */
            return 1;
        }

        /* A playing clip, with the list closed — opening an entry hides the
         * panel, so this is the state a clip actually plays in.  Space pauses
         * and resumes, Esc stops it and returns to the live view.  It is
         * deliberately not a key that swallows everything else: 'g' must still
         * reopen the list, and 'q' must still quit. */
        if (play_ && play_->active) {
            if (raw == ' ') {
                play_->paused = !play_->paused;
                if (strip_)
                    strip_->set_playback(QString::fromStdString(play_->label()));
                return 1;
            }
            if (raw == 27) {                    /* Esc stops, back to live */
                stop_playback();
                return 1;
            }
        }

        /* Capture: 's' saves a still, 'v' toggles a clip.  Both are free of
         * the device keys (r/d), the measurement keys (p/l/b/n/a/i) and the
         * parameter ladder (e/A/R/D/y), and both are lowercase. */
        if (raw == 's' && on_still_) {
            on_still_();
            return 1;
        }
        if (raw == 'v' && on_record_) {
            on_record_();
            return 1;
        }

        /* Rapid Diagnostics — the panel row routes 'F' here too.  Placed
         * before the view keys so the shift form is not folded away, and
         * guarded on view_ so a synthetic press with no canvas is inert. */
        if (raw == 'F' && view_) {
            view_->rapid_diagnostics();
            return 1;
        }

        /* The circuit modes, on their shift forms — 's', 'l' and 'm' are still,
         * line and tracking.  Before the view keys and the fold below, so the
         * shift is not erased and 'L' cannot fall through to the line tool. */
        if (raw == 'S' || raw == 'L' || raw == 'M') {
            apply_leak_mode(raw == 'S' ? DYT_LEAK_SHORT
                          : raw == 'L' ? DYT_LEAK_LARGE
                                       : DYT_LEAK_SMALL);
            return 1;
        }

        /* The 3D page's height mode, also on its shift forms — 'p' is the
         * point tool and 'c' is chart analysis, and both would otherwise be
         * reached by the fold. */
        if ((raw == 'P' || raw == 'C') && panel_ && panel_->surface()) {
            panel_->set_surface_mode(raw == 'P' ? DYT_SURFACE_SHAPE
                                                : DYT_SURFACE_COLOR);
            return 1;
        }

        /* The Mark annotations, on their shift forms — 't' is the fixed-range
         * toggle, so the Text tool is 'T'.  Above the fold, or the shift would
         * be erased and the range would flip instead of a label being armed.
         * A second press puts the tool away, so the key that armed it also
         * disarms it (the vendor's label button toggles the same way). */
        if (raw == 'T' && view_) {
            view_->set_mark_mode(view_->mark_mode() == MarkText ? MarkNone
                                                                : MarkText);
            sync_actions();
            return 1;
        }

        /* How the picture is shown: palette, unit, range, flip, zoom, fusion.
         * Routed with the unfolded character, because two of the bindings are
         * Shift forms ('H' flips vertically where 'h' flips horizontally) and
         * the fold below would erase the difference. */
        if (view_ && view_->view_key(raw))
            return 1;

        /* Everything else is the measurement bindings, which are lowercase —
         * so fold the unfolded character back down before consulting them.
         * The return value decides, rather than a list of letters here, so
         * --selftest and the window cannot disagree about what is bound. */
        int k = raw;
        if (k >= 'A' && k <= 'Z')
            k += 'a' - 'A';

        /* The Analysis group's chart, above the tool keys because 'l' is one
         * of them: the row and the key must move the chart the same way, and
         * the rows route exactly these two keys.  'l' falls through to the
         * tool dispatch below, which is what selects the line; 'c' is consumed
         * here, and picks the line tool up on its way through the same
         * dispatch — a chart with no line has nothing to show, and a row that
         * only lights up is the dead control the on-screen-controls rule
         * exists to prevent. */
        if (panel_ && (k == 'l' || k == 'c')) {
            panel_->set_chart_mode(k == 'c' ? ProfilePlot::Analysis
                                            : ProfilePlot::Line);
            if (k == 'c') {
                if (view_)
                    view_->measure_key('l');
                return 1;
            }
        }

        if (view_ && view_->measure_key(k))
            return 1;

        return 0;
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

        if (!handle_key(raw))
            QWidget::keyPressEvent(e);
    }

private:
    /* ---- the palette picker's items ------------------------------------
     *
     * These two builders outlived the menu bar they were written for: the
     * palette popup is the one menu left, and it follows the same rule the
     * retired menus did — an item that has a key *runs that key* through
     * handle_key(), so the on-screen route can never be a second
     * implementation.  Nothing here sets a QAction shortcut: Qt's shortcut map
     * consumes a matching key *before* keyPressEvent runs, so a shortcut would
     * fire while a runtime-parameter candidate is armed and break the ladder's
     * swallow contract (assertion 30).  The key's name goes in the tooltip
     * instead.
     *
     * The one exception is the entry that selects an *absolute* value no key
     * can express — a palette past the tenth — which sets the session directly.
     * It is a single session setter, so there is no rule for the two to
     * disagree about. */

    /* An item that does exactly what its key does.  `hint` names the key, for
     * the tooltip only. */
    QAction *key_action(const QString &text, int key, const char *hint,
                        bool checkable = false)
    {
        QAction *a = new QAction(text, this);
        a->setCheckable(checkable);
        a->setToolTip(QStringLiteral("%1    %2")
                          .arg(text, QString::fromUtf8(hint)));
        connect(a, &QAction::triggered, this, [this, key]() {
            handle_key(key);
            setFocus();     /* the click took it; the keys need it back */
        });
        return a;
    }

    /* An item with no key of its own. */
    QAction *plain_action(const QString &text, std::function<void()> fn,
                          bool checkable = false)
    {
        QAction *a = new QAction(text, this);
        a->setCheckable(checkable);
        connect(a, &QAction::triggered, this, [this, fn]() {
            if (fn)
                fn();
            setFocus();
        });
        return a;
    }

    /* Opens palette_menu() under the rail's button.  The build/show split is
     * the same one the Settings dialog makes: --selftest has to inspect the
     * picker without entering a nested event loop.
     *
     * popup() rather than exec(): popup() returns at once, so no nested event
     * loop sits on the stack while the picker is open — the pump keeps
     * painting, and the button can be driven in a test.  The menu deletes
     * itself when it is dismissed (WA_DeleteOnClose), so opening it repeatedly
     * does not accumulate menus; it is parented to the window as a backstop,
     * so one still open when the window dies is freed with it. */
    void show_palette_popup()
    {
        QPushButton *btn = rail_ ? rail_->button(IconRail::Palette) : nullptr;
        QMenu *menu = palette_menu();
        if (!btn || !menu)
            return;
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->popup(btn->mapToGlobal(QPoint(0, btn->height())));
    }

    /* Bring the on-screen checkmarks up to date with the frame that was just
     * painted.
     *
     * The snapshot is the authority, not the button's own toggle: a control
     * whose key was refused (a super-resolution plane with no model, a write
     * the device rejected) must not stay lit.  ControlPanel::sync() blocks the
     * buttons' own signals for the same reason.
     *
     * Called only from set_frame_status — never from set_state_line, whose
     * snapshot is zeroed on the no-device path and would clear every
     * checkmark.  A stalled stream therefore keeps the last real frame's
     * states, which is what the canvas is still showing. */
    void sync_actions()
    {
        /* The panel's checkmarks, from the snapshot plus the four states that
         * live on the canvas or the window. */
        if (panel_)
            panel_->sync(snap_, view_ && view_->hot_shown(),
                         strip_ && !strip_->recording_label().isEmpty(),
                         gal_.open != 0,
                         view_ ? view_->mark_mode() : MarkNone);

        /* The Analysis chart.  The profile is read here rather than in the
         * panel, so the widget stays free of the session and its lock — and
         * because this is the only place that has the session at all.  A
         * profile the session refuses (no frame, no line tool) is passed as
         * none, which puts the chart back to its placeholder. */
        if (panel_) {
            float prof[DYT_PROFILE_MAX];
            int   n = sess_ ? dyt_session_profile(sess_, prof, DYT_PROFILE_MAX) : 0;
            panel_->sync_analysis(snap_, n > 0 ? prof : nullptr, n > 0 ? n : 0);
        }

        /* The rail's Mark, by the same rule: a rail button is checkable, so a
         * click that unchecked it would leave the mark disagreeing with the
         * session until the next frame put it right.  Recording the last
         * *active* tool is what lets Mark re-arm it rather than always
         * choosing Spot. */
        if (snap_.tool != DYT_TOOL_NONE)
            last_tool_ = snap_.tool;
        if (rail_) {
            if (QPushButton *b = rail_->button(IconRail::Mark)) {
                const QSignalBlocker block(b);
                b->setChecked(snap_.tool != DYT_TOOL_NONE);
            }
            /* Rotate is a *mode* rather than an action, so its lit state is
             * worth showing: it says the picture is turned, and it goes out
             * again when Reset Image clears the rotation. */
            if (QPushButton *b = rail_->button(IconRail::Rotate)) {
                const QSignalBlocker block(b);
                b->setChecked(snap_.xform.rot != DYT_ROT_NONE);
            }
        }

        /* The Settings dialog's two window-state boxes, so 'd' toggled from the
         * keyboard cannot leave the dialog showing the other state.  Only when
         * it exists: it is created on first use, and there is nothing to tell
         * before that. */
        if (settings_)
            settings_->sync_window(fullscreen_, view_ && view_->info_shown());

        /* The Super Resolution page reads the session rather than the
         * snapshot, so it is synced from here too — it is correct even for the
         * states the snapshot zeroes. */
        sync_sr_panel();
    }

    void sync_sr_panel()
    {
        if (panel_)
            panel_->sync_sr(sr_model_loaded(),
                            sess_ ? dyt_session_get_sr(sess_) : DYT_SR_OFF);
    }

    FrameView   *view_  = nullptr;
    StatusStrip *strip_ = nullptr;
    IconRail    *rail_  = nullptr;   /* the left icon rail, a child of this */
    ControlPanel *panel_ = nullptr;  /* the right tabbed panel, a child of this */
    /* The Settings dialog, created on first use.  A child of this, so it is
     * destroyed with the window and needs no lifetime of its own. */
    SettingsDialog *settings_ = nullptr;
    /* The scratch the dialog is seeded from, indexed by dyt_order_type_t. */
    float settings_cur_[DYT_ORDER_DISTANCE + 1] = {};
    /* The last measurement tool that was not NONE, so the rail's Mark can
     * re-arm it instead of always choosing Spot.  Spot is the start-up value,
     * which is also the default the reference's Mark button has. */
    dyt_tool_t last_tool_ = DYT_TOOL_POINT;
    dyt_session_t *sess_ = nullptr;      /* borrowed */
    /* The clip playing on the canvas, if any.  Borrowed from the pump, which
     * advances it — the same pattern as sess_: the window reads and steers the
     * state, the pump owns its lifetime. */
    PlaybackCtl *play_ = nullptr;
    /* The last painted frame, so an action lambda can read the current state
     * (which super-resolution plane to switch off, say) without a second
     * snapshot call. */
    dyt_snapshot_t snap_{};
    QSize        fitted_{};
    /* Whether showFullScreen() is in force.  Tracked here rather than asked of
     * the window manager, because fit_to_view() consults it on every painted
     * frame and isFullScreen() is not free. */
    bool         fullscreen_ = false;
    dyt_vm_gallery_t gal_{};
    GalleryPanel *gal_panel_ = nullptr;   /* child of view_, owned by Qt */
    QString      viewing_label_;
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

    /* The still writer and the one clip.  It lives here rather than in
     * run_gui because step() is what feeds it a frame. */
    CaptureCtl capture;

    /* The clip being played, the mirror of `capture`.  Also here because
     * step() is what advances it — and at the per-tick hook, not the frame
     * path, so a clip keeps playing on the WAIT and no-frame ticks.  The
     * window borrows it (MainWindow::play_) to pause and stop it. */
    PlaybackCtl playback;

    /* The reference still for the Comparison tab.  Loaded by the "Load
     * reference" button (or --reference PATH) and held here because the pump
     * owns the session and the per-frame temps: it is what can run
     * dyt_compare_stats() against the live grid.  The path is kept so the
     * panel can show what is loaded.  An empty path means nothing is. */
    std::string  ref_path;
    std::vector<float> ref_temps;
    int          ref_w = 0, ref_h = 0;

    /* Post a transient notice.  A method so the callers cannot set one without
     * the other and leave a message that never expires. */
    void notice(const std::string &s, int ttl = 150)
    {
        msg     = s;
        msg_ttl = ttl;
    }

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

        /* Keep the recording badge current even on a tick that paints nothing:
         * the clock has to advance whether or not a frame arrived. */
        win->strip()->set_recording(QString::fromStdString(capture.label()));
        win->strip()->set_playback(QString::fromStdString(playback.label()));

        /* A playing clip advances here, at the frame-independent hook, so it
         * keeps moving on the WAIT and no-frame ticks — which is exactly when
         * a live stream has nothing to show and a recorded clip does.  It is
         * also before the no-source return below, so browsing works with no
         * camera attached.  The frame goes through the same set_viewing()
         * override a saved still uses, so the live frame keeps painting
         * underneath and Esc has nothing to swap back. */
        if (playback.active) {
            std::string why;
            if (playback.advance(why)) {
                const QImage frame(playback.buf.data(), playback.w, playback.h,
                                   playback.w * 3, QImage::Format_RGB888);
                win->set_viewing(frame.copy(),
                                 QString::fromStdString(playback.view_label()));
            }
            if (!why.empty())
                notice(why);
        }

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
         * because Qt may pad bytesPerLine while the pass assumes tightly
         * packed w*3 rows.  The buffer is RGB, not BGR, but the pass halves
         * every channel, so the order does not matter.
         *
         * The scaled entry point is the one that matters here: with
         * super-resolution on the image is 2x the temperature plane, so the
         * pass has to sample the plane it was magnified from.  The unscaled
         * one would refuse (its `temps_n >= w*h` guard) and silently dim
         * nothing. */
        std::vector<uint8_t> iso_buf;
        const uint8_t       *pix = rgb;
        if (snap.iso_on && scr.temps &&
            scr.cap >= snap.width * snap.height) {
            iso_buf.assign(rgb, rgb + (size_t)w * (size_t)h * 3);
            dyt_vm_apply_isotherm_scaled(iso_buf.data(), w, h, scr.temps,
                                         scr.cap, snap.width, snap.height,
                                         snap.iso_lo, snap.iso_hi);
            pix = iso_buf.data();
        }

        /* Record the frame as displayed — the same buffer the canvas gets,
         * isotherm dimming included.  A clip that stopped itself (the disk
         * filled) leaves its reason in the notice, so the strip says why rather
         * than the badge just vanishing. */
        if (capture.recording) {
            std::string why;
            if (!capture.feed(pix, w, h, why))
                notice(why);
        }

        /* The engine's buffer is tightly packed RGB, so QImage wraps it with
         * no conversion.  transformed() owns what it returns, because the
         * source buffer dies on the next next(). */
        const QImage wrapped(pix, w, h, w * 3, QImage::Format_RGB888);
        win->view()->set_frame(snap, transformed(wrapped, snap.xform), pal,
                               scr.temps);
        win->set_frame_status(snap, ds, fps.fps(), mode,
                              msg.empty() ? nullptr : msg.c_str());
        sync_compare_panel(snap, scr.temps, scr.cap);
        sync_surface(snap, scr.temps, scr.cap);

        frames++;                        /* painted frames, for --frames/--png */
        return true;
    }

    /* Push the Comparison tab's path + difference stats.  The reference grid
     * lives here (the pump owns the session); the stats are computed against
     * the live grid the snapshot just produced, so the page is correct on
     * every painted frame.  The threshold comes from the panel's spin box,
     * which is the only input the user has for it.  No reference, or a
     * reference whose size does not match the live frame, leaves the stats
     * box empty rather than handing the engine a size-mismatched pair. */
    void sync_compare_panel(const dyt_snapshot_t &snap,
                            const float *live, int live_cap)
    {
        if (!win || !win->panel())
            return;

        const bool have_ref = !ref_path.empty() &&
                              ref_w == snap.width && ref_h == snap.height &&
                              live && live_cap >= snap.width * snap.height &&
                              (int)ref_temps.size() >= snap.width * snap.height;
        QString stats;
        if (have_ref) {
            dyt_compare_stats_t s;
            const double thresh = win->panel()->compare_threshold();
            if (dyt_compare_stats(live, ref_temps.data(),
                                  snap.width, snap.height,
                                  (float)thresh, &s) == 0 && s.n > 0) {
                stats = QStringLiteral(
                    "Pixels compared: %1\n"
                    "Difference: min %2 C, max %3 C, mean %4 C\n"
                    "Largest |Δ|: %5 C at (%6, %7)\n"
                    "Beyond threshold (%8 C): %9")
                    .arg(s.n)
                    .arg(s.min, 0, 'f', 1)
                    .arg(s.max, 0, 'f', 1)
                    .arg(s.mean, 0, 'f', 2)
                    .arg(s.max_abs, 0, 'f', 1)
                    .arg(s.max_abs_x)
                    .arg(s.max_abs_y)
                    .arg(thresh, 0, 'f', 1)
                    .arg(s.beyond);
            } else if (s.n == 0) {
                stats = QStringLiteral("No finite overlap.");
            }
        }
        win->panel()->sync_compare(
            QString::fromStdString(ref_path), have_ref, stats);
    }

    /* Feed the 3D Analysis view.  It gets the same Celsius plane the canvas
     * just drew and the same display range the snapshot resolved for it, so a
     * peak reads the same colour in the surface as in the 2D view — which is
     * the whole claim the tab makes.  The widget does the decimation and the
     * mesh build; the pump only hands over the data, the way sync_compare_panel
     * hands over a grid rather than formatting pixels itself. */
    void sync_surface(const dyt_snapshot_t &snap, const float *live, int live_cap)
    {
        if (!win || !win->panel() || !win->panel()->surface())
            return;
        if (!live || live_cap < snap.width * snap.height)
            return;
        win->panel()->surface()->set_grid(live, snap.width, snap.height,
                                          pal, snap.lo, snap.hi);
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

/* ---------------------------------------------------------- the key list
 *
 * One table, two renderings.  The About box shows the bare list; the Help
 * dialog shows the same lines under their headings.  The two therefore cannot
 * disagree about what a key does — the same rule the status lines follow — and
 * --selftest pins that they do not.
 *
 * The lines are stored *whole* rather than formatted from a label and a
 * description.  About's output is what a user reads off a screenshot, and a
 * `%-14s` that is one column off would be a silent regression no test would
 * name.  A key that needs a heading of its own goes in as its own group. */

struct key_line_t {
    const char *group;
    const char *line;
};

static const key_line_t kKeyLines[] = {
    { "measurement", "  p l b o n     point / line / box / polygon / clear\n" },
    { "measurement", "  T             Mark: place a text label\n" },
    { "measurement", "  enter bksp    polygon: finish the outline / undo a vertex\n" },
    { "measurement", "  c             Analysis: annotate the line's chart\n" },
    { "measurement", "  a i           alarm / isotherm\n" },
    { "measurement", "  m             hottest/coldest markers on/off\n" },
    { "the device",  "  e A R D y     emissivity / ambient / reflected / distance, send\n" },
    { "the device",  "  d r           device panel / retry\n" },
    { "capture",     "  s v           save a still / record a clip\n" },
    { "capture",     "  g             gallery: browse\n" },
    { "capture",     "  o enter x     (in the gallery) open / open / export\n" },
    { "capture",     "  space         pause / resume a playing clip\n" },
    { "the picture", "  1-0 , .       palette        u  unit\n" },
    { "the picture", "  t             range auto/fixed\n" },
    { "the picture", "  F             rapid diagnostics: auto-fit the range\n" },
    { "the picture", "  S L M         circuit: short / large leak / small leak\n" },
    { "the picture", "  P C           3D height: morphological / colour changes\n" },
    { "the picture", "  h H           flip horizontally / vertically\n" },
    { "the picture", "  + -           zoom\n" },
    { "the picture", "  z Z           super-resolve the visible / thermal plane\n" },
    { "the picture", "  f [ ] ; '     fusion pattern, alignment\n" },
    { "the window",  "  q             quit\n" },
};

/* The bare list, in the order About has always shown it. */
static std::string key_list_text()
{
    std::string s;
    for (const key_line_t &k : kKeyLines)
        s += k.line;
    return s;
}

/* ------------------------------------------------------------- selftest
 *
 * The offline check.  It runs the same path the window does, under
 * QT_QPA_PLATFORM=offscreen, and asserts on the result rather than leaving a
 * human to look at a window: the fixture really converted (not the ~238.85 C
 * start-up filler), the geometry is the sensor's, all three status lines are
 * populated, and the canvas actually painted more than one colour.
 */

/* Defined with the About action below; the selftest reads them to check the
 * version About reports, the super-resolution line, and that the guide covers
 * every key the About list does. */
static std::string about_text(bool have_model = false);
static std::string help_text();

static int selftest(const opts &o)
{
    int fails = 0;
    int want  = o.frames > 0 ? o.frames : 25;

    std::fprintf(stderr, "dytqt --selftest: %s (%dpx), %d frames at %d fps\n",
                 o.fixture.c_str(), o.width, want, o.fps);

    dyt_session_t *sess = setup_session(o);
    if (!sess)
        return 1;
    setup_super_resolution(sess, o);

    dyt_frame_source_t *fs = dyt_frame_source_open_fixture(
        sess, o.fixture.c_str(), o.width, DYT_MODE_1000,
        DYT_PLANE_BOTTOM_HALF, o.cap.t_amb, o.cap.sensor_mode,
        o.cap.fix_mode, 0);
    if (!fs) {
        dyt_session_free(sess);
        return 1;
    }

    MainWindow win;
    win.set_session(sess);
    pump       pm;
    pm.fs   = fs;
    pm.sess = sess;
    pm.win  = &win;
    pm.mode = DYT_MODE_1000;
    win.set_playback(&pm.playback);    /* borrowed, exactly as run_gui wires it */
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

        /* The canvas is written, not the window the check above grabs: --png
         * is documented as "write the canvas here".  The window is what --shot
         * is for — the rail and the panel are in it, and this file's assertion
         * 6 is the only place a window grab is otherwise needed. */
        if (!o.png.empty() && win.view()->has_frame()) {
            if (win.view()->grab().save(QString::fromUtf8(o.png.c_str()))) {
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
     * hold for an untransformed frame too, and would prove nothing.  The
     * super-resolution factor is in the rendered size as well — the engine
     * renders 2x and the window magnifies that by zoom — so it is read from
     * the snapshot rather than assumed to be 1. */
    {
        const int   z    = snap.xform.zoom;
        const int   sr   = snap.xform.sr >= 1 ? snap.xform.sr : 1;
        const QSize is   = win.view()->imageSize();
        const QSize hint = win.view()->sizeHint();
        const bool  ok   = z > 1 && is == QSize(256 * z * sr, 192 * z * sr) &&
                           hint.height() >= is.height();
        std::printf("  %-4s zoom %d is applied to the frame (%dx%d, hint %dx%d)\n",
                    ok ? "ok" : "FAIL", z, is.width(), is.height(),
                    hint.width(), hint.height());
        if (!ok)
            fails++;
    }

    /* 16. The mirror is applied, in the sense dyt_view_transform_map() inverts.
     * Driven directly through transformed() rather than by pressing a key,
     * because what is under test here is the *render*: the flip's routing to
     * the session is assertion 44's, and the mapping is display_test's, but
     * neither proves Qt mirrors the picture the way the mapping says.  A 3x1
     * source with a distinct pixel at each end, at zoom 2 with flip_h, must
     * come out 6 wide with the ends swapped. */
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

    /* 16b. The rotation is applied — and, the part that actually matters, the
     * Qt render and the engine's mapping agree about *which way*.  A source
     * encodes its own coordinates in its pixel colours, and every pixel of the
     * turned output must be the source pixel dyt_view_transform_map() names.
     * That is the same property the pointer routing rests on, and the one a
     * front end that turned the picture the other way round would fail while
     * still producing a correctly sized image. */
    {
        const int SW = 3, SH = 2;
        QImage src(SW, SH, QImage::Format_RGB888);
        for (int y = 0; y < SH; y++)
            for (int x = 0; x < SW; x++)
                src.setPixelColor(x, y, QColor(x * 40, y * 40, 0));

        dyt_view_transform_t t{};
        t.rot = DYT_ROT_90;
        const QImage out = transformed(src, t);

        int bad = 0, seen = 0;
        for (int oy = 0; oy < out.height(); oy++) {
            for (int ox = 0; ox < out.width(); ox++) {
                int sx = -1, sy = -1;

                if (dyt_view_transform_map(&t, SW, SH, out.width(),
                                           out.height(), ox, oy,
                                           &sx, &sy) != 0 ||
                    out.pixelColor(ox, oy) != src.pixelColor(sx, sy))
                    bad++;
                seen++;
            }
        }

        /* And the turn is clockwise, which is the direction the rail's button
         * names: the source's top-left corner comes out at the output's
         * top-right, and its bottom-right at the output's bottom-left. */
        const bool dir =
            out.width() == SH && out.height() == SW &&
            out.pixelColor(out.width() - 1, 0) == src.pixelColor(0, 0) &&
            out.pixelColor(0, out.height() - 1) == src.pixelColor(SW - 1, SH - 1);

        const bool ok = bad == 0 && seen == SW * SH && dir;
        std::printf("  %-4s the rotation is applied (rot %d, %dx%d, %d of %d "
                    "pixels off, clockwise %s)\n", ok ? "ok" : "FAIL", t.rot,
                    out.width(), out.height(), bad, seen,
                    dir ? "yes" : "NO");
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
    /* (lx, ly) are image pixels, as the assertions below reason in.  The
     * widget position that names one is the *display transform's*, not kPad's:
     * the canvas is centred and scaled whenever the window is bigger than it
     * needs to be.  The centre of the pixel is sent rather than its corner, so
     * the floor() in pointer() lands on the same pixel at any scale. */
    auto send_mouse = [&](QEvent::Type t, Qt::MouseButton b,
                          Qt::MouseButtons bs, int lx, int ly) {
        const double  s   = win.view()->display_scale();
        const QPointF org = win.view()->display_origin();
        const QPointF p(org.x() + s * (kPad + lx + 0.5),
                        org.y() + s * (kPad + ly + 0.5));
        QMouseEvent e(t, p, p, b, bs, Qt::NoModifier);
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

    /* 24b. The polygon is placed a click at a time through the same widget,
     * and its gestures are the window's: a click adds a vertex, Enter finishes
     * the outline, Backspace takes a vertex back, and the right button
     * finishes it too.
     *
     * The outline has to reach the overlay as well as the session, so the last
     * check counts the mark colour on the canvas rather than sampling it — the
     * same counting-not-sampling rule assertion 34 uses.  The difference is
     * what is counted, not the absolute total: the colour bar is drawn from a
     * palette that contains pure yellow, so an absolute count would be reading
     * the palette as much as the outline. */
    {
        const int px[3] = { 20, 60, 20 };
        const int py[3] = { 20, 20, 60 };
        dyt_snapshot_t s{};

        send_key(Qt::Key_O);            /* the polygon tool, by its key */

        for (int i = 0; i < 3; i++)
            send_mouse(QEvent::MouseButtonPress, Qt::LeftButton,
                       Qt::LeftButton, px[i], py[i]);

        dyt_session_snapshot(sess, &s, nullptr, 0);
        const bool tool     = s.tool == DYT_TOOL_POLYGON;
        const bool placed   = s.poly_n == 3;
        const bool measured = s.roi_ok && s.roi.n > 0;
        const bool open     = s.poly_closed == 0;

        /* Enter finishes the outline: it takes no more vertices, but it keeps
         * measuring the ones it has. */
        send_key(Qt::Key_Return);
        dyt_session_snapshot(sess, &s, nullptr, 0);
        const bool closed = s.poly_closed == 1 && s.poly_n == 3 && s.roi_ok;

        /* Backspace takes the last vertex back and reopens the outline, so it
         * can be added to again — the only way to correct a misclick without
         * losing the whole shape. */
        send_key(Qt::Key_Backspace);
        dyt_session_snapshot(sess, &s, nullptr, 0);
        const bool undone = s.poly_n == 2 && s.poly_closed == 0;

        /* The right button finishes it too — but not a two-vertex outline,
         * which is not a region yet. */
        send_mouse(QEvent::MouseButtonPress, Qt::RightButton, Qt::RightButton,
                   px[0], py[0]);
        dyt_session_snapshot(sess, &s, nullptr, 0);
        const bool refused = s.poly_closed == 0 && s.poly_n == 2;

        send_mouse(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton,
                   px[2], py[2]);
        send_mouse(QEvent::MouseButtonPress, Qt::RightButton, Qt::RightButton,
                   px[0], py[0]);
        dyt_session_snapshot(sess, &s, nullptr, 0);
        const bool shut = s.poly_closed == 1 && s.poly_n == 3;

        /* And the outline is on the canvas, not only in the session. */
        auto count_mark = [](const QImage &im) {
            int n = 0;
            for (int y = 0; y < im.height(); y++)
                for (int x = 0; x < im.width(); x++)
                    if (im.pixelColor(x, y) == QColor(255, 255, 0))
                        n++;
            return n;
        };
        const int painted = count_mark(win.view()->render_canvas());
        send_key(Qt::Key_N);            /* clear: the outline goes with it */
        const int cleared = count_mark(win.view()->render_canvas());

        const bool ok = tool && placed && measured && open && closed &&
                        undone && refused && shut &&
                        painted > cleared + 60;
        std::printf("  %-4s the polygon is placed a click at a time, and its "
                    "outline is painted (3 clicks, closed, undone, "
                    "right-button %s, %d mark px then %d)\n",
                    ok ? "ok" : "FAIL", shut ? "yes" : "NO", painted, cleared);
        if (!ok)
            fails++;
    }

    /* 24c. The Analysis chart plots the line's profile, and the peak it marks
     * is the frame's own hot pixel.  The line is drawn along the row that pixel
     * is in and right across the image, so sample i is source x = i — which
     * makes the peak bin the hot pixel's own column, a number taken from the
     * snapshot rather than from the chart.
     *
     * "Chart analysis" has to change what is *drawn*, not only what is checked,
     * so the two modes are compared by the pixels of the peak marker — which
     * only the annotated one has, and which no palette produces. */
    {
        pm.step();
        QApplication::processEvents();

        dyt_snapshot_t s{};
        dyt_session_snapshot(sess, &s, nullptr, 0);
        const QSize is = win.view()->imageSize();
        const int   hy = s.stats.hot_y;

        /* The canvas coordinates of the row's two ends, so the line lands on
         * the source pixels the assertion reasons in whatever the transform. */
        int ax = 0, ay = 0, bx = 0, by = 0;
        const bool ends =
            dyt_view_transform_project(&s.xform, s.width, s.height,
                                       is.width(), is.height(),
                                       0, hy, &ax, &ay) == 0 &&
            dyt_view_transform_project(&s.xform, s.width, s.height,
                                       is.width(), is.height(),
                                       s.width - 1, hy, &bx, &by) == 0;

        send_key(Qt::Key_L);                    /* the line tool */
        send_mouse(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton,
                   ax, ay);
        send_mouse(QEvent::MouseMove, Qt::NoButton, Qt::LeftButton, bx, by);
        send_mouse(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton,
                   bx, by);

        pm.step();                              /* the chart is fed on paint */
        QApplication::processEvents();

        ProfilePlot *plot = win.panel() ? win.panel()->plot() : nullptr;
        const bool peak = plot && plot->peak_bin() == s.stats.hot_x;

        auto count_marker = [](const QImage &im) {
            int n = 0;
            for (int y = 0; y < im.height(); y++)
                for (int x = 0; x < im.width(); x++)
                    if (im.pixelColor(x, y) == QColor(255, 209, 102))
                        n++;
            return n;
        };

        const int bare = plot ? count_marker(plot->grab().toImage()) : -1;

        send_key(Qt::Key_C);                    /* the annotated chart */
        const bool analysis = win.panel() && win.panel()->plot() &&
                              win.panel()->plot()->mode() == ProfilePlot::Analysis;
        const int annotated = plot ? count_marker(plot->grab().toImage()) : -1;

        send_key(Qt::Key_L);                    /* and back to the curve */
        const bool back = win.panel() && win.panel()->plot() &&
                          win.panel()->plot()->mode() == ProfilePlot::Line;

        /* "Chart analysis" picks the line tool up with the chart.  Cleared
         * first, so the check is not satisfied by the tool the assertion
         * placed the line with. */
        send_key(Qt::Key_N);                    /* drop the tool */
        send_key(Qt::Key_C);                    /* and ask for the chart */
        dyt_snapshot_t s2{};
        dyt_session_snapshot(sess, &s2, nullptr, 0);
        const bool picks = s2.tool == DYT_TOOL_LINE;

        send_key(Qt::Key_L);                    /* leave it as it was found */

        const bool ok = ends && peak && analysis && back && picks &&
                        bare == 0 && annotated > 0;
        std::printf("  %-4s the Analysis chart plots the line's profile and "
                    "marks its peak (peak bin %d of hot x %d, chart %s, "
                    "picks line %s, marker %d px then %d)\n", ok ? "ok" : "FAIL",
                    plot ? plot->peak_bin() : -1, s.stats.hot_x,
                    analysis ? "analysis" : "NO", picks ? "yes" : "NO",
                    bare, annotated);
        if (!ok)
            fails++;
    }

    /* 25. The alarm key arms at the panel's threshold field, and a second press
     * disarms.  The Windows panel's High TEMP. Alarm is a *setpoint* — a
     * number the user types beside the row (num_alarm_val) — not the band the
     * port used to derive from the frame, so what the key arms at is the
     * field's value with the low side parked on the clamp's floor where
     * nothing a sensor reports can reach it.
     *
     * The field itself is exercised further down, where the panel is in scope;
     * this pins the arm/disarm the key alone is responsible for. */
    {
        dyt_snapshot_t s{};

        send_key(Qt::Key_A);
        dyt_session_snapshot(sess, &s, nullptr, 0);
        const bool armed = s.alarm_on != 0;
        const bool at_field =
            std::fabs(s.alarm_hi - DYT_ALARM_SETPOINT_DEFAULT) < 1e-4f &&
            std::fabs(s.alarm_lo - DYT_ALARM_SETPOINT_MIN) < 1e-4f;
        const float armed_lo = s.alarm_lo, armed_hi = s.alarm_hi;

        send_key(Qt::Key_A);
        dyt_session_snapshot(sess, &s, nullptr, 0);
        const bool disarmed = s.alarm_on == 0;

        const bool ok = armed && at_field && disarmed;
        std::printf("  %-4s the alarm key arms at the threshold field's value, "
                    "then disarms (%.1f..%.1f)\n", ok ? "ok" : "FAIL",
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

    /* 33. The device panel starts hidden and is painted once it is shown.
     * Its background is opaque and sits at a fixed spot, so an interior pixel
     * must be the fill colour while it is up; that is what pins the overlay
     * reaching the canvas, not just the rows.
     *
     * The probe is placed through the display transform, because the canvas is
     * centred whenever the window is wider than it needs — and the check is
     * that the pixels *change* when the panel is hidden, because the letterbox
     * is the same colour as the panel's fill and would otherwise pass this on
     * its own.
     *
     * The start-up state is asserted first and left alone at the end: the
     * Windows app opens on the bare picture, so "hidden until asked for" is
     * part of the contract rather than an incidental default. */
    {
        /* Sampled through render_canvas(), in canvas coordinates: the overlay's
         * box starts at canvas (kPad+6, kPad+6), so (kPad+7, kPad+7) is just
         * inside its top-left corner and clear of the text.  Being in canvas
         * space keeps this honest when the rail and panel leave the window
         * narrower than the canvas. */
        auto fill_at = [&](const QImage &im) {
            return im.pixelColor(kPad + 7, kPad + 7) == QColor(16, 16, 16) &&
                   im.pixelColor(kPad + 8, kPad + 7) == QColor(16, 16, 16) &&
                   im.pixelColor(kPad + 7, kPad + 8) == QColor(16, 16, 16);
        };

        /* Hidden at start-up: nothing to probe until it is asked for. */
        const bool starts_hidden = !fv->info_shown();

        fv->toggle_info();              /* show */
        const QImage on   = fv->render_canvas();
        const bool   fill = fv->info_shown() && fill_at(on);

        fv->toggle_info();              /* hide again */
        const bool   hidden  = !fv->info_shown();
        const QImage off     = fv->render_canvas();
        const bool   covered = !fill_at(off);   /* the picture is under it */

        const bool ok = starts_hidden && fill && hidden && covered;
        std::printf("  %-4s the device panel is hidden at start-up and painted "
                    "over the image when shown "
                    "(hidden %s, fill %s, toggle %s, covers %s)\n",
                    ok ? "ok" : "FAIL", starts_hidden ? "yes" : "NO",
                    fill ? "yes" : "NO", hidden ? "yes" : "NO",
                    covered ? "yes" : "NO");
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
        const int before = count_fill(fv->render_canvas());

        send_char('e');                 /* armed */
        const int after = count_fill(fv->render_canvas());
        send_esc();
        const int gone = count_fill(fv->render_canvas());

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

    /* ---- capture: a still, a clip, and the disk guard --------------------
     *
     * The still writer and the recorder are real here — the fixture has given
     * the session a frame — so these write actual files into a temporary
     * directory and look at what landed.  What they cannot cover is the live
     * payload; the fixture's raw is the same shape, and the writer's own
     * format is dytjpeg_test's subject, not this one. */
    {
        char  tmpl[] = "/tmp/dytqt-selftest-XXXXXX";
        char *dir    = mkdtemp(tmpl);
        const std::string d = dir ? dir : ".";

        /* Count the files a capture left, by extension. */
        auto count_files = [&](const char *ext) {
            DIR *dp = opendir(d.c_str());
            int  n  = 0;
            if (!dp)
                return -1;
            for (struct dirent *e; (e = readdir(dp)) != nullptr;) {
                const size_t l = strlen(e->d_name);
                const size_t x = strlen(ext);
                if (l > x && strcmp(e->d_name + l - x, ext) == 0)
                    n++;
            }
            closedir(dp);
            return n;
        };
        auto any_size = [&](const char *ext) {
            DIR *dp = opendir(d.c_str());
            long long best = -1;
            if (!dp)
                return best;
            for (struct dirent *e; (e = readdir(dp)) != nullptr;) {
                const size_t l = strlen(e->d_name);
                const size_t x = strlen(ext);
                if (l <= x || strcmp(e->d_name + l - x, ext) != 0)
                    continue;
                char full[600];
                snprintf(full, sizeof full, "%s/%s", d.c_str(), e->d_name);
                FILE *f = fopen(full, "rb");
                if (!f)
                    continue;
                fseek(f, 0, SEEK_END);
                best = ftell(f);
                fclose(f);
            }
            closedir(dp);
            return best;
        };
        /* The one PNG in the directory, with its IHDR's width and height read
         * back.  Reading the header rather than trusting a size the writer
         * reported is what makes "the still is 2x" a claim about the bytes on
         * disk. */
        auto png_size = [&](const char *dir, int &pw, int &ph) {
            DIR *dp = opendir(dir);
            pw = ph = 0;
            if (!dp)
                return;
            for (struct dirent *e; (e = readdir(dp)) != nullptr;) {
                const size_t l = strlen(e->d_name);
                if (l < 4 || strcmp(e->d_name + l - 4, ".png") != 0)
                    continue;
                char full[600];
                snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
                FILE *f = fopen(full, "rb");
                if (f) {
                    unsigned char h[24] = {0};
                    if (fread(h, 1, sizeof h, f) == sizeof h &&
                        memcmp(h + 12, "IHDR", 4) == 0) {
                        pw = (h[16] << 24) | (h[17] << 16) | (h[18] << 8) | h[19];
                        ph = (h[20] << 24) | (h[21] << 16) | (h[22] << 8) | h[23];
                    }
                    fclose(f);
                }
                remove(full);
            }
            closedir(dp);
        };

        /* 37. A still writes both halves: the DYT container and the PNG. */
        {
            std::string msg;
            const bool  wrote = save_still(sess, d, msg);
            const int   nd    = count_files(".dyt.jpg");
            const int   np    = count_files(".png");
            const bool  sized = any_size(".dyt.jpg") > 1000 && any_size(".png") > 1000;

            const bool ok = wrote && nd == 1 && np == 1 && sized &&
                            msg.find("saved ") == 0;
            std::printf("  %-4s a still writes the container and the PNG "
                        "(wrote %s, %d + %d, sized %s)\n",
                        ok ? "ok" : "FAIL", wrote ? "yes" : "NO", nd, np,
                        sized ? "yes" : "NO");
            if (!ok) {
                fails++;
                if (!msg.empty())
                    std::printf("       %s\n", msg.c_str());
            }
        }

        /* 37b. With super-resolution on, the still's PNG is 2x — and without a
         * model the same call must still write a native still rather than fail.
         * The 2x case is what save_still()'s buffer sizing has to get right:
         * sized from the native geometry, render_rgb() refuses with -2 and
         * nothing is written at all.  The thermal mode is used because it is
         * the one that applies whatever the fusion pattern is, so this does not
         * depend on the run's own fusion state. */
        {
            char  st[] = "/tmp/dytqt-sr-XXXXXX";
            char *sd   = mkdtemp(st);
            const std::string ds = sd ? sd : ".";
            const dyt_sr_t    was = dyt_session_get_sr(sess);
            dyt_snapshot_t    sr_snap{};
            int  pw = 0, ph = 0;
            bool wrote = false;
            std::string msg;

            dyt_session_snapshot(sess, &sr_snap, nullptr, 0);
            const bool have_model = sr_snap.sr_cap != 0;

            dyt_session_set_sr(sess, DYT_SR_THERMAL);
            wrote = save_still(sess, ds, msg);
            png_size(ds.c_str(), pw, ph);
            rmdir(sd);

            /* Leave the mode as the run found it. */
            dyt_session_set_sr(sess, was);

            /* 512x384 with a model (the 2x render); 256x192 without one, where
             * the mode is refused and the still must still be written. */
            const bool ok = wrote && (have_model ? (pw == 512 && ph == 384)
                                                 : (pw == 256 && ph == 192));
            std::printf("  %-4s a still with SR %s is written at %dx%d "
                        "(wrote %s, PNG %dx%d)\n", ok ? "ok" : "FAIL",
                        have_model ? "on" : "unavailable",
                        have_model ? 512 : 256, have_model ? 384 : 192,
                        wrote ? "yes" : "NO", pw, ph);
            if (!ok) {
                fails++;
                if (!msg.empty())
                    std::printf("       %s\n", msg.c_str());
            }
        }

        /* 38. The gallery scan finds the still, and opening it as a frame
         * source re-renders the same scene the fixture showed — through the
         * live pipeline, not a second path.  The temperature check is what
         * makes it meaningful: the filler decodes to ~238.85 C. */
        {
            dyt_vm_item_t items[4];
            int           found = dyt_vm_scan(d.c_str(), items, 4);
            int           still_at = -1;
            for (int i = 0; i < found; i++)
                if (items[i].kind == DYT_VM_ITEM_STILL)
                    still_at = i;

            dyt_session_t      *gs = dyt_session_create();
            dyt_frame_source_t *sf = nullptr;
            const uint8_t      *grgb = nullptr;
            int                 gw = 0, gh = 0;
            dyt_fs_status_t     st = DYT_FS_ERROR;
            float               temps[256 * 192];
            int                 ntemps = 0;
            float               lo = 1e9f, hi = -1e9f;

            if (gs && still_at >= 0)
                sf = dyt_frame_source_open_still(
                    gs, items[still_at].path, 256, DYT_MODE_1000,
                    DYT_PLANE_BOTTOM_HALF, 25.0f, 0, 0);
            if (sf)
                st = dyt_frame_source_next(sf, &grgb, &gw, &gh);
            if (sf && st == DYT_FS_FRAME) {
                ntemps = dyt_frame_source_temps(sf, temps, 256 * 192);
                for (int i = 0; i < ntemps; i++) {
                    if (temps[i] < lo) lo = temps[i];
                    if (temps[i] > hi) hi = temps[i];
                }
            }

            const bool ok = found >= 1 && still_at >= 0 && sf && grgb &&
                            st == DYT_FS_FRAME && gw == 256 && gh == 192 &&
                            ntemps == 256 * 192 && hi > 20.0f && hi < 60.0f;
            std::printf("  %-4s the gallery opens a saved still "
                        "(scan %d, still %d, frame %dx%d, temps %.2f..%.2f C)\n",
                        ok ? "ok" : "FAIL", found, still_at, gw, gh,
                        ntemps ? lo : 0.0f, ntemps ? hi : 0.0f);
            if (!ok)
                fails++;

            dyt_frame_source_close(sf);
            dyt_session_free(gs);
        }

        /* 39. The clip state machine: start, feed, stop — and the indicator
         * tracks it.  Needs OpenCV, which is the point of the gate. */
        {
            dyt_snapshot_t snap;
            int            rw = 0, rh = 0;
            std::vector<uint8_t> rgb;

            if (dyt_session_snapshot(sess, &snap, nullptr, 0) == 0 && snap.ready) {
                const int f = snap.xform.sr >= 1 ? snap.xform.sr : 1;
                rgb.resize((size_t)snap.width * f * (size_t)snap.height * f * 3);
                if (dyt_session_render_rgb(sess, rgb.data(), (int)rgb.size(),
                                           &rw, &rh) != 0)
                    rgb.clear();
            }

            CaptureCtl  c;
            std::string why;
            c.dir = d;

            const bool started = c.start(25.0, why);
            const bool labelled = !c.label().empty();
            bool       fed = true;
            for (int i = 0; started && rgb.size() && i < 5; i++) {
                std::string f;
                if (!c.feed(rgb.data(), rw, rh, f)) {
                    fed = false;
                    break;
                }
            }
            const bool was_recording = c.recording;
            const long long stopped  = c.stop();
            const int  clips = count_files(".mp4");
            const bool gone  = !c.recording && c.label().empty();

#ifdef DYT_HAVE_OPENCV
            const bool ok = started && labelled && fed && was_recording &&
                            stopped == 5 && gone && clips == 1 &&
                            any_size(".mp4") > 1000;
            std::printf("  %-4s a clip starts, takes frames, and stops "
                        "(start %s, label %s, fed %s, %lld frames, "
                        "%d file, gone %s)\n",
                        ok ? "ok" : "FAIL", started ? "yes" : "NO",
                        labelled ? "yes" : "NO", fed ? "yes" : "NO",
                        stopped, clips, gone ? "yes" : "NO");
#else
            /* Without OpenCV the refusal has to be the defined one, not a
             * crash and not a silent no-op. */
            const bool ok = !started && !labelled && clips == 0 && gone &&
                            why.find("OpenCV") != std::string::npos;
            std::printf("  %-4s recording is refused without OpenCV "
                        "(refused %s, says why %s)\n",
                        ok ? "ok" : "FAIL", !started ? "yes" : "NO",
                        why.find("OpenCV") != std::string::npos ? "yes" : "NO");
#endif
            if (!ok) {
                fails++;
                if (!why.empty())
                    std::printf("       %s\n", why.c_str());
            }
        }

        /* 40. The disk guard refuses a directory it cannot examine, and says
         * which one. */
        {
            CaptureCtl  c;
            std::string why;
            c.dir = "/nonexistent-dir-xyz/sub";
            const bool started = c.start(25.0, why);
            const bool ok = !started && why.find("not writable") != std::string::npos;
            std::printf("  %-4s a clip is refused where there is no disk "
                        "(refused %s, says why %s)\n",
                        ok ? "ok" : "FAIL", !started ? "yes" : "NO",
                        why.find("not writable") != std::string::npos ? "yes"
                                                                     : "NO");
            if (!ok)
                fails++;
        }

        /* Clean up, so a selftest run leaves nothing behind. */
        {
            DIR *dp = opendir(d.c_str());
            for (struct dirent *e; dp && (e = readdir(dp)) != nullptr;) {
                if (e->d_name[0] == '.')
                    continue;
                char full[600];
                snprintf(full, sizeof full, "%s/%s", d.c_str(), e->d_name);
                unlink(full);
            }
            if (dp)
                closedir(dp);
        }
        if (dir)
            rmdir(dir);
    }

    /* 41. The capture keys reach the front end.  's' and 'v' are new bindings
     * and must not have been swallowed by the measurement set. */
    {
        int stills = 0, toggles = 0;
        win.on_still_  = [&]() { stills++; };
        win.on_record_ = [&]() { toggles++; };
        send_char('s');
        send_char('v');
        const bool ok = stills == 1 && toggles == 1;
        std::printf("  %-4s the capture keys route (still %d, record %d)\n",
                    ok ? "ok" : "FAIL", stills, toggles);
        if (!ok)
            fails++;
        win.on_still_  = nullptr;
        win.on_record_ = nullptr;
    }

    /* 42. The gallery keys.  'g' opens the list and rescans, the arrows move
     * the highlight, Return opens the highlighted entry and 'x' exports it,
     * Esc closes — and while the list is up every other binding is swallowed,
     * so a key that moved the highlight cannot also change the tool. */
    {
        char  tmpl2[] = "/tmp/dytqt-gal-XXXXXX";
        char *dir2    = mkdtemp(tmpl2);
        const std::string d2 = dir2 ? dir2 : ".";

        std::string m;
        save_still(sess, d2, m);          /* one entry to browse */

        int                  opens = 0, exports = 0, refreshes = 0;
        const dyt_vm_item_t *opened = nullptr;

        win.on_gallery_refresh_ = [&]() {
            refreshes++;
            dyt_vm_gallery_load(win.gallery(), d2.c_str());
        };
        win.on_gallery_open_   = [&](const dyt_vm_item_t *it) -> bool {
            opens++;
            opened = it;
            /* Stand in for the real render, so the test can pin that closing
             * the gallery clears the still and returns to the stream. */
            QImage probe(4, 4, QImage::Format_RGB888);
            probe.fill(Qt::black);
            win.set_viewing(probe, QStringLiteral("viewing test"));
            return true;
        };
        win.on_gallery_export_ = [&](const dyt_vm_item_t *) { exports++; };

        send_char('g');
        const bool opened_ok = win.gallery()->open && refreshes == 1 &&
                               win.gallery()->n == 1 && win.gallery()->sel == 0;

        /* On a one-entry list both moves land back on it. */
        send_key(Qt::Key_Down);
        send_key(Qt::Key_Up);
        const bool moved = win.gallery()->sel == 0;

        /* A key the list does not bind must not reach the measurement set. */
        dyt_snapshot_t before{};
        dyt_session_snapshot(sess, &before, nullptr, 0);
        send_char('p');
        dyt_snapshot_t after{};
        dyt_session_snapshot(sess, &after, nullptr, 0);
        const bool swallowed = after.tool == before.tool;

        /* Opening hides the list, so the entry fills the canvas rather than
         * sitting under the panel. */
        send_key(Qt::Key_Return);
        const bool hid = !win.gallery()->open && win.viewing() && opens == 1;

        /* 'x' acts on the highlighted entry and needs the list up, so reopen
         * first — which is exactly what a user does to export after looking. */
        send_char('g');
        send_char('x');
        send_esc();

        const bool ok = opened_ok && moved && swallowed && hid &&
                        exports == 1 && !win.gallery()->open &&
                        !win.viewing() &&
                        opened == &win.gallery()->items[0];
        std::printf("  %-4s the gallery keys browse, open and export "
                    "(open %s, move %s, swallow %s, opened %d, hid %s, "
                    "exported %d, closed %s, back to live %s)\n",
                    ok ? "ok" : "FAIL", opened_ok ? "yes" : "NO",
                    moved ? "yes" : "NO", swallowed ? "yes" : "NO", opens,
                    hid ? "yes" : "NO", exports,
                    !win.gallery()->open ? "yes" : "NO",
                    !win.viewing() ? "yes" : "NO");
        if (!ok)
            fails++;

        win.on_gallery_refresh_ = nullptr;
        win.on_gallery_open_    = nullptr;
        win.on_gallery_export_  = nullptr;

        {
            DIR *dp = opendir(d2.c_str());
            for (struct dirent *e; dp && (e = readdir(dp)) != nullptr;) {
                if (e->d_name[0] == '.')
                    continue;
                char full[600];
                snprintf(full, sizeof full, "%s/%s", d2.c_str(), e->d_name);
                unlink(full);
            }
            if (dp)
                closedir(dp);
        }
        if (dir2)
            rmdir(dir2);
    }

    /* 57. The gallery is a real widget, not paint inside the canvas.  This is
     * the shape of the bug it replaces: the old list was drawn under the
     * display transform, from a code path the no-frame and degenerate-geometry
     * early returns skipped — so it could scale with the picture or fail to
     * appear at all (a fullscreen window showed nothing).  A child widget is
     * composited in widget space, so it can do neither.  This pins that it is
     * up on 'g', that it is still up in fullscreen, that it actually *paints*
     * there (isVisible() alone would not catch a widget that is up but never
     * drawn), and that it names the folder — the one place a user can see
     * where the files they are looking at are kept. */
    {
        char  tmplw[] = "/tmp/dytqt-galw-XXXXXX";
        char *dirw    = mkdtemp(tmplw);
        const std::string dw = dirw ? dirw : ".";

        auto touch = [](const std::string &d, const char *name) {
            FILE *f = fopen((d + "/" + name).c_str(), "wb");
            if (f) { fputs("x", f); fclose(f); }
        };
        touch(dw, "dyt_20260101-000000.dyt.jpg");

        win.on_gallery_refresh_ = [&]() {
            dyt_vm_gallery_load(win.gallery(), dw.c_str());
        };
        win.on_gallery_open_ = [&](const dyt_vm_item_t *) { return true; };
        win.set_folder(QString::fromStdString(dw));

        /* Closed first, so the "with the panel" grab has a baseline. */
        send_key(Qt::Key_F11);                  /* fullscreen, list closed */
        QApplication::processEvents();
        const QImage without = win.view()->grab().toImage();

        send_char('g');
        QApplication::processEvents();

        GalleryPanel *panel = win.gallery_panel();
        const QImage  with  = win.view()->grab().toImage();

        int diff = 0;
        if (!without.isNull() && without.size() == with.size())
            for (int y = 0; y < with.height(); y += 2)
                for (int x = 0; x < with.width(); x += 2)
                    if (without.pixel(x, y) != with.pixel(x, y))
                        diff++;

        const bool shown = panel && panel->isVisible() &&
                           panel->list()->count() == 1;
        const bool named = panel &&
                           panel->folder_text() == QString::fromStdString(dw);
        const bool fs   = panel && panel->isVisible() && win.fullscreen();
        const bool drew = diff > 500;           /* the panel is thousands of px */

        send_key(Qt::Key_F11);                  /* back out of fullscreen */
        QApplication::processEvents();
        const bool back = panel && panel->isVisible() && !win.fullscreen();

        send_esc();
        QApplication::processEvents();
        const bool gone = panel && !panel->isVisible();

        const bool ok = shown && named && fs && drew && back && gone;
        std::printf("  %-4s the gallery is a real widget: up on 'g', painted "
                    "and still up in fullscreen, names the folder (shown %s, "
                    "folder %s, fullscreen %s, painted %d px, back %s, "
                    "hidden %s)\n",
                    ok ? "ok" : "FAIL", shown ? "yes" : "NO",
                    named ? "yes" : "NO", fs ? "yes" : "NO", diff,
                    back ? "yes" : "NO", gone ? "yes" : "NO");
        if (!ok)
            fails++;

        win.on_gallery_refresh_ = nullptr;
        win.on_gallery_open_    = nullptr;

        {
            DIR *dp = opendir(dw.c_str());
            for (struct dirent *e; dp && (e = readdir(dp)) != nullptr;) {
                if (e->d_name[0] == '.')
                    continue;
                unlink((dw + "/" + e->d_name).c_str());
            }
            if (dp)
                closedir(dp);
        }
        if (dirw)
            rmdir(dirw);
    }

    /* 58. The mouse works on the list.  A click is what a user reaches for
     * first, and the painted gallery had no mouse handling at all — the click
     * fell through to the canvas and placed a measurement tool instead.  The
     * panel is Qt::NoFocus so the keys still reach the window, which is why a
     * click must be able to select without taking focus. */
    {
        char  tmplm[] = "/tmp/dytqt-galm-XXXXXX";
        char *dirm    = mkdtemp(tmplm);
        const std::string dm = dirm ? dirm : ".";

        auto touch = [](const std::string &d, const char *name) {
            FILE *f = fopen((d + "/" + name).c_str(), "wb");
            if (f) { fputs("x", f); fclose(f); }
        };
        touch(dm, "dyt_20260101-000000.dyt.jpg");
        touch(dm, "dyt_20260101-000001.dyt.jpg");

        int opens = 0;
        win.on_gallery_refresh_ = [&]() {
            dyt_vm_gallery_load(win.gallery(), dm.c_str());
        };
        win.on_gallery_open_ = [&](const dyt_vm_item_t *) { opens++; return true; };

        send_char('g');
        QApplication::processEvents();

        GalleryPanel *panel = win.gallery_panel();
        QListWidget  *lw    = panel ? panel->list() : nullptr;

        auto click_row = [&](int row, QEvent::Type type) {
            QListWidgetItem *it = lw ? lw->item(row) : nullptr;
            if (!it)
                return;
            const QPoint p = lw->visualItemRect(it).center();
            QMouseEvent e(type, QPointF(p), QPointF(lw->viewport()->mapToGlobal(p)),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(lw->viewport(), &e);
        };

        /* A press and a release on the same row is a click. */
        click_row(1, QEvent::MouseButtonPress);
        click_row(1, QEvent::MouseButtonRelease);
        QApplication::processEvents();
        const bool clicked = win.gallery()->sel == 1;

        /* And a double-click opens it. */
        click_row(1, QEvent::MouseButtonDblClick);
        QApplication::processEvents();
        const bool opened = opens == 1;

        send_esc();
        QApplication::processEvents();

        const bool ok = lw && lw->count() == 2 && clicked && opened;
        std::printf("  %-4s the gallery takes the mouse (rows %d, click "
                    "selects %s, double-click opens %s)\n",
                    ok ? "ok" : "FAIL", lw ? lw->count() : -1,
                    clicked ? "yes" : "NO", opened ? "yes" : "NO");
        if (!ok)
            fails++;

        win.on_gallery_refresh_ = nullptr;
        win.on_gallery_open_    = nullptr;

        {
            DIR *dp = opendir(dm.c_str());
            for (struct dirent *e; dp && (e = readdir(dp)) != nullptr;) {
                if (e->d_name[0] == '.')
                    continue;
                unlink((dm + "/" + e->d_name).c_str());
            }
            if (dp)
                closedir(dp);
        }
        if (dirm)
            rmdir(dirm);
    }

    /* 59. A clip plays.  The reader itself is tools/player.{h,cpp} and its own
     * test; what this pins is the window's half — that opening a clip starts
     * playback, that the pump advances it and pushes the frame through the
     * canvas override, that space pauses without losing the position, and that
     * Esc stops it and returns to the live view.
     *
     * The clip is written from a solid magenta frame rather than from this
     * session's render, so "the clip reached the screen" is decidable: no
     * thermal palette produces magenta, so counting magenta pixels in a canvas
     * grab cannot be confused with the live picture.  A diff against the live
     * frame would prove nothing, because a clip recorded from this session *is*
     * the live frame. */
    {
        char  tmplp[] = "/tmp/dytqt-play-XXXXXX";
        char *dirp    = mkdtemp(tmplp);
        const std::string dp = dirp ? dirp : ".";

#ifdef DYT_HAVE_OPENCV
        const int cw = 256, ch = 192;
        std::vector<uint8_t> mag((size_t)cw * ch * 3);
        for (size_t i = 0; i + 2 < mag.size(); i += 3) {
            mag[i + 0] = 255;
            mag[i + 1] = 0;
            mag[i + 2] = 255;
        }

        std::string clip;
        {
            CaptureCtl  c;
            std::string why;
            c.dir = dp;
            if (c.start(25.0, why)) {
                clip = c.path;
                for (int i = 0; i < 8; i++) {
                    std::string f;
                    if (!c.feed(mag.data(), cw, ch, f))
                        break;
                }
            }
            c.stop();
        }

        auto magenta = [](const QImage &im) {
            int n = 0;
            for (int y = 0; y < im.height(); y++)
                for (int x = 0; x < im.width(); x++) {
                    const QRgb p = im.pixel(x, y);
                    if (qRed(p) > 220 && qGreen(p) < 60 && qBlue(p) > 220)
                        n++;
                }
            return n;
        };

        QApplication::processEvents();
        const int off_before = magenta(win.view()->grab().toImage());

        std::string why;
        const bool  opened  = pm.playback.open(clip, why);
        const long long nf  = pm.playback.count;
        const bool  named   = opened && nf == 8 &&
                              pm.playback.label().find("playing") != std::string::npos;

        /* The pump advances it — a few ticks is enough to see the position
         * move and the canvas take the frame. */
        for (int i = 0; i < 3; i++)
            pm.step();
        QApplication::processEvents();

        const bool advanced = pm.playback.pos > 0;
        /* The canvas sizes to the clip, not to the live frame under it. */
        const bool sized = win.view()->sizeHint().height() == ch + 2 * kPad;
        const bool shown = win.viewing();
        const bool badge = !win.strip()->playback_label().isEmpty();
        const int  on    = magenta(win.view()->grab().toImage());

        /* Space pauses without losing the position. */
        const long long held = pm.playback.pos;
        send_char(' ');
        for (int i = 0; i < 3; i++)
            pm.step();
        const bool paused = pm.playback.paused && pm.playback.pos == held &&
                            pm.playback.label().find("paused") != std::string::npos;

        /* Space resumes, and Esc stops it and returns to the live view. */
        send_char(' ');
        const bool resumed = !pm.playback.paused;
        send_esc();
        QApplication::processEvents();
        const int  off_after = magenta(win.view()->grab().toImage());
        const bool stopped   = !pm.playback.active && !win.viewing() &&
                               win.strip()->playback_label().isEmpty();

        /* And the reader refuses what is not a clip. */
        {
            FILE *f = fopen((dp + "/not-a-clip.dyt.jpg").c_str(), "wb");
            if (f) { fputs("not a video", f); fclose(f); }
        }
        std::string why2;
        const bool refused = !pm.playback.open(dp + "/not-a-clip.dyt.jpg", why2);

        const bool ok = opened && named && advanced && sized && shown && badge &&
                        on > 1000 && off_before < on / 10 && paused && resumed &&
                        stopped && off_after < on / 10 && refused;
        std::printf("  %-4s a clip plays, pauses and stops (open %s, %lld "
                    "frames, moved %s, sized %s, magenta %d vs %d/%d, badge %s, "
                    "paused %s, resumed %s, stopped %s, refused a still %s)\n",
                    ok ? "ok" : "FAIL", opened ? "yes" : "NO", nf,
                    advanced ? "yes" : "NO", sized ? "yes" : "NO", on,
                    off_before, off_after, badge ? "yes" : "NO",
                    paused ? "yes" : "NO", resumed ? "yes" : "NO",
                    stopped ? "yes" : "NO", refused ? "yes" : "NO");
        if (!ok) {
            fails++;
            if (!why.empty())
                std::printf("       %s\n", why.c_str());
        }
#else
        /* Without OpenCV there is no reader at all, so the refusal has to be
         * the defined one — not a crash and not a silent no-op. */
        std::string why;
        const bool refused = !pm.playback.open(dp + "/x.mp4", why) &&
                             why.find("OpenCV") != std::string::npos;
        std::printf("  %-4s clip playback is refused without OpenCV "
                    "(refused %s, says why %s)\n",
                    refused ? "ok" : "FAIL", refused ? "yes" : "NO",
                    why.find("OpenCV") != std::string::npos ? "yes" : "NO");
        if (!refused)
            fails++;
#endif

        {
            DIR *dpp = opendir(dp.c_str());
            for (struct dirent *e; dpp && (e = readdir(dpp)) != nullptr;) {
                if (e->d_name[0] == '.')
                    continue;
                unlink((dp + "/" + e->d_name).c_str());
            }
            if (dpp)
                closedir(dpp);
        }
        if (dirp)
            rmdir(dirp);
    }

    /* 43. The view keys reach the session through the window: palette, the
     * two flips (which the case split must keep apart) and the zoom. */
    {
        dyt_snapshot_t s1{};
        dyt_session_snapshot(sess, &s1, nullptr, 0);

        send_char('3');
        dyt_session_snapshot(sess, &s1, nullptr, 0);
        const bool pal = s1.palette == 2;

        const int h0 = s1.xform.flip_h, v0 = s1.xform.flip_v;
        send_char('h');
        dyt_session_snapshot(sess, &s1, nullptr, 0);
        const bool fh = s1.xform.flip_h != h0 && s1.xform.flip_v == v0;

        send_char('H');
        dyt_session_snapshot(sess, &s1, nullptr, 0);
        const bool fv = s1.xform.flip_v != v0;

        const int z0 = s1.xform.zoom;
        send_char('+');
        dyt_session_snapshot(sess, &s1, nullptr, 0);
        const bool zoom = s1.xform.zoom == z0 + 1;

        const bool ok = pal && fh && fv && zoom;
        std::printf("  %-4s the view keys reach the session "
                    "(palette %s, flip h %s, flip v %s, zoom %s)\n",
                    ok ? "ok" : "FAIL", pal ? "yes" : "NO", fh ? "yes" : "NO",
                    fv ? "yes" : "NO", zoom ? "yes" : "NO");
        if (!ok)
            fails++;
    }

    /* 44. Preferences round-trip, and a command-line flag wins over a saved
     * one.  Written to a temporary file so a test run never touches the real
     * config. */
    {
        char  tmpl[] = "/tmp/dytqt-prefs-XXXXXX";
        char *dir    = mkdtemp(tmpl);
        std::string path;
        bool round = false, prec = false, from = false;

        if (dir) {
            path = std::string(dir) + "/prefs.ini";

            prefs p;
            p.palette = 4; p.unit = 1; p.zoom = 3; p.fusion = 2; p.sr = 2;
            p.capture_dir = "/tmp/elsewhere";
            prefs_save(path, p);

            prefs q;
            prefs_load(path, q);
            round = q.palette == 4 && q.unit == 1 && q.zoom == 3 &&
                    q.fusion == 2 && q.sr == 2 &&
                    q.capture_dir == "/tmp/elsewhere";

            opts eff;                       /* --palette named, --zoom not */
            eff.palette = 2; eff.palette_set = true;
            opts_apply_prefs(eff, q);
            prec = eff.palette == 2 && eff.zoom == 3 &&
                   eff.capture_dir == "/tmp/elsewhere" &&
                   eff.unit == 1 && eff.fusion == 2 && eff.sr == 2;

            prefs cur = prefs_from_session(sess, ".");
            from = cur.palette >= 0 && cur.zoom >= 1 &&
                   !cur.capture_dir.empty();

            remove(path.c_str());
            rmdir(dir);
        }

        const bool ok = round && prec && from;
        std::printf("  %-4s preferences round-trip and the command line wins "
                    "(round-trip %s, precedence %s, from session %s)\n",
                    ok ? "ok" : "FAIL", round ? "yes" : "NO",
                    prec ? "yes" : "NO", from ? "yes" : "NO");
        if (!ok)
            fails++;
    }

    /* 45. The About key routes, and F1 is the same key. */
    {
        int about = 0;
        win.on_about_ = [&]() { about++; };
        send_char('?');
        send_key(Qt::Key_F1);
        const bool ok = about == 2;
        std::printf("  %-4s the About key routes ('?' and F1, %d)\n",
                    ok ? "ok" : "FAIL", about);
        if (!ok)
            fails++;
        win.on_about_ = nullptr;
    }

    /* 46. The About text names the app and the version the package carries, and
     * reports the one thing the build's feature macros cannot: whether a model
     * was actually loaded.  kAppVersion is the Makefile's VERSION when built
     * that way, so this is also what a stale About box would fail.
     *
     * The last two conditions are the guard for the *shared* key list: About
     * renders it verbatim, and the guide renders the same lines under their
     * headings.  A key added to the table and forgotten by one of them is
     * exactly the drift this catches, and it is checked here rather than
     * against a copy of the list because a copy is the thing that drifts. */
    {
        const std::string t  = about_text(true);
        const std::string t0 = about_text(false);
        const bool named = kAppVersion[0] != '\0' &&
                           t.find(kAppName) != std::string::npos &&
                           t.find(kAppVersion) != std::string::npos;
        const bool keys  = t.find("z Z") != std::string::npos;
        const bool model = t.find("a model is loaded") != std::string::npos &&
                           t0.find("no model loaded") != std::string::npos;

        const std::string list  = key_list_text();
        const std::string guide = help_text();
        const bool shared = !list.empty() &&
                            t.find(list) != std::string::npos;
        bool covered = !list.empty() && guide.find(kAppName) != std::string::npos;
        for (const key_line_t &k : kKeyLines)
            if (guide.find(k.line) == std::string::npos)
                covered = false;

        const bool ok = named && keys && model && shared && covered;
        std::printf("  %-4s the About text names the app and its version "
                    "(%s), the SR keys, the model state and the shared key "
                    "list (about %s, guide %s)\n",
                    ok ? "ok" : "FAIL", kAppVersion, shared ? "yes" : "NO",
                    covered ? "yes" : "NO");
        if (!ok)
            fails++;
    }

    /* 47. The super-resolution keys route to the session and post the notice
     * that is their only feedback — the status line says nothing while the mode
     * is off, so a refused key would otherwise be invisible.  Both cases are
     * exercised because the window must keep them apart, exactly as it keeps
     * 'e' from 'A'.  The *wording* is view_model_test's; what this pins is that
     * the window delivers it at all, through the same callback the pump's
     * notice uses. */
    {
        std::string got;
        fv->on_notice_ = [&](const std::string &s) { got = s; };

        dyt_snapshot_t s0{};
        dyt_session_snapshot(sess, &s0, nullptr, 0);
        const dyt_sr_t before = s0.sr;
        const bool     cap    = s0.sr_cap != 0;

        /* What each key must do, read from where the mode already was rather
         * than assumed, so a run that started with --sr already on is tested
         * too: with a model the key selects its own plane unless that plane is
         * already selected, in which case it turns it off; without one it is
         * refused. */
        const dyt_sr_t want_z =
            !cap                    ? DYT_SR_OFF :
            s0.sr == DYT_SR_VISIBLE ? DYT_SR_OFF : DYT_SR_VISIBLE;

        send_char('z');
        dyt_snapshot_t s1{};
        dyt_session_snapshot(sess, &s1, nullptr, 0);
        const bool z_ok = s1.sr == want_z && !got.empty();

        const dyt_sr_t want_Z =
            !cap                       ? DYT_SR_OFF :
            s1.sr == DYT_SR_THERMAL    ? DYT_SR_OFF : DYT_SR_THERMAL;

        got.clear();
        send_char('Z');
        dyt_snapshot_t s2{};
        dyt_session_snapshot(sess, &s2, nullptr, 0);
        const bool Z_ok = s2.sr == want_Z && !got.empty();

        const bool ok = z_ok && Z_ok;
        std::printf("  %-4s the super-resolution keys route, keep their case "
                    "and post a notice ('z'->%s, 'Z'->%s, \"%s\")\n",
                    ok ? "ok" : "FAIL", dyt_sr_name(s1.sr), dyt_sr_name(s2.sr),
                    got.c_str());
        if (!ok)
            fails++;

        /* Leave the mode as the run found it, so the frames the earlier
         * assertions painted are what a run without this block would have. */
        dyt_session_set_sr(sess, before);
        fv->on_notice_ = nullptr;
    }

    /* 48. A 2x render maps a click back to the native pixel.  This is the whole
     * point of carrying the factor in the view transform (display.h): the
     * window hands map() the native source size and the transformed destination
     * size, and sr is what makes the two agree.  Without it a click on a 2x
     * picture would land at half the coordinate and every measurement would be
     * placed wrong — silently, since the picture would still look right.
     *
     * Skipped, and says so, on a build with no model: the mode is refused and
     * there is no 2x picture to map. */
    {
        dyt_snapshot_t s{};
        dyt_session_snapshot(sess, &s, nullptr, 0);
        const dyt_sr_t was = s.sr;

        dyt_session_set_sr(sess, DYT_SR_THERMAL);    /* applies at any fusion */
        dyt_snapshot_t s2{};
        dyt_session_snapshot(sess, &s2, nullptr, 0);

        bool ok = false, skipped = false;
        if (!s2.sr_active) {
            skipped = true;
            ok      = true;
        } else {
            int sw = 0, sh = 0, ax = -1, ay = -1, bx = -1, by = -1;

            /* The destination the window actually has: the rendered picture
             * (native x sr) magnified by the window's zoom. */
            const int dw = s2.width * s2.xform.sr * s2.xform.zoom;
            const int dh = s2.height * s2.xform.sr * s2.xform.zoom;

            dyt_view_transform_size(&s2.xform, s2.width, s2.height, &sw, &sh);

            const bool a =
                dyt_view_transform_map(&s2.xform, s2.width, s2.height, dw, dh,
                                       0, 0, &ax, &ay) == 0;
            const bool b =
                dyt_view_transform_map(&s2.xform, s2.width, s2.height, dw, dh,
                                       dw - 1, dh - 1, &bx, &by) == 0;

            /* The two corner output pixels are the plane's two corners,
             * whichever way the mirror sends them — so the check is on the
             * span, not on which corner is which.  A transform that scaled by
             * zoom alone would send the last output pixel past the plane and
             * map() would refuse it. */
            const int xlo = a && b ? (ax < bx ? ax : bx) : -1;
            const int xhi = a && b ? (ax > bx ? ax : bx) : -1;
            const int ylo = a && b ? (ay < by ? ay : by) : -1;
            const int yhi = a && b ? (ay > by ? ay : by) : -1;

            ok = sw == dw && sh == dh &&
                 xlo == 0 && xhi == s2.width - 1 &&
                 ylo == 0 && yhi == s2.height - 1;
        }

        std::printf("  %-4s a 2x render maps a click back to the native pixel "
                    "(%s)\n", ok ? "ok" : "FAIL",
                    skipped ? "skipped: no model" : "both corners");
        if (!ok)
            fails++;

        dyt_session_set_sr(sess, was);
    }

    /* 49. F11 is full screen, and leaving it puts the window back.  No window
     * manager is involved under the offscreen platform, so what this pins is
     * the route (the key is not swallowed before it gets here), the state flag
     * the canvas and the menu read, and the re-fit on the way out — the guard
     * in fit_to_view() must not leave the window stuck at the screen's size. */
    {
        const QSize before = win.size();
        const bool  was    = win.isFullScreen();

        send_key(Qt::Key_F11);
        QApplication::processEvents();
        const bool on = win.isFullScreen() && win.fullscreen();

        send_key(Qt::Key_F11);
        QApplication::processEvents();
        const bool off = !win.isFullScreen() && !win.fullscreen();

        /* The window came back the size it went in at. */
        const bool refit = win.size() == before;

        const bool ok = !was && on && off && refit;
        std::printf("  %-4s F11 is full screen, and leaving it re-fits the "
                    "window (entered %s, left %s, back to %dx%d %s)\n",
                    ok ? "ok" : "FAIL", on ? "yes" : "NO", off ? "yes" : "NO",
                    win.size().width(), win.size().height(),
                    refit ? "yes" : "NO");
        if (!ok)
            fails++;
    }

    /* 50b. A control-panel button does what its key does.  The panel is the
     * Windows shell's route to the same actions the keyboard reaches and must
     * not become a second implementation.  The Line and Polygon buttons must
     * move the session exactly as 'l' and 'o' do, and None must clear exactly
     * as 'n' does.
     * (The menu bar this assertion was paired with is gone; the panel is now
     * the only on-screen route for these, so the check stands on its own.) */
    {
        dyt_snapshot_t s{};
        send_char('p');                         /* start somewhere to move from */
        dyt_session_snapshot(sess, &s, nullptr, 0);
        const bool start = s.tool == DYT_TOOL_POINT;

        QPushButton *line = win.panel()
            ? win.panel()->button(ControlPanel::Line) : nullptr;
        if (line)
            line->click();
        dyt_session_snapshot(sess, &s, nullptr, 0);
        const bool moved = s.tool == DYT_TOOL_LINE;

        QPushButton *poly = win.panel()
            ? win.panel()->button(ControlPanel::Poly) : nullptr;
        if (poly)
            poly->click();
        dyt_session_snapshot(sess, &s, nullptr, 0);
        const bool poly_on = s.tool == DYT_TOOL_POLYGON;

        QPushButton *none = win.panel()
            ? win.panel()->button(ControlPanel::ToolNone) : nullptr;
        if (none)
            none->click();
        dyt_session_snapshot(sess, &s, nullptr, 0);
        const bool cleared = s.tool == DYT_TOOL_NONE;

        const bool ok = start && line && moved && poly && poly_on &&
                        none && cleared;
        std::printf("  %-4s a panel button reaches the session like its key "
                    "(line %s, polygon %s, clear %s)\n", ok ? "ok" : "FAIL",
                    moved ? "yes" : "NO", poly_on ? "yes" : "NO",
                    cleared ? "yes" : "NO");
        if (!ok)
            fails++;
    }

    /* 50c. The tracking key hides and shows the extremes markers.  'm' is a
     * new binding for the Windows panel's Tracking switch; the markers were
     * always drawn before, so this pins both that the key reaches the canvas
     * flag and that the flag changes what is painted.  The two renders are
     * compared as images rather than sampled at a coordinate: the marker's
     * position depends on the transform the *painted* frame was rendered with,
     * which the session's current snapshot need not still match, but "the
     * drawing changed" holds regardless. */
    {
        /* Re-sync the canvas with the session first.  An earlier assertion
         * changed the transform (super-resolution) without a frame being
         * painted, and the marker projection is against the snapshot the
         * *painted* frame was rendered with — so a stale frame would project
         * the hot pixel off the image and the marker would silently not draw.
         * In real use the pump repaints every tick and the two never diverge;
         * here the pump is only stepped deliberately. */
        pm.step();
        QApplication::processEvents();

        if (!fv->hot_shown())                   /* make sure they start on */
            send_char('m');
        const QImage on_img = fv->render_canvas();

        send_char('m');
        const bool   hid = !fv->hot_shown();
        const QImage off_img = fv->render_canvas();

        send_char('m');
        const bool shown = fv->hot_shown();

        const bool differ = on_img != off_img;
        const bool ok = hid && shown && differ;
        std::printf("  %-4s the tracking key hides and shows the extremes "
                    "(hidden %s, back %s, drawing changed %s)\n",
                    ok ? "ok" : "FAIL", hid ? "yes" : "NO",
                    shown ? "yes" : "NO", differ ? "yes" : "NO");
        if (!ok)
            fails++;
    }

    /* 51. The checkmarks follow the frame, not the click.  The flip is driven
     * from a *key* here, so a sync that merely echoed the button's own toggled
     * state would leave the mark wrong — which is the failure this exists to
     * catch, since a checkable button is the only place the app shows a state
     * the engine owns. */
    {
        /* Read the session's own state rather than assuming where the flip
         * started: assertion 43 has already turned it on, so a test that
         * expected the first 'h' to light the mark would be testing the start
         * state rather than the sync. */
        auto flip_h = [&]() {
            dyt_snapshot_t s{};
            dyt_session_snapshot(sess, &s, nullptr, 0);
            return s.xform.flip_h != 0;
        };
        QPushButton *mark = win.panel()
            ? win.panel()->button(ControlPanel::FlipH) : nullptr;

        send_char('h');
        pm.step();                  /* a painted frame is what drives the sync */
        const bool a  = flip_h();
        const bool c1 = mark && mark->isChecked() == a;

        send_char('h');
        pm.step();
        const bool b  = flip_h();
        const bool c2 = mark && mark->isChecked() == b;

        /* The mark matched the frame both times *and* the two frames differed,
         * so neither a sync that never ran nor one that echoed the button's own
         * toggle can pass.  The two 'h' presses cancel, leaving the flip as it
         * was found. */
        const bool ok = mark && a != b && c1 && c2;
        std::printf("  %-4s the checkmarks follow the frame, not the click "
                    "(flip h %d then %d, matched %s / %s)\n",
                    ok ? "ok" : "FAIL", (int)a, (int)b, c1 ? "yes" : "NO",
                    c2 ? "yes" : "NO");
        if (!ok)
            fails++;
    }

    /* 53. The control panel cannot take the keyboard.  A focused button
     * swallows the keys before keyPressEvent sees them, which would break every
     * binding in the app the moment someone clicked one — and it stays
     * invisible until a user reports that the keys stopped working after they
     * clicked something.  (Assertion 53b covers the rail the same way; the menu
     * bar and toolbars this used to check are gone.) */
    {
        ControlPanel *panel = win.panel();
        const QList<QPushButton *> btns =
            panel ? panel->findChildren<QPushButton *>()
                  : QList<QPushButton *>();
        int bad = 0;
        for (QPushButton *b : btns)
            if (b->focusPolicy() != Qt::NoFocus)
                bad++;

        const bool ok = panel && !btns.isEmpty() && bad == 0;
        std::printf("  %-4s the control panel cannot take the keyboard "
                    "(%d control(s), %d that would)\n", ok ? "ok" : "FAIL",
                    (int)btns.size(), bad);
        if (!ok)
            fails++;
    }

    /* 53b. The icon rail cannot take the keyboard either, for the same reason
     * as 53: a focused rail button swallows the keys, including an armed
     * parameter ladder.  Checked by walking the rail's own buttons rather than
     * trusting the constructor, so a rail button added later without the policy
     * fails here instead of silently breaking the keyboard. */
    {
        IconRail *rail = win.rail();
        const QList<QPushButton *> btns =
            rail ? rail->findChildren<QPushButton *>()
                 : QList<QPushButton *>();
        int bad = 0;
        for (QPushButton *b : btns)
            if (b->focusPolicy() != Qt::NoFocus)
                bad++;
        const bool ok = rail && btns.size() == (int)IconRail::N_RailItems
                        && bad == 0;
        std::printf("  %-4s the icon rail cannot take the keyboard "
                    "(%d button(s), %d that would)\n",
                    ok ? "ok" : "FAIL", (int)btns.size(), bad);
        if (!ok)
            fails++;
    }

    /* 53c. The Super Resolution tab.  Three things have to hold, and each
     * fails differently: the radios must show the mode the *session* holds
     * (not the last click), the status line must tell the truth about whether
     * a model is behind the feature, and a click must reach the session
     * through the same dispatch as 'z'/'Z' — including the Off row, whose key
     * is not fixed, because 'z' and 'Z' each toggle their own plane and
     * neither alone means "off" from every mode. */
    {
        ControlPanel *panel = win.panel();
        auto checked = [&](ControlPanel::Id i) {
            QPushButton *b = panel ? panel->button(i) : nullptr;
            return b && b->isChecked();
        };

        /* Sync the panel with the session as it stands, so the read-back below
         * is against the current state rather than whatever the previous
         * assertion last painted. */
        pm.step();
        QApplication::processEvents();

        dyt_snapshot_t s0{};
        dyt_session_snapshot(sess, &s0, nullptr, 0);
        const dyt_sr_t before = s0.sr;
        const bool     cap    = win.sr_model_loaded();

        const bool reflect =
            checked(ControlPanel::SrOff)     == (before == DYT_SR_OFF) &&
            checked(ControlPanel::SrVisible) == (before == DYT_SR_VISIBLE) &&
            checked(ControlPanel::SrThermal) == (before == DYT_SR_THERMAL);

        QLabel *st = panel ? panel->sr_status() : nullptr;
        const QString text = st ? st->text() : QString();
        const bool status = cap ? text.contains(QStringLiteral("Model loaded"))
                                : text.contains(QStringLiteral("No model"));

        /* Click a plane, then Off, and require the session and the radios to
         * agree after each.  With no model the radios are disabled instead, and
         * a disabled radio that still reported a mode would be a lie. */
        bool moved = true, followed = true, off_ok = true, disabled = true;
        if (cap) {
            const dyt_sr_t want = before == DYT_SR_VISIBLE ? DYT_SR_THERMAL
                                                           : DYT_SR_VISIBLE;
            QPushButton *b = panel->button(want == DYT_SR_VISIBLE
                                           ? ControlPanel::SrVisible
                                           : ControlPanel::SrThermal);
            if (b)
                b->click();
            pm.step();
            QApplication::processEvents();
            dyt_snapshot_t s1{};
            dyt_session_snapshot(sess, &s1, nullptr, 0);
            moved    = s1.sr == want;
            followed = checked(ControlPanel::SrVisible) ==
                           (s1.sr == DYT_SR_VISIBLE) &&
                       checked(ControlPanel::SrThermal) ==
                           (s1.sr == DYT_SR_THERMAL) &&
                       checked(ControlPanel::SrOff) == (s1.sr == DYT_SR_OFF);

            /* The Off row, from a mode that is not off — which is where its
             * key has to be chosen rather than assumed. */
            QPushButton *off = panel->button(ControlPanel::SrOff);
            if (off)
                off->click();
            pm.step();
            QApplication::processEvents();
            dyt_snapshot_t s2{};
            dyt_session_snapshot(sess, &s2, nullptr, 0);
            off_ok = s2.sr == DYT_SR_OFF && checked(ControlPanel::SrOff);
        } else {
            QPushButton *v = panel->button(ControlPanel::SrVisible);
            disabled = v && !v->isEnabled() && !checked(ControlPanel::SrVisible);
        }

        /* Leave the mode as the run found it. */
        dyt_session_set_sr(sess, before);
        pm.step();
        QApplication::processEvents();

        const bool ok = panel && st && reflect && status && moved && followed
                        && off_ok && disabled;
        std::printf("  %-4s the Super Resolution tab reflects the session "
                    "(mode %s, model %s, plane %s, off %s)\n",
                    ok ? "ok" : "FAIL", dyt_sr_name(before),
                    cap ? "loaded" : "none", moved ? "yes" : "NO",
                    off_ok ? "yes" : "NO");
        if (!ok)
            fails++;
    }

    /* 53d. Every tab of the control panel is reachable.  A QTabWidget whose
     * tabs do not fit hides the overflow behind scroll arrows — the "control
     * the user cannot reach" failure, and a live risk here because the tab
     * count only grows (the Windows panel's four tabs need a much wider column
     * than the two we have so far).  53h is the vertical twin of this.
     *
     * Measured *themed*, unlike every other assertion here.  The tabs' fit is a
     * property of the stylesheet's padding and font size, and the selftest runs
     * unthemed on purpose (see run_gui); measuring the default style would
     * answer a question nobody asks.  The stylesheet is put back immediately so
     * the geometry assertions after this one still see the unthemed metrics
     * they were calibrated against. */
    {
        ControlPanel *panel = win.panel();
        QTabWidget *tabs = panel ? panel->tabs() : nullptr;
        const int   n    = tabs ? tabs->count() : 0;

        qApp->setStyleSheet(QString::fromUtf8(kDarkQss));
        QApplication::processEvents();
        const int want = tabs ? tabs->tabBar()->sizeHint().width() : -1;
        const int have = tabs ? tabs->width() - 2 : -1;  /* less the pane border */
        qApp->setStyleSheet(QString());
        QApplication::processEvents();

        const bool ok = n >= 2 && want > 0 && want <= have;
        std::printf("  %-4s every control-panel tab fits, with no scroll arrow "
                    "(%d tab(s), %d px of %d)\n",
                    ok ? "ok" : "FAIL", n, want, have);
        if (!ok)
            fails++;
    }

    /* 53e. The Settings dialog.  Five things, each a different failure: the
     * rail's Setting item opens it at all; it is non-modal, so the reading it
     * exists to change is not hidden behind it; each parameter starts from the
     * value the session knows, an override superseding the stored value and a
     * *failed* write not doing so; a Send reaches FrameView::on_param_send_ —
     * the very callback the ladder's 'y' uses — with exactly the value the
     * field holds, so the dialog is an input method and not a second write
     * path; and a refusal is reported rather than swallowed. */
    {
        /* A known starting state, the same one assertion 34 left: an
         * emissivity write that succeeded, and a distance write that did not. */
        fv->set_param_result(DYT_ORDER_EMISSIVITY, 0.80f, 0);
        fv->set_param_result(DYT_ORDER_DISTANCE, 5.0f, -1);

        dyt_order_type_t got_type  = (dyt_order_type_t)0;
        float            got_value = -1.f;
        int              calls     = 0;
        bool             accept    = true;
        fv->on_param_send_ = [&](dyt_order_type_t t, float v) {
            got_type = t; got_value = v; calls++;
            return accept;
        };

        /* Through the rail, which is how a user reaches it. */
        QPushButton *rail_setting = win.rail()
            ? win.rail()->button(IconRail::Setting) : nullptr;
        if (rail_setting)
            rail_setting->click();
        QApplication::processEvents();

        SettingsDialog *dlg = win.settings_dialog();
        const bool opened  = rail_setting && dlg && dlg->isVisible();
        /* Non-modal on purpose: sending a parameter is only useful if you can
         * watch the reading move, and exec() would put the reading behind the
         * dialog.  A modal dialog here would also block the pump's timer. */
        const bool modeless = dlg && dlg->isModal() == false;

        int rows = 0;
        for (int i = 0; i < dyt_vm_ladder_count(); i++) {
            const dyt_vm_ladder_t *L = dyt_vm_ladder_at(i);
            if (L && dlg->spin(L->type) && dlg->send_button(L->type))
                rows++;
        }

        /* Three seeding rules, and each needs a different value to be visible.
         * Assertion 32 left the device reporting emissivity 0.75 and distance
         * 1.00 m, and this assertion left an emissivity write of 0.80 that
         * succeeded beside a distance write of 5.00 that did not — so:
         *
         *   emissivity  0.80  the override supersedes the stored 0.75;
         *   distance    1.00  the failed write did NOT supersede the stored
         *                     value (5.00 would be the wrong answer);
         *   ambient    20.00  the device never reported it, so the row falls
         *                     back to the ladder's first rung rather than to a
         *                     zero that looks like a reading.
         *
         * All three are distinct from the value a row would show if it simply
         * echoed the ladder, so a seeding bug cannot pass. */
        QDoubleSpinBox *se = dlg->spin(DYT_ORDER_EMISSIVITY);
        QDoubleSpinBox *sd = dlg->spin(DYT_ORDER_DISTANCE);
        QDoubleSpinBox *sa = dlg->spin(DYT_ORDER_AMBIENT);
        const bool seeded =
            se && sd && sa &&
            std::fabs(se->value() - 0.80)  < 1e-6 &&
            std::fabs(sd->value() - 1.00)  < 1e-6 &&
            std::fabs(sa->value() - 20.00) < 1e-6;

        /* Send what the field holds, and require the write path to see exactly
         * that.  0.50 is a ladder rung, so the value is one the keyboard could
         * also produce — the dialog may reach more values, never others. */
        bool sent = false, refused = false;
        if (se) {
            se->setValue(0.50);
            QPushButton *b = dlg->send_button(DYT_ORDER_EMISSIVITY);
            if (b)
                b->click();
            sent = calls == 1 && got_type == DYT_ORDER_EMISSIVITY &&
                   std::fabs(got_value - 0.50f) < 1e-6 &&
                   dlg->status_text().contains(QStringLiteral("sent"));

            /* Now a refusal — the device is busy, or there is none — must be
             * reported, not swallowed. */
            accept = false;
            if (b)
                b->click();
            refused = calls == 2 &&
                      dlg->status_text().contains(QStringLiteral("refused"));
        }

        /* Re-seeded on every open: a value written since must be what the field
         * shows, not the one it was built with. */
        bool reseeded = false;
        if (se) {
            fv->set_param_result(DYT_ORDER_EMISSIVITY, 0.10f, 0);
            win.settings_dialog();          /* as the rail's handler does */
            reseeded = std::fabs(se->value() - 0.10) < 1e-6;
        }

        const bool ok = opened && modeless && rows == dyt_vm_ladder_count() &&
                        seeded && sent && refused && reseeded;
        std::printf("  %-4s the Settings dialog opens from the rail, is "
                    "modeless, and sends what its fields hold through the "
                    "ladder's own write path (%d row(s), open %s, modeless %s, "
                    "seeded %s, sent %s, refusal %s, re-seeded %s)\n",
                    ok ? "ok" : "FAIL", rows,
                    opened ? "yes" : "NO", modeless ? "yes" : "NO",
                    seeded ? "yes" : "NO",
                    sent ? "yes" : "NO", refused ? "yes" : "NO",
                    reseeded ? "yes" : "NO");
        if (!ok)
            fails++;

        if (dlg)
            dlg->hide();
        fv->on_param_send_ = nullptr;
        /* Leave the override table as the run found it. */
        fv->set_param_result(DYT_ORDER_EMISSIVITY, 0.80f, 0);
    }

    /* 53f. The Settings dialog's Display section — the view options the retired
     * menu bar carried.  Pinned for the same reason as every other control:
     * each one must reach the session or the window through the one dispatch,
     * and must show the state the session is in rather than the last thing
     * clicked.  Unit and fusion are the two the keyboard cannot express (they
     * are absolute choices), so those go to the session directly, exactly as
     * the menu bar did; the rest have keys and press them. */
    {
        SettingsDialog *dlg = win.settings_dialog();
        dyt_snapshot_t  s0{};
        dyt_session_snapshot(sess, &s0, nullptr, 0);

        /* Seeded from the session, not from a default. */
        const bool seeded =
            dlg->unit_button(s0.unit) &&
            dlg->unit_button(s0.unit)->isChecked() &&
            dlg->fusion_box() &&
            dlg->fusion_box()->currentIndex() == (int)s0.fusion;

        /* Unit: an absolute choice, so the radio moves the session itself. */
        const dyt_unit_t want_u =
            s0.unit == DYT_UNIT_K ? DYT_UNIT_C : DYT_UNIT_K;
        if (QPushButton *ub = dlg->unit_button(want_u))
            ub->click();
        dyt_snapshot_t s1{};
        dyt_session_snapshot(sess, &s1, nullptr, 0);
        const bool unit_ok = s1.unit == want_u;

        /* Fusion: likewise.  Driven through the combo's own index so the
         * signal a user's selection would raise is the one that runs. */
        const dyt_fusion_t want_f = s1.fusion == DYT_FUSION_BLEND
                                        ? DYT_FUSION_INFRARED : DYT_FUSION_BLEND;
        dlg->fusion_box()->setCurrentIndex((int)want_f);
        dyt_snapshot_t s2{};
        dyt_session_snapshot(sess, &s2, nullptr, 0);
        const bool fusion_ok = s2.fusion == want_f;

        /* Zoom is a delta, so its buttons press the keys. */
        const int zoom_before = s2.xform.zoom;
        if (QPushButton *zb = dlg->zoom_button(true))
            zb->click();
        dyt_snapshot_t s3{};
        dyt_session_snapshot(sess, &s3, nullptr, 0);
        const bool zoom_ok = s3.xform.zoom > zoom_before;
        if (QPushButton *zb = dlg->zoom_button(false))
            zb->click();                    /* put it back */

        /* Full screen and the device panel are window/canvas flags, and both
         * have keys — so the checkbox must press its key, not set the flag. */
        const bool fs_before = win.fullscreen();
        if (QCheckBox *cb = dlg->fullscreen_box())
            cb->click();
        const bool fs_ok = win.fullscreen() != fs_before;
        if (QCheckBox *cb = dlg->fullscreen_box())
            cb->click();                    /* back */
        const bool fs_restored = win.fullscreen() == fs_before;

        const bool info_before = fv->info_shown();
        if (QCheckBox *cb = dlg->info_box())
            cb->click();
        const bool info_ok = fv->info_shown() != info_before;
        if (QCheckBox *cb = dlg->info_box())
            cb->click();

        /* Retry and About reach the window's own callbacks, which is where the
         * device lifecycle and the About box live. */
        int  retries = 0, abouts = 0;
        win.on_retry_ = [&]() { retries++; };
        win.on_about_ = [&]() { abouts++; };
        if (QPushButton *rb = dlg->retry_button())
            rb->click();
        if (QPushButton *ab = dlg->about_button())
            ab->click();
        win.on_retry_ = nullptr;
        win.on_about_ = nullptr;
        const bool wired = retries == 1 && abouts == 1;

        /* Restore the session state this assertion moved. */
        dyt_session_set_unit(sess, s0.unit);
        dyt_session_set_fusion(sess, s0.fusion);

        const bool ok = dlg && seeded && unit_ok && fusion_ok && zoom_ok &&
                        fs_ok && fs_restored && info_ok && wired;
        std::printf("  %-4s the Settings Display section drives the session and "
                    "the window (seeded %s, unit %s, fusion %s, zoom %s, "
                    "full screen %s, panel %s, retry+about %s)\n",
                    ok ? "ok" : "FAIL", seeded ? "yes" : "NO",
                    unit_ok ? "yes" : "NO", fusion_ok ? "yes" : "NO",
                    zoom_ok ? "yes" : "NO",
                    (fs_ok && fs_restored) ? "yes" : "NO",
                    info_ok ? "yes" : "NO", wired ? "yes" : "NO");
        if (!ok)
            fails++;

        if (dlg)
            dlg->hide();
    }

    /* 53g. The rail's items reach what they claim to.  Each is clicked for
     * real — QPushButton::click() raises the same clicked signal a press does,
     * so the button's own wiring is under test and not just the handler — and
     * each is then checked against the *session*, so a handler that lit a flag
     * nobody reads cannot pass.  The two items with no engine behind them
     * (Rotate, Compare) are checked to be disabled, which is the honest state
     * for a control whose feature has not landed: greyed reads as "not yet",
     * where a live button that does nothing reads as broken. */
    {
        IconRail *rail = win.rail();
        ControlPanel *panel = win.panel();
        pm.step();              /* so snap_ (which the picker marks from) is fresh */

        /* --- Palette.  One entry per loaded palette, the mark on the current
         * one, and a choice that moves the session — by *both* routes the
         * picker offers: a digit-keyed entry (index < 10, which runs its key)
         * and a keyless one (index >= 10, which sets the palette directly). */
        dyt_snapshot_t p0{};
        dyt_session_snapshot(sess, &p0, nullptr, 0);

        QMenu *menu = win.palette_menu();
        QList<QAction *> pals;
        for (QAction *a : menu ? menu->actions() : QList<QAction *>())
            if (a->isCheckable())
                pals.append(a);

        const bool count_ok = menu && pals.size() == p0.palette_n;

        /* The mark is on the session's current palette, not on the first
         * entry: a menu that always marked entry 0 would pass a "something is
         * marked" test and fail this one. */
        const bool mark_ok = p0.palette >= 0 && p0.palette < pals.size() &&
                             pals[p0.palette]->isChecked();

        int low = -1;
        if (p0.palette_n > 1)
            low = (p0.palette == 0) ? 1 : 0;
        bool low_ok = true;
        if (low >= 0 && low < pals.size()) {
            pals[low]->trigger();
            dyt_snapshot_t s{};
            dyt_session_snapshot(sess, &s, nullptr, 0);
            low_ok = s.palette == low;
        }

        int high = -1;
        if (p0.palette_n > 10)
            high = (p0.palette == 10) ? 11 : 10;
        bool high_ok = true;
        if (high >= 10 && high < pals.size()) {
            pals[high]->trigger();
            dyt_snapshot_t s{};
            dyt_session_snapshot(sess, &s, nullptr, 0);
            high_ok = s.palette == high;
        }
        delete menu;
        dyt_session_set_palette(sess, p0.palette);      /* put it back */

        /* The rail button opens the picker.  popup() shows a menu and returns,
         * so this neither blocks nor needs a nested loop. */
        QPushButton *pbtn = rail ? rail->button(IconRail::Palette) : nullptr;
        if (pbtn)
            pbtn->click();
        QMenu *popped = nullptr;
        for (QMenu *m : win.findChildren<QMenu *>())
            if (m->isVisible())
                popped = m;
        const bool popup_ok = popped != nullptr;
        if (popped)
            popped->close();

        /* --- Mark re-arms the last active tool.  'l' makes Line the last
         * active tool, 'n' clears the tool without changing that memory, and
         * the button must bring Line back. */
        dyt_snapshot_t t0{};
        dyt_session_snapshot(sess, &t0, nullptr, 0);
        send_char('l');
        pm.step();
        send_char('n');
        pm.step();
        dyt_snapshot_t t1{};
        dyt_session_snapshot(sess, &t1, nullptr, 0);
        QPushButton *mbtn = rail ? rail->button(IconRail::Mark) : nullptr;
        if (mbtn)
            mbtn->click();
        dyt_snapshot_t t2{};
        dyt_session_snapshot(sess, &t2, nullptr, 0);
        const bool mark_tool_ok = t1.tool == DYT_TOOL_NONE &&
                                  t2.tool == DYT_TOOL_LINE;
        dyt_session_set_tool(sess, t0.tool);            /* restore */

        /* --- Reset Image puts the view back.  A known non-canonical view is
         * built first — zoom to the floor then one step up, and the flip set
         * *to* 1 rather than toggled, so the parity is not guesswork — because
         * a reset that did nothing would otherwise pass on a view that was
         * already canonical. */
        dyt_snapshot_t v0{};
        dyt_session_snapshot(sess, &v0, nullptr, 0);

        dyt_session_zoom(sess, -1000);                  /* clamp to the floor */
        dyt_session_zoom(sess, +1);
        dyt_snapshot_t r0{};
        dyt_session_snapshot(sess, &r0, nullptr, 0);
        if (r0.xform.flip_h == 0)
            dyt_session_toggle_flip_h(sess);
        pm.step();
        dyt_snapshot_t r1{};
        dyt_session_snapshot(sess, &r1, nullptr, 0);

        QPushButton *rbtn = rail ? rail->button(IconRail::ResetImage) : nullptr;
        if (rbtn)
            rbtn->click();
        dyt_snapshot_t r2{};
        dyt_session_snapshot(sess, &r2, nullptr, 0);
        const bool reset_ok = r1.xform.zoom != DYT_ZOOM_MIN &&
                              r1.xform.flip_h != 0 &&
                              r2.xform.flip_h == 0 && r2.xform.flip_v == 0 &&
                              r2.xform.zoom == DYT_ZOOM_MIN &&
                              r2.range_mode == DYT_RANGE_AUTO;

        /* --- Tutorials opens the guide, through the front end's callback, so
         * no modal opens here. */
        int help = 0;
        win.on_help_ = [&]() { help++; };
        QPushButton *tbtn = rail ? rail->button(IconRail::Tutorials) : nullptr;
        if (tbtn)
            tbtn->click();
        win.on_help_ = nullptr;
        const bool help_ok = help == 1;

        /* --- Rotate.  Clicked for real, then checked against the *session*:
         * a quarter turn per click, the output geometry swapped with it, and
         * Reset Image taking it back off — which is the reset's own promise,
         * and the reason rotation is a session field rather than a GUI one. */
        QPushButton *rot = rail ? rail->button(IconRail::Rotate) : nullptr;
        QPushButton *cmp = rail ? rail->button(IconRail::Compare) : nullptr;

        /* The canvas's own hint, before and after: it is what fit_to_view()
         * sizes the window from, so a hint that did not swap would leave the
         * turned picture cropped.  A frame is painted first, so the "before"
         * hint is the same zoom the "after" one is — the reset above changed
         * the framing, and comparing across that would measure the reset. */
        pm.step();
        const QSize hint0 = win.view() ? win.view()->sizeHint() : QSize();

        if (rot)
            rot->click();
        dyt_snapshot_t q1{};
        dyt_session_snapshot(sess, &q1, nullptr, 0);
        pm.step();                      /* a frame painted through the turn */

        const QSize hint1 = win.view() ? win.view()->sizeHint() : QSize();
        const bool rot_fit_ok = hint0.isValid() && hint1.isValid() &&
                                hint1.width()  < hint0.width() &&
                                hint1.height() > hint0.height();

        /* The turned geometry must be the untilted one with the axes swapped.
         * Compared against size() of the same transform with rot cleared rather
         * than against v0.width/height, because size() carries the zoom and the
         * sr factor too — and those are not what is under test here. */
        dyt_view_transform_t flat = q1.xform;
        flat.rot = DYT_ROT_NONE;

        int rw = 0, rh = 0, pw = 0, ph = 0, wx = 0, wy = 0, bx2 = 0, by2 = 0;
        dyt_view_transform_size(&flat, v0.width, v0.height, &pw, &ph);
        dyt_view_transform_size(&q1.xform, v0.width, v0.height, &rw, &rh);
        const bool rot_ok = q1.xform.rot == DYT_ROT_90 &&
                            rw == ph && rh == pw &&
                            dyt_view_transform_project(&q1.xform, v0.width,
                                                       v0.height, rw, rh,
                                                       0, 0, &wx, &wy) == 0 &&
                            dyt_view_transform_map(&q1.xform, v0.width,
                                                   v0.height, rw, rh,
                                                   wx, wy, &bx2, &by2) == 0 &&
                            bx2 == 0 && by2 == 0;

        if (rbtn)
            rbtn->click();
        dyt_snapshot_t q2{};
        dyt_session_snapshot(sess, &q2, nullptr, 0);
        const bool rot_reset_ok = q2.xform.rot == DYT_ROT_NONE;

        /* Put the framing back as it was found.  Without this the reset — and
         * the Rotate above, which ends by pressing the same button — would
         * quietly become the state the geometry assertions after this one
         * measure, so a change here could break them for a reason that has
         * nothing to do with geometry. */
        dyt_session_zoom(sess, -1000);
        dyt_session_zoom(sess, v0.xform.zoom - DYT_ZOOM_MIN);
        dyt_snapshot_t r3{};
        dyt_session_snapshot(sess, &r3, nullptr, 0);
        if ((r3.xform.flip_h != 0) != (v0.xform.flip_h != 0))
            dyt_session_toggle_flip_h(sess);
        if ((r3.xform.flip_v != 0) != (v0.xform.flip_v != 0))
            dyt_session_toggle_flip_v(sess);
        if (r3.xform.rot != v0.xform.rot)
            dyt_session_rotate(sess, v0.xform.rot - r3.xform.rot);
        dyt_session_set_range_mode(sess, v0.range_mode);
        pm.step();

        /* --- Compare is live now: enabled, and clicking it switches the panel
         * to the Comparison tab.  This replaces the old "pending" check that
         * held while Compare had no engine behind it — the engine has landed
         * (src/compare.c), so the button's claim is now "it switches to the
         * Comparison tab", not "it is greyed out".  The tab is named, not
         * numbered, because inserting 3D Analysis ahead of it moved the
         * Comparison page from 1 to 2 — the drift this pins against. */
        const int before_tab = panel ? panel->tabs()->currentIndex() : -1;
        if (cmp)
            cmp->click();
        const int after_tab  = panel ? panel->tabs()->currentIndex() : -1;
        const bool compare_ok = cmp && cmp->isEnabled() &&
                                before_tab != ControlPanel::TabComparison &&
                                after_tab == ControlPanel::TabComparison;
        /* Leave the panel where the user would expect it (not on Comparison
         * unless they were already there) so later assertions are not shifted
         * onto the Comparison page. */
        if (panel && panel->tabs() && before_tab != ControlPanel::TabComparison)
            panel->tabs()->setCurrentIndex(before_tab);

        const bool ok = count_ok && mark_ok && low_ok && high_ok &&
                        popup_ok && mark_tool_ok && reset_ok && help_ok &&
                        rot_ok && rot_fit_ok && rot_reset_ok && compare_ok;
        std::printf("  %-4s the rail's items reach what they claim "
                    "(%d palette entries %s, mark %s, pick %s/%s, popup %s, "
                    "re-arm %s, reset %s, rotate %s, refit %s, rotate-reset %s, "
                    "tutorials %s, compare %s)\n",
                    ok ? "ok" : "FAIL", (int)pals.size(),
                    count_ok ? "yes" : "NO", mark_ok ? "yes" : "NO",
                    low_ok ? "yes" : "NO", high_ok ? "yes" : "NO",
                    popup_ok ? "yes" : "NO", mark_tool_ok ? "yes" : "NO",
                    reset_ok ? "yes" : "NO", rot_ok ? "yes" : "NO",
                    rot_fit_ok ? "yes" : "NO", rot_reset_ok ? "yes" : "NO",
                    help_ok ? "yes" : "NO", compare_ok ? "yes" : "NO");
        if (!ok)
            fails++;
    }

    /* 53h. The control panel's page is not squeezed.  A QScrollArea with
     * widgetResizable() resizes its page to the viewport, so a window sized
     * from the scroll area's own — much smaller — hint silently *clips* the
     * page instead of scrolling it: the last group becomes a control the user
     * cannot reach, with no scrollbar to hint that anything is missing.  That
     * is the vertical twin of 53d's tab-bar fit, and it was a real bug: the
     * Capture group was cut off at the bottom of the window.  Checked by
     * measuring the page against its own hint rather than by counting rows, so
     * it holds whatever the platform's font metrics make the rows. */
    {
        ControlPanel *panel = win.panel();
        QTabWidget   *tabs  = panel ? panel->tabs() : nullptr;
        QWidget      *page  = nullptr;

        /* Measure the Troubleshoot page, not whatever a previous assertion
         * left current.  This is a scroll-area assertion — the 3D Analysis
         * page is deliberately not one (a scroll area would eat the wheel that
         * zooms it) — so it has to be looking at a scroll page, and Troubleshoot
         * is both the default and the tallest.  Named, not numbered, for the
         * same reason the rail's Compare handler is. */
        if (tabs)
            tabs->setCurrentIndex(ControlPanel::TabTroubleshoot);
        QApplication::processEvents();

        QScrollArea  *sa = qobject_cast<QScrollArea *>(
            tabs ? tabs->currentWidget() : nullptr);
        if (sa)
            page = sa->widget();

        /* The panel must ask for its content's height.  Asserted on the
         * panel's own hint rather than on the window's, because the window's is
         * the max of the panel's and the canvas's — and the canvas can be tall
         * for reasons of its own (a super-resolved frame, a zoom), which would
         * hand the panel the room it needs even while the panel was reporting
         * the scroll area's small hint.  The window's hint is the max, so
         * covering the panel here is what makes the window tall enough. */
        const int need = (tabs ? tabs->tabBar()->sizeHint().height() : 0) +
                         (page ? page->sizeHint().height() : 0);
        const bool room = panel && panel->sizeHint().height() >= need;

        /* And the window can still be made shorter than that: the scroll area
         * is then what keeps the lower groups reachable.  A minimum that
         * refused the shrink would leave the scroll area inert, and a window
         * that cannot be made small enough for the screen is its own trap. */
        const QSize was = win.size();
        win.resize(was.width(), 260);
        QApplication::processEvents();
        const bool shrank = win.height() < was.height();
        const bool scrolls = sa && sa->verticalScrollBar() &&
                             sa->verticalScrollBar()->maximum() > 0;
        /* The page must keep its height while the window is short — the scroll
         * area is what gives the user the rest.  Squeezed to the viewport
         * instead, the lower groups are clipped with no scrollbar to hint that
         * anything is missing, which is exactly the bug this caught. */
        const bool kept = page && page->height() >= page->sizeHint().height();
        win.resize(was);
        QApplication::processEvents();

        const bool ok = room && shrank && kept && scrolls;
        std::printf("  %-4s the control panel asks for its content's height "
                    "(panel %d of %d, page %d of %d, shrinks %s, keeps %s, "
                    "scrolls %s)\n", ok ? "ok" : "FAIL",
                    panel ? panel->sizeHint().height() : -1, need,
                    page ? page->height() : -1,
                    page ? page->sizeHint().height() : -1,
                    shrank ? "yes" : "NO", kept ? "yes" : "NO",
                    scrolls ? "yes" : "NO");
        if (!ok)
            fails++;
    }

    /* The toolbar-overflow assertion (56) is gone with the toolbars.  Its
     * concern — an on-screen control hidden behind Qt's overflow arrow — did
     * not go away, it moved: the tab bar is now the row that can outgrow its
     * container, and assertion 53d measures that fit. */

    /* 54. The canvas is drawn at its natural size until the window is bigger
     * than it needs, and scales to fit after that.  Both halves matter: the
     * first is what keeps the pixel assertions above honest (at the natural
     * size the scale is exactly 1 and the origin exactly (0,0)), and the second
     * is what makes full screen actually fill the screen — the sensor is
     * 256x384, so no integer zoom could. */
    {
        const QSize before     = win.size();
        const bool  at_natural = fv->display_scale() == 1.0;

        win.resize(before + QSize(360, 240));
        QApplication::processEvents();
        const double  grown = fv->display_scale();
        const QPointF org   = fv->display_origin();

        /* Centred in whichever axis has room to spare; on the binding axis the
         * scaled canvas is exactly the widget, so there is nothing to centre. */
        const QSize nat = fv->sizeHint();
        const double slack_x = win.view()->width()  - nat.width()  * grown;
        const double slack_y = win.view()->height() - nat.height() * grown;
        const bool centred =
            (slack_x < 2.0 || std::fabs(org.x() - slack_x / 2.0) < 1.5) &&
            (slack_y < 2.0 || std::fabs(org.y() - slack_y / 2.0) < 1.5);

        win.resize(before);
        QApplication::processEvents();
        const bool back = fv->display_scale() == 1.0;

        const bool ok = at_natural && grown > 1.0 && centred && back;
        std::printf("  %-4s the canvas fits the window when there is room "
                    "(1:1 %s, grown %.2fx %s, centred %s, back %s)\n",
                    ok ? "ok" : "FAIL", at_natural ? "yes" : "NO", grown,
                    grown > 1.0 ? "yes" : "NO", centred ? "yes" : "NO",
                    back ? "yes" : "NO");
        if (!ok)
            fails++;
    }

    /* 55. A click at a scaled position names the pixel it looks like it names.
     * The display transform is invisible to the mapping — pointer() is its
     * exact inverse — and this is the assertion that catches a scale applied
     * to the paint but not to the pointer.  That failure would place every
     * marker somewhere else on the picture while looking entirely plausible,
     * which is exactly the kind of bug the pointer/marker agreement rule
     * exists to prevent. */
    {
        const QSize before = win.size();
        win.resize(before + QSize(360, 240));
        QApplication::processEvents();
        const double s = fv->display_scale();

        dyt_snapshot_t s0{};
        dyt_session_snapshot(sess, &s0, nullptr, 0);
        const QSize is = fv->imageSize();

        /* A source pixel well inside the frame, so neither the projection nor
         * the click can land on an edge. */
        const int  sx = s0.width / 3, sy = s0.height / 3;
        int        dx = -1, dy = -1;
        const bool proj = dyt_view_transform_project(&s0.xform, s0.width,
                                                     s0.height, is.width(),
                                                     is.height(), sx, sy,
                                                     &dx, &dy) == 0;

        send_key(Qt::Key_P);                    /* the point tool */
        send_mouse(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton,
                   dx, dy);

        dyt_snapshot_t s1{};
        dyt_session_snapshot(sess, &s1, nullptr, 0);
        const bool ok = s > 1.0 && proj && s1.tool == DYT_TOOL_POINT &&
                        s1.p0.x == sx && s1.p0.y == sy;
        std::printf("  %-4s a click at a scaled position names the right pixel "
                    "(%.2fx, (%d,%d) -> (%d,%d), wanted (%d,%d))\n",
                    ok ? "ok" : "FAIL", s, dx, dy, s1.p0.x, s1.p0.y, sx, sy);
        if (!ok)
            fails++;

        send_key(Qt::Key_N);                    /* clear, and no tool */
        win.resize(before);
        QApplication::processEvents();
    }

    /* 60. The 3D Analysis tab.  The vendor's panel has a 3D Analysis page
     * between Troubleshoot and Comparison, and the port now does too.  The
     * risk this guards is the one a 3D view hides best: a mesh that is not
     * built from the frame (so the landscape is fiction), a colour that did
     * not come from the active palette (so the surface and the canvas disagree
     * about what a temperature looks like), or a tab that is dead on a machine
     * without GL.  The software renderer is therefore asserted directly — it is
     * the fallback and the only path that exists everywhere — while the GL path
     * is asserted as "the container shows GL exactly when GL is usable", so the
     * check holds whether or not a context came up. */
    {
        ControlPanel *panel = win.panel();
        SurfaceView  *surf  = panel ? panel->surface() : nullptr;
        QTabWidget   *tabs  = panel ? panel->tabs() : nullptr;

        /* The tab exists, is named as the vendor names it, and sits where the
         * vendor's panel puts it. */
        const bool tab_ok = tabs && surf &&
                            tabs->count() == ControlPanel::TabCount &&
                            tabs->tabText(ControlPanel::TabTroubleshoot) ==
                                QStringLiteral("Troubleshoot") &&
                            tabs->tabText(ControlPanel::TabAnalysis3D) ==
                                QStringLiteral("3D Analysis") &&
                            tabs->tabText(ControlPanel::TabComparison) ==
                                QStringLiteral("Comparison");

        /* The pump feeds it every painted frame, so one step is enough for the
         * model to hold the frame the session is showing. */
        pm.step();
        dyt_snapshot_t s{};
        const bool have_snap = dyt_session_snapshot(sess, &s, nullptr, 0) == 0;
        std::vector<float> src;
        if (have_snap) {
            src.resize((size_t)s.width * s.height);
            dyt_session_snapshot(sess, &s, src.data(), (int)src.size());
        }

        bool mesh_ok = false, height_ok = false, color_ok = false;
        bool cam_ok = false, drag_ok = false, wheel_ok = false, paint_ok = false;
        bool backend_ok = false;

        if (surf) {
            const SurfaceModel &m = surf->model();
            const bool ready = have_snap && s.width >= 2 && s.height >= 2 &&
                               (int)src.size() >= s.width * s.height;

            mesh_ok = !m.empty() && m.w >= 2 && m.h >= 2 &&
                      m.w <= kSurfaceMaxDim && m.h <= kSurfaceMaxDim &&
                      m.vertex_count() == (long)m.w * m.h &&
                      (long)m.idx.size() == (long)(m.w - 1) * (m.h - 1) * 6;

            /* Height is the temperature: the decimation pins both ends, so the
             * first and last vertices are the first and last source pixels. */
            height_ok = ready && !m.empty() &&
                        std::fabs(m.verts.front().z - src.front()) < 1e-4f &&
                        std::fabs(m.verts.back().z - src.back()) < 1e-4f;

            /* Every colour is the palette's colour for that vertex's
             * temperature at the frame's display range — the canvas's own
             * lookup, not a second one. */
            color_ok = ready && !m.empty();
            for (size_t k = 0; color_ok && k < m.verts.size(); k++) {
                const int pi = dyt_palette_index(m.verts[k].z, s.lo, s.hi);
                color_ok = m.col[k * 3 + 0] == pm.pal.rgb[pi * 3 + 0] &&
                           m.col[k * 3 + 1] == pm.pal.rgb[pi * 3 + 1] &&
                           m.col[k * 3 + 2] == pm.pal.rgb[pi * 3 + 2];
            }

            /* The camera: reset, set, a drag of 30 px right and 15 px down
             * (0.01 rad per px), and the wheel. */
            surf->reset_camera();
            cam_ok = std::fabs(m.yaw - kSurfaceYaw0) < 1e-4f &&
                     std::fabs(m.pitch - kSurfacePitch0) < 1e-4f &&
                     std::fabs(m.zoom - kSurfaceZoom0) < 1e-4f;

            surf->set_camera(0.0f, 0.0f, 1.0f);
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(10, 10),
                              QPointF(10, 10), Qt::LeftButton, Qt::LeftButton,
                              Qt::NoModifier);
            QApplication::sendEvent(surf, &press);
            QMouseEvent drag(QEvent::MouseMove, QPointF(40, 25),
                             QPointF(40, 25), Qt::NoButton, Qt::LeftButton,
                             Qt::NoModifier);
            QApplication::sendEvent(surf, &drag);
            drag_ok = std::fabs(m.yaw - 0.30f) < 1e-3f &&
                      std::fabs(m.pitch - 0.15f) < 1e-3f;

            const float z0 = m.zoom;
            QWheelEvent we(QPointF(20, 20), QPointF(20, 20), QPoint(0, 0),
                           QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                           Qt::NoScrollPhase, false);
            QApplication::sendEvent(surf, &we);
            wheel_ok = m.zoom > z0;

            /* The container shows GL exactly when GL is usable. */
            backend_ok = surf->using_gl() == surf->gl_ready();

            /* The software renderer paints the surface.  The claim checked is
             * the one a blank or single-colour widget cannot fake: the
             * non-background pixels form a sizeable, roughly *square* patch.
             * Square is the interesting part — the mesh is square, and the
             * aspect term in the matrix is what keeps it square in a viewport
             * of any shape, so a stretched footprint would mean the aspect
             * correction had broken.  The widget's own size is not asserted
             * (the layout owns it), only the shape of what it drew. */
            surf->set_camera(kSurfaceYaw0, kSurfacePitch0, kSurfaceZoom0);
            QWidget *sv = surf->software_view();
            if (sv) {
                const QImage img =
                    sv->grab().toImage().convertToFormat(QImage::Format_RGB32);
                int  x0 = img.width(), y0 = img.height(), x1 = -1, y1 = -1;
                QSet<QRgb> shades;
                for (int y = 0; y < img.height(); y++)
                    for (int x = 0; x < img.width(); x++) {
                        const QRgb c = img.pixel(x, y);
                        if (qRed(c) <= 30 && qGreen(c) <= 30 && qBlue(c) <= 30)
                            continue;              /* the background */
                        shades.insert(c);
                        x0 = std::min(x0, x); x1 = std::max(x1, x);
                        y0 = std::min(y0, y); y1 = std::max(y1, y);
                    }
                const int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
                const double ratio = bh > 0 ? (double)bw / (double)bh : 0.0;
                paint_ok = bw > 16 && bh > 16 &&
                           ratio > 0.4 && ratio < 2.5 && shades.size() > 16;
            }
        }

        const bool ok = tab_ok && mesh_ok && height_ok && color_ok && cam_ok &&
                        drag_ok && wheel_ok && backend_ok && paint_ok;
        std::printf("  %-4s the 3D Analysis tab builds a mesh from the frame, "
                    "colours it from the palette and orbits "
                    "(tab %s, mesh %s %dx%d, height %s, colour %s, camera %s, "
                    "drag %s, wheel %s, backend %s (%s), paint %s)\n",
                    ok ? "ok" : "FAIL",
                    tab_ok ? "yes" : "NO", mesh_ok ? "yes" : "NO",
                    surf ? surf->model().w : -1, surf ? surf->model().h : -1,
                    height_ok ? "yes" : "NO", color_ok ? "yes" : "NO",
                    cam_ok ? "yes" : "NO", drag_ok ? "yes" : "NO",
                    wheel_ok ? "yes" : "NO", backend_ok ? "yes" : "NO",
                    surf && surf->using_gl() ? "GL" : "software",
                    paint_ok ? "yes" : "NO");
        if (!ok)
            fails++;
    }

    /* 61. The Circuit Design tab.  The vendor's panel overlays a board layout
     * on the thermal picture; with no gerber/CAD parser in scope the layout is
     * an image the user loads.  Six risks, each a different failure: the tab is
     * not where the vendor puts it or is not named for what it does; the align
     * control accepts a value the canvas will not honour, so the two disagree
     * about what the control means; the panel and the canvas are not actually
     * joined; the overlay does not hold what was loaded; it does not composite
     * at all — a loaded image that changed no pixel looks exactly like a
     * working one in a screenshot of the page; and it does not follow the view,
     * so it stays put while the thermal content turns under it.  The composite
     * is therefore asserted by *difference* against the same canvas rendered
     * with no layout: an opaque stripe must appear where the layout puts it,
     * move by exactly the offset, and turn with a quarter turn. */
    {
        ControlPanel *panel = win.panel();
        QTabWidget   *tabs  = panel ? panel->tabs() : nullptr;
        FrameView    *view  = win.view();

        const bool tab_ok = tabs && panel &&
                            tabs->count() == ControlPanel::TabCount &&
                            tabs->tabText(ControlPanel::TabCircuitDesign) ==
                                QStringLiteral("Circuit Design") &&
                            tabs->tabText(ControlPanel::TabComparison) ==
                                QStringLiteral("Comparison") &&
                            tabs->tabText(ControlPanel::TabSuperResolution) ==
                                QStringLiteral("Super Resolution");

        /* The align fields clamp to the vendor's fusion range.  The canvas
         * clamps with the same function (dyt_fusion_clamp_align), so a value
         * the spin accepted but the canvas refused cannot exist. */
        bool clamp_ok = false;
        if (panel && panel->layout_spin_x() && panel->layout_spin_y()) {
            QSpinBox *sx = panel->layout_spin_x();
            QSpinBox *sy = panel->layout_spin_y();
            sx->setValue(1000);
            sy->setValue(-1000);
            const bool hi = sx->value() ==  DYT_FUSION_ALIGN_MAX &&
                            sy->value() == -DYT_FUSION_ALIGN_MAX;
            sx->setValue(-1000);
            sy->setValue(1000);
            const bool lo = sx->value() == -DYT_FUSION_ALIGN_MAX &&
                            sy->value() ==  DYT_FUSION_ALIGN_MAX;
            sx->setValue(0);
            sy->setValue(0);
            clamp_ok = hi && lo;
        }

        /* The panel→canvas push, through the very function run_gui wires, so
         * the contract the app uses is the one under test. */
        bool wire_ok = false;
        if (panel && view && panel->layout_spin_x()) {
            panel->on_layout_changed_ = [&]() { push_layout(panel, view); };
            panel->layout_spin_x()->setValue(7);
            panel->layout_spin_y()->setValue(-9);
            wire_ok = view->layout_dx() == 7 && view->layout_dy() == -9;
            panel->layout_spin_x()->setValue(0);
            panel->layout_spin_y()->setValue(0);
            panel->on_layout_changed_ = nullptr;
        }

        /* The High TEMP. Alarm threshold field.  Its own clamp, then the whole
         * field→push→session path through the very function run_gui wires — a
         * field whose value the session never sees would arm at whatever the
         * default was, which is exactly the "no way to set the temperature"
         * gap the Windows panel closes with num_alarm_val. */
        bool alarm_clamp_ok = false, alarm_push_ok = false,
             alarm_follows_ok = false;
        if (panel && view && panel->alarm_spin()) {
            panel->on_alarm_setpoint_changed_ = [&]() {
                push_alarm(panel, view);
            };

            QDoubleSpinBox *sp = panel->alarm_spin();
            sp->setValue(-1000.0);
            const bool lo_ok =
                std::fabs(sp->value() - (double)DYT_ALARM_SETPOINT_MIN) < 1e-6;
            sp->setValue(1000.0);
            const bool hi_ok =
                std::fabs(sp->value() - (double)DYT_ALARM_SETPOINT_MAX) < 1e-6;
            alarm_clamp_ok = lo_ok && hi_ok;

            sp->setValue(42.5);              /* fires the push */
            alarm_push_ok = std::fabs(view->alarm_setpoint() - 42.5f) < 1e-3f;

            /* Arm at the field, then edit it: the session must follow, or the
             * number on screen and the number being compared against would
             * disagree about when the alarm fires. */
            view->measure_key('a');
            dyt_snapshot_t a1{};
            dyt_session_snapshot(sess, &a1, nullptr, 0);
            const bool armed_at_42 = a1.alarm_on != 0 &&
                                     std::fabs(a1.alarm_hi - 42.5f) < 1e-3f;

            sp->setValue(55.0);              /* fires the push, which re-arms */
            dyt_snapshot_t a2{};
            dyt_session_snapshot(sess, &a2, nullptr, 0);
            alarm_follows_ok = armed_at_42 &&
                               std::fabs(a2.alarm_hi - 55.0f) < 1e-3f;

            view->measure_key('a');          /* disarm */

            sp->setValue((double)DYT_ALARM_SETPOINT_DEFAULT);
            panel->on_alarm_setpoint_changed_ = nullptr;
        }
        {
            const bool ok = alarm_clamp_ok && alarm_push_ok && alarm_follows_ok;
            std::printf("  %-4s the alarm's threshold field clamps to -20..450 "
                        "and the session arms at what it holds "
                        "(clamp %s, pushed %s, follows an armed edit %s)\n",
                        ok ? "ok" : "FAIL", alarm_clamp_ok ? "yes" : "NO",
                        alarm_push_ok ? "yes" : "NO",
                        alarm_follows_ok ? "yes" : "NO");
            if (!ok)
                fails++;
        }

        /* 64. The colour bar's range handles.  The bar is a scale over the
         * frame's own extremes and the two handles mark the display window
         * inside it — the Windows panel's panel_lut_max_but / panel_lut_min_but,
         * which drag the window's ends.  A drag on the top one narrows the
         * window from above and turns the range fixed; a double-click hands it
         * back to the frame, which is the way out of a window dragged to
         * nothing.
         *
         * The handle colours (255,90,60) and (90,180,255) are checked against
         * every shipped palette: no palette entry is either, so a pixel probe
         * for one is about the handle and not about the picture. */
        {
            bool geom_ok = false, paint_ok = false, drag_ok = false,
                 moved_ok = false, auto_ok = false;
            float seen_lo = 0.f, seen_hi = 0.f;

            if (view && fv) {
                dyt_session_reset_view(sess);
                pm.step();

                dyt_snapshot_t s0{};
                dyt_session_snapshot(sess, &s0, nullptr, 0);
                const QRect bar = fv->bar_rect();

                /* The scale is the frame's own extremes, so at AUTO — where
                 * the window *is* the frame — both handles sit on the bar's
                 * ends. */
                geom_ok = s0.stats.hi > s0.stats.lo && bar.height() > 8 &&
                          bar.width() > 4 &&
                          s0.range_mode == DYT_RANGE_AUTO &&
                          fv->bar_handle_row(s0.hi) == bar.top() &&
                          fv->bar_handle_row(s0.lo) == bar.bottom();

                const QImage before = fv->render_canvas();
                int hot_px = 0;
                for (int y = 0; y < before.height(); y++)
                    for (int x = bar.left(); x <= bar.right(); x++)
                        if (before.pixelColor(x, y) == QColor(255, 90, 60))
                            hot_px++;
                paint_ok = hot_px >= 3;

                /* Aim a drag at a row a third of the way down the bar.  That
                 * is inside the window, so the nearer handle is the top one
                 * and it must follow the pointer down. */
                const int    ty = bar.top() + bar.height() / 3;
                const QPoint grab =
                    fv->canvas_to_widget(QPointF(bar.center().x(),
                                                 bar.center().y()));
                const QPoint to =
                    fv->canvas_to_widget(QPointF(bar.center().x(), ty));

                fv->on_bar_range_ = [&](float lo, float hi) {
                    seen_lo = lo;
                    seen_hi = hi;
                    dyt_session_set_fixed_range(sess, lo, hi);
                };
                fv->on_bar_auto_ = [&]() {
                    dyt_session_set_range_mode(sess, DYT_RANGE_AUTO);
                };

                QMouseEvent press(QEvent::MouseButtonPress, QPointF(grab),
                                  QPointF(grab), Qt::LeftButton,
                                  Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(fv, &press);
                QMouseEvent move(QEvent::MouseMove, QPointF(to), QPointF(to),
                                 Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(fv, &move);
                QMouseEvent rel(QEvent::MouseButtonRelease, QPointF(to),
                                QPointF(to), Qt::LeftButton, Qt::NoButton,
                                Qt::NoModifier);
                QApplication::sendEvent(fv, &rel);

                dyt_snapshot_t s1{};
                dyt_session_snapshot(sess, &s1, nullptr, 0);
                drag_ok = s1.range_mode == DYT_RANGE_FIXED &&
                          seen_hi < s0.hi - 1e-3f &&
                          std::fabs(seen_lo - s0.lo) < 1e-3f &&
                          std::fabs(s1.hi - seen_hi) < 1e-2f;

                /* The handle moved down the bar — the geometry the painting
                 * follows, so this is what makes the drag visible at all. */
                moved_ok = fv->bar_handle_row(s1.hi) > bar.top() &&
                           fv->bar_handle_row(s1.hi) < bar.bottom() &&
                           fv->bar_handle_row(s1.lo) == bar.bottom();

                QMouseEvent dbl(QEvent::MouseButtonDblClick, QPointF(grab),
                                QPointF(grab), Qt::LeftButton, Qt::LeftButton,
                                Qt::NoModifier);
                QApplication::sendEvent(fv, &dbl);
                dyt_snapshot_t s2{};
                dyt_session_snapshot(sess, &s2, nullptr, 0);
                auto_ok = s2.range_mode == DYT_RANGE_AUTO;

                fv->on_bar_range_ = nullptr;
                fv->on_bar_auto_  = nullptr;
                dyt_session_reset_view(sess);
                pm.step();
            }

            const bool ok = geom_ok && paint_ok && drag_ok && moved_ok &&
                            auto_ok;
            std::printf("  %-4s the colour bar's range handles sit on the "
                        "frame's scale, drag the window and reset to auto "
                        "(geometry %s, painted %s, drag %s, moved %s, "
                        "auto %s)\n", ok ? "ok" : "FAIL",
                        geom_ok ? "yes" : "NO", paint_ok ? "yes" : "NO",
                        drag_ok ? "yes" : "NO", moved_ok ? "yes" : "NO",
                        auto_ok ? "yes" : "NO");
            if (!ok)
                fails++;
        }

        /* ---- 65. Rapid Diagnostics --------------------------------------
         *
         * The panel row and the 'F' key must both latch a fixed window that
         * brackets the frame's own extremes.  Driven through the window's key
         * dispatch, so the row's *wiring* — not only the arithmetic — is what
         * this pins; the vendor's rounding is view_model_test's job. */
        {
            bool row_ok = false, mode_ok = false, reframe_ok = false,
                 idem_ok = false, row_same_ok = false, contains_ok = false;
            if (view && fv) {
                send_char(27);              /* nothing armed: the ladder would
                                             * swallow 'F' otherwise */
                dyt_session_reset_view(sess);
                pm.step();

                dyt_snapshot_t s0{};
                dyt_session_snapshot(sess, &s0, nullptr, 0);

                QPushButton *rb = win.panel()
                                      ? win.panel()->button(ControlPanel::RapidDiag)
                                      : nullptr;
                row_ok = rb && rb->text() == QStringLiteral("Rapid Diagnostics") &&
                         rb->isEnabled();

                send_char('F');

                dyt_snapshot_t s1{};
                dyt_session_snapshot(sess, &s1, nullptr, 0);
                /* The vendor's own expression, written out here rather than
                 * called, so a change to dyt_vm_rapid_window() cannot pass by
                 * agreeing with itself. */
                const float e_lo =
                    (float)((int)((s0.stats.lo + 5.0f) / 10.0f) * 10 - 10);
                const float e_hi =
                    (float)((int)((s0.stats.hi + 5.0f) / 10.0f) * 10 + 10);
                mode_ok = s1.range_mode == DYT_RANGE_FIXED &&
                          std::fabs(s1.lo - e_lo) < 1e-2f &&
                          std::fabs(s1.hi - e_hi) < 1e-2f;
                contains_ok = s1.lo <= s0.stats.lo && s1.hi >= s0.stats.hi &&
                              (s1.hi - s1.lo) > (s0.stats.hi - s0.stats.lo);

                /* The source must be the *frame's* extremes, not whatever
                 * window is showing.  At AUTO the two coincide, so the check
                 * above cannot tell them apart.  Two presses can: the window
                 * the first one latched is wider than the frame, so a second
                 * press that read *that* back would derive a wider window
                 * again and walk outward.  Reading the frame, it must be a
                 * no-op.  (A narrow window is tried first, because that is the
                 * state a user actually leaves the range in.) */
                const float mid = (s0.stats.lo + s0.stats.hi) * 0.5f;
                dyt_session_set_fixed_range(sess, mid - 0.5f, mid + 0.5f);
                pm.step();
                send_char('F');
                dyt_snapshot_t s3{};
                dyt_session_snapshot(sess, &s3, nullptr, 0);
                reframe_ok = std::fabs(s3.lo - s1.lo) < 1e-2f &&
                             std::fabs(s3.hi - s1.hi) < 1e-2f;

                send_char('F');
                dyt_snapshot_t s4{};
                dyt_session_snapshot(sess, &s4, nullptr, 0);
                idem_ok = std::fabs(s4.lo - s1.lo) < 1e-2f &&
                          std::fabs(s4.hi - s1.hi) < 1e-2f;

                /* The row is not a second implementation: pressing it from
                 * AUTO must land on the same window the key did. */
                dyt_session_set_range_mode(sess, DYT_RANGE_AUTO);
                pm.step();
                if (rb)
                    rb->click();
                dyt_snapshot_t s2{};
                dyt_session_snapshot(sess, &s2, nullptr, 0);
                row_same_ok = s2.range_mode == DYT_RANGE_FIXED &&
                              std::fabs(s2.lo - s1.lo) < 1e-2f &&
                              std::fabs(s2.hi - s1.hi) < 1e-2f;

                dyt_session_reset_view(sess);
                pm.step();
            }

            const bool ok = row_ok && mode_ok && contains_ok && reframe_ok &&
                            idem_ok && row_same_ok;
            std::printf("  %-4s rapid diagnostics latches a fixed window over "
                        "the frame, by the key and by the row (row %s, mode "
                        "%s, contains %s, reframe %s, idempotent %s, "
                        "row-same %s)\n",
                        ok ? "ok" : "FAIL", row_ok ? "yes" : "NO",
                        mode_ok ? "yes" : "NO", contains_ok ? "yes" : "NO",
                        reframe_ok ? "yes" : "NO", idem_ok ? "yes" : "NO",
                        row_same_ok ? "yes" : "NO");
            if (!ok)
                fails++;
        }

        /* ---- 66. The circuit modes ---------------------------------------
         *
         * The three rows must set the range the vendor's handlers set, and
         * only Small Current Leakage may move the panel to the 3D tab — a 2D
         * mode that yanked the user off whatever tab they were reading would
         * be its own bug.  The checkmarks are this panel's state, so they are
         * asserted too: a row that applied the range but left the wrong one
         * lit is the "dead control" in a different dress. */
        {
            bool row_ok = false, def_ok = false, large_ok = false,
                 small_ok = false, short_ok = false;
            if (view && panel) {
                send_char(27);
                dyt_session_reset_view(sess);
                pm.step();

                QPushButton *bs = panel->button(ControlPanel::LeakShort);
                QPushButton *bl = panel->button(ControlPanel::LeakLarge);
                QPushButton *bm = panel->button(ControlPanel::LeakSmall);
                row_ok = bs && bl && bm &&
                         bs->text() == QStringLiteral("Short-circuit") &&
                         bl->text() == QStringLiteral("Large Current Leakage") &&
                         bm->text() == QStringLiteral("Small Current Leakage");

                /* The vendor's default is 3 (Short-circuit), which is also the
                 * port's plain full-range 2D view — so the start-up state must
                 * be that row lit and an auto range. */
                def_ok = bs && bs->isChecked() && !bl->isChecked() &&
                         !bm->isChecked() &&
                         panel->leak_mode() == DYT_LEAK_SHORT;

                /* Large: a fixed window, and the tab left alone. */
                panel->tabs()->setCurrentIndex(ControlPanel::TabComparison);
                send_char('L');
                dyt_snapshot_t s1{};
                dyt_session_snapshot(sess, &s1, nullptr, 0);
                large_ok = s1.range_mode == DYT_RANGE_FIXED && s1.hi > s1.lo &&
                           panel->tabs()->currentIndex() ==
                               ControlPanel::TabComparison &&
                           bl && bl->isChecked() && !bs->isChecked() &&
                           !bm->isChecked();

                /* Small: back to the full range, and now the 3D tab. */
                send_char('M');
                dyt_snapshot_t s2{};
                dyt_session_snapshot(sess, &s2, nullptr, 0);
                small_ok = s2.range_mode == DYT_RANGE_AUTO &&
                           panel->tabs()->currentIndex() ==
                               ControlPanel::TabAnalysis3D &&
                           bm && bm->isChecked() && !bs->isChecked() &&
                           !bl->isChecked();

                /* Short: the full range again, and *not* a tab move — it must
                 * leave the user on the 3D page they were just sent to. */
                send_char('S');
                dyt_snapshot_t s3{};
                dyt_session_snapshot(sess, &s3, nullptr, 0);
                short_ok = s3.range_mode == DYT_RANGE_AUTO &&
                           panel->tabs()->currentIndex() ==
                               ControlPanel::TabAnalysis3D &&
                           bs && bs->isChecked() && !bl->isChecked() &&
                           !bm->isChecked();

                panel->tabs()->setCurrentIndex(ControlPanel::TabTroubleshoot);
                dyt_session_reset_view(sess);
                pm.step();
            }

            const bool ok = row_ok && def_ok && large_ok && small_ok &&
                            short_ok;
            std::printf("  %-4s the circuit modes set the range and the view "
                        "(rows %s, default %s, large %s, small %s, short "
                        "%s)\n", ok ? "ok" : "FAIL", row_ok ? "yes" : "NO",
                        def_ok ? "yes" : "NO", large_ok ? "yes" : "NO",
                        small_ok ? "yes" : "NO", short_ok ? "yes" : "NO");
            if (!ok)
                fails++;
        }

        /* ---- 67. The 3D page's height modes ------------------------------
         *
         * The two modes differ only in the range the mesh's height is
         * normalised across, so they are indistinguishable until the window is
         * *narrower* than the frame — which is the state this sets up.  A
         * narrow window makes the shape mode clamp most of the scene into
         * plateaus at 0 and 1; the colour mode, normalising across the frame's
         * own extremes, clamps almost nothing.  Counting the clamped vertices
         * is what separates them. */
        {
            ControlPanel *panel = win.panel();
            SurfaceView  *surf  = panel ? panel->surface() : nullptr;

            bool rows_ok = false, key_ok = false, shape_ok = false,
                 color_ok = false, differ_ok = false, inrange_ok = false;

            /* How many vertices sit at exactly 0 or 1 — the plateaus.  The
             * heights are also required to *be* in [0, 1]: the vendor clamps
             * its height index (:2636), and an unclamped one would put the
             * mesh far off the viewport rather than plateauing. */
            auto clamped_frac = [&](bool *in_range) -> double {
                *in_range = true;
                if (!surf)
                    return -1.0;
                const SurfaceModel &m = surf->model();
                long n = 0, c = 0;
                for (size_t k = 0; k < m.verts.size(); k++) {
                    if (!std::isfinite(m.verts[k].z))
                        continue;
                    n++;
                    const float h = m.pos[k * 3 + 2];
                    if (!(h >= 0.0f && h <= 1.0f))
                        *in_range = false;
                    if (h <= 0.0f || h >= 1.0f)
                        c++;
                }
                return n ? (double)c / (double)n : -1.0;
            };

            if (surf && panel) {
                send_char(27);
                panel->tabs()->setCurrentIndex(ControlPanel::TabAnalysis3D);
                dyt_session_reset_view(sess);
                pm.step();

                QPushButton *ps = panel->button(ControlPanel::SurfShape);
                QPushButton *pc = panel->button(ControlPanel::SurfColor);
                rows_ok = ps && pc &&
                          ps->text() == QStringLiteral("Morphological Change") &&
                          pc->text() == QStringLiteral("Color Changes") &&
                          surf->mode() == DYT_SURFACE_SHAPE &&
                          ps->isChecked() && !pc->isChecked();

                dyt_snapshot_t s{};
                dyt_session_snapshot(sess, &s, nullptr, 0);
                const float span = s.stats.hi - s.stats.lo;
                const float mid  = (s.stats.hi + s.stats.lo) * 0.5f;

                if (span > 0.2f) {
                    /* A fifth of the frame's span: most of the scene falls
                     * outside it. */
                    dyt_session_set_fixed_range(sess, mid - span * 0.1f,
                                                mid + span * 0.1f);
                    pm.step();

                    send_char('P');
                    pm.step();
                    dyt_snapshot_t sp{};
                    dyt_session_snapshot(sess, &sp, nullptr, 0);
                    bool shape_in = false;
                    const double shape_frac = clamped_frac(&shape_in);

                    send_char('C');
                    pm.step();
                    dyt_snapshot_t sc{};
                    dyt_session_snapshot(sess, &sc, nullptr, 0);
                    bool color_in = false;
                    const double color_frac = clamped_frac(&color_in);

                    /* The keys must not have been folded into the tools they
                     * shadow: 'P' is not the point tool and 'C' is not chart
                     * analysis, which would have picked up the line tool. */
                    key_ok = surf->mode() == DYT_SURFACE_COLOR &&
                             pc && pc->isChecked() && ps && !ps->isChecked() &&
                             sp.tool != DYT_TOOL_POINT &&
                             sc.tool != DYT_TOOL_LINE;

                    shape_ok = shape_frac > 0.25;
                    color_ok = color_frac >= 0.0 && color_frac < 0.10;
                    differ_ok = shape_frac > color_frac + 0.20;
                    inrange_ok = shape_in && color_in;
                }

                panel->set_surface_mode(DYT_SURFACE_SHAPE);
                dyt_session_reset_view(sess);
                panel->tabs()->setCurrentIndex(ControlPanel::TabTroubleshoot);
                pm.step();
            }

            const bool ok = rows_ok && key_ok && shape_ok && color_ok &&
                            differ_ok && inrange_ok;
            std::printf("  %-4s the 3D height modes scale the mesh to the "
                        "window or to the frame (rows %s, keys %s, shape "
                        "clamps %s, colour clamps %s, differ %s, in range "
                        "%s)\n",
                        ok ? "ok" : "FAIL", rows_ok ? "yes" : "NO",
                        key_ok ? "yes" : "NO", shape_ok ? "yes" : "NO",
                        color_ok ? "yes" : "NO", differ_ok ? "yes" : "NO",
                        inrange_ok ? "yes" : "NO");
            if (!ok)
                fails++;
        }

        /* ---- 68. The Mark Text annotation ---------------------------------
         *
         * A Mark is the front end's own — there is no engine call behind a
         * label — so what is pinned is the whole path: 'T' arms the tool, a
         * click asks for the text and lands the label on the source pixel the
         * click names, the chip is painted at that pixel's projection, and the
         * tool puts itself away.  The text comes from on_mark_text_ (a dialog
         * in the real window), stubbed here for the same reason on_about_ is.
         *
         * 'T' is a shift form of 't', so the range must not have moved: a fold
         * that reached the view keys would have flipped the range instead of
         * arming the tool.  And the chip is asserted by *difference* against
         * the same canvas with no mark, so "it painted something" cannot be
         * satisfied by an overlay that was already there. */
        {
            bool rows_ok = false, key_ok = false, place_ok = false,
                 pixel_ok = false, disarm_ok = false, off_ok = false,
                 cancel_ok = false, one_tool_ok = false;

            if (view && fv && panel) {
                send_char(27);
                panel->tabs()->setCurrentIndex(ControlPanel::TabTroubleshoot);
                send_char('n');            /* a clean tool state to start from */
                dyt_session_reset_view(sess);
                pm.step();

                view->clear_marks();

                QPushButton *bt = panel->button(ControlPanel::MarkTextRow);
                rows_ok = bt && bt->text() == QStringLiteral("Text") &&
                          bt->isEnabled();

                /* The source pixel the click below names, read through the
                 * mapping the placement itself uses. */
                const QSize is = view->imageSize();
                const int   lx = 100, ly = 120;
                dyt_snapshot_t s0{};
                dyt_session_snapshot(sess, &s0, nullptr, 0);
                int ex = -1, ey = -1;
                const bool mapped =
                    dyt_view_transform_map(&s0.xform, s0.width, s0.height,
                                           is.width(), is.height(),
                                           lx, ly, &ex, &ey) == 0;

                /* The picture before the mark, so "was the chip painted?" is a
                 * difference rather than a guess about colours. */
                const QImage before = view->render_canvas();

                int prompts = 0;
                view->on_mark_text_ = [&](const QPointF &) {
                    prompts++;
                    return QStringLiteral("T1");
                };

                dyt_snapshot_t sb{};
                dyt_session_snapshot(sess, &sb, nullptr, 0);
                send_char('T');
                dyt_snapshot_t sa{};
                dyt_session_snapshot(sess, &sa, nullptr, 0);
                /* The row follows the tool, and the Tool None row goes out
                 * while it is armed: a Mark is a tool, so lighting both would
                 * claim two at once.  The arm came from the keyboard, so a
                 * checkmark that only tracked the *click* would stay dark. */
                QPushButton *bn = panel->button(ControlPanel::ToolNone);
                key_ok = view->mark_mode() == MarkText && mapped &&
                         sa.range_mode == sb.range_mode &&
                         bt && bt->isChecked() && bn && !bn->isChecked();

                send_mouse(QEvent::MouseButtonPress, Qt::LeftButton,
                           Qt::LeftButton, lx, ly);

                place_ok = view->mark_count() == 1 && prompts == 1 && mapped &&
                           (int)std::lround(view->mark_at(0).x) == ex &&
                           (int)std::lround(view->mark_at(0).y) == ey &&
                           view->mark_at(0).text == QStringLiteral("T1");
                /* One label per click, as the vendor's `cursor_code = None`
                 * after a placement does. */
                disarm_ok = view->mark_mode() == MarkNone;

                /* The chip is on the canvas at the pixel's own projection: the
                 * dot beside the plate is the mark colour, and none of it was
                 * there before.  A 9x9 window around the anchor, because the
                 * dot is a 2px-radius ellipse. */
                const QImage after = view->render_canvas();
                auto yellow_near = [](const QImage &im, int cx, int cy) {
                    int n = 0;
                    for (int y = cy - 4; y <= cy + 4; y++)
                        for (int x = cx - 4; x <= cx + 4; x++) {
                            if (x < 0 || y < 0 ||
                                x >= im.width() || y >= im.height())
                                continue;
                            const QRgb c = im.pixel(x, y);
                            if (qRed(c) == 255 && qGreen(c) == 255 &&
                                qBlue(c) == 0)
                                n++;
                        }
                    return n;
                };
                int ox = 0, oy = 0;
                const bool proj_ok =
                    dyt_view_transform_project(&sa.xform, sa.width, sa.height,
                                               is.width(), is.height(),
                                               ex, ey, &ox, &oy) == 0;
                const int ax = kPad + ox, ay = kPad + oy;
                pixel_ok = proj_ok &&
                           yellow_near(before, ax, ay) == 0 &&
                           yellow_near(after,  ax, ay) >= 6;

                /* Off the picture: the tool is put away and nothing placed,
                 * rather than a label stored where project() could never draw
                 * it.  The prompt must not even be asked. */
                send_char('T');
                send_mouse(QEvent::MouseButtonPress, Qt::LeftButton,
                           Qt::LeftButton, 100, -8);
                off_ok = view->mark_mode() == MarkNone &&
                         view->mark_count() == 1 && prompts == 1;

                /* A cancelled prompt refuses the click and leaves the tool
                 * armed, so the next click can try again. */
                view->on_mark_text_ = [&](const QPointF &) {
                    prompts++;
                    return QString();
                };
                send_char('T');
                send_mouse(QEvent::MouseButtonPress, Qt::LeftButton,
                           Qt::LeftButton, lx, ly);
                cancel_ok = view->mark_count() == 1 && prompts == 2 &&
                            view->mark_mode() == MarkText;

                /* One tool at a time, both ways: the measurement tool and the
                 * Mark tool are the vendor's single `cursor_code`, so arming
                 * one puts the other away. */
                view->set_mark_mode(MarkNone);
                send_char('p');
                pm.step();
                dyt_snapshot_t s_p{};
                dyt_session_snapshot(sess, &s_p, nullptr, 0);
                const bool had_spot = view->mark_mode() == MarkNone &&
                                      s_p.tool == DYT_TOOL_POINT;

                send_char('T');
                pm.step();
                dyt_snapshot_t s_m{};
                dyt_session_snapshot(sess, &s_m, nullptr, 0);
                const bool cleared_mark_tool =
                    view->mark_mode() == MarkText && s_m.tool == DYT_TOOL_NONE;

                send_char('p');
                pm.step();
                const bool cleared_mark = view->mark_mode() == MarkNone;

                one_tool_ok = had_spot && cleared_mark_tool && cleared_mark;

                view->clear_marks();
                view->on_mark_text_ = nullptr;
                view->set_mark_mode(MarkNone);
                send_char('n');
                dyt_session_reset_view(sess);
                pm.step();
            }

            const bool ok = rows_ok && key_ok && place_ok && pixel_ok &&
                            disarm_ok && off_ok && cancel_ok && one_tool_ok;
            std::printf("  %-4s the Mark Text tool places a label at the "
                        "clicked pixel and paints it there (row %s, key %s, "
                        "place %s, painted %s, disarms %s, off-image %s, "
                        "cancel %s, one tool %s)\n",
                        ok ? "ok" : "FAIL", rows_ok ? "yes" : "NO",
                        key_ok ? "yes" : "NO", place_ok ? "yes" : "NO",
                        pixel_ok ? "yes" : "NO", disarm_ok ? "yes" : "NO",
                        off_ok ? "yes" : "NO", cancel_ok ? "yes" : "NO",
                        one_tool_ok ? "yes" : "NO");
            if (!ok)
                fails++;
        }

        /* A 256x192 layout, one opaque stripe and the rest transparent — the
         * shape that makes "did it composite?" a pixel question rather than a
         * colour one, so it cannot be confused with the picture underneath. */
        QImage layout(256, 192, QImage::Format_ARGB32);
        layout.fill(Qt::transparent);
        {
            QPainter lp(&layout);
            lp.fillRect(QRect(0, 0, 8, 192), QColor(0, 255, 0));
        }

        bool hold_ok = false, paint_ok = false, move_ok = false, rot_ok = false;
        if (view) {
            /* Establish the framing this probe assumes: the layout is given
             * the view's rotation and mirrors, so the stripe's position is
             * only predictable with the view unrotated and unmirrored.  Reset
             * and re-render rather than trusting what earlier assertions left
             * — the same rule 53g follows in the other direction. */
            dyt_session_reset_view(sess);
            pm.step();

            /* Hide the overlays that draw over the picture, so the probe is
             * about the layout and not about a marker that happens to sit on
             * it.  Restored before returning. */
            const bool had_hot = view->hot_shown();
            const bool had_info = fv->info_shown();
            if (had_hot)
                view->toggle_hot();
            if (had_info)
                fv->toggle_info();
            fv->measure_key('n');       /* no tool outline over the probe */

            view->clear_layout();
            view->set_layout_opacity(100);
            view->set_layout_shown(false);
            const QImage base = view->render_canvas();

            view->set_layout(layout);
            view->set_layout_shown(true);
            hold_ok = view->has_layout() && view->layoutSize() == QSize(256, 192);

            const QImage with = view->render_canvas();

            /* The stripe is scaled to the drawn picture, so probe in drawn
             * pixels: 8/256 = 1/32 of the picture's width, at its mid height. */
            const int iw = view->imageSize().width();
            const int ih = view->imageSize().height();
            const int py = kPad + ih / 2;
            const int px = kPad + iw / 64;      /* inside the stripe */
            const int ox = kPad + iw / 4;       /* clear of it */

            paint_ok = iw > 0 && ih > 0 &&
                       with.pixel(px, py) != base.pixel(px, py) &&
                       with.pixel(ox, py) == base.pixel(ox, py);

            /* Moving the layout +20 px in x must move the covered pixel by
             * exactly that: the old probe is clear again, and the probe 20 px
             * to its right is now covered. */
            view->set_layout_align(20, 0);
            const QImage moved = view->render_canvas();
            move_ok = moved.pixel(px, py) == base.pixel(px, py) &&
                      moved.pixel(px + 20, py) != base.pixel(px + 20, py) &&
                      view->layout_dx() == 20;

            /* The canvas clamps too, not only the spin. */
            view->set_layout_align(1000, -1000);
            move_ok = move_ok && view->layout_dx() ==  DYT_FUSION_ALIGN_MAX &&
                      view->layout_dy() == -DYT_FUSION_ALIGN_MAX;
            view->set_layout_align(0, 0);

            /* The layout follows the view's rotation.  A quarter turn turns
             * the vertical stripe into a horizontal band along the top, so the
             * probe moves from the left edge to the top edge — the check that
             * the overlay is not pinned to the unrotated picture while the
             * thermal content turns under it.  Rendered against the same
             * rotated frame with no layout, so the comparison is the layout's
             * contribution and not the rotation's. */
            dyt_session_rotate(sess, DYT_ROT_90);
            pm.step();
            view->set_layout_shown(false);
            const QImage rbase = view->render_canvas();
            view->set_layout_shown(true);
            const QImage rwith = view->render_canvas();
            const int rw = view->imageSize().width();
            const int rh = view->imageSize().height();
            rot_ok = rw > 0 && rh > 0 &&
                     rwith.pixel(kPad + rw / 2, kPad + rh / 64) !=
                         rbase.pixel(kPad + rw / 2, kPad + rh / 64);

            /* Leave the view and the canvas as the rest of the run found them. */
            dyt_session_reset_view(sess);
            pm.step();
            view->clear_layout();
            if (had_hot)
                view->toggle_hot();
            if (had_info)
                fv->toggle_info();
        }

        const bool ok = tab_ok && clamp_ok && wire_ok && hold_ok &&
                        paint_ok && move_ok && rot_ok;
        std::printf("  %-4s the Circuit Design tab overlays a layout and "
                    "aligns it (tab %s, clamp %s, push %s, hold %s, paint %s, "
                    "move %s, rot %s)\n",
                    ok ? "ok" : "FAIL", tab_ok ? "yes" : "NO",
                    clamp_ok ? "yes" : "NO", wire_ok ? "yes" : "NO",
                    hold_ok ? "yes" : "NO", paint_ok ? "yes" : "NO",
                    move_ok ? "yes" : "NO", rot_ok ? "yes" : "NO");
        if (!ok)
            fails++;
    }

    /* 62. The camera's serial number on the status line.  The Windows app's
     * status bar carries the connection word and "Camera SN: <serial>"; the
     * port puts the same pair on its third line.  Three risks: the segment
     * appears when there is no device (a blank field read as a serial of
     * spaces), it is missing when a device did report one, and the status line
     * and the info panel disagree about which camera is attached — which a
     * second copy of the identity would allow, and is why the strip reads
     * FrameView's dyt_device_info_t rather than a session field. */
    {
        dyt_snapshot_t s{};
        const bool have = dyt_session_snapshot(sess, &s, nullptr, 0) == 0;

        /* With no device identity the segment is omitted, not shown empty. */
        fv->clear_info();
        win.set_state_line(DevState::Live, s, 25.0);
        const QString without = win.strip() ? win.strip()->line(2) : QString();

        dyt_device_info_t d{};
        d.have_sn = 1;
        std::snprintf(d.sn_str, sizeof d.sn_str, "%s", "CA09DDC00212");
        d.sn_len = (int)std::strlen(d.sn_str);
        fv->set_info(d, true);
        win.set_state_line(DevState::Live, s, 25.0);
        const QString with = win.strip() ? win.strip()->line(2) : QString();

        /* The info panel's serial row carries the same value, because it is
         * the same struct — not a second field that could drift. */
        const QString info = fv->info_line(0);

        const bool absent_ok = !without.contains(QStringLiteral("Camera SN"));
        const bool shown_ok =
            with.contains(QStringLiteral("Camera SN: CA09DDC00212"));
        const bool agree_ok  = info.contains(QStringLiteral("CA09DDC00212"));
        const bool ok = have && absent_ok && shown_ok && agree_ok;

        std::printf("  %-4s the status line names the camera's serial "
                    "(absent %s, shown %s, agrees %s)\n",
                    ok ? "ok" : "FAIL", absent_ok ? "yes" : "NO",
                    shown_ok ? "yes" : "NO", agree_ok ? "yes" : "NO");
        if (!ok)
            fails++;
    }

    /* 63. The connection dot.  The Windows app's status bar carries a green dot
     * before its "Camera connected"; the port's third line carries the same dot
     * with the states it actually has.  Two risks: the colour does not
     * distinguish the states (so the dot is decoration), and the dot is never
     * actually painted — a colour function that nothing calls would pass a
     * colour-only check.  So each state's colour is asserted *and* the dot is
     * found in a grab of the strip.  The fixture case is the one that matters
     * most: green there would claim a camera that is not attached, which is the
     * lie the state word already refuses to tell. */
    {
        StatusStrip *strip = win.strip();
        struct Case { DevState st; QColor want; } cases[] = {
            { DevState::Live,       QColor(0x2e, 0xcc, 0x40) },
            { DevState::Stalled,    QColor(0xe0, 0xa0, 0x20) },
            { DevState::Connecting, QColor(0xe0, 0xa0, 0x20) },
            { DevState::WarmingUp,  QColor(0xe0, 0xa0, 0x20) },
            { DevState::NoDevice,   QColor(0xd0, 0x30, 0x30) },
            { DevState::Fixture,    QColor(0x70, 0x78, 0x88) },
        };

        bool colour_ok = strip && strip->conn_dot_shown();
        bool painted_ok = strip != nullptr;
        const int cy = kStripPad + 2 * kLineH + kLineH / 2;
        for (const Case &c : cases) {
            if (!strip)
                break;
            strip->set_conn(c.st);
            colour_ok = colour_ok && strip->conn_dot_colour() == c.want;
            const QImage im =
                strip->grab().toImage().convertToFormat(QImage::Format_RGB32);
            painted_ok = painted_ok && cy < im.height() &&
                         im.pixel(kStripPad + kDotW / 2, cy) == c.want.rgb();
        }
        /* Leave the strip on the state the run is actually in. */
        if (strip)
            strip->set_conn(DevState::Fixture);

        const bool ok = colour_ok && painted_ok;
        std::printf("  %-4s the connection dot names the state in colour "
                    "(colour %s, painted %s)\n", ok ? "ok" : "FAIL",
                    colour_ok ? "yes" : "NO", painted_ok ? "yes" : "NO");
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

/* The About box's text.  Kept here rather than in a resource so it can be
 * built from the same feature macros the build actually used — an About box
 * that claims OpenCV in a build without it is worse than none.
 *
 * `have_model` is the one thing the feature macros cannot answer: "built with
 * MNN" says a runtime is linked, not that a model was found and loaded.  The
 * two are separate failures and the SR keys behave differently under each, so
 * the box says which one it is. */
/* ---------------------------------------------------------- the help guide
 *
 * The guide: what the app is, how to start, then the shared key list under its
 * headings.  Reached from Help -> Keyboard shortcuts and the toolbar.  About
 * shows the bare list; both render kKeyLines, so the two cannot disagree. */
static std::string help_text()
{
    std::string s;

    s += std::string(kAppName) + " - how to use it\n\n";
    s += "This window shows the picture from a DYT / Mechanic-Ti USB thermal\n";
    s += "camera.  With no camera attached it replays a saved raw frame instead,\n";
    s += "so the app is still useful for looking at a recording.  Every pixel and\n";
    s += "every string comes from libdyt, the same engine the command-line viewer\n";
    s += "(tools/dytview) draws with, so the two always agree.\n\n";

    s += "getting started\n";
    s += "  * Drag on the picture to place the selected tool (point, line or box)\n";
    s += "    and read a temperature.  The polygon is placed a click at a time\n";
    s += "    instead: each click adds a vertex, Enter or the right button\n";
    s += "    finishes the outline, and Backspace takes a vertex back.  The\n";
    s += "    reading appears in the strip below the picture, beside the frame's\n";
    s += "    hottest and coldest pixels (marked H and L on the picture).\n";
    s += "  * Everything the keys do is also on the control panel down the right,\n";
    s += "    so a key you have forgotten can be found there.  The two cannot\n";
    s += "    disagree: a control runs exactly what its key runs.\n";
    s += "  * 's' saves a still and 'v' records a clip, into the capture directory\n";
    s += "    (the current directory by default); 'g' browses what has been saved.\n";
    s += "  * F11 is full screen.  The picture scales up to fill the screen, keeping\n";
    s += "    its shape, with the leftover margin in black; F11 again restores the\n";
    s += "    window.  A window enlarged by hand scales the same way.\n";
    s += "  * 'd' shows the device's own identity and its stored parameters; the\n";
    s += "    panel is drawn over the picture.\n\n";

    s += "keys\n";
    const char *group = nullptr;
    for (const key_line_t &k : kKeyLines) {
        if (!group || std::strcmp(group, k.group) != 0) {
            group = k.group;
            s += "  ";
            s += group;
            s += "\n";
        }
        s += k.line;
    }

    s += "\nthe runtime parameters (e A R D) arm a value that 'y' then sends and\n";
    s += "Esc cancels.  While one is armed it takes the keyboard, so a stray key\n";
    s += "cannot slip past a pending write.\n";
    return s;
}

static std::string about_text(bool have_model)
{
    std::string s;

    s += std::string(kAppName) + " " + kAppVersion + "\n\n";
    s += "A Linux port of the Mechanic-Ti / DYT USB thermal camera viewer.\n";
    s += "Every pixel and every string comes from libdyt, the same engine\n";
    s += "the OpenCV viewer (tools/dytview) draws with.\n\n";
    s += "Qt " QT_VERSION_STR " (Widgets)\n";
    s += "built with:";
#ifdef DYT_HAVE_LIBUSB
    s += " libusb";
#endif
#ifdef DYT_HAVE_OPENCV
    s += " OpenCV";
#endif
#ifdef DYT_HAVE_MNN
    s += " MNN";
#endif
    s += "\nsuper-resolution: ";
    s += have_model ? "a model is loaded (2x)\n" : "no model loaded\n";
    s += "\nkeys\n";
    s += key_list_text();
    s += "\nHelp -> Keyboard shortcuts has the full guide.\n";
    return s;
}

static int run_gui(const opts &o_in, QApplication &app)
{
    /* The dark shell, applied to the real run only — never to --selftest.  The
     * stylesheet changes widget metrics (padding, borders), and the selftest's
     * geometry assertions (fit-to-view, toolbar overflow) were calibrated
     * against the default look; restyling the test would move its goalposts.
     * The theme is presentation, so the test deliberately measures without it. */
    app.setStyleSheet(QString::fromUtf8(kDarkQss));

    /* A saved preference fills in only what the command line did not name, so
     * an explicit flag always wins. */
    opts o = o_in;
    bool dir_from_prefs = false;
    if (!o.no_prefs) {
        prefs p;
        prefs_load(prefs_path(o_in), p);
        dir_from_prefs = !o.capture_dir_set && !p.capture_dir.empty();
        opts_apply_prefs(o, p);
    }

    /* Where captures go when neither the flag nor a saved preference said.
     * Not "." — a menu launch has no meaningful working directory, so the
     * gallery would scan somewhere the user never chose. */
    if (!o.capture_dir_set && !dir_from_prefs) {
        char dir[4096];
        if (dyt_vm_default_capture_dir(dir, sizeof dir) == 0)
            o.capture_dir = dir;
    }

    dyt_session_t *sess = setup_session(o);
    if (!sess)
        return 1;
    setup_super_resolution(sess, o);

    MainWindow win;
    win.set_session(sess);
    pump       pm;
    pm.sess = sess;
    pm.win  = &win;
    pm.live = o.live;
    win.set_playback(&pm.playback);    /* the window steers the pump's reader */

    /* The Comparison tab's "Load reference" button.  Opens a still container
     * through the same pipeline a gallery still uses, but pulls the Celsius
     * grid (dyt_frame_source_temps) rather than the RGB — the live frame's
     * grid is what the stats run against.  Held in the pump because the
     * pump's step() is what recomputes the stats each frame. */
    win.panel()->on_load_reference_ = [&]() {
        const QString qpath = QFileDialog::getOpenFileName(
            &win, QStringLiteral("Load reference still"),
            QString::fromStdString(o.capture_dir),
            QStringLiteral("DYT stills (*.dyt.jpg);;All files (*)"));
        if (qpath.isEmpty())
            return;
        const std::string path = qpath.toStdString();

        dyt_session_t      *gs = setup_session(o);
        dyt_frame_source_t *sf = gs ? dyt_frame_source_open_still(
            gs, path.c_str(), o.width, DYT_MODE_1000, DYT_PLANE_BOTTOM_HALF,
            o.cap.t_amb, o.cap.sensor_mode, o.cap.fix_mode) : nullptr;
        const uint8_t      *px = nullptr;
        int                 w = 0, h = 0;
        if (sf && dyt_frame_source_next(sf, &px, &w, &h) == DYT_FS_FRAME && px) {
            std::vector<float> temps((size_t)w * (size_t)h);
            if (dyt_frame_source_temps(sf, temps.data(), w * h) == 0) {
                pm.ref_path  = path;
                pm.ref_temps = std::move(temps);
                pm.ref_w      = w;
                pm.ref_h      = h;
                pm.notice("reference loaded", 120);
            } else {
                pm.notice("reference has no thermal plane", 200);
            }
        } else {
            pm.notice("cannot open reference", 200);
        }
        dyt_frame_source_close(sf);
        dyt_session_free(gs);
    };
    win.panel()->on_clear_reference_ = [&]() {
        pm.ref_path.clear();
        pm.ref_temps.clear();
        pm.ref_w = pm.ref_h = 0;
        pm.notice("reference cleared", 120);
    };
    win.panel()->on_threshold_changed_ = [&]() {
        /* The threshold is read live from the spin box in sync_compare_panel,
         * so a change takes effect on the next frame — nothing to do here but
         * let that happen.  The hook exists so the panel can grow a side
         * effect later without a second wiring. */
    };

    /* The Circuit Design tab.  The overlay lives on the canvas, so the window
     * is what loads the image and what pushes the alignment; the panel only
     * holds the controls.  Loading turns the show toggle on, so an image that
     * loaded while the toggle was off cannot look like a failed load. */
    win.panel()->on_load_layout_ = [&]() {
        const QString qpath = QFileDialog::getOpenFileName(
            &win, QStringLiteral("Load circuit layout"),
            QString::fromStdString(o.capture_dir),
            QStringLiteral("Images (*.png *.jpg *.jpeg *.bmp *.gif);;"
                           "All files (*)"));
        if (qpath.isEmpty())
            return;
        const QImage img(qpath);
        if (img.isNull()) {
            pm.notice("cannot read layout image", 200);
            return;
        }
        win.view()->set_layout(img);
        win.panel()->set_layout_shown(true);
        win.panel()->sync_layout(qpath);
        push_layout(win.panel(), win.view());
        pm.notice("layout loaded", 120);
    };
    win.panel()->on_clear_layout_ = [&]() {
        win.view()->clear_layout();
        win.panel()->sync_layout(QString());
        pm.notice("layout cleared", 120);
    };
    win.panel()->on_layout_changed_ = [&]() {
        push_layout(win.panel(), win.view());
    };
    /* The controls start where the page built them (centred, opaque, shown),
     * so the canvas and the page agree before any click. */
    push_layout(win.panel(), win.view());

    /* The High TEMP. Alarm threshold.  The field is the panel's; the canvas is
     * what arms at it, so every change is pushed — including while the alarm
     * is up, which is what makes the threshold editable live. */
    win.panel()->on_alarm_setpoint_changed_ = [&]() {
        push_alarm(win.panel(), win.view());
    };
    push_alarm(win.panel(), win.view());

    /* The colour bar's range handles.  The bar reports; the window owns the
     * session, so this is where a drag becomes a fixed range — and a
     * double-click hands the range back to the frame, the same thing 't' does
     * from the keyboard. */
    win.view()->on_bar_range_ = [sess](float lo, float hi) {
        dyt_session_set_fixed_range(sess, lo, hi);
    };
    win.view()->on_bar_auto_ = [sess]() {
        dyt_session_set_range_mode(sess, DYT_RANGE_AUTO);
    };

    /* An explicit --layout-image loads at start-up, the way the button does,
     * so a scripted run can pin the overlay without a click.  A path that
     * cannot be read is reported and skipped rather than fatal: the layout is
     * an overlay, not the app's reason to run. */
    if (!o.layout_image.empty()) {
        const QImage img(QString::fromStdString(o.layout_image));
        if (img.isNull()) {
            std::fprintf(stderr, "dytqt: --layout-image %s is not readable\n",
                         o.layout_image.c_str());
        } else {
            win.view()->set_layout(img);
            win.panel()->set_layout_shown(true);
            win.panel()->sync_layout(QString::fromStdString(o.layout_image));
            push_layout(win.panel(), win.view());
        }
    }

    /* An explicit --reference loads the still at start-up, the same way the
     * button does, so a scripted run can pin the comparison without a click. */
    if (!o.reference.empty()) {
        dyt_session_t      *gs = setup_session(o);
        dyt_frame_source_t *sf = gs ? dyt_frame_source_open_still(
            gs, o.reference.c_str(), o.width, DYT_MODE_1000,
            DYT_PLANE_BOTTOM_HALF, o.cap.t_amb, o.cap.sensor_mode,
            o.cap.fix_mode) : nullptr;
        const uint8_t      *px = nullptr;
        int                 w = 0, h = 0;
        if (sf && dyt_frame_source_next(sf, &px, &w, &h) == DYT_FS_FRAME && px) {
            std::vector<float> temps((size_t)w * (size_t)h);
            if (dyt_frame_source_temps(sf, temps.data(), w * h) == 0) {
                pm.ref_path  = o.reference;
                pm.ref_temps = std::move(temps);
                pm.ref_w      = w;
                pm.ref_h      = h;
            }
        }
        dyt_frame_source_close(sf);
        dyt_session_free(gs);
    }
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

    /* A view key's own notice (super-resolution), shown the same way a
     * parameter write's outcome is.  Held longer than a param result because
     * it usually names something the user has to change. */
    win.view()->on_notice_ = [&](const std::string &s) { pm.notice(s, 200); };

    /* Capture.  `s` writes a still straight away — it is a few milliseconds of
     * work and has nothing to keep between keypresses — while `v` toggles the
     * one clip, which the pump then feeds a frame per tick. */
    pm.capture.dir = o.capture_dir;

    win.on_still_ = [&]() {
        std::string m;
        save_still(sess, pm.capture.dir, m);
        pm.notice(m, 150);
    };

    /* The Mark tool's text prompt.  A dialog rather than a fixed string,
     * because the whole point of the tool is the user's own note.  Cancelling
     * returns empty, which add_mark() refuses — so a cancelled prompt leaves
     * the tool armed and nothing placed, and the next click can try again. */
    win.view()->on_mark_text_ = [&](const QPointF &) -> QString {
        bool ok = false;
        const QString t = QInputDialog::getText(
            &win, QStringLiteral("Mark"), QStringLiteral("Label:"),
            QLineEdit::Normal, QString(), &ok);
        return ok ? t.trimmed() : QString();
    };

    win.on_record_ = [&]() {
        std::string why;

        if (pm.capture.recording) {
            const std::string     path = pm.capture.path;
            const long long       n    = pm.capture.stop();
            const char           *base = strrchr(path.c_str(), '/');
            char                  b[192];
            base = base ? base + 1 : path.c_str();
            snprintf(b, sizeof b, "clip stopped: %s (%lld frame%s)", base, n,
                     n == 1 ? "" : "s");
            pm.notice(b, 150);
        } else if (pm.capture.start((double)o.fps, why)) {
            pm.notice(why, 90);           /* "recording <path>" */
        } else {
            pm.notice(why, 200);          /* the refusal, held longer */
        }
    };

    /* The gallery.  It scans the capture directory — the same place 's' and
     * 'v' write, so the gallery shows what this session and earlier ones
     * saved.  Opening an entry renders it through a throwaway session, so the
     * live stream (if any) is untouched: the window shows the still as an
     * override while the pump keeps running underneath. */
    win.set_folder(QString::fromStdString(o.capture_dir));
    win.on_gallery_refresh_ = [&]() {
        dyt_vm_gallery_load(win.gallery(), o.capture_dir.c_str());
    };

    /* Choosing the folder.  One directory serves both browsing and saving, so
     * what was saved is what the list shows; the choice rides out at exit
     * through prefs_from_session(), which already persists the capture dir. */
    win.on_choose_folder_ = [&]() {
        const QString dir = QFileDialog::getExistingDirectory(
            &win, QStringLiteral("Folder for stills and clips"),
            QString::fromStdString(o.capture_dir),
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (dir.isEmpty())
            return;                             /* cancelled */
        o.capture_dir     = dir.toStdString();
        o.capture_dir_set = true;               /* so the default cannot override it */
        pm.capture.dir    = o.capture_dir;
        win.set_folder(dir);
        win.rescan_gallery();
        pm.notice("gallery folder: " + o.capture_dir, 150);
    };

    win.on_about_ = [&]() {
        /* Read the model state now rather than remembering it from start-up:
         * a failed upscale withdraws the capability (session.c), and the box
         * must not keep claiming a model that is no longer there. */
        dyt_snapshot_t snap{};
        const bool have_model =
            dyt_session_snapshot(sess, &snap, nullptr, 0) == 0 && snap.sr_cap;
        QMessageBox::about(&win, QString("About ") + kAppName,
                           QString::fromStdString(about_text(have_model)));
    };

    /* The usage guide.  A dialog rather than a message box, because the text is
     * long enough to need scrolling and a user copying a key out of it should
     * be able to select it — neither of which a QMessageBox's label allows.
     * The text itself is help_text(), which renders the same key table the
     * About box does, so the two cannot disagree about what a key does. */
    win.on_help_ = [&]() {
        QDialog dlg(&win);
        dlg.setWindowTitle(QString("Keyboard shortcuts - ") + kAppName);
        dlg.resize(680, 560);

        auto *box = new QTextBrowser(&dlg);
        box->setPlainText(QString::fromStdString(help_text()));
        box->setReadOnly(true);

        auto *close = new QPushButton(QStringLiteral("Close"), &dlg);
        QObject::connect(close, &QPushButton::clicked, &dlg, &QDialog::accept);

        auto *lay = new QVBoxLayout(&dlg);
        lay->addWidget(box, 1);
        lay->addWidget(close, 0, Qt::AlignRight);
        dlg.exec();
    };

    /* Render a still at the source's own size with the app's palette, into
     * `rgb`.  Returns false and leaves `rgb` empty when it cannot.  Shared by
     * open and export so the two cannot show/export different pixels. */
    auto render_still = [&](const dyt_vm_item_t *it, std::vector<uint8_t> &rgb,
                            int &w, int &h) -> bool {
        dyt_session_t      *gs = nullptr;
        dyt_frame_source_t *sf = nullptr;
        const uint8_t      *px = nullptr;
        bool                ok = false;

        if (!it || it->kind != DYT_VM_ITEM_STILL)
            return false;

        gs = setup_session(o);
        if (gs)
            sf = dyt_frame_source_open_still(
                gs, it->path, o.width, DYT_MODE_1000, DYT_PLANE_BOTTOM_HALF,
                o.cap.t_amb, o.cap.sensor_mode, o.cap.fix_mode);
        if (sf && dyt_frame_source_next(sf, &px, &w, &h) == DYT_FS_FRAME && px) {
            rgb.assign(px, px + (size_t)w * (size_t)h * 3);
            ok = true;
        }
        dyt_frame_source_close(sf);
        dyt_session_free(gs);
        return ok;
    };

    win.on_gallery_open_ = [&](const dyt_vm_item_t *it) -> bool {
        if (!it) {
            pm.notice("gallery: nothing selected");
            return false;
        }

        /* Whatever was on the canvas goes, so a still opened over a playing
         * clip is not overwritten by the next playback tick — and a clip
         * opened over a still is not drawn under it.  The badge goes with it,
         * so it cannot briefly claim a clip is playing after it stopped. */
        pm.playback.stop();
        win.strip()->set_playback(QString());

        if (it->kind != DYT_VM_ITEM_STILL) {
            /* A clip.  Opening it starts the reader; the pump advances it at
             * its per-tick hook, and MainWindow has already hidden the list
             * (a success return) so the clip fills the canvas.  The badge on
             * the strip carries the position, and space pauses it. */
            std::string why;
            if (!pm.playback.open(it->path, why)) {
                pm.notice(why, 200);
                return false;           /* keep the list up */
            }
            pm.notice(std::string("playing ") + it->name, 150);
            return true;
        }

        std::vector<uint8_t> rgb;
        int                  w = 0, h = 0;
        if (!render_still(it, rgb, w, h)) {
            pm.notice(std::string("cannot open ") + it->name, 200);
            return false;
        }

        dyt_vm_still_info_t info;
        QString             label = QStringLiteral("viewing %1")
                                        .arg(QString::fromUtf8(it->name));
        if (dyt_vm_still_info(it->path, &info) == 0 && !info.have_thermal)
            label += QStringLiteral("   (no thermal geometry)");

        const QImage img(rgb.data(), w, h, w * 3, QImage::Format_RGB888);
        win.set_viewing(img.copy(), label);
        pm.notice(std::string("opened ") + it->name, 120);
        return true;
    };

    win.on_gallery_export_ = [&](const dyt_vm_item_t *it) {
        if (!it) {
            pm.notice("gallery: nothing selected");
            return;
        }
        if (it->kind != DYT_VM_ITEM_STILL) {
            pm.notice("only a still can be exported as a PNG", 200);
            return;
        }

        std::vector<uint8_t> rgb;
        int                  w = 0, h = 0;
        if (!render_still(it, rgb, w, h)) {
            pm.notice(std::string("cannot export ") + it->name, 200);
            return;
        }

        char ts[32], path[512];
        if (dyt_vm_timestamp(ts, sizeof ts) != 0 ||
            dyt_vm_capture_name(path, sizeof path, o.capture_dir.c_str(), ts,
                                "png") != 0) {
            pm.notice("cannot export: no usable file name", 200);
            return;
        }
        if (dyt_write_png(path, rgb.data(), w, h) != 0) {
            pm.notice("cannot export: the PNG was not written", 200);
            return;
        }
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        /* No rescan: the export is a .png, which is not a gallery item — the
         * list holds the containers and the clips, and it is unchanged. */
        pm.notice(std::string("exported ") + base, 150);
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

    /* The default source is the source tree's fixture, and that path is
     * relative to the checkout.  An installed binary launched from the menu
     * runs with cwd $HOME, so there is no fixture to replay — and replaying a
     * frozen file is not what someone opening a camera viewer wants anyway.
     * So: replay the fixture when it is actually there, and otherwise use the
     * camera.  An explicit --fixture that cannot be read stays an error rather
     * than becoming a silent switch to the device. */
    if (!o.live && !o.fixture_set && access(o.fixture.c_str(), R_OK) != 0)
        o.live = true;

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
            /* The canvas, not the window: --help calls this "write the canvas
             * here", and since the menu bar and toolbar arrived a window grab
             * would put the chrome in the file too. */
            const QPixmap grab = win.view()->grab();
            if (grab.save(QString::fromUtf8(o.png.c_str())))
                std::printf("dytqt: canvas written to %s\n", o.png.c_str());
            else
                std::fprintf(stderr, "dytqt: could not write %s\n", o.png.c_str());
        }
    }

    if (!o.shot.empty()) {
        /* The whole window, chrome and all: the shell is what --shot is for,
         * where --png is the picture alone.  Same "needs a painted frame" rule
         * as --png. */
        if (pm.frames == 0) {
            std::fprintf(stderr, "dytqt: no frame painted; nothing written\n");
        } else {
            const QPixmap grab = win.grab();
            if (grab.save(QString::fromUtf8(o.shot.c_str())))
                std::printf("dytqt: window written to %s\n", o.shot.c_str());
            else
                std::fprintf(stderr, "dytqt: could not write %s\n",
                             o.shot.c_str());
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

    /* Save the view state the user ended on, so the next run opens the way
     * this one closed.  Done before the session is freed, because the state
     * comes from the session. */
    if (!o.no_prefs)
        prefs_save(prefs_path(o), prefs_from_session(sess, o.capture_dir));

    dyt_session_free(sess);
    return rc;
}

int main(int argc, char **argv)
{
    opts o;
    if (!parse_args(argc, argv, o))
        return 2;

    /* "--png PATH / --shot PATH" both mean "write it and exit", so one frame
     * is enough.  Without this the loop runs until the window is closed and the
     * file is never written — which is what the README's own example did. */
    if ((!o.png.empty() || !o.shot.empty()) && o.frames == 0)
        o.frames = 1;

    /* The selftest must not need a display, and must not steal one if the
     * user has one.  This has to happen before QApplication exists. */
    if (o.selftest) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        /* And it must not read the user's saved view state: a test that
         * changed its answer depending on what the developer last pressed is
         * not a test. */
        o.no_prefs = true;
    }

    QApplication app(argc, argv);

    if (o.selftest)
        return selftest(o);
    return run_gui(o, app);
}
