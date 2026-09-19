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
 * build:  via the Makefile (make)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <libusb.h>
#include <libuvc/libuvc.h>

#include "capture.h"
#include "control.h"

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

    /* resolved geometry (first frame) */
    int width, active, total, rec_base, n_pix, out_n;
    int ready, lut_built, err;

    /* pipeline buffers */
    uint16_t *staging;
    float    *lut;
    float    *out;

    /* params */
    float t_amb;
    int sensor_mode, fix_mode;

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

void dyt_capture_opts_default(dyt_capture_opts *o)
{
    memset(o, 0, sizeof *o);
    o->t_amb       = 25.0f;
    o->sensor_mode = 0x82;   /* vendor default (RE Docs 09 §7.2) */
    o->fix_mode    = 0;      /* 0x78 enables GetFix */
}

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

    /* Frame: explicit size, else the default frame, else the first. */
    for (fr = best->frame_descs; fr; fr = fr->next) {
        if (o->width && o->height) {
            if (fr->wWidth == o->width && fr->wHeight == o->height) { bestfr = fr; break; }
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

    err = uvc_find_device(c->ctx, &c->dev, o->vid, o->pid, NULL);
    if (err != UVC_SUCCESS) {
        fprintf(stderr, "capture: no device found (%04x:%04x): %s\n"
                        "         run the probe tool to list devices\n",
                o->vid, o->pid, uvc_strerror(err));
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

    c->t_amb       = o->t_amb;
    c->sensor_mode = o->sensor_mode;
    c->fix_mode    = o->fix_mode;

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
    pthread_cond_destroy(&c->cv);
    pthread_mutex_destroy(&c->m);
    free(c);
    return rc;
}

/* ---------------------------------------------------------------- callback */

static void frame_cb(struct uvc_frame *f, void *user)
{
    dyt_capture_t *c = user;
    frame_t ft;
    uint16_t cur = 0;

    if (c->err || !f->data || !f->data_bytes || !f->width)
        return;

    if (!c->ready) {
        int w = f->width;
        int active, tot, rec_base;

        if (dyt_frame_resolve(c->mode, w, f->data_bytes,
                              &active, &tot, &rec_base) != 0) {
            fprintf(stderr, "capture: cannot resolve frame geometry from "
                            "%d B at width %d (mode %s)\n",
                    (int)f->data_bytes, w,
                    c->mode == DYT_MODE_44C ? "0x44c" : "1000");
            c->err = 1;
            return;
        }

        c->width = w;
        c->active = active;
        c->total = tot;
        c->rec_base = rec_base;
        c->n_pix = w * (c->mode == DYT_MODE_44C ? active : tot);
        c->out_n = c->n_pix + (c->mode == DYT_MODE_44C ? 10 : 0);

        c->staging = malloc((size_t)w * tot * 2);
        c->lut     = malloc(LUT_N * sizeof(float));
        c->out     = malloc((size_t)c->out_n * sizeof(float));
        if (!c->staging || !c->lut || !c->out) {
            fprintf(stderr, "capture: out of memory\n");
            c->err = 1;
            return;
        }
        c->ready = 1;
        fprintf(stderr, "capture: frame %dx%d (active %d, %d ref rows)\n",
                w, tot, active, tot - active);
    }

    if (f->data_bytes < (size_t)c->width * c->total * 2)
        return;                           /* short frame */

    /* Copy: libuvc reuses the buffer for the next frame. */
    memcpy(c->staging, f->data, (size_t)c->width * c->total * 2);

    ft.width = c->width;
    ft.total_height = c->total;
    ft.rec_base = c->rec_base;
    ft.raw = c->staging;

    /* The calibration record is per-unit and static, so the LUT is built
     * once from the first frame and reused.  `corr` is re-read from each
     * frame inside thermometrySearch. */
    if (!c->lut_built) {
        dyt_frame_build_lut(&ft, c->t_amb, c->sensor_mode, c->fix_mode, c->lut);
        c->lut_built = 1;
    }

    /* Auto-shutter (FFC) trigger — mode 0x44c only (RE Docs 04 §4.5.5). */
    if (c->mode == DYT_MODE_44C &&
        dyt_frame_needs_shutter(&ft, c->last_ref, &cur))
        dyt_capture_request_shutter(c);
    if (c->mode == DYT_MODE_44C)
        c->last_ref = cur;

    /* Saturated/corrupt frame: skip it rather than clamp (RE Docs 08 §8.5). */
    if (dyt_frame_convert_mode(c->mode, &ft, c->lut, c->out) != 0)
        return;

    if (c->cb)
        c->cb(c->out, c->out_n, c->width, c->active, c->user);
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
    if (width)        *width = c->ready ? c->width : 0;
    if (active_height) *active_height = c->ready ? c->active : 0;
}

const uint16_t *dyt_capture_last_raw(const dyt_capture_t *c, int *n_samples)
{
    if (!c->ready || !c->staging) {
        if (n_samples) *n_samples = 0;
        return NULL;
    }
    if (n_samples) *n_samples = c->width * c->total;
    return c->staging;
}

void dyt_capture_print_diag(dyt_capture_t *c)
{
    if (c && c->devh)
        uvc_print_diag(c->devh, stderr);
}
