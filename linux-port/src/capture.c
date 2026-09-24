/*
 * capture.c — live UVC capture on top of the vendored upstream libuvc.
 *
 * Mirrors the vendor's own stack (a modified libuvc), because the kernel
 * uvcvideo driver would claim the interface and block the libusb vendor
 * control transfers the protocol needs (RE Docs 04 §4.1, 08 §8.2).
 *
 * Threading model
 * ---------------
 * libuvc delivers frames on its own callback thread and forbids calling
 * any uvc_* function from within a frame callback.  So:
 *
 *   libuvc cb thread ──frame_cb──▶ copy frame, run the verified pipeline,
 *                                  raise a shutter flag if FFC is due
 *
 *   control thread   ──ctrl_thread──▶ wait for the flag, then issue
 *                                     uvc_set_zoom_abs() (RE Docs 04 §4.5.5)
 *
 * The flag is guarded by a mutex + condvar so dyt_capture_request_shutter()
 * is safe to call from the callback.
 *
 * The live UVC stack needs libusb (libuvc is built on it), so when this
 * file is compiled without -DDYT_HAVE_LIBUSB every entry point returns
 * "not supported" — see the #else at the bottom.  That keeps the
 * byte-verified pipeline and its tests buildable on a machine with just a
 * C compiler; only capture_demo and probe (which talk to real hardware)
 * are omitted in that configuration.
 *
 * build:  via the Makefile (make)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "capture.h"
#include "control.h"

#ifdef DYT_HAVE_LIBUSB
#include <libusb.h>
#include <libuvc/libuvc.h>

/* RE Docs 04 §4.5.5 writes uvc_set_zoom_abs(cam, 0xffff8000).  Upstream
 * libuvc's signature takes uint16_t, so that value truncates to 0x8000. */
#define DYT_FFC_ZOOM 0x8000

struct dyt_capture {
    uvc_context_t       *ctx;
    uvc_device_t        *dev;
    uvc_device_handle_t *devh;
    struct libusb_device_handle *usb;
    uvc_stream_ctrl_t    ctrl;

    dyt_mode_t mode;

    /* negotiated stream format */
    int fmt_index, frame_index, fmt_w, fmt_h;

    /* device-independent per-frame pipeline (geometry + LUT policy +
     * conversion) — see frame.h / dyt_pipeline_* */
    dyt_pipeline_t pipe;
    int err;

    /* pipeline buffers */
    uint16_t *staging;
    float    *lut;
    float    *out;

    /* params */
    int send_start_orders;

    /* auto-shutter state */
    uint16_t last_ref;
    pthread_mutex_t m;
    pthread_cond_t  cv;
    int shutter_req;
    int run;
    pthread_t tid;
    int tid_valid;

    /* user callback */
    dyt_frame_cb_t cb;
    void          *user;
    int streaming;
};

/* ------------------------------------------------------------------ helpers */

/* Find the VideoStreaming interface number (class 0x0e, subclass 0x02)
 * from the libusb config descriptor.  Needed only for the manual ctrl
 * fallback when a vendor GUID defeats libuvc's GUID matching. */
static int vs_interface_number(struct libusb_device_handle *h)
{
    struct libusb_config_descriptor *cfg = NULL;
    int i, a, found = -1;

    if (libusb_get_config_descriptor(libusb_get_device(h), 0, &cfg) != 0)
        return -1;

    for (i = 0; i < cfg->bNumInterfaces && found < 0; i++)
        for (a = 0; a < cfg->interface[i].num_altsetting; a++) {
            const struct libusb_interface_descriptor *id =
                &cfg->interface[i].altsetting[a];
            if (id->bInterfaceClass == 0x0e && id->bInterfaceSubClass == 0x02) {
                found = id->bInterfaceNumber;
                break;
            }
        }

    libusb_free_config_descriptor(cfg);
    return found;
}

/* Pick the frame interval (100 ns units) closest to the requested fps. */
static uint32_t pick_interval(const uvc_frame_desc_t *f, int fps)
{
    if (f->intervals) {
        const uint32_t *p;
        for (p = f->intervals; *p; ++p)
            if (fps == 0 || (int)(10000000u / *p) == fps)
                return *p;
        return f->intervals[0];
    }
    if (fps > 0) {
        uint32_t iv = 10000000u / (uint32_t)fps;
        if (iv >= f->dwMinFrameInterval && iv <= f->dwMaxFrameInterval)
            return iv;
    }
    return f->dwDefaultFrameInterval;
}

