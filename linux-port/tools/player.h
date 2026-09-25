/*
 * player.h — read the mp4 clips the port writes back into RGB frames.
 *
 * The mirror of recorder.h, and it lives here for the same reason: decoding is
 * codec I/O, not device logic, so it stays out of libdyt and keeps the library
 * free of OpenCV.  It is also not *tool* logic — the Qt app's gallery plays
 * clips, and a command-line front end might later too — so the reader is one
 * object both can drive, exactly as the writer is.
 *
 * Frames come out as RGB — what dyt_session_render_rgb() produces and what the
 * rest of the port speaks — and the BGR swap OpenCV hands back happens in here,
 * so no caller has to remember it.  The writer does the opposite swap on the
 * way in; the two are a pair and must stay symmetric, or a clip would come back
 * with red and blue exchanged.
 *
 * The decoder is OpenCV's, reached through cv::VideoCapture and its bundled
 * ffmpeg.  It reads what the writer produces and, in practice, anything else
 * ffmpeg can demux.
 *
 * dyt_player_next() loops: at the end of the stream it rewinds and delivers the
 * first frame again, so a caller playing a short clip does not have to own the
 * wrap-around.  A caller that wants to stop at the end compares the frame index
 * it reports against dyt_player_count().
 *
 * build:  via the Makefile (needs OpenCV)
 */
#ifndef DYT_PLAYER_H
#define DYT_PLAYER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyt_player dyt_player_t;

/* Open `path` for reading.  Returns NULL when the file cannot be opened or
 * carries no video stream — a still image, a truncated file or a missing path
 * all fail here rather than at the first dyt_player_next(). */
dyt_player_t *dyt_player_open(const char *path);

/* The frame geometry, for sizing the caller's buffer.  Returns 0 and writes
 * both, or -1 on a NULL argument. */
int dyt_player_size(const dyt_player_t *p, int *w, int *h);

/* The stream's frame rate.  0.0 when the container does not say. */
double dyt_player_fps(const dyt_player_t *p);

/* Frames in the stream, or 0 when the container does not say — so a caller can
 * show a position without having to know whether the total is trustworthy. */
long long dyt_player_count(const dyt_player_t *p);

/* Decode the next frame into `rgb` as tightly-packed RGB (w*h*3 bytes, w and h
 * from dyt_player_size()).  At the end of the stream this rewinds and delivers
 * the first frame, so it keeps returning frames.  `pos`, when non-NULL,
 * receives the 0-based index of the frame just delivered.
 *
 * Returns 0 on success, -1 on a bad argument, a buffer too small for the frame,
 * or a decode failure.  The reason is on stderr. */
int dyt_player_next(dyt_player_t *p, uint8_t *rgb, int cap, long long *pos);

/* Release the decoder.  Safe on NULL. */
void dyt_player_close(dyt_player_t *p);

#ifdef __cplusplus
}
#endif

#endif /* DYT_PLAYER_H */
