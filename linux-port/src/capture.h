/*
 * capture.h — live UVC capture for the DYT thermal camera Linux port.
 *
 * Wires upstream libuvc (vendored under third_party/libuvc) into the
 * byte-verified frame→temperature pipeline in frame.c.  The vendor's own
 * stack is a modified libuvc, so this mirrors it rather than going
 * through V4L2 — the kernel uvcvideo driver would claim the interface
 * and block the libusb vendor control transfers the protocol needs.
 *
 * Two constraints shape the API:
 *
 *  1. libuvc forbids calling any uvc_* function from inside a frame
 *     callback.  The auto-shutter (FFC) trigger is therefore *deferred*:
 *     the callback calls dyt_capture_request_shutter(), and a separate
 *     control thread issues the uvc_set_zoom_abs() write.  See
 *     RE Docs 04 §4.5.5.
 *
 *  2. Which UVC bFormatIndex carries the thermal stream is not known
 *     until hardware is attached, so the format is selected at runtime
 *     (uncompressed 16-bpp preferred) with CLI overrides available.
 *     dyt_capture_print_diag() dumps everything libuvc parsed.
 */
#ifndef DYT_CAPTURE_H
#define DYT_CAPTURE_H

#include <stdint.h>

#include "frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque capture handle. */
typedef struct dyt_capture dyt_capture_t;

/* Frame delivery callback.  `temps` uses the vendor's layout:
 *    mode 0x44c — 10-float header + width*active_height pixel floats
 *    mode 1000  — width*active_height floats, no header
 * `n` is the total number of floats.  Called on libuvc's callback
 * thread; must not call any uvc_* API and must not retain `temps`
 * (the buffer is reused for the next frame). */
typedef void (*dyt_frame_cb_t)(const float *temps, int n,
                               int width, int active_height, void *user);

typedef struct {
    uint16_t vid, pid;        /* 0,0 = first matching DYT/Realtek device */
    int  format_index;        /* 0 = auto (uncompressed 16-bpp) */
    int  width, height, fps;  /* 0 = auto */
    float t_amb;              /* ambient for the LUT (default 25.0) */
    int  sensor_mode;         /* 0x44 or 0x82 (default 0x82) */
    int  fix_mode;            /* 0x78 enables GetFix (default 0 = off) */
    int  send_start_orders;   /* send the vendor's one bring-up order —
                               * setTinyCOutputADValue — AFTER streaming has
                               * started (default 0).  Required on the
                               * 0bda:5840 unit, which otherwise streams a
                               * flat 0x8000 placeholder.  Ordering matters:
                               * sent before the stream exists the device
                               * latches status 0x0e and never recovers until
                               * a replug.  See MechaniscoutPcap/4.pcapng. */
} dyt_capture_opts;

/* Fill *o with the defaults. */
void dyt_capture_opts_default(dyt_capture_opts *o);

/* Open the device and negotiate the stream format, but do not start
 * streaming.  Returns 0 on success, negative on failure (messages on
 * stderr).  On success *out owns the handle; release with
 * dyt_capture_close(). */
int dyt_capture_open(dyt_capture_t **out, const dyt_capture_opts *o);

/* Begin streaming; `cb` is invoked per frame.  Returns 0 on success. */
int dyt_capture_start(dyt_capture_t *c, dyt_frame_cb_t cb, void *user);

/* Ask for a flat-field correction (shutter) on the next control-thread
 * tick.  Safe to call from the frame callback.  Returns 0. */
int dyt_capture_request_shutter(dyt_capture_t *c);

/* Stop streaming (idempotent).  Does not free the handle. */
void dyt_capture_stop(dyt_capture_t *c);

/* Stop if needed and free everything. */
void dyt_capture_close(dyt_capture_t *c);

/* Device mode derived from VID/PID (RE Docs 04 §4.10). */
dyt_mode_t dyt_capture_mode(const dyt_capture_t *c);

/* Negotiated geometry.  Any output pointer may be NULL.  width/active
 * are only meaningful once a frame has been seen (0 before that). */
void dyt_capture_geometry(const dyt_capture_t *c, int *width, int *active_height);

/* The raw uint16 samples behind the frame currently being delivered.
 * Intended to be called from inside the frame callback (same thread, so
 * no race); the pointer is only valid until the next frame.  Returns
 * NULL before the first frame.  *n_samples receives the sample count. */
const uint16_t *dyt_capture_last_raw(const dyt_capture_t *c, int *n_samples);

/* Dump every format/frame/fps libuvc parsed — the on-arrival answer to
 * "which bFormatIndex is the thermal stream?". */
void dyt_capture_print_diag(dyt_capture_t *c);

#ifdef __cplusplus
}
#endif

#endif /* DYT_CAPTURE_H */