/* Choose the thermal format and one of its frame descriptors.
 * Preference: an explicit bFormatIndex, else the first uncompressed
 * 16-bpp format (raw16), else the first uncompressed format. */
static int pick_format(uvc_device_handle_t *devh, const dyt_capture_opts *o,
                       int *fmt_index, int *frame_index,
                       int *fw, int *fh)
{
    const uvc_format_desc_t *f, *best = NULL;
    const uvc_frame_desc_t  *fr, *bestfr = NULL;

    for (f = uvc_get_format_descs(devh); f; f = f->next) {
        if (o->format_index) {
            if (f->bFormatIndex == o->format_index) { best = f; break; }
        } else if (f->bDescriptorSubtype == UVC_VS_FORMAT_UNCOMPRESSED &&
                   f->bBitsPerPixel == 16) {
            best = f;
            break;
        }
    }
    if (!best && !o->format_index)
        for (f = uvc_get_format_descs(devh); f; f = f->next)
            if (f->bDescriptorSubtype == UVC_VS_FORMAT_UNCOMPRESSED) { best = f; break; }
    if (!best)
        return -1;

    /* Frame: explicit size, else the default frame, else the first.  Either
     * axis may be given alone, so `--height 384` selects by height without
     * also needing `--width`. */
    for (fr = best->frame_descs; fr; fr = fr->next) {
        if (o->width || o->height) {
            if ((!o->width  || fr->wWidth  == o->width) &&
                (!o->height || fr->wHeight == o->height)) { bestfr = fr; break; }
        } else if (fr->bFrameIndex == best->bDefaultFrameIndex) {
            bestfr = fr;
            break;
        }
    }
    if (!bestfr)
        bestfr = best->frame_descs;
    if (!bestfr)
        return -1;

    *fmt_index   = best->bFormatIndex;
    *frame_index = bestfr->bFrameIndex;
    *fw          = bestfr->wWidth;
    *fh          = bestfr->wHeight;
    return 0;
}

/* -------------------------------------------------------------------- open */

