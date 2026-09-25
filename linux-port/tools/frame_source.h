/*
 * frame_source.h — where a recorder's frames come from.
 *
 * Phase 7 records mp4.  A recorder needs one thing: a stream of rendered RGB
 * frames.  Where they come from differs — the device, or a frozen .raw fixture
 * replayed through the same device-free pipeline — and that difference should
 * not leak into the recorder.  So it lives behind this interface.
 *
 * The fixture path is the important one.  It does not merely feed pixels: it
 * runs the *real* path, `dyt_pipeline_resolve/frame` (frame.c) plus
 * `dyt_visible_extract` (visible.c), and installs the results with
 * `dyt_session_process()` / `dyt_session_process_visible()` exactly as
 * `session_capture.c` does for a live frame.  So an offline recording exercises
 * the same conversion, the same visible-half extraction and the same render as
 * the live one — it just gets its payload from a file.  That is what makes
 * `dytrec --selftest` a meaningful check with no hardware attached.
 *
 * The live path is deliberately thinner: the capture adapter
 * (session_capture.c) is already filling the session from libuvc's callback
 * thread, so `next()` only has to render the session's latest state.
 *
 * A saved still is a third source (the gallery's): it opens a `.dyt.jpg`
 * container, recovers its raw payload, and replays it through the *same*
 * pipeline as the fixture — so a still on screen was rendered exactly the way a
 * live frame is, not by a second path that could disagree.
 *
 * Frames come out as RGB (what `dyt_session_render_rgb()` produces), not BGR —
 * the recorder, which is the only OpenCV-aware layer, does that swap.
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -I../src -c frame_source.c
 */
#ifndef DYT_FRAME_SOURCE_H
#define DYT_FRAME_SOURCE_H

#include <stdint.h>

#include "frame.h"
#include "session.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyt_frame_source dyt_frame_source_t;

/* What dyt_frame_source_next() did.  The WAIT/END split matters: a live source
 * returns WAIT until libuvc has delivered something, which is normal and must
 * not be mistaken for "the recording finished". */
typedef enum {
    DYT_FS_FRAME = 1,    /* a frame is available: rgb, w and h are all set */
    DYT_FS_WAIT  = 0,    /* nothing yet — call again (live, pre-first-frame) */
    DYT_FS_END   = -1,   /* the fixture's frame budget is spent */
    DYT_FS_ERROR = -2    /* failure; the message is already on stderr */
} dyt_fs_status_t;

/* Replay a raw device payload from `path` through the device-free pipeline.
 *
 * `path` holds `width * total` little-endian uint16 samples, exactly as the
 * device streamed them (the same files testdata/README.md describes).  The
 * geometry is resolved from the byte count by dyt_pipeline_resolve(), so a
 * `width`/`plane` mismatch with the file is a clean open failure rather than a
 * misread frame.
 *
 * `mode`/`plane`/`t_amb`/`sensor_mode`/`fix_mode` mirror dyt_capture_opts, so a
 * fixture is configured the way the live device would be.  `limit` is how many
 * frames to produce before DYT_FS_END; <= 0 means replay forever (the caller
 * then bounds the recording itself).
 *
 * The fixture frame is replayed as-is each time — one file is one frame — so a
 * recording made from a single fixture is a still held for its duration.  That
 * is enough to pin the container and the pipeline; motion needs a real device
 * or a multi-frame fixture.
 *
 * Returns NULL on a bad argument, an unreadable file, or a geometry mismatch.
 */
dyt_frame_source_t *dyt_frame_source_open_fixture(
    dyt_session_t *sess, const char *path, int width, dyt_mode_t mode,
    dyt_plane_t plane, float t_amb, int sensor_mode, int fix_mode,
    long long limit);

/* Render whatever the capture adapter has most recently installed in `sess`.
 * `sess` must outlive the source; the capture handle and adapter are owned by
 * the caller (see session_capture.h for the wiring order). */
dyt_frame_source_t *dyt_frame_source_open_live(dyt_session_t *sess);

/* Open a saved still (a `.dyt.jpg` container) as a frame source, so the gallery
 * shows it through the same pipeline as a live frame instead of a second render
 * path.  The payload is replayed rather than consumed: a still is one frame
 * shown for as long as the caller looks at it.
 *
 * The container's recorded geometry supplies the width when it has one.  A
 * container the vendor wrote has no geometry record, so `fallback_width` (the
 * width the app is configured with) is used and the row count follows from the
 * payload's length, exactly as it does for a fixture file.
 *
 * Returns NULL on a bad argument, a file that is not a DYT still, or a payload
 * that does not resolve — including a vendor still with no width to fall back
 * on. */
dyt_frame_source_t *dyt_frame_source_open_still(
    dyt_session_t *sess, const char *path, int fallback_width, dyt_mode_t mode,
    dyt_plane_t plane, float t_amb, int sensor_mode, int fix_mode);

/* Produce the next frame.
 *
 * On DYT_FS_FRAME, `rgb` points at the source's own buffer — width * height * 3
 * bytes, RGB, valid until the next call or dyt_frame_source_close() — and `w`
 * and `h` receive its dimensions.  The source grows that buffer itself, so the
 * caller never has to guess a frame size up front (which matters on the live
 * path, where the geometry is not known until the first frame arrives).  All
 * three outputs are left untouched on any other return.  See dyt_fs_status_t. */
dyt_fs_status_t dyt_frame_source_next(dyt_frame_source_t *fs,
                                      const uint8_t **rgb, int *w, int *h);

/* Frames produced so far.  Cheap, and valid for both kinds. */
long long dyt_frame_source_produced(const dyt_frame_source_t *fs);

/* The frame's temperature plane as of the last DYT_FS_FRAME, copied under the
 * session lock.  Returns the number of floats written (0 if none, negative if
 * `cap` is too small).  Provided so a caller can assert on the data, not just
 * the pixel count — `--selftest` uses it to prove the fixture actually
 * converted rather than replaying the filler. */
int dyt_frame_source_temps(dyt_frame_source_t *fs, float *out, int cap);

void dyt_frame_source_close(dyt_frame_source_t *fs);

#ifdef __cplusplus
}
#endif

#endif /* DYT_FRAME_SOURCE_H */
