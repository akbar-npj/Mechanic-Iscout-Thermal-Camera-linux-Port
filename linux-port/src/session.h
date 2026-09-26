/*
 * session.h — the device-free model object the front-ends drive.
 *
 * Every front-end needs the same three things: somewhere to put the frame the
 * capture callback hands over, a consistent view of it to draw from, and a
 * place to keep the display state (range, palette, units, mirror/zoom) that
 * the keys and menus change.  Doing that per front-end means the OpenCV
 * viewer and the Qt6 app drift apart; doing it here means the Qt6 app is a
 * shell.  So the composition lives in the library.
 *
 * Three deliberate constraints keep this testable and honest:
 *
 *  1. **Device-free.**  It does not link libuvc, spawn threads, or call any
 *     uvc_* function.  It consumes plain float images (plus, for fusion, the
 *     8-bit grey visible plane), so it is exercised in `make check` with
 *     synthetic frames and no hardware (session_test.c).
 *     src/session_capture.c is the separate adapter that feeds it.
 *
 *  2. **Poll-based.**  The GUI takes a snapshot under the lock rather than
 *     being called back.  No callback-into-GUI API is invented ahead of need;
 *     a Qt6 front-end can poll on a timer or emit a queued signal itself.
 *
 *  3. **Celsius in, display units out.**  The pipeline produces Celsius; the
 *     snapshot carries Celsius; only dyt_temp_format()/dyt_temp_convert()
 *     turn it into the selected unit.  So changing units never re-renders.
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -I. -c session.c
 */
#ifndef DYT_SESSION_H
#define DYT_SESSION_H

#include <stdint.h>

#include "alarm.h"
#include "display.h"
#include "fusion.h"
#include "measure.h"
#include "palette.h"
#include "sr.h"
#include "units.h"
#include "visible.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyt_session dyt_session_t;

/* One frame, handed in from the capture layer's callback thread.
 *
 * `temps` is width*height floats in Celsius, row-major, with any mode-0x44c
 * header already stripped — exactly what dyt_capture_t's frame callback
 * delivers.  The buffer is borrowed for the duration of the call and is not
 * retained. */
typedef struct {
    const float *temps;
    int          width, height;
} dyt_frame_info_t;

/* Which measurement tool the user has active.  The geometry is held by the
 * session so the readings can be recomputed from every new frame without the
 * front-end re-placing them. */
typedef enum {
    DYT_TOOL_NONE = 0,   /* no measurement */
    DYT_TOOL_POINT,      /* a single pixel, at p0 */
    DYT_TOOL_LINE,       /* a profile from p0 to p1 */
    DYT_TOOL_BOX,        /* a rectangle ROI spanned by p0 and p1 */
    DYT_TOOL_POLYGON     /* an arbitrary region through poly[] */
} dyt_tool_t;

/* A consistent view of the session for the GUI thread.  Everything here is
 * copied out under the lock; nothing aliases session state. */