int dyt_capture_open(dyt_capture_t **out, const dyt_capture_opts *o)
{
    dyt_capture_t *c;
    uvc_error_t err;
    uvc_device_descriptor_t *dd = NULL;
    uint16_t vid, pid;
    int rc = -1;

    *out = NULL;
    c = calloc(1, sizeof *c);
    if (!c) return -1;

    pthread_mutex_init(&c->m, NULL);
    pthread_cond_init(&c->cv, NULL);

    err = uvc_init(&c->ctx, NULL);
    if (err != UVC_SUCCESS) {
        fprintf(stderr, "capture: uvc_init: %s\n", uvc_strerror(err));
        goto fail;
    }

    /* No VID/PID given: choose the first enumerated device that is actually
     * one of ours, not merely the first UVC device libuvc returns.  A machine
     * that also has a webcam or the 4K microscope camera attached would
     * otherwise hand us that device and fail with a confusing "not a
     * supported device".  Supported IDs are the RE Docs 04 §4.10 table. */
    vid = o->vid;
    pid = o->pid;
    if (!vid && !pid) {
        uvc_device_t **list = NULL;

        if (uvc_get_device_list(c->ctx, &list) == UVC_SUCCESS && list) {
            int i;
            for (i = 0; list[i]; i++) {
                uvc_device_descriptor_t *d = NULL;

                if (uvc_get_device_descriptor(list[i], &d) != UVC_SUCCESS)
                    continue;
                if (dyt_mode_for_vidpid(d->idVendor, d->idProduct) != DYT_MODE_0) {
                    vid = d->idVendor;
                    pid = d->idProduct;
                    uvc_free_device_descriptor(d);
                    break;
                }
                uvc_free_device_descriptor(d);
            }
            uvc_free_device_list(list, 1);
        }

        if (!vid && !pid)
            fprintf(stderr, "capture: no supported DYT device attached — "
                            "run the probe tool to list devices\n");
    }

    err = uvc_find_device(c->ctx, &c->dev, vid, pid, NULL);
    if (err != UVC_SUCCESS) {
        fprintf(stderr, "capture: no device found (%04x:%04x): %s\n"
                        "         run the probe tool to list devices\n",
                vid, pid, uvc_strerror(err));
        goto fail;
    }

    err = uvc_get_device_descriptor(c->dev, &dd);
    if (err != UVC_SUCCESS) {
        fprintf(stderr, "capture: get_device_descriptor: %s\n", uvc_strerror(err));
        goto fail;
    }
    vid = dd->idVendor;
    pid = dd->idProduct;
    uvc_free_device_descriptor(dd);
    dd = NULL;

    c->mode = dyt_mode_for_vidpid(vid, pid);
    if (c->mode == DYT_MODE_0) {
        fprintf(stderr, "capture: %04x:%04x is not a supported device "
                        "(RE Docs 04 §4.10)\n", vid, pid);
        goto fail;
    }
    if (c->mode == DYT_MODE_3EB) {
        fprintf(stderr, "capture: mode 0x3eb has no known thermometry path yet "
                        "(RE Docs 08 §8.5)\n");
        goto fail;
    }

    err = uvc_open(c->dev, &c->devh);
    if (err != UVC_SUCCESS) {
        fprintf(stderr, "capture: uvc_open: %s (permissions? add a udev rule)\n",
                uvc_strerror(err));
        goto fail;
    }
    c->usb = uvc_get_libusb_handle(c->devh);

    if (pick_format(c->devh, o, &c->fmt_index, &c->frame_index,
                    &c->fmt_w, &c->fmt_h) != 0) {
        fprintf(stderr, "capture: no usable streaming format found — "
                        "run dyt_capture_print_diag() to see what the device offers\n");
        goto fail;
    }

    /* Negotiate.  libuvc matches formats by GUID; UVC_FRAME_FORMAT_ANY
     * expands to every known uncompressed/compressed GUID, which covers a
     * standard Y16 (raw16) thermal stream.  A vendor GUID maps to UNKNOWN
     * and matches nothing, so fall back to building the ctrl directly —
     * negotiation only compares bFormatIndex/bFrameIndex. */
    {
        const uvc_format_desc_t *f = uvc_get_format_descs(c->devh);
        enum uvc_frame_format ff = UVC_FRAME_FORMAT_ANY;
        for (; f; f = f->next)
            if (f->bFormatIndex == c->fmt_index) {
                /* libuvc's signature takes a non-const guid; copy so we
                 * never cast away const on the descriptor. */
                uint8_t guid[16];
                enum uvc_frame_format g;
                memcpy(guid, f->guidFormat, sizeof guid);
                g = uvc_frame_format_for_guid(guid);
                if (g != UVC_FRAME_FORMAT_UNKNOWN) ff = g;
                break;
            }

        err = uvc_get_stream_ctrl_format_size(c->devh, &c->ctrl, ff,
                                              c->fmt_w, c->fmt_h, o->fps);
        if (err != UVC_SUCCESS) {
            int vsif = vs_interface_number(c->usb);
            const uvc_frame_desc_t *fr = NULL, *g;
            for (f = uvc_get_format_descs(c->devh); f; f = f->next)
                if (f->bFormatIndex == c->fmt_index)
                    for (g = f->frame_descs; g; g = g->next)
                        if (g->bFrameIndex == c->frame_index) { fr = g; break; }

            if (!fr || vsif < 0) {
                fprintf(stderr, "capture: cannot negotiate stream (%s)\n",
                        uvc_strerror(err));
                goto fail;
            }
            memset(&c->ctrl, 0, sizeof c->ctrl);
            c->ctrl.bInterfaceNumber = (uint8_t)vsif;
            c->ctrl.bmHint          = 1;   /* don't negotiate the interval */
            c->ctrl.bFormatIndex    = (uint8_t)c->fmt_index;
            c->ctrl.bFrameIndex     = (uint8_t)c->frame_index;
            c->ctrl.dwFrameInterval = pick_interval(fr, o->fps);
            err = uvc_probe_stream_ctrl(c->devh, &c->ctrl);
            if (err != UVC_SUCCESS) {
                fprintf(stderr, "capture: probe_stream_ctrl: %s\n", uvc_strerror(err));
                goto fail;
            }
        }
    }

    c->send_start_orders = o->send_start_orders;

    /* The LUT is 64 KiB; allocate it once and hand it to the pipeline, which
     * fills it lazily from the first valid 0x44c frame. */
    c->lut = malloc(LUT_N * sizeof(float));
    if (!c->lut) {
        fprintf(stderr, "capture: out of memory\n");
        goto fail;
    }
    dyt_pipeline_init(&c->pipe, c->mode, o->t_amb, o->sensor_mode,
                      o->fix_mode, c->lut);

    fprintf(stderr, "capture: %04x:%04x mode %s, format %d frame %d, %dx%d\n",
            vid, pid,
            c->mode == DYT_MODE_44C ? "0x44c" :
            c->mode == DYT_MODE_1000 ? "1000" : "?",
            c->fmt_index, c->frame_index, c->fmt_w, c->fmt_h);

    *out = c;
    return 0;

fail:
    if (dd) uvc_free_device_descriptor(dd);
    if (c->devh) uvc_close(c->devh);
    if (c->dev)  uvc_unref_device(c->dev);
    if (c->ctx)  uvc_exit(c->ctx);
    free(c->staging);
    free(c->lut);
    free(c->out);
    pthread_cond_destroy(&c->cv);
    pthread_mutex_destroy(&c->m);
    free(c);
    return rc;
}

