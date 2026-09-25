/*
 * recorder.h — write the port's rendered frames to mp4.
 *
 * Recording is muxing and codec I/O, not device logic, so it stays out of
 * libdyt: putting it in the library would drag OpenCV into everything that
 * links it and cost libdyt its no-dependency, `make check`-able property.
 *
 * It is also not *tool* logic, which is why it lives here rather than inside
 * one of them.  Two front ends now record — `dytrec` on the command line and
 * the Qt app's `v` key — and they must not disagree about the container, the
 * pixel order or the frame rate.  So the writer is one object both drive.
 *
 * The writer is opened from the *first* frame's geometry, which is the only
 * point where the size is known on the live path: the session does not know
 * the sensor's dimensions until a frame has arrived.
 *
 * Frames go in as RGB — what `dyt_session_render_rgb()` produces — and the
 * BGR swap OpenCV wants happens in here, so no caller has to remember it.
 *
 * The codec is OpenCV's, reached through cv::VideoWriter; its bundled ffmpeg
 * writes real H.264 mp4 on this host (verified — see the Phase 7 note in
 * RE Docs 09).  "mp4v" is offered for players that dislike H.264.
 *
 * build:  via the Makefile (needs OpenCV)
 */
#ifndef DYT_RECORDER_H
#define DYT_RECORDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyt_recorder dyt_recorder_t;

/* Create a recorder writing `path` at `fps` with the four-character `codec`
 * ("avc1", "mp4v", …).  Nothing is created on disk until the first frame
 * arrives and the geometry is known, so a bad argument is refused here and a
 * bad codec only shows up at the first dyt_recorder_write().
 *
 * Returns NULL on a bad argument or allocation failure. */
dyt_recorder_t *dyt_recorder_open(const char *path, double fps,
                                  const char *codec);

/* Write one tightly-packed RGB frame (w*h*3 bytes).  Opens the writer on the
 * first call.  Returns 0 on success, -1 on a bad argument, an unusable codec,
 * or a write failure — the reason is on stderr. */
int dyt_recorder_write(dyt_recorder_t *r, const uint8_t *rgb, int w, int h);

/* Frames written so far.  0 when the writer has not opened yet. */
long long dyt_recorder_frames(const dyt_recorder_t *r);

/* Finalise and release.  Returns the number of frames written, or -1 when
 * nothing was written (the writer never opened) — so a caller can tell "a clip
 * with no frames in it" from a real one.  Safe on NULL. */
long long dyt_recorder_close(dyt_recorder_t *r);

#ifdef __cplusplus
}
#endif

#endif /* DYT_RECORDER_H */