typedef struct {
    int   width, height;      /* 0 until the first frame */
    long  seq;                /* frames processed so far */
    int   ready;              /* 0 while the frame is still start-up filler */

    dyt_frame_stats_t stats;  /* min/max/mean, extrema coords, filler flag */
    float lo, hi;             /* the display range resolved for this frame */
    dyt_range_mode_t range_mode;  /* whether lo/hi are per-frame or latched */
    dyt_unit_t unit;          /* the selected display unit */
    int   palette, palette_n; /* active palette index and how many loaded */
    char  palette_name[32];   /* the active palette's name, for status lines */
    dyt_view_transform_t xform;

    int   centre_x, centre_y; /* centre pixel, in source coordinates */
    float centre_c;           /* its temperature, Celsius; NaN if not ready */

    /* ---- measurement ---------------------------------------------------
     * The geometry is what the user placed; the readings are computed from
     * the current frame on demand (see dyt_session_snapshot), because a ROI
     * median is far too expensive for the per-frame callback thread. */
    dyt_tool_t  tool;         /* which tool is active */
    dyt_point_t p0, p1;       /* the placed points, in source pixels */

    /* The polygon tool's outline, in source pixels.  It is *derived*, not
     * placed: the polygon is a drag tool like the box, and the snapshot fits a
     * DYT_POLYGON_SIDES-sided shape to the p0/p1 box (dyt_polygon_fit_box).
     * So `poly_n` is DYT_POLYGON_SIDES whenever the tool is POLYGON and the box
     * is placed, and 0 otherwise — there is no half-drawn outline to report. */
    int         poly_n;                           /* 0, or DYT_POLYGON_SIDES */
    dyt_point_t poly[DYT_POLYGON_SIDES];

    int   point_ok;           /* 1 when tool == POINT and p0 has a reading */
    float point_c;            /* the reading at p0, Celsius */

    /* The area tool's statistics: the box (p0..p1) or the polygon, whichever
     * `tool` selects.  One set of fields rather than two, because a front end
     * shows them in the same place and only one area tool is ever active —
     * `tool` is what says which geometry produced them. */
    int             roi_ok;   /* 1 when the region is non-empty */
    dyt_roi_stats_t roi;

    int             iso_on;   /* the alarm band is being shown as an isotherm */
    dyt_isotherm_t  iso;      /* pixels inside [iso_lo, iso_hi] */
    float           iso_lo, iso_hi;

    /* ---- alarm --------------------------------------------------------- */
    int               alarm_on;   /* the alarm is armed */
    dyt_alarm_state_t alarm;      /* its state as of the last frame */
    float             alarm_lo, alarm_hi;

    /* ---- fusion (Phase 5) ----------------------------------------------
     * The pattern the user selected, where the visible plane is aligned
     * against the thermal one, and whether render_rgb() is actually able to
     * fuse on this frame.  `fusion_active` is what a front-end keys its
     * status line off: the pattern can be EDGE while the AD output mode has
     * no visible half at all, in which case render_rgb() falls back to the
     * thermal picture and the UI should say so rather than silently showing
     * something the user did not ask for. */
    dyt_fusion_t fusion;          /* the selected pattern */
    char         fusion_name[16]; /* "ir" / "visible" / "edge" / ... */
    int          fusion_dx, fusion_dy;  /* visible-plane alignment */
    int          fusion_active;   /* 1 when the render is actually fused */
    int          have_visible;    /* a visible plane is installed */
    dyt_visible_stats_t visible;  /* its min/max/mean; n == 0 when none */

    /* ---- the raw device payload (Phase 6) -------------------------------
     * What the device actually sent, kept so a still can be written from the
     * GUI thread — the capture layer's own accessor is only safe on the frame
     * callback thread.  `raw_n` is width * raw_total_rows uint16 samples. */
    int          have_raw;
    int          raw_n;
    int          raw_total_rows;

    /* ---- super-resolution (the optional 2x model) -----------------------
     * What the user asked for, whether a model is actually loaded, and whether
     * this frame can be super-resolved at all.  `sr_active` is the one a front
     * end keys its status line off: the mode can be VISIBLE while the AD
     * output mode has no visible half to upscale, in which case render_rgb()
     * renders the plain picture and the UI should say so rather than silently
     * showing something the user did not ask for — exactly the treatment
     * `fusion_active` gets.
     *
     * `xform.sr` carries the factor the render produced (1 or 2), so a front
     * end needs no arithmetic of its own to map a pointer back. */
    dyt_sr_t sr;                  /* the selected mode */
    char     sr_name[16];         /* "off" / "visible" / "thermal" */
    int      sr_cap;              /* a model is loaded and can run */
    int      sr_active;           /* this frame really is super-resolved */
} dyt_snapshot_t;

/* --------------------------------------------------------------- lifecycle */

dyt_session_t *dyt_session_create(void);

/* Stop nothing and free everything.  Safe on NULL. */
void dyt_session_free(dyt_session_t *s);

/* ---------------------------------------------------------- frame pipeline */

/* Install one frame and recompute the statistics, display range and extrema.
 * Called on the capture callback thread; takes the lock internally and
 * returns immediately — it must stay cheap, since it runs per frame.
 * Returns 0 on success, -1 on a bad argument. */
int dyt_session_process(dyt_session_t *s, const dyt_frame_info_t *fi);

/* Install the grayscale visible plane belonging to the frame just processed
 * (the dual-half payload's top half — see visible.h).  `grey` is width*height
 * bytes.
 *
 * A plane whose geometry does not match the frame just processed is refused
 * (-1), so a plane from a previous frame size can never be fused with the
 * current temperatures.  A frame with no visible half (the AD output mode)
 * simply never calls this: the previously installed plane stays in place
 * rather than being cleared, so fusion does not flicker off and on.
 *
 * Cheap — a memcpy plus one pass for the summary statistics.  Call it from
 * the frame callback, immediately after dyt_session_process(), so the pair
 * belongs to the same frame (the adapter in session_capture.c does).
 *
 * Returns 0 on success, -1 on a bad argument or a geometry mismatch. */
