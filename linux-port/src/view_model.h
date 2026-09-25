/*
 * view_model.h — "what to show", computed from a session snapshot.
 *
 * dytview.cpp draws with OpenCV; the planned Qt6 app draws with QPainter.
 * Between them lies a layer that has nothing to do with either: taking a
 * consistent snapshot, turning it into the status and readout lines, the
 * colour bar's ticks, the device panel's rows, the runtime-parameter ladder,
 * and the pointer-to-tool mapping.  Writing that twice is how two front-ends
 * drift apart, so it lives here — the same reason display.c was lifted out of
 * the viewer in Phase 1 (see the header comment in session.h).
 *
 * Nothing here draws, and nothing here includes a toolkit.  Every entry point
 * is a pure function of a dyt_snapshot_t plus, where it matters, the
 * front-end's own small state (which key is armed, whether a drag is in
 * progress), so all of it is exercised in `make check` with no hardware and
 * no window — view_model_test.c.
 *
 * The split of responsibility is deliberate: this module decides *what the
 * words are*, the front-end decides where to put them.  So it never returns a
 * canvas and never takes a font size.
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -Isrc -c view_model.c
 */
#ifndef DYT_VIEW_MODEL_H
#define DYT_VIEW_MODEL_H

#include <stddef.h>
#include <stdint.h>

#include "capture.h"   /* dyt_device_info_t — header-only, no libuvc */
#include "frame.h"     /* dyt_mode_t */
#include "params.h"    /* dyt_order_type_t */
#include "session.h"   /* dyt_session_t, dyt_snapshot_t, dyt_view_transform_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- polling
 *
 * The GUI takes a snapshot under the session's lock rather than being called
 * back (session.h constraint 2).  Two details make that awkward enough to be
 * worth sharing rather than rewriting: the temperature plane has to be sized
 * to the frame, which is only known after a first attempt has reported the
 * geometry, and an empty buffer's data() may be NULL, which
 * dyt_session_snapshot() reads as "scalars only" and silently skips the copy.
 */
typedef struct {
    float *temps;   /* caller-owned scratch, grown to width*height */
    int    cap;     /* its capacity in floats */
} dyt_vm_scratch_t;

#define DYT_VM_SCRATCH_INIT { NULL, 0 }

/* Fill *snap, and through `scr` its temperature plane, so the hover readout
 * and the isotherm overlay have random access to it.
 *
 * Returns 1 on success, 0 when no frame has arrived yet (or on a bad
 * argument / allocation failure).  Allocates into *scr on first use and
 * whenever the frame size changes; release with dyt_vm_scratch_free(). */
int  dyt_vm_grab(dyt_session_t *s, dyt_snapshot_t *snap, dyt_vm_scratch_t *scr);
void dyt_vm_scratch_free(dyt_vm_scratch_t *scr);

/* ------------------------------------------------------------------ text */

/* A temperature in the selected unit, e.g. "31.2 C".  The conversion is
 * units.c's, so no front-end can disagree with another, and so is the failure
 * rule: "---" replaces a *truncated* result.
 *
 * Note a NaN is not an error to dyt_temp_format() — it formats as whatever
 * the host spells NaN — so a caller that can see one must guard it, exactly
 * as the viewer's hover readout does with `t == t`.  That is the behaviour
 * the viewer had before this module existed, kept deliberately. */
int dyt_vm_temp(const dyt_snapshot_t *snap, float celsius, char *out, size_t n);

/* The first status line: capture mode, fusion pattern, palette, unit,
 * mirror/zoom and the frame counter.  A fusion pattern that cannot be
 * honoured on this frame — the AD output mode has no visible half — is called
 * out rather than left to look like it is working.
 *
 * Returns the number of characters that would have been written, so a caller
 * can detect truncation; the buffer is always NUL-terminated. */
int dyt_vm_status_line(const dyt_snapshot_t *snap, dyt_mode_t mode,
                       char *out, size_t n);

/* The second status line: the measurement and alarm state, plus `msg` — a
 * transient notice the front-end owns — when it is non-NULL and non-empty.
 * Kept compact because it is only as wide as the image, and the unit is
 * already on the line above, so the numbers here carry no suffix. */
int dyt_vm_readout_line(const dyt_snapshot_t *snap, const char *msg,
                        char *out, size_t n);

/* The hover readout: the temperature under the pointer with the source pixel
 * it came from, e.g. "31.2 C  (12,7)".  A NaN reads "---  (12,7)".
 *
 * This is where the `t == t` guard lives.  dyt_vm_temp() deliberately does not
 * apply one — a NaN formats as whatever the host spells NaN — and every
 * front-end that shows a temperature under the pointer needs the same guard,
 * so it belongs here rather than in each of them. */
