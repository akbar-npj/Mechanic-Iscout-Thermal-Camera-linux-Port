/*
 * session_capture.h — the adapter between the live capture layer and the
 * device-free session.
 *
 * dyt_capture_t delivers frames to a plain C callback whose only context is a
 * single `void *`, and it offers the raw payload (dyt_capture_last_raw) and
 * the payload's plane geometry (dyt_capture_plane_geometry) separately from
 * the converted temperatures.  Turning that into
 *
 *     dyt_session_process()  +  dyt_session_process_visible()
 *
 * is a small amount of geometry arithmetic plus a YUYV luma extraction, and
 * it is exactly the same work for the OpenCV viewer and for the Qt6 app.  It
 * lives here so there is one copy — this is the "separate adapter" that
 * session.h refers to.
 *
 * Device-free in the sense that matters for `make check`: it calls only the
 * port's own capture API, never libusb or uvc_*, and it builds and links with
 * or without DYT_HAVE_LIBUSB.
 *
 * usage:
 *     dyt_session_capture_t *sc = dyt_session_capture_create(sess);
 *     dyt_session_capture_set_capture(sc, cap);
 *     dyt_capture_start(cap, dyt_session_capture_on_frame, sc);
 *     ...
 *     dyt_session_capture_free(sc);
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -I. -c session_capture.c
 */
#ifndef DYT_SESSION_CAPTURE_H
#define DYT_SESSION_CAPTURE_H

#include "capture.h"
#include "session.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyt_session_capture dyt_session_capture_t;

/* Wrap `s`.  The session is borrowed, not owned: free it yourself, and only
 * after the adapter.  Returns NULL on a NULL session or out of memory. */
dyt_session_capture_t *dyt_session_capture_create(dyt_session_t *s);

/* Free the adapter's scratch buffers.  Safe on NULL. */
void dyt_session_capture_free(dyt_session_capture_t *sc);

/* Point the adapter at the capture handle whose payload carries the visible
 * half.  Optional: without it the adapter feeds temperatures only, which is
 * all the AD output mode has.  The handle is borrowed, and must stay open
 * while frames are being delivered.  Returns 0, or -1 on a NULL argument. */
int dyt_session_capture_set_capture(dyt_session_capture_t *sc, dyt_capture_t *c);

/* The dyt_frame_cb_t to hand to dyt_capture_start().  Runs on libuvc's
 * callback thread: it does no I/O and never calls a uvc_* function. */
void dyt_session_capture_on_frame(const float *temps, int n, int width,
                                  int active_height, void *user);

#ifdef __cplusplus
}
#endif

#endif /* DYT_SESSION_CAPTURE_H */