int dyt_session_process_visible(dyt_session_t *s, const uint8_t *grey,
                                int width, int height);

/* Install the raw device payload belonging to the frame just processed: the
 * uint16 samples exactly as the device sent them (width * total_rows of them),
 * which is what a DYT still stores verbatim (see dytjpeg.h).
 *
 * This exists because the capture layer's own accessor is only valid on the
 * frame callback thread.  Keeping the copy here means a still can be written
 * from the GUI thread without racing the next frame.  Cheap — one memcpy.
 *
 * A payload that does not belong to the frame just processed is refused (-1),
 * the same rule dyt_session_process_visible() follows.  In the dual-half mode
 * total_rows (384) is larger than the thermal height (192); that is expected.
 *
 * Returns 0 on success, -1 on a bad argument or a geometry mismatch. */
int dyt_session_process_raw(dyt_session_t *s, const uint16_t *raw,
                            int n_samples, int width, int total_rows);

/* Copy the stored raw payload into `out` (cap uint16 samples).  Returns the
 * number of samples copied, 0 when none is held, or the negated requirement
 * when cap is too small.  The geometry is in the snapshot (raw_n,
 * raw_total_rows). */
int dyt_session_raw(dyt_session_t *s, uint16_t *out, int cap);

/* Copy the current state into *out.  If temps_out is non-NULL the temperature
 * plane is copied into it as well (temps_cap floats) — the GUI needs random
 * access for hover readouts and, later, measurement tools, and copying under
 * the lock is what keeps that safe.  Pass NULL/0 to fetch only the scalars.
 *
 * Returns 0 on success, -1 if no frame has arrived yet, -2 if temps_out is
 * too small. */
int dyt_session_snapshot(dyt_session_t *s, dyt_snapshot_t *out,
                         float *temps_out, int temps_cap);

/* Render the latest frame through the active palette and range.  The
 * mirror/zoom in the snapshot is *not* applied here — the caller applies it
 * when it presents, and uses dyt_view_transform_map() to map pointer
 * coordinates back.
 *
 * With super-resolution active the render is at 2x: `w`/`h` come back doubled
 * and the picture is the model's, not a scaled copy of the 1x one.  The
 * snapshot's `xform.sr` reports the factor the render will use, so a caller
 * sizes its buffer from `snapshot width * xform.sr` and needs no other
 * arithmetic.  Returns 0 and writes the dimensions through `w` and `h` on
 * success, -1 if there is no frame, -2 if out_rgb is too small. */
int dyt_session_render_rgb(dyt_session_t *s, uint8_t *out_rgb, int rgb_cap,
                           int *w, int *h);

/* ------------------------------------------------------ super-resolution */

/* The 2x upscaler a front end hands the session.
 *
 * It is the seam's own signature (mnn.h's dyt_mnn_zoom2) carried as a
 * *pointer*, and that is the point: the engine holds no reference to MNN, so
 * a binary that never enables super-resolution still links no runtime at all.
 * The seam makes the same promise at the object level (mnn.h); this keeps it
 * true at the library level too, which matters because the session is the
 * module every front end links.
 *
 * A front end that wants the feature loads the model and hands the seam in:
 *
 *     if (dyt_mnn_load(path) == 0 && dyt_mnn_available())
 *         dyt_session_set_sr_upscaler(sess, dyt_mnn_zoom2);
 *
 * Passing NULL withdraws it, which forces the mode back to OFF. */
typedef int (*dyt_sr_upscale_fn)(const uint8_t *in, uint8_t *out,
                                 int out_cap, int *out_n);

void dyt_session_set_sr_upscaler(dyt_session_t *s, dyt_sr_upscale_fn fn);

/* Which plane to upscale.  DYT_SR_OFF, or DYT_SR_VISIBLE / DYT_SR_THERMAL
 * (sr.h).  Selecting a mode on a session with no upscaler installed is refused
 * — the mode stays OFF — because the seam's rule is never to invent a frame.
 * An out-of-range value is ignored rather than clamped. */
void dyt_session_set_sr(dyt_session_t *s, dyt_sr_t m);

/* The mode the session is holding (OFF when it cannot run one). */
dyt_sr_t dyt_session_get_sr(dyt_session_t *s);

/* Whether an upscaler is installed, i.e. whether a mode could be selected at
 * all.  This is the same fact the snapshot reports as `sr_cap`, but it is
 * readable with no frame in hand — which is what a control panel needs, since
 * it draws its "model loaded / no model" line before the first frame arrives
 * and the snapshot cannot be taken yet. */