int dyt_vm_hover_label(const dyt_snapshot_t *snap, float celsius,
                       int x, int y, char *out, size_t n);

/* The box tool's statistics, e.g.
 * "min 30.0 C  max 40.0 C  avg 35.0 C  med 36.0 C".  A statistic that is NaN
 * — a region with no finite samples (measure.h) — reads "--", so an empty box
 * cannot be mistaken for a reading of 0 C.  Returns the number of characters
 * that would have been written, so a caller can detect truncation, or -1 on a
 * bad argument. */
int dyt_vm_roi_label(const dyt_snapshot_t *snap, char *out, size_t n);

/* ------------------------------------------------------------- colour bar */

/* The palette index for a row of a bar `rows` tall.  Row 0 is the top, which
 * is the hottest, matching the palette's own order.  Returns 0..DYT_PALETTE_N-1,
 * or -1 on a bad argument. */
int dyt_vm_bar_index(int row, int rows);

/* The bar's three labels.  `which` 0 = top (snap->hi), 1 = middle, 2 = bottom
 * (snap->lo).  Returns 0 on success, -1 on a bad argument. */
int dyt_vm_bar_label(const dyt_snapshot_t *snap, int which, char *out, size_t n);

/* ----------------------------------------------------------- device panel */

#define DYT_VM_INFO_MAX_LINES 8
#define DYT_VM_INFO_LINE_CAP  96

typedef struct {
    char line[DYT_VM_INFO_MAX_LINES][DYT_VM_INFO_LINE_CAP];
    int  n;
    int  any_override;   /* a runtime write superseded a stored value */
} dyt_vm_info_t;

/* Build the device panel's rows: the module serial, the decoded user serial,
 * the four stored radiometric parameters, and the slot count.
 *
 * `override_v` and `override_on` are the front-end's record of the runtime
 * writes it has sent this session, indexed by dyt_order_type_t (1..4; index 0
 * unused), or NULL for neither.  A value that came from an override is
 * suffixed `*`, so the panel never silently shows a stored value a write has
 * superseded — the device cannot be re-read while streaming (RE Docs 04
 * §4.8).  `any_override` is set when at least one row carries one.
 *
 * Returns the number of rows written, or -1 on a bad argument. */
int dyt_vm_info(const dyt_device_info_t *d, const float *override_v,
                const int *override_on, dyt_vm_info_t *out);

/* -------------------------------------------------------- parameter ladder
 *
 * Phase 4's runtime write.  A parameter key only *arms* a candidate value;
 * nothing is sent until the user confirms.  That is deliberate — the device
 * applies these immediately, so a stray keypress visibly changes the reading
 * — and it is the same rule for every front-end.  The ladder and the
 * transition therefore live here; the wording of the confirmation stays with
 * the front-end, which is why this returns an event rather than a string.
 */
typedef struct {
    dyt_order_type_t type;
    const char      *name;
    int              n;
    const float     *vals;
} dyt_vm_ladder_t;

int                    dyt_vm_ladder_count(void);
const dyt_vm_ladder_t *dyt_vm_ladder_at(int i);
const dyt_vm_ladder_t *dyt_vm_ladder(dyt_order_type_t type);

/* A candidate value with its unit, e.g. "0.10" / "25.0 C" / "1.00 m". */
int dyt_vm_param_format(dyt_order_type_t type, float v, char *out, size_t n);

/* What a keypress does to the arming state. */
typedef enum {
    DYT_VM_PARAM_NONE = 0,  /* not ours: the front-end should handle it */
    DYT_VM_PARAM_SWALLOW,   /* ours, consumed, nothing armed changes */
    DYT_VM_PARAM_ARMED,     /* a (new) candidate is armed */
    DYT_VM_PARAM_SEND,      /* send the armed candidate */
    DYT_VM_PARAM_CANCEL     /* drop the armed candidate */
} dyt_vm_param_action_t;

typedef struct {
    dyt_vm_param_action_t action;
    dyt_order_type_t      type;   /* which parameter; 0 when NONE */
    float                 value;  /* ARMED and SEND */
    int                   rung;   /* the ladder position to remember */
} dyt_vm_param_event_t;

/* Decide what `key` means, given the front-end's arming state (`armed_type`
 * 0 = nothing armed, `armed_rung` the ladder position it is on).  Re-pressing
 * a parameter's own key advances its candidate; a different parameter key
 * starts its own ladder at the first rung.
 *
 * Returns 1 when the key was consumed, in which case *ev says what to do.  A
 * consumed key whose action is SWALLOW is the rule that while something is
 * armed every other key is ignored — so a stray palette key cannot slip past
 * a pending confirmation.  Returns 0 for a key that is not ours, which
 * includes `q` even while armed: quit is never swallowed. */
int dyt_vm_param_key(int key, dyt_order_type_t armed_type, int armed_rung,
                     dyt_vm_param_event_t *ev);