/* ---------------------------------------------------------------- callback */

static void frame_cb(struct uvc_frame *f, void *user)
{
    dyt_capture_t *c = user;
    size_t bytes;

    if (c->err || !f->data || !f->data_bytes || !f->width)
        return;

    if (!c->pipe.ready) {
        if (dyt_pipeline_resolve(&c->pipe, f->width, f->data_bytes / 2) != 0) {
            fprintf(stderr, "capture: cannot resolve frame geometry from "
                            "%d B at width %d (mode %s)\n",
                    (int)f->data_bytes, f->width,
                    c->mode == DYT_MODE_44C ? "0x44c" : "1000");
            c->err = 1;
            return;
        }

        bytes = (size_t)c->pipe.width * c->pipe.total * 2;
        c->staging = malloc(bytes);
        c->out     = malloc((size_t)c->pipe.out_n * sizeof(float));
        if (!c->staging || !c->out) {
            fprintf(stderr, "capture: out of memory\n");
            c->err = 1;
            return;
        }
        fprintf(stderr, "capture: frame %dx%d (active %d, %d ref rows)\n",
                c->pipe.width, c->pipe.total, c->pipe.active,
                c->pipe.total - c->pipe.active);
    }

    bytes = (size_t)c->pipe.width * c->pipe.total * 2;
    if (f->data_bytes < bytes)
        return;                           /* short frame */

    /* Copy: libuvc reuses the buffer for the next frame. */
    memcpy(c->staging, f->data, bytes);

    /* Device-independent step (frame.c): LUT policy + conversion.  The LUT
     * is built from the first frame that actually carries calibration data,
     * so the pre-bring-up 0x8000 filler cannot seed it. */
    if (dyt_pipeline_frame(&c->pipe, c->staging, f->data_bytes / 2,
                           c->out) != 0)
        return;

    /* Auto-shutter (FFC) trigger — mode 0x44c only (RE Docs 04 §4.5.5). */
    if (c->mode == DYT_MODE_44C) {
        frame_t ft;
        uint16_t cur = 0;

        ft.width        = c->pipe.width;
        ft.total_height = c->pipe.total;
        ft.rec_base     = c->pipe.rec_base;
        ft.raw          = c->staging;
        if (dyt_frame_needs_shutter(&ft, c->last_ref, &cur))
            dyt_capture_request_shutter(c);
        c->last_ref = cur;
    }

    if (c->cb)
        c->cb(c->out, c->pipe.out_n, c->pipe.width, c->pipe.active, c->user);
}

/* ---------------------------------------------------------- control thread */

static void *ctrl_thread(void *arg)
{
    dyt_capture_t *c = arg;

    pthread_mutex_lock(&c->m);
    while (c->run) {
        while (c->run && !c->shutter_req)
            pthread_cond_wait(&c->cv, &c->m);
        if (!c->run)
            break;
        c->shutter_req = 0;
        pthread_mutex_unlock(&c->m);

        /* Not the libuvc callback thread, so uvc_* calls are allowed. */
        uvc_set_zoom_abs(c->devh, DYT_FFC_ZOOM);

        pthread_mutex_lock(&c->m);
    }
    pthread_mutex_unlock(&c->m);
    return NULL;
}

int dyt_capture_request_shutter(dyt_capture_t *c)
{
    pthread_mutex_lock(&c->m);
    c->shutter_req = 1;
    pthread_cond_signal(&c->cv);
    pthread_mutex_unlock(&c->m);
    return 0;
}

/* -------------------------------------------------------------- lifecycle */

/* ---------------------------------------------------------------- bring-up */