int dyt_session_sr_capable(dyt_session_t *s);

/* ------------------------------------------------------------------ state */

/* Load every palette in `dir`; if `dir` is NULL or yields none, fall back to
 * the built-in ramps so the session always has at least one.  Returns the
 * number available (>= 1), or -1 on a bad argument. */
int dyt_session_load_palettes(dyt_session_t *s, const char *dir);

/* Copy palette `idx` into *out — a copy rather than a pointer, so the caller
 * never aliases session state.  Front-ends need it to draw a colour bar.
 * Returns 0 on success, -1 if `idx` is out of range or an argument is NULL. */
int dyt_session_get_palette(dyt_session_t *s, int idx, dyt_palette_t *out);

void dyt_session_set_unit(dyt_session_t *s, dyt_unit_t u);

/* Step to the next unit in the app's cycle order (the viewer's "u" key). */
void dyt_session_cycle_unit(dyt_session_t *s);
void dyt_session_set_range_mode(dyt_session_t *s, dyt_range_mode_t m);

/* AUTO <-> FIXED, latching the current range on the way in. */
void dyt_session_toggle_range(dyt_session_t *s);

void dyt_session_set_fixed_range(dyt_session_t *s, float lo, float hi);
void dyt_session_set_palette(dyt_session_t *s, int idx);
void dyt_session_cycle_palette(dyt_session_t *s, int dir);
void dyt_session_toggle_flip_h(dyt_session_t *s);
void dyt_session_toggle_flip_v(dyt_session_t *s);
void dyt_session_zoom(dyt_session_t *s, int delta);

/* Step the clockwise rotation by `delta` degrees (the rail's Rotate button
 * passes 90).  Part of the framing, like the mirrors and the zoom. */
void dyt_session_rotate(dyt_session_t *s, int delta);

/* Put the view back to its start-up framing: zoom to DYT_ZOOM_MIN, both
 * mirrors off, the rotation back to 0, and the range back to AUTO.  This is the
 * rail's Reset Image.
 *
 * One operation rather than five setters called in a row, because the *reset*
 * is the meaning: a caller that forgot one of them would leave a picture reset
 * in every way but one, which is the failure this exists to prevent.  Reading
 * the state under the session's own lock is the other half of that — a caller
 * composing the five from a snapshot could act on a frame that has already been
 * superseded.
 *
 * The palette, the unit, the fusion pattern and the super-resolution plane are
 * deliberately NOT touched: they are how the picture is rendered, not how it is
 * framed, and a user resetting the image does not expect the colours to
 * change. */
void dyt_session_reset_view(dyt_session_t *s);

/* ----------------------------------------------------------------- fusion */

/* Which fusion pattern to render (fusion.h).  An out-of-range value is
 * ignored rather than clamped, so a bad index cannot silently become a
 * different pattern.  INFRARED (thermal only) is the start-up state. */
void dyt_session_set_fusion(dyt_session_t *s, dyt_fusion_t f);

/* Step through the patterns in the vendor's order (the viewer's "f" key).
 * `dir` is +1 for the next pattern or -1 for the previous; it wraps. */
void dyt_session_cycle_fusion(dyt_session_t *s, int dir);

/* Nudge the visible plane against the thermal one.  The offsets are clamped
 * to ±DYT_FUSION_ALIGN_MAX, the range the vendor's own UI allows
 * (RE Docs 03 §3.5.2), so a held-down key cannot wind the alignment out of
 * the range the device's coefficients are defined over. */
void dyt_session_adjust_fusion_align(dyt_session_t *s, int ddx, int ddy);
void dyt_session_set_fusion_align(dyt_session_t *s, int dx, int dy);

/* ------------------------------------------------------------- measurement */

/* Which measurement tool is active.  Selecting a tool keeps whatever points
 * were already placed, so switching between LINE and BOX reuses them. */
void dyt_session_set_tool(dyt_session_t *s, dyt_tool_t t);

/* Place point 0 or 1 (in source pixels).  The POINT tool uses point 0 only.
 * Coordinates outside the frame are stored as given — the measurement then
 * simply reports no reading, rather than the front-end having to clamp. */
void dyt_session_set_point(dyt_session_t *s, int which, int x, int y);

/* Forget the whole measurement — both points, and with them whatever shape
 * they were describing (the viewer's "n" key).  One action rather than one per
 * tool: a front end has a single "clear" affordance. */