/* --------------------------------------------------------- measurement UI */

typedef enum {
    DYT_VM_MOUSE_DOWN = 0,
    DYT_VM_MOUSE_MOVE,
    DYT_VM_MOUSE_UP
} dyt_vm_mouse_ev_t;

/* The front-end's pointer state.  `x`/`y` are window coordinates, kept so the
 * hover readout can be drawn; `dragging` spans a press to its release. */
typedef struct {
    int dragging;
    int x, y;
} dyt_vm_pointer_t;

#define DYT_VM_POINTER_INIT { 0, -1, -1 }

/* Apply a pointer event to the session's active tool.  `tool` is the tool the
 * front-end has selected (the session keeps its own copy for the readings),
 * and the geometry is the *last rendered* frame, so a window coordinate maps
 * back to the source pixel the user actually pointed at.
 *
 * A press places both points, a move while dragging moves point 1, and a
 * release ends the drag — so one gesture draws a line or a box, and a plain
 * click leaves both points on the same pixel, which is exactly a point probe.
 * A pointer outside the image still updates x/y (for the readout) but places
 * nothing.
 *
 * Returns 1 when the session's geometry changed, 0 otherwise. */
int dyt_vm_tool_mouse(dyt_session_t *s, dyt_vm_pointer_t *p,
                      dyt_vm_mouse_ev_t ev, dyt_tool_t tool,
                      const dyt_view_transform_t *xform,
                      int src_w, int src_h, int dst_w, int dst_h,
                      int x, int y);

/* The alarm band a front-end arms when the user asks for one: the middle 40 %
 * of the frame's current display range, with a 10 % hysteresis.  The band's
 * edges sit inside the range, so the hottest and coldest parts of the scene
 * trip it.
 *
 * A policy rather than a reading, which is why it lives here and not in each
 * front-end: it is one spelling of "arm an alarm across what is on screen",
 * and the reference viewer and the Qt app must not disagree about it.  A flat
 * frame (hi == lo) falls back to a span of 1.0, which makes the returned `hi`
 * *below* the returned `lo` — the reference viewer's behaviour, kept rather
 * than quietly corrected.
 *
 * Returns 0 on success, -1 on a bad argument. */
int dyt_vm_alarm_band(const dyt_snapshot_t *snap,
                      float *lo, float *hi, float *hyst);

/* --------------------------------------------------------------- overlays */

/* Halve the brightness of every pixel whose temperature falls outside
 * [lo, hi], so the pixels that would trip the alarm stand out.
 *
 * `bgr` is w*h*3 bytes and `temps` is w*h floats with NaN where unmeasured,
 * so this is a plain buffer operation with no toolkit in it.  Returns the
 * number of pixels dimmed, or -1 on a bad argument. */
long dyt_vm_apply_isotherm(uint8_t *bgr, int w, int h,
                           const float *temps, int temps_n,
                           float lo, float hi);

/* ----------------------------------------------------------------- output */

/* Write a DYT still: the rendered picture as JPEG, with the device's own raw
 * payload and the frame geometry spliced in as APP2 segments, so a vendor
 * tool can re-render the raw data with its own palette and range (dytjpeg.h).
 *
 * The raw payload comes from the session, not from dyt_capture_last_raw():
 * that pointer is only valid on the frame callback thread.
 *
 * Returns 0 on success.  On failure returns -1 and, when `msg` is non-NULL,
 * writes why into it. */
int dyt_vm_write_still(dyt_session_t *s, const char *path, char *msg, size_t n);

/* ------------------------------------------------- capture and recording
 *
 * What a front end needs to put material on disk: where it goes, whether there
 * is room for it, and what the recording indicator says.  All of it is a pure
 * function of its arguments (plus statvfs), so it is pinned in `make check`
 * with no device and no window — which is the point, because the alternative
 * is a filename rule and a disk guard written once in the Qt app and again in
 * the reference viewer.
 */

/* `<dir>/dyt_<ts>.<ext>`, or `dyt_<ts>.<ext>` when `dir` is NULL or empty.
 *
 * `ts` is the `YYYYmmdd-HHMMSS` dyt_vm_timestamp() produces, passed in rather
 * than read here so the result is testable; `ext` is the extension without its
 * dot ("png", "dyt.jpg", "mp4").
 *
 * Returns 0, or -1 on a bad argument or a name that does not fit `n`. */
int dyt_vm_capture_name(char *out, size_t n, const char *dir, const char *ts,
                        const char *ext);

/* Bytes an unprivileged writer may still use on the filesystem holding `path`
 * (statvfs's f_bavail, not f_bfree — the root reserve is not available to us).
 * Returns -1 on a bad argument or when `path` cannot be examined. */
long long dyt_vm_free_bytes(const char *path);

