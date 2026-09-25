/*
 * capture.h — live UVC capture for the DYT thermal camera Linux port.
 *
 * Wires upstream libuvc (vendored under third_party/libuvc) into the
 * byte-verified frame→temperature pipeline in frame.c.  The vendor's own
 * stack is a modified libuvc, so this mirrors it rather than going
 * through V4L2: the vendor control protocol (the FFC trigger, and the
 * setTinyCOutputADValue mode switch) has no V4L2 equivalent.
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
#include "params.h"

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

/* Which of the device's two output modes to run.
 *
 * The mode determines both the frame geometry and whether the vendor order
 * is sent, so it is one setting rather than two.  Measured 2026-09-24 on the
 * 0bda:5840 unit:
 *
 *   DYT_OUTPUT_DEFAULT  256x384 — top half is the device's grayscale visible
 *                       image, bottom half is the thermal plane.  No vendor
 *                       order needed; the thermal plane is present as soon as
 *                       the ~6 s filler clears.  This is the layout the
 *                       Topdon/InfiRay family's own apps consume.
 *
 *   DYT_OUTPUT_AD       256x192 — the whole frame is raw 16-bit AD, but only
 *                       after setTinyCOutputADValue is sent *once streaming
 *                       has started*.  Sending it earlier latches the device
 *                       at status 0x0e until a replug (MechaniscoutPcap/
 *                       4.pcapng).  In this mode the 256x384 frame is a flat
 *                       placeholder, so the mode really is a mode switch and
 *                       not just an enable.
 *
 * Both modes carry the same thermal data (A-B-A interleave, 2026-09-24), so
 * this only chooses where the thermal rows are read from. */
typedef enum {
    DYT_OUTPUT_DEFAULT = 0,   /* device's own dual-half frame; no vendor order */
    DYT_OUTPUT_AD             /* send the AD order; flat raw-AD frame */
} dyt_output_t;

typedef struct {
    uint16_t vid, pid;        /* 0,0 = first matching DYT/Realtek device */
    int  format_index;        /* 0 = auto (uncompressed 16-bpp) */
    int  width, height, fps;  /* 0 = auto (see pick_format for what auto means) */
    float t_amb;              /* ambient for the LUT (default 25.0) */
    int  sensor_mode;         /* 0x44 or 0x82 (default 0x82) */
    int  fix_mode;            /* 0x78 enables GetFix (default 0 = off) */
    dyt_output_t output;      /* default DYT_OUTPUT_DEFAULT */
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
 * NULL before the first frame.  *n_samples receives the sample count.
 *
 * This is the *whole streamed payload* (width x total), not the thermal
 * plane — in DYT_OUTPUT_DEFAULT that includes the visible top half.  Use
 * dyt_capture_geometry() for the thermal height. */
const uint16_t *dyt_capture_last_raw(const dyt_capture_t *c, int *n_samples);

/* Where the thermal plane sits inside that payload: the payload's total row
 * count and the thermal plane's first row and height.  Read-only, and all
 * zero before the first frame.
 *
 * In DYT_OUTPUT_DEFAULT the rows *above* plane_y are the visible half, so
 * this plus dyt_capture_last_raw() is everything needed to extract it (see
 * src/visible.h).  In DYT_OUTPUT_AD plane_y is 0 and plane_h == total.
 * Any output pointer may be NULL. */
void dyt_capture_plane_geometry(const dyt_capture_t *c, int *total,
                                int *plane_y, int *plane_h);

/* Dump every format/frame/fps libuvc parsed — the on-arrival answer to
 * "which bFormatIndex is the thermal stream?". */
void dyt_capture_print_diag(dyt_capture_t *c);

/* ---------------------------------------------------------- device info
 *
 * The read-only identity and stored-parameter dump (RE Docs 04 §4.8,
 * verified live 2026-09-25).  All three reads are safe: none of them
 * writes device state.
 */
typedef struct {
    int      have_sn;              /* module serial read succeeded */
    uint8_t  sn[16];               /* raw 16-byte record */
    char     sn_str[17];           /* printable prefix, NUL-terminated */
    int      sn_len;               /* strlen(sn_str), -1 if not read */

    int      have_usn;             /* raw user serial read succeeded */
    uint8_t  usn_raw[15];          /* raw 15-byte user serial */
    uint8_t  usn_key;              /* key byte derived from the module serial */
    char     usn_str[16];          /* DecryptSNE output, printable */
    int      usn_len;              /* strlen(usn_str), -1 if not decoded */
    int      usn_variant;          /* decoded serial's variant flag (SN[2]=='C') */

    dyt_params_t     params;
    dyt_radiometry_t radio;
    int      params_read;          /* slots read, -1 if the read did not run */
} dyt_device_info_t;

/* Fill *out with the device's serial and stored parameters.
 *
 * Needs the device open (dyt_capture_open) but must be called *before*
 * dyt_capture_start: measured 2026-09-25 the identity reads only answer
 * cleanly while the device is idle — with the isochronous stream running
 * the same reads return a partial block and a failed serial.  This also
 * matches the vendor, which reads the parameter block at connect.
 *
 * It is a handful of control transfers, so do not call it per frame.
 *
 * Returns 0 if the transfers were issued (individual reads may still have
 * failed — check the have_* flags and params_read), -1 if the handle is not
 * usable. */
int dyt_capture_read_info(dyt_capture_t *c, dyt_device_info_t *out);

/* --------------------------------------------------- runtime parameter write
 *
 * Send one vendor `sendOrder(type, value)` — the *only* device write the port
 * implements (RE Docs 04 §4.2).  This sets a runtime parameter
 * (emissivity / ambient / reflected / distance); it is NOT calibration and
 * cannot touch factory data.
 *
 * `type` is a dyt_order_type_t (params.h); `value` is in that type's natural
 * unit.  Call it from the owning thread — never from a frame callback.
 *
 * The call blocks for DYT_ORDER_SETTLE_US after a successful order.  Measured
 * 2026-09-25: two orders sent back-to-back lose the first, so consecutive
 * calls must be spaced.  Setting several parameters is therefore a few
 * hundred ms of work, not instantaneous — do it off the render path.
 *
 * Returns 0, or a negative dyt_write_param / range error.  Verify a write by
 * reading the same index back with dyt_read_param(). */
int dyt_capture_set_param(dyt_capture_t *c, int type, float value);

/* Read one stored parameter slot back — the same index dyt_capture_set_param()
 * writes (params.h).  This is how a write is verified: set_param's return says
 * the transfer was issued, not that the device stored the value.
 *
 * `index` is a dyt_order_type_t (1..4).  On success writes the raw 16-bit slot
 * value to *raw and returns DYT_READ_OK; otherwise a negative dyt_read_param
 * error, with *raw untouched.
 *
 * A handful of control transfers, so never from a frame callback — the same
 * rule as set_param, and it runs on the caller's thread for the same reason
 * (libusb serialises a control transfer against the isochronous stream).
 * Unlike the identity reads it is a single slot and so does not need the
 * device idle; but the device is known to drop most slots when the whole block
 * is read while streaming (capture.c), so a failure means "unverified", not
 * "the write failed". */
int dyt_capture_read_param(dyt_capture_t *c, int index, uint16_t *raw);

/* Settle gap enforced after each successful write order (µs). */
#define DYT_ORDER_SETTLE_US 250000

#ifdef __cplusplus
}
#endif

#endif /* DYT_CAPTURE_H */