void dyt_session_clear_points(dyt_session_t *s);

/* ---- moving and resizing a placed region --------------------------------
 * The vendor's own model (CAAnalyzer.decompiled.cs, `SelsectShape` and
 * `stretchShape`): a placed rectangle is dragged around by its *body* and
 * resized by any of eight handles — the four corners and the four edge
 * midpoints.  The port keeps the geometry in the session, so both operations
 * are session calls and the reading follows on the next frame exactly as a
 * fresh placement does; a front end that moved the points itself would be a
 * second copy of the clamp rules.
 *
 * Both tools that place a region share this: the box *and* the polygon, whose
 * five vertices are derived from the same two points.  Moving or resizing a
 * pentagon is therefore moving or resizing its bounding box, and the shape
 * follows.
 *
 * The handle numbering is the vendor's, because the front end draws them and
 * the two must agree about which is which:
 *
 *      0 --- 1 --- 2        0 top-left       4 bottom-right
 *      |           |        1 top-middle     5 bottom-middle
 *      7           3        2 top-right      6 bottom-left
 *      |           |        3 right-middle   7 left-middle
 *      6 --- 5 --- 4
 */
#define DYT_ROI_HANDLES 8

/* The smallest a stretched region may become, on either axis.  The vendor's
 * own floor is 5 raw pixels, in `SelsectShape`'s corner case, where the dragged
 * corner stops that far short of the region's own opposite edge
 * (`num6 -= point.X + num6 - (sHAPE_COM2.max_p.X - 5)`).  It is what stops a
 * handle dragged past its opposite edge from inverting the region into an empty
 * one.  It is applied only to the axes the handle actually moves, and the
 * result is clamped into the frame, so a frame smaller than this cannot push an
 * edge out of bounds. */
#define DYT_ROI_MIN 5

/* Move the region so its top-left corner lands on (x, y) — the *absolute*
 * form, which is what a drag wants.  A front end keeps the pointer's offset
 * from the grab and calls this, so a move that was clamped does not leave the
 * region lagging behind the pointer.  The region is clamped as a whole: it
 * stops at the frame's edge rather than being partly cropped, which would
 * silently change what is being measured.
 *
 * Returns 0, or -1 on a bad argument, no frame yet, or no region to move. */
int dyt_session_roi_move_to(dyt_session_t *s, int x, int y);

/* Put the dragged `handle` at (x, y).  The point is clamped into the frame and
 * the region is kept at least DYT_ROI_MIN wide and tall, so the opposite edge
 * never crosses the one being dragged.  A middle handle moves one axis only.
 *
 * Returns 0, or -1 on a bad argument, an out-of-range handle, no frame yet, or
 * no region. */
int dyt_session_roi_stretch(dyt_session_t *s, int handle, int x, int y);

/* ---- the polygon tool ---------------------------------------------------
 * The polygon is placed like the box: a press sets the first corner, the drag
 * the second, and the shape is a DYT_POLYGON_SIDES-sided figure fitted to the
 * box between them.  There is no click-by-click outline and no commit step —
 * the shape exists as soon as the drag does, and it is always closed.
 *
 * The vertices are not stored: the snapshot derives them from p0/p1 every time
 * (see dyt_polygon_fit_box), so a moved or resized polygon is simply a moved or
 * resized box, and the two tools cannot drift apart. */

/* Copy the current line profile into `out` (cap floats).  Returns the number
 * of points written, -1 if there is no frame, or the negated requirement if
 * cap is too small.  Empty (0) unless the LINE tool is active. */
int dyt_session_profile(dyt_session_t *s, float *out, int cap);

/* -------------------------------------------------------------- alarms */

/* Arm the alarm.  A non-positive `hyst` uses DYT_ALARM_HYST_DEFAULT.  The
 * session evaluates it against the frame's hottest and coldest readings, so
 * it trips on the scene rather than on one probe. */
void dyt_session_set_alarm(dyt_session_t *s, float lo, float hi, float hyst);
void dyt_session_alarm_disable(dyt_session_t *s);

/* Clear the latches but stay armed. */
void dyt_session_alarm_reset(dyt_session_t *s);

/* Show the alarm band as an isotherm overlay, and report how many pixels
 * fall inside it (snapshot.iso). */
void dyt_session_set_isotherm(dyt_session_t *s, int on);

#ifdef __cplusplus
}
#endif

#endif /* DYT_SESSION_H */