/* The disk guard.  Returns 1 when `need_bytes` still fit, 0 when they do not,
 * -1 when the filesystem cannot be examined; *free_out, when non-NULL, always
 * receives the free-byte count (or -1).
 *
 * Deliberately reports rather than acts: refusing to start a clip and stopping
 * one that is already running are different decisions, and they belong to the
 * front end. */
int dyt_vm_disk_room(const char *path, long long need_bytes,
                     long long *free_out);

/* "0:07", or "1:02:03" past an hour.  A negative or non-finite `seconds`
 * reads "0:00", so a clock that has not started cannot print a negative time.
 * Returns 0, or -1 on a bad argument. */
int dyt_vm_elapsed(double seconds, char *out, size_t n);

/* The recording indicator: "REC 0:07  175 frames".  Returns the number of
 * characters that would have been written, so a caller can detect truncation,
 * or -1 on a bad argument. */
int dyt_vm_rec_label(double seconds, long long frames, char *out, size_t n);

/* ---------------------------------------------------------------- gallery
 *
 * A gallery is a directory of the material the port saved.  Listing it and
 * describing one entry is filesystem and container work with no toolkit and no
 * device, so it lives here and is pinned in `make check`.
 *
 * What is deliberately *not* here: decoding the JPEG (the front end's toolkit
 * does that) and re-rendering the thermal frame (the frame source runs the
 * pipeline, because that needs the thermometry the container does not carry).
 * What this decides is which files are worth offering and what each one holds.
 */

#define DYT_VM_NAME_CAP 256
#define DYT_VM_PATH_CAP 1024

typedef enum {
    DYT_VM_ITEM_NONE  = 0,
    DYT_VM_ITEM_STILL = 1,   /* a `.dyt.jpg` container */
    DYT_VM_ITEM_CLIP  = 2    /* an `.mp4` clip */
} dyt_vm_item_kind_t;

typedef struct {
    char               name[DYT_VM_NAME_CAP];   /* file name, no directory */
    char               path[DYT_VM_PATH_CAP];   /* as scanned */
    long long          mtime;                   /* seconds since the epoch */
    long long          bytes;
    dyt_vm_item_kind_t kind;
} dyt_vm_item_t;

/* Classify a file name by its extension: `.dyt.jpg` is a still, `.mp4` is a
 * clip, anything else (including NULL) is NONE.  The extension match is
 * case-insensitive; the name has to be longer than the extension, so `.mp4`
 * itself is not a clip. */
dyt_vm_item_kind_t dyt_vm_item_kind(const char *name);

/* Scan `dir` (NULL or "" means ".") for stills and clips and fill up to `cap`
 * entries of `out`, newest first — ties broken by name, so the order is stable
 * across runs and filesystems.
 *
 * Returns the number of matching entries *found* — which may exceed `cap`, so a
 * caller can tell it was truncated — or -1 on a bad argument or an unreadable
 * directory.  A `cap` of 0 counts without writing, so a caller can size its
 * buffer with one call and fill it with the next. */
int dyt_vm_scan(const char *dir, dyt_vm_item_t *out, int cap);

/* What a saved still turned out to hold. */
typedef struct {
    int       have_thermal;   /* the container recorded the thermal geometry */
    int       width;          /* thermal plane width, 0 when unknown */
    int       active_rows;    /* thermal plane height, 0 when unknown */
    int       total_rows;     /* payload rows, including any visible half */
    unsigned  flags;          /* DYT_DYT_FLAG_* */
    int       n_samples;      /* raw payload samples */
    long long raw_bytes;
    long long jpeg_bytes;     /* the embedded image, for a viewer to decode */
} dyt_vm_still_info_t;

/* Read a still's container and describe it.  A container the vendor wrote is
 * described too, with `have_thermal` 0 and the geometry fields zeroed: it is a
 * valid still whose JPEG can be shown, just not re-rendered thermally.
 *
 * Returns 0 on success, or -1 when the file is not a DYT container. */
int dyt_vm_still_info(const char *path, dyt_vm_still_info_t *info);

/* -------------------------------------------------------------- utilities */

/* The directory the running executable lives in, or "" when it cannot be
 * determined.  Used to find palettes/ beside the binary rather than relative
 * to the cwd. */
int dyt_vm_exe_dir(char *out, size_t n);

/* Find the palette directory: `dir_opt` first when non-NULL and non-empty,
 * then "palettes", "../palettes", and both again relative to the executable.
 * Returns 1 and fills `out` when one is readable, 0 otherwise. */
int dyt_vm_find_palette_dir(const char *dir_opt, char *out, size_t n);

/* "YYYYMMDD-HHMMSS" in local time, for a filename.  Returns 0 on success. */
int dyt_vm_timestamp(char *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* DYT_VIEW_MODEL_H */