/* Send one 8-byte order to `reg`, then trace the raw status byte from
 * 0x0200 and the 15-byte result from 0x1d08 — WITHOUT aborting on the
 * documented ERROR bits.
 *
 * Why not just call dyt_transaction_ex: its poll loop treats status bits
 * 2..7 as an error and aborts (RE Docs 04 §4.2).  That bit layout is
 * decompiled, and this unit answers setTinyCOutputADValue with 0x0e, so
 * observe the real progression rather than trusting the decode. */
static void order_trace(dyt_capture_t *c, const char *name,
                        const uint8_t cmd[8], uint16_t reg)
{
    dyt_transfer_fn xfer = dyt_libusb_transfer(NULL);
    uint8_t out[8], status = 0, result[15];
    int rc, i, last = -1;

    if (!xfer)
        return;
    memcpy(out, cmd, sizeof out);

    rc = dyt_diy_communicate(xfer, c->usb, 0x41, 0x45, 0x0078, reg, out, 8);
    fprintf(stderr, "order %-22s reg=0x%04x OUT rc=%d\n", name, reg, rc);

    fprintf(stderr, "      status:");
    for (i = 0; i < 40; i++) {
        if (dyt_diy_communicate(xfer, c->usb, 0xC1, 0x44, 0x0078, 0x0200,
                                &status, 1) != 1)
            break;
        if ((int)status != last) {
            fprintf(stderr, " [%d]=0x%02x", i, status);
            last = status;
        }
        if (status == 0)
            break;
        usleep(1000);
    }
    fprintf(stderr, "\n");

    memset(result, 0, sizeof result);
    rc = dyt_diy_communicate(xfer, c->usb, 0xC1, 0x44, 0x0078, 0x1d08,
                             result, 15);
    fprintf(stderr, "      result 0x1d08 rc=%d :", rc);
    for (i = 0; i < 15; i++)
        fprintf(stderr, " %02x", result[i]);
    fprintf(stderr, "\n");
}

/* The one bring-up order that matters (MechaniscoutPcap/4.pcapng).
 *
 * The Windows vendor app sends exactly one TinyC order while bringing the
 * device up:
 *
 *     OUT  0x41/0x45  wValue=0x0078  wIndex=0x1D00
 *     data 0a 01 00 00 00 00 00 00        (setTinyCOutputADValue)
 *
 * and it sends it *after* the UVC stream is committed and running.  The
 * capture shows the whole startup: SET_INTERFACE (interface 1, alt 7) at
 * t=62.56 s, isochronous data flowing on EP 0x81, and this order at
 * t=65.69 s — followed by the app polling 0x0200, which returns 0x01.
 *
 * The ordering is the entire point.  Sent *before* a stream exists — as
 * this port originally did — the device answers 0x01 and then latches
 * status 0x0e permanently, after which every order and every 0x1d08 read
 * returns stale bytes.  That self-inflicted latch is what produced the
 * earlier "three commands return identical data" and "0x1d08 echoes the
 * last write" observations.
 *
 * It is not a WRITE-classified opcode, so it cannot touch factory
 * calibration (RE Docs 04 §4.8).  The other orders the old sequence sent
 * (tinyStartStream, tinyStartStream2, getTinyCParams) do not appear in the
 * vendor's startup at all; they remain available through `probe --op`. */
static void send_ad_order(dyt_capture_t *c)
{
    static const uint8_t cmd[8] = { 0x0a, 0x01, 0, 0, 0, 0, 0, 0 };
    order_trace(c, "setTinyCOutputADValue", cmd, 0x1d00);
}

int dyt_capture_start(dyt_capture_t *c, dyt_frame_cb_t cb, void *user)
{
    uvc_error_t err;

    c->cb = cb;
    c->user = user;

    pthread_mutex_lock(&c->m);
    c->run = 1;
    pthread_mutex_unlock(&c->m);

    if (pthread_create(&c->tid, NULL, ctrl_thread, c) != 0) {
        pthread_mutex_lock(&c->m);
        c->run = 0;
        pthread_mutex_unlock(&c->m);
        fprintf(stderr, "capture: cannot start control thread\n");
        return -1;
    }
    c->tid_valid = 1;

    err = uvc_start_streaming(c->devh, &c->ctrl, frame_cb, c, 0);
    if (err != UVC_SUCCESS) {
        fprintf(stderr, "capture: start_streaming: %s\n", uvc_strerror(err));
        pthread_mutex_lock(&c->m);
        c->run = 0;
        pthread_cond_signal(&c->cv);
        pthread_mutex_unlock(&c->m);
        pthread_join(c->tid, NULL);
        c->tid_valid = 0;
        return -1;
    }

    c->streaming = 1;

    if (c->send_start_orders) {
        int i;

        /* The vendor issues this order only once the stream is running
         * (see send_ad_order).  Wait for the first assembled frame so the
         * order cannot land before the device is streaming, then give the
         * stream a moment to settle. */
        for (i = 0; i < 300 && !c->pipe.ready; i++)
            usleep(10000);
        usleep(200000);
        send_ad_order(c);
    }

    return 0;
}

void dyt_capture_stop(dyt_capture_t *c)
{
    if (!c) return;

    /* uvc_stop_streaming joins libuvc's callback thread, so after it
     * returns no further frame_cb calls can occur. */
    if (c->streaming) {
        uvc_stop_streaming(c->devh);
        c->streaming = 0;
    }

    pthread_mutex_lock(&c->m);
    if (c->run) {
        c->run = 0;
        pthread_cond_signal(&c->cv);
    }
    pthread_mutex_unlock(&c->m);

    if (c->tid_valid) {
        pthread_join(c->tid, NULL);
        c->tid_valid = 0;
    }
}

void dyt_capture_close(dyt_capture_t *c)
{
    if (!c) return;

    dyt_capture_stop(c);

    free(c->staging);
    free(c->lut);
    free(c->out);
    pthread_cond_destroy(&c->cv);
    pthread_mutex_destroy(&c->m);

    if (c->devh) uvc_close(c->devh);
    if (c->dev)  uvc_unref_device(c->dev);
    if (c->ctx)  uvc_exit(c->ctx);
    free(c);
}

dyt_mode_t dyt_capture_mode(const dyt_capture_t *c)
{
    return c->mode;
}

void dyt_capture_geometry(const dyt_capture_t *c, int *width, int *active_height)
{
    if (width)        *width = c->pipe.ready ? c->pipe.width : 0;
    if (active_height) *active_height = c->pipe.ready ? c->pipe.active : 0;
}

const uint16_t *dyt_capture_last_raw(const dyt_capture_t *c, int *n_samples)
{
    if (!c->pipe.ready || !c->staging) {
        if (n_samples) *n_samples = 0;
        return NULL;
    }
    if (n_samples) *n_samples = c->pipe.width * c->pipe.total;
    return c->staging;
}

void dyt_capture_print_diag(dyt_capture_t *c)
{
    if (c && c->devh)
        uvc_print_diag(c->devh, stderr);
}

#else /* !DYT_HAVE_LIBUSB — no live UVC stack, so every entry point declines. */

struct dyt_capture { int dummy; };

int dyt_capture_open(dyt_capture_t **out, const dyt_capture_opts *o)
{
    (void)o;
    if (out) *out = NULL;
    fprintf(stderr, "capture: built without libusb — live capture unavailable "
                    "(rebuild with -DDYT_HAVE_LIBUSB)\n");
    return -1;
}

int dyt_capture_start(dyt_capture_t *c, dyt_frame_cb_t cb, void *user)
{
    (void)c; (void)cb; (void)user;
    return -1;
}

int dyt_capture_request_shutter(dyt_capture_t *c)
{
    (void)c;
    return -1;
}

void dyt_capture_stop(dyt_capture_t *c)
{
    (void)c;
}

void dyt_capture_close(dyt_capture_t *c)
{
    (void)c;
}

dyt_mode_t dyt_capture_mode(const dyt_capture_t *c)
{
    (void)c;
    return DYT_MODE_0;
}

void dyt_capture_geometry(const dyt_capture_t *c, int *width, int *active_height)
{
    (void)c;
    if (width) *width = 0;
    if (active_height) *active_height = 0;
}

const uint16_t *dyt_capture_last_raw(const dyt_capture_t *c, int *n_samples)
{
    (void)c;
    if (n_samples) *n_samples = 0;
    return NULL;
}

void dyt_capture_print_diag(dyt_capture_t *c)
{
    (void)c;
    fprintf(stderr, "capture: built without libusb — live capture unavailable\n");
}

#endif /* DYT_HAVE_LIBUSB */

/* Fill *o with the defaults.  libusb-independent, so it is always present. */
void dyt_capture_opts_default(dyt_capture_opts *o)
{
    memset(o, 0, sizeof *o);
    o->t_amb       = 25.0f;
    o->sensor_mode = 0x82;   /* vendor default (RE Docs 09 §7.2) */
    o->fix_mode    = 0;      /* 0x78 enables GetFix */
}
