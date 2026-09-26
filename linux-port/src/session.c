/*
 * session.c — the device-free model object (see session.h).
 *
 * One mutex guards everything.  dyt_session_process() runs on the capture
 * callback thread and dyt_session_snapshot()/render/setters on the GUI
 * thread, so the lock is the only thing keeping them apart.  Nothing here
 * blocks on I/O or allocation in the steady state: the temperature buffer is
 * grown once and reused, and the per-frame work is a memcpy plus one pass for
 * the statistics.
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -I. -c session.c -o session.o
 */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session.h"

struct dyt_session {
    pthread_mutex_t m;

    /* Latest frame.  `cap` is the allocated size; `width`/`height` are 0
     * until the first frame arrives. */
    float *temps;
    int    cap;
    int    width, height;
    int    have;

    long              seq;
    dyt_frame_stats_t stats;   /* computed in process(), read in snapshot() */

    dyt_display_t        disp;
    dyt_view_transform_t xform;
    dyt_unit_t           unit;

    /* Measurement.  The points are in source pixels; -1 means "unset".  The
     * scratch buffer is for the ROI median, which is far too expensive for
     * the callback thread — so it is done in snapshot(), on the GUI thread,
     * and grown here on demand.  The polygon's outline is a second placement
     * (session.h) and shares the same scratch. */
    dyt_tool_t   tool;
    dyt_point_t  p0, p1;
    dyt_point_t  poly[DYT_POLYGON_MAX_VTX];
    int          poly_n;
    int          poly_closed;
    float       *roi_scratch;
    int          roi_cap;

    /* The alarm is stateful but O(1), so it is evaluated per frame in
     * process() — unlike the measurements, it must not miss a frame or the
     * hysteresis would be evaluated against the wrong sequence. */
    dyt_alarm_t  alarm;
    int          iso_on;

    /* Fusion (Phase 5).  The visible plane is the dual-half payload's top
     * half, extracted by the caller (session_capture.c) and handed over in
     * process_visible().  It is kept at the thermal geometry; `have_grey` is
     * only trusted together with a matching grey_w/grey_h, so a plane left
     * over from a previous frame size is never fused. */
    uint8_t          *grey;
    int               grey_cap;
    int               grey_w, grey_h;
    int               have_grey;
    dyt_visible_stats_t grey_stats;
    dyt_fusion_cfg_t  fusion;

    /* The device's own samples for the latest frame (Phase 6).  Held so the
     * GUI thread can write a still from them; the capture layer's pointer is
     * only valid on the frame callback thread.  `raw_total_rows` is the
     * payload's row count, which in the dual-half mode exceeds height. */
    uint16_t *raw;
    int       raw_cap;
    int       raw_n;
    int       raw_total_rows;
    int       have_raw;

    dyt_palette_t pal[DYT_PALETTE_MAX];
    int           pal_n;

    /* Super-resolution (the optional 2x model).  `sr_mode` is what the user
     * asked for and `sr_fn` the upscaler a front end handed in — the engine
     * holds no reference to MNN itself, so a binary that never enables the
     * feature links no runtime (session.h).
     *
     * The buffers are sized together for one frame geometry — `sr_n` is the
     * pixel count they hold — and are grown on the first super-resolved frame
     * or after a mode switch.  `sr_in` is filled under the lock, the model runs
     * with no lock held, and the rest is read back under the lock, so no
     * inference ever happens inside the critical section. */
    dyt_sr_t           sr_mode;
    dyt_sr_upscale_fn  sr_fn;
    int                sr_n;
    uint8_t           *sr_in;    /* 2n: the model's packed input                */
    uint8_t           *sr_out;   /* 8n: its packed output (2x, two B a sample)  */
    uint8_t           *sr_work;  /* 4n: the 2x grey plane fusion is handed      */
    float             *sr_t2;    /* 4n: the 2x temperature plane the render eats */
    uint8_t           *sr_g1;    /* n:  the 1x display grey the thermal mode makes */
};

/* --------------------------------------------------------------- lifecycle */

/* Defined with the rest of the super-resolution code below; the snapshot needs
 * the factor the next render will use, and the render needs it first. */
static int sr_factor_locked(const dyt_session_t *s);

dyt_session_t *dyt_session_create(void)
{
    dyt_session_t *s = calloc(1, sizeof *s);
    int i;

    if (!s)
        return NULL;

    if (pthread_mutex_init(&s->m, NULL) != 0) {
        free(s);
        return NULL;
    }

    /* Start with the built-in ramps so there is always something to draw
     * with, even before the caller points us at palettes/.  The fixed window
     * gets the vendor's configured default rather than 0/0, so a mode that
     * engages it without the user having picked one still shows a picture. */
    dyt_display_init(&s->disp, DYT_FIXED_DEFAULT_LO, DYT_FIXED_DEFAULT_HI, 0);
    dyt_view_transform_init(&s->xform);
    s->unit  = DYT_UNIT_C;
    s->pal_n = DYT_PALETTE_BUILTIN_N;
    for (i = 0; i < s->pal_n; i++)
        dyt_palette_builtin(&s->pal[i], i);
    s->disp.palette_n = s->pal_n;

    s->tool   = DYT_TOOL_NONE;
    s->p0.x = s->p0.y = -1;
    s->p1.x = s->p1.y = -1;
    dyt_alarm_init(&s->alarm);
    s->iso_on = 0;

    /* Thermal only until the caller says otherwise: a fresh session has no
     * visible plane and must not pretend to fuse one. */
    dyt_fusion_cfg_default(&s->fusion);

    return s;
}

void dyt_session_free(dyt_session_t *s)
{
    if (!s)
        return;

    pthread_mutex_destroy(&s->m);
    free(s->roi_scratch);
    free(s->grey);
    free(s->raw);
    free(s->temps);
    free(s->sr_in);
    free(s->sr_out);
    free(s->sr_work);
    free(s->sr_t2);
    free(s->sr_g1);
    free(s);
}

/* ---------------------------------------------------------- frame pipeline */

int dyt_session_process(dyt_session_t *s, const dyt_frame_info_t *fi)
{
    dyt_frame_stats_t st;
    int npix, rc;

    if (!s || !fi || !fi->temps || fi->width <= 0 || fi->height <= 0)
        return -1;

    npix = fi->width * fi->height;

    /* Compute the statistics outside the lock — they only touch the caller's
     * buffer, and this is the expensive part of the per-frame work. */
    rc = dyt_frame_stats(fi->temps, fi->width, fi->height, &st);
    if (rc != 0)
        return -1;

    pthread_mutex_lock(&s->m);

    if (s->cap < npix) {
        float *grown = realloc(s->temps, (size_t)npix * sizeof *grown);
        if (!grown) {
            pthread_mutex_unlock(&s->m);
            return -1;
        }
        s->temps = grown;
        s->cap   = npix;
    }
    memcpy(s->temps, fi->temps, (size_t)npix * sizeof *s->temps);
    s->width  = fi->width;
    s->height = fi->height;
    s->have   = 1;
    s->seq++;
    s->stats  = st;

    dyt_display_update(&s->disp, &st);

    /* Filler frames are not a scene: the mode-1000 placeholder decodes to
     * 238.85 C, which would trip any sane high alarm during warm-up.  So the
     * alarm only ever sees real readings. */
    if (!st.all_filler)
        dyt_alarm_update(&s->alarm, st.hi, st.lo);

    pthread_mutex_unlock(&s->m);
    return 0;
}

int dyt_session_process_visible(dyt_session_t *s, const uint8_t *grey,
                                int width, int height)
{
    dyt_visible_stats_t st;
    int npix;

    if (!s || !grey || width <= 0 || height <= 0)
        return -1;

    npix = width * height;

    /* Summarise outside the lock: it only touches the caller's buffer. */
    if (dyt_visible_stats(grey, npix, &st) != 0)
        return -1;

    pthread_mutex_lock(&s->m);

    /* A plane that does not match the frame just processed is dropped rather
     * than resized into place: fusing a stale plane with the current
     * temperatures would draw the wrong picture, and silently stretching it
     * would draw a worse one.  The caller sees the mismatch and can fix its
     * geometry. */
    if (!s->have || width != s->width || height != s->height) {
        pthread_mutex_unlock(&s->m);
        return -1;
    }

    if (s->grey_cap < npix) {
        uint8_t *grown = realloc(s->grey, (size_t)npix);
        if (!grown) {
            pthread_mutex_unlock(&s->m);
            return -1;
        }
        s->grey      = grown;
        s->grey_cap  = npix;
    }
    memcpy(s->grey, grey, (size_t)npix);
    s->grey_w     = width;
    s->grey_h     = height;
    s->grey_stats = st;
    s->have_grey  = 1;

    pthread_mutex_unlock(&s->m);
    return 0;
}

int dyt_session_process_raw(dyt_session_t *s, const uint16_t *raw,
                            int n_samples, int width, int total_rows)
{
    if (!s || !raw || n_samples <= 0 || width <= 0 || total_rows <= 0)
        return -1;
    if (n_samples != width * total_rows)
        return -1;

    pthread_mutex_lock(&s->m);

    /* Same rule as process_visible(): the payload must belong to the frame
     * just processed, or a still would mix two frames.  The payload may be
     * taller than the thermal plane (dual-half), never shorter. */
    if (!s->have || width != s->width || total_rows < s->height) {
        pthread_mutex_unlock(&s->m);
        return -1;
    }

    if (s->raw_cap < n_samples) {
        uint16_t *grown = realloc(s->raw, (size_t)n_samples * sizeof *grown);
        if (!grown) {
            pthread_mutex_unlock(&s->m);
            return -1;
        }
        s->raw     = grown;
        s->raw_cap = n_samples;
    }
    memcpy(s->raw, raw, (size_t)n_samples * sizeof *raw);
    s->raw_n          = n_samples;
    s->raw_total_rows = total_rows;
    s->have_raw       = 1;

    pthread_mutex_unlock(&s->m);
    return 0;
}

/* Is the stored payload still usable?  A payload left over from a differently
 * sized frame must not be handed out.  Both the snapshot's `have_raw` and
 * dyt_session_raw() go through this, so a caller that checks have_raw and then
 * reads cannot get a payload the snapshot said was not there. */
static int raw_usable(const dyt_session_t *s)
{
    return s->have_raw && s->raw_n == s->width * s->raw_total_rows &&
           s->raw_total_rows >= s->height;
}

int dyt_session_raw(dyt_session_t *s, uint16_t *out, int cap)
{
    int n;

    if (!s || !out || cap <= 0)
        return -1;

    pthread_mutex_lock(&s->m);

    if (!raw_usable(s)) {
        pthread_mutex_unlock(&s->m);
        return 0;
    }

    n = s->raw_n;
    if (cap < n) {
        pthread_mutex_unlock(&s->m);
        return -n;                  /* the caller's buffer is too small */
    }
    memcpy(out, s->raw, (size_t)n * sizeof *out);

    pthread_mutex_unlock(&s->m);
    return n;
}

int dyt_session_snapshot(dyt_session_t *s, dyt_snapshot_t *out,
                         float *temps_out, int temps_cap)
{
    int npix;

    if (!s || !out)
        return -1;

    pthread_mutex_lock(&s->m);

    if (!s->have) {
        pthread_mutex_unlock(&s->m);
        return -1;
    }

    npix = s->width * s->height;

    /* Report the geometry before the buffer-size check so a caller that sized
     * its buffer from a previous frame can grow and retry on -2. */
    out->width  = s->width;
    out->height = s->height;

    if (temps_out) {
        if (temps_cap < npix) {
            pthread_mutex_unlock(&s->m);
            return -2;
        }
        memcpy(temps_out, s->temps, (size_t)npix * sizeof *temps_out);
    }

    out->seq       = s->seq;
    out->stats     = s->stats;
    out->ready     = !s->stats.all_filler;
    out->lo        = s->disp.lo;
    out->hi        = s->disp.hi;
    out->range_mode = s->disp.mode;
    out->unit      = s->unit;
    out->palette   = s->disp.palette;
    out->palette_n = s->pal_n;
    out->xform     = s->xform;

    /* The factor the *next* render will use.  Reported here so a front end can
     * size its buffer for the frame it is about to be handed, and map a
     * pointer back through the same scale as the picture it holds. */
    {
        const int srf = sr_factor_locked(s);

        out->sr_active = (srf == 2);
        dyt_view_transform_set_sr(&out->xform, srf);
    }
    out->sr     = s->sr_mode;
    out->sr_cap = s->sr_fn != NULL;
    snprintf(out->sr_name, sizeof out->sr_name, "%s",
             dyt_sr_name(s->sr_mode));
    snprintf(out->palette_name, sizeof out->palette_name, "%s",
             s->pal[s->disp.palette].name);

    out->centre_x = s->width / 2;
    out->centre_y = s->height / 2;
    out->centre_c = out->ready ? s->temps[out->centre_y * s->width +
                                           out->centre_x]
                               : NAN;   /* no reading while filler */

    /* ---- measurement ----------------------------------------------------
     * Geometry is copied as-is; the readings are computed here, on the GUI
     * thread, because a ROI median must not run in the frame callback. */
    out->tool = s->tool;
    out->p0   = s->p0;
    out->p1   = s->p1;

    out->poly_n      = s->poly_n;
    out->poly_closed = s->poly_closed;
    if (s->poly_n > 0)
        memcpy(out->poly, s->poly, (size_t)s->poly_n * sizeof out->poly[0]);

    out->point_ok = 0;
    out->point_c  = NAN;
    out->roi_ok   = 0;
    memset(&out->roi, 0, sizeof out->roi);
    out->roi.min = out->roi.max = out->roi.mean = out->roi.median = NAN;
    out->roi.min_x = out->roi.min_y = out->roi.max_x = out->roi.max_y = -1;

    out->alarm_on = s->alarm.enabled;
    out->alarm    = dyt_alarm_state(&s->alarm);
    out->alarm_lo = s->alarm.lo;
    out->alarm_hi = s->alarm.hi;

    out->iso_on = s->iso_on;
    out->iso_lo = s->alarm.lo;
    out->iso_hi = s->alarm.hi;
    memset(&out->iso, 0, sizeof out->iso);
    out->iso.min = out->iso.max = NAN;

    /* ---- fusion ---------------------------------------------------------
     * `have_visible` re-checks the geometry even though process_visible()
     * already did: the frame could have changed size since the plane was
     * installed, and a front-end must be told the pattern cannot be honoured
     * rather than shown a fusion of mismatched planes. */
    out->fusion    = s->fusion.mode;
    out->fusion_dx = s->fusion.dx;
    out->fusion_dy = s->fusion.dy;
    snprintf(out->fusion_name, sizeof out->fusion_name, "%s",
             dyt_fusion_name(s->fusion.mode));

    out->have_visible = s->have_grey && s->grey_w == s->width &&
                        s->grey_h == s->height;
    if (out->have_visible) {
        out->visible = s->grey_stats;
    } else {
        memset(&out->visible, 0, sizeof out->visible);
        out->visible.mean = NAN;
    }
    out->fusion_active = out->have_visible &&
                         s->fusion.mode != DYT_FUSION_INFRARED;

    /* ---- the raw payload ------------------------------------------------
     * Same re-check: a payload left over from a differently sized frame must
     * not be offered to the still writer. */
    out->have_raw = raw_usable(s);
    out->raw_n          = out->have_raw ? s->raw_n : 0;
    out->raw_total_rows = out->have_raw ? s->raw_total_rows : 0;

    if (s->tool == DYT_TOOL_POINT) {
        float v;
        if (dyt_measure_point(s->temps, s->width, s->height,
                              s->p0.x, s->p0.y, &v) == 0) {
            out->point_ok = 1;
            out->point_c  = v;
        }
    }

    if (s->tool == DYT_TOOL_BOX || s->tool == DYT_TOOL_POLYGON) {
        int need = s->width * s->height;   /* the largest possible region */
        int rc   = -1;

        /* Both measure functions only compute the median when they are given
         * a buffer, so the session must always own one — passing NULL would
         * silently report NaN.  Size it for the whole frame once, and reuse
         * it for every smaller region.  That size also rules out -2: neither
         * region can cover more pixels than the frame. */
        if (s->roi_cap < need) {
            float *grown = realloc(s->roi_scratch,
                                   (size_t)need * sizeof *grown);
            if (grown) {
                s->roi_scratch = grown;
                s->roi_cap     = need;
            }
        }

        if (s->tool == DYT_TOOL_BOX)
            rc = dyt_measure_roi(s->temps, s->width, s->height,
                                 s->p0.x, s->p0.y, s->p1.x, s->p1.y,
                                 s->roi_scratch, s->roi_cap, &out->roi);
        else
            rc = dyt_measure_polygon(s->temps, s->width, s->height,
                                     s->poly, s->poly_n,
                                     s->roi_scratch, s->roi_cap, &out->roi);

        /* Fewer than three polygon vertices is rc == -1, which leaves roi_ok
         * clear: an unfinished outline has no region to report. */
        out->roi_ok = (rc == 0 && out->roi.n > 0);
    }

    /* The isotherm is the alarm band: the pixels that would trip it. */
    if (s->iso_on)
        dyt_measure_isotherm(s->temps, s->width, s->height,
                             out->iso_lo, out->iso_hi, &out->iso);

    pthread_mutex_unlock(&s->m);
    return 0;
}

/* ------------------------------------------------------- super-resolution */

/* The factor the next render will use: 2 when super-resolution is on, a model
 * is loaded, the frame matches the model's fixed geometry, and — for the
 * visible mode — the plane it upscales is actually being shown; 1 otherwise.
 *
 * This is a *predicate*, not "what the last render did", and that is what lets
 * a front end size its buffer for the frame it is about to be handed: a caller
 * that sized from the last render could never grow into the first
 * super-resolved frame.  The snapshot reports it, and the render obeys it.
 *
 * Caller holds the lock. */
static int sr_factor_locked(const dyt_session_t *s)
{
    if (!s->sr_fn || s->sr_mode == DYT_SR_OFF)
        return 1;

    /* The model is fixed 256x192 -> 512x384, so a 240/384/640-wide sensor
     * cannot use it at all.  Refusing here is what keeps a differently sized
     * frame from being fed to it. */
    if (s->width != DYT_SR_IN_W || s->height != DYT_SR_IN_H)
        return 1;

    if (s->sr_mode == DYT_SR_VISIBLE) {
        /* The vendor's plane.  It is only worth upscaling when it is shown, so
         * the infrared pattern (which draws no visible channel) and a frame
         * with no visible half both leave the render alone rather than
         * upscaling a plane nobody sees. */
        if (s->fusion.mode == DYT_FUSION_INFRARED)
            return 1;
        if (!s->have_grey || s->grey_w != s->width || s->grey_h != s->height)
            return 1;
    }

    return 2;
}

/* Grow the super-resolution buffers for an n-pixel frame.  Called under the
 * lock, on the first super-resolved frame or after a geometry change — never
 * from the frame callback, so the one-off allocation is not in the per-frame
 * path.
 *
 * Returns 0, or -1 when an allocation failed, in which case the render falls
 * back to the plain picture rather than failing. */
static int sr_grow_locked(dyt_session_t *s, int n)
{
    uint8_t *in, *out, *work, *g1;
    float   *t2;

    if (s->sr_n == n)
        return 0;

    in   = realloc(s->sr_in,   (size_t)n * 2);
    out  = realloc(s->sr_out,  (size_t)n * 8);
    work = realloc(s->sr_work, (size_t)n * 4);
    t2   = realloc(s->sr_t2,   (size_t)n * 4 * sizeof *t2);
    g1   = realloc(s->sr_g1,   (size_t)n);

    /* Keep whatever succeeded, so a later frame retries only what failed.
     * `sr_n` is left alone, so nothing here is used until all five are big
     * enough. */
    if (in)   s->sr_in   = in;
    if (out)  s->sr_out  = out;
    if (work) s->sr_work = work;
    if (t2)   s->sr_t2   = t2;
    if (g1)   s->sr_g1   = g1;

    if (!in || !out || !work || !t2 || !g1)
        return -1;

    s->sr_n = n;
    return 0;
}

/* Pack the plane this mode upscales into sr_in.  Caller holds the lock.
 * Returns 0, or -1 when the plane is not there (the render then falls back to
 * the plain picture rather than upscaling something else). */
static int sr_fill_input_locked(dyt_session_t *s, int n)
{
    if (s->sr_mode == DYT_SR_VISIBLE) {
        /* The vendor's plane: the dual-half grey picture.  It is already in
         * the form the model reads (low byte = luma, high byte = neutral
         * chroma), so packing just re-states that layout. */
        if (!s->have_grey || s->grey_w != s->width || s->grey_h != s->height)
            return -1;
        return dyt_sr_pack(s->grey, n, s->sr_in);
    }

    /* The thermal mode: the *display-domain* grey, which is the picture the
     * user is looking at, not the 14-bit raw whose low byte is a sawtooth
     * (sr.h).  Encoding it through the display range is also what lets the
     * result be rendered by the ordinary palette path below. */
    if (dyt_sr_thermal_grey(s->temps, n, s->disp.lo, s->disp.hi,
                            s->sr_g1) != 0)
        return -1;
    return dyt_sr_pack(s->sr_g1, n, s->sr_in);
}

/* The plain render: source size, thermal RGB, then fusion in place.  Caller
 * holds the lock.  Shared by the normal path and the fallback. */
static int render_plain_locked(dyt_session_t *s, uint8_t *out_rgb, int rgb_cap,
                               int w, int h, int *out_w, int *out_h)
{
    int rc;

    if (w <= 0 || h <= 0)
        return -1;
    if (rgb_cap < w * h * 3)
        return -2;

    rc = dyt_render_rgb(s->temps, w, h, &s->pal[s->disp.palette],
                        s->disp.lo, s->disp.hi, out_rgb);
    if (rc != 0)
        return rc;

    /* dyt_fusion_apply() validates its arguments before writing anything, so a
     * pattern that cannot be honoured leaves the thermal render intact and
     * this falls back to plain thermal rather than failing the whole render. */
    if (s->fusion.mode != DYT_FUSION_INFRARED) {
        const uint8_t *grey =
            (s->have_grey && s->grey_w == w && s->grey_h == h) ? s->grey : NULL;
        dyt_fusion_apply(&s->fusion, out_rgb, grey, w, h, out_rgb);
    }

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return 0;
}

/* Build the 2x picture from the model's output in sr_out.  Caller holds the
 * lock and has already checked the geometry still matches.
 *
 * Whichever channel was *not* super-resolved gets a plain nearest 2x, so the
 * two planes fusion is handed are the same size.  Returns 0, or -1. */
static int render_sr_locked(dyt_session_t *s, uint8_t *out_rgb, int w, int h,
                            int *out_w, int *out_h)
{
    int             n2 = 4 * w * h;
    const uint8_t  *vis2 = NULL;   /* the 2x visible plane, when one is shown */
    int             rc;

    /* The model's output carries the upscaled grey in every even byte (sr.h),
     * so unpacking it is the whole 2x grey plane. */
    if (dyt_sr_unpack(s->sr_out, n2, s->sr_work) != 0)
        return -1;

    if (s->sr_mode == DYT_SR_THERMAL) {
        /* The super-resolved channel is the thermal one, so the palette has to
         * come from it: turn the grey back into temperatures through the same
         * range and render through the ordinary path, so a temperature gets
         * the colour it would have had at 1x. */
        if (dyt_sr_grey_to_temps(s->sr_work, n2, s->disp.lo, s->disp.hi,
                                 s->sr_t2) != 0)
            return -1;

        /* sr_work has been consumed, so it is free to hold the visible plane
         * for fusion — and only when a pattern actually shows it. */
        if (s->fusion.mode != DYT_FUSION_INFRARED &&
            s->have_grey && s->grey_w == w && s->grey_h == h) {
            if (dyt_sr_nearest2(s->grey, w, h, s->sr_work) != 0)
                return -1;
            vis2 = s->sr_work;
        }
    } else {
        /* The visible channel was super-resolved, so it is the thermal plane
         * that gets the plain 2x — as a float plane, so the palette path is
         * unchanged. */
        if (dyt_sr_nearest2_f(s->temps, w, h, s->sr_t2) != 0)
            return -1;
        vis2 = s->sr_work;
    }

    rc = dyt_render_rgb(s->sr_t2, 2 * w, 2 * h, &s->pal[s->disp.palette],
                        s->disp.lo, s->disp.hi, out_rgb);
    if (rc != 0)
        return rc;

    if (s->fusion.mode != DYT_FUSION_INFRARED && vis2) {
        /* The alignment is in *source* pixels, so at 2x it is twice as many
         * output pixels.  Without this the planes would sit half as far apart
         * as the user set them. */
        dyt_fusion_cfg_t cfg = s->fusion;

        cfg.dx *= 2;
        cfg.dy *= 2;
        dyt_fusion_apply(&cfg, out_rgb, vis2, 2 * w, 2 * h, out_rgb);
    }

    if (out_w) *out_w = 2 * w;
    if (out_h) *out_h = 2 * h;
    return 0;
}

int dyt_session_render_rgb(dyt_session_t *s, uint8_t *out_rgb, int rgb_cap,
                           int *w, int *h)
{
    int rc, src_w, src_h, npix, f;

    if (!s || !out_rgb)
        return -1;

    /* ---- phase 1: under the lock, decide and stage the model's input ------
     * The model itself runs in phase 2, with no lock held: inference inside
     * the critical section would stall the capture callback for its whole
     * duration. */
    pthread_mutex_lock(&s->m);

    if (!s->have) {
        pthread_mutex_unlock(&s->m);
        return -1;
    }

    src_w = s->width;
    src_h = s->height;
    npix  = src_w * src_h;
    f     = sr_factor_locked(s);

    if (f == 2) {
        /* The caller sizes from the snapshot, which reports this same factor,
         * so a short buffer here is a caller error and -2 is what it already
         * handles. */
        if (rgb_cap < npix * 4 * 3) {
            pthread_mutex_unlock(&s->m);
            return -2;
        }
        if (sr_grow_locked(s, npix) != 0 ||
            sr_fill_input_locked(s, npix) != 0)
            f = 1;                      /* no memory, or no plane: render plain */
    }

    if (f == 1) {
        rc = render_plain_locked(s, out_rgb, rgb_cap, src_w, src_h, w, h);
        pthread_mutex_unlock(&s->m);
        return rc;
    }

    /* ---- phase 2: the model, with no lock held --------------------------- */
    pthread_mutex_unlock(&s->m);
    {
        int n = 0;
        rc = s->sr_fn(s->sr_in, s->sr_out, npix * 8, &n);
    }
    pthread_mutex_lock(&s->m);

    /* ---- phase 3: back under the lock ------------------------------------
     * The geometry cannot change on a single front-end thread, but a capture
     * mode switch on the other thread could have changed it while the model
     * ran — and a 2x picture built from a plane that no longer matches would
     * mix two frames.  That is not the upscaler's fault, so the capability
     * stays and only this frame falls back.  The *current* geometry is used,
     * because the new frame is what s->temps now holds. */
    if (!s->have || s->width != src_w || s->height != src_h)
        rc = render_plain_locked(s, out_rgb, rgb_cap, s->width, s->height,
                                 w, h);
    else if (rc == 0)
        rc = render_sr_locked(s, out_rgb, src_w, src_h, w, h);

    if (rc != 0) {
        /* An upscaler that fails at run time will not start working on the
         * next frame, so withdraw it rather than paying for it every frame:
         * the snapshot then reports sr 1, and the front end's mapping stays
         * consistent with the picture it actually gets.  The frame still
         * renders — plain — because a failed upscale is not a reason to show
         * nothing. */
        s->sr_fn   = NULL;
        s->sr_mode = DYT_SR_OFF;
        rc = render_plain_locked(s, out_rgb, rgb_cap, src_w, src_h, w, h);
    }

    pthread_mutex_unlock(&s->m);
    return rc;
}

/* ------------------------------------------------------------------ state */

int dyt_session_load_palettes(dyt_session_t *s, const char *dir)
{
    dyt_palette_t loaded[DYT_PALETTE_MAX];
    int n = 0, i;

    if (!s)
        return -1;

    if (dir) {
        n = dyt_palette_load_dir(loaded, DYT_PALETTE_MAX, dir);
        if (n < 0)
            n = 0;
    }

    if (n == 0) {
        /* No directory, or nothing loadable in it: fall back to the ramps so
         * the session always has at least one palette to draw with. */
        for (i = 0; i < DYT_PALETTE_BUILTIN_N; i++)
            dyt_palette_builtin(&loaded[i], i);
        n = DYT_PALETTE_BUILTIN_N;
    }

    pthread_mutex_lock(&s->m);
    memcpy(s->pal, loaded, (size_t)n * sizeof loaded[0]);
    s->pal_n = n;
    /* Set the count *before* re-clamping the index: if the new set is smaller
     * than the old one, clamping against the old count would leave the index
     * pointing past the end of the new array. */
    s->disp.palette_n = n;
    dyt_display_set_palette(&s->disp, s->disp.palette);
    pthread_mutex_unlock(&s->m);

    return n;
}

int dyt_session_get_palette(dyt_session_t *s, int idx, dyt_palette_t *out)
{
    int rc = -1;

    if (!s || !out)
        return -1;

    pthread_mutex_lock(&s->m);
    if (idx >= 0 && idx < s->pal_n) {
        *out = s->pal[idx];
        rc = 0;
    }
    pthread_mutex_unlock(&s->m);
    return rc;
}

void dyt_session_set_unit(dyt_session_t *s, dyt_unit_t u)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->m);
    s->unit = u;
    pthread_mutex_unlock(&s->m);
}

void dyt_session_cycle_unit(dyt_session_t *s)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->m);
    s->unit = dyt_unit_next(s->unit);
    pthread_mutex_unlock(&s->m);
}

void dyt_session_set_range_mode(dyt_session_t *s, dyt_range_mode_t m)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->m);
    dyt_display_set_mode(&s->disp, m);
    pthread_mutex_unlock(&s->m);
}

void dyt_session_toggle_range(dyt_session_t *s)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->m);
    dyt_display_toggle_mode(&s->disp);
    pthread_mutex_unlock(&s->m);
}

void dyt_session_set_fixed_range(dyt_session_t *s, float lo, float hi)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->m);
    dyt_display_set_fixed(&s->disp, lo, hi);
    pthread_mutex_unlock(&s->m);
}

void dyt_session_set_palette(dyt_session_t *s, int idx)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->m);
    dyt_display_set_palette(&s->disp, idx);
    pthread_mutex_unlock(&s->m);
}

void dyt_session_cycle_palette(dyt_session_t *s, int dir)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->m);
    dyt_display_cycle_palette(&s->disp, dir);
    pthread_mutex_unlock(&s->m);
}

void dyt_session_toggle_flip_h(dyt_session_t *s)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->m);
    dyt_view_transform_toggle_flip_h(&s->xform);
    pthread_mutex_unlock(&s->m);
}

void dyt_session_toggle_flip_v(dyt_session_t *s)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->m);
    dyt_view_transform_toggle_flip_v(&s->xform);
    pthread_mutex_unlock(&s->m);
}

void dyt_session_zoom(dyt_session_t *s, int delta)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->m);
    dyt_view_transform_zoom(&s->xform, delta);
    pthread_mutex_unlock(&s->m);
}

void dyt_session_rotate(dyt_session_t *s, int delta)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->m);
    dyt_view_transform_rotate(&s->xform, delta);
    pthread_mutex_unlock(&s->m);
}

void dyt_session_reset_view(dyt_session_t *s)
{
    if (!s)
        return;

    pthread_mutex_lock(&s->m);
    /* The transform's own init is the start-up framing, so the reset and the
     * start-up state cannot drift apart.  It also clears `sr`, which is inert:
     * the factor is a *render* property that sr_factor_locked() re-derives and
     * writes into every snapshot, so nothing reads this field between a reset
     * and the next frame.  The palette, the unit and the fusion pattern are not
     * framing and are left alone. */
    dyt_view_transform_init(&s->xform);
    s->disp.mode = DYT_RANGE_AUTO;
    pthread_mutex_unlock(&s->m);
}

/* ------------------------------------------------------- super-resolution */

void dyt_session_set_sr_upscaler(dyt_session_t *s, dyt_sr_upscale_fn fn)
{
    if (!s)
        return;

    pthread_mutex_lock(&s->m);
    s->sr_fn = fn;
    /* Withdrawing the upscaler withdraws the mode with it: a mode with nothing
     * behind it is exactly what the seam refuses to pretend about. */
    if (!s->sr_fn)
        s->sr_mode = DYT_SR_OFF;
    pthread_mutex_unlock(&s->m);
}

void dyt_session_set_sr(dyt_session_t *s, dyt_sr_t m)
{
    if (!s || m < 0 || m >= DYT_SR_N)
        return;

    pthread_mutex_lock(&s->m);
    /* A mode with no upscaler behind it is refused rather than remembered: the
     * seam's rule is that the caller gets the real upscale or a refusal, and
     * "selected but impossible" is neither. */
    s->sr_mode = s->sr_fn ? m : DYT_SR_OFF;
    pthread_mutex_unlock(&s->m);
}

dyt_sr_t dyt_session_get_sr(dyt_session_t *s)
{
    dyt_sr_t m;

    if (!s)
        return DYT_SR_OFF;

    pthread_mutex_lock(&s->m);
    m = s->sr_mode;
    pthread_mutex_unlock(&s->m);
    return m;
}

int dyt_session_sr_capable(dyt_session_t *s)
{
    int cap;

    if (!s)
        return 0;

    pthread_mutex_lock(&s->m);
    cap = s->sr_fn != NULL;
    pthread_mutex_unlock(&s->m);
    return cap;
}

/* ----------------------------------------------------------------- fusion */
void dyt_session_set_fusion(dyt_session_t *s, dyt_fusion_t f)
{
    if (!s || f < 0 || f >= DYT_FUSION_N)
        return;
    pthread_mutex_lock(&s->m);
    s->fusion.mode = f;
    pthread_mutex_unlock(&s->m);
}

void dyt_session_cycle_fusion(dyt_session_t *s, int dir)
{
    int n = DYT_FUSION_N;
    int next;

    if (!s || dir == 0)
        return;

    pthread_mutex_lock(&s->m);
    /* Modulo rather than a clamp so the cycle wraps both ways; `dir` is only
     * ever ±1 from the key handler, but a caller could pass anything. */
    next = ((int)s->fusion.mode + (dir > 0 ? 1 : -1)) % n;
    if (next < 0)
        next += n;
    s->fusion.mode = (dyt_fusion_t)next;
    pthread_mutex_unlock(&s->m);
}

void dyt_session_adjust_fusion_align(dyt_session_t *s, int ddx, int ddy)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->m);
    s->fusion.dx = dyt_fusion_clamp_align(s->fusion.dx + ddx);
    s->fusion.dy = dyt_fusion_clamp_align(s->fusion.dy + ddy);
    pthread_mutex_unlock(&s->m);
}

void dyt_session_set_fusion_align(dyt_session_t *s, int dx, int dy)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->m);
    s->fusion.dx = dyt_fusion_clamp_align(dx);
    s->fusion.dy = dyt_fusion_clamp_align(dy);
    pthread_mutex_unlock(&s->m);
}

/* ------------------------------------------------------------- measurement */

void dyt_session_set_tool(dyt_session_t *s, dyt_tool_t t)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->m);
    s->tool = t;
    pthread_mutex_unlock(&s->m);
}

void dyt_session_set_point(dyt_session_t *s, int which, int x, int y)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->m);
    if (which == 0) {
        s->p0.x = x;
        s->p0.y = y;
    } else {
        s->p1.x = x;
        s->p1.y = y;
    }
    pthread_mutex_unlock(&s->m);
}

void dyt_session_clear_points(dyt_session_t *s)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->m);
    s->p0.x = s->p0.y = -1;
    s->p1.x = s->p1.y = -1;
    /* The polygon is a placement too, so "forget the measurement" forgets it:
     * a front end has one clear action, not one per tool (session.h). */
    s->poly_n      = 0;
    s->poly_closed = 0;
    pthread_mutex_unlock(&s->m);
}

int dyt_session_polygon_add(dyt_session_t *s, int x, int y)
{
    int rc = 0;

    if (!s)
        return -1;

    pthread_mutex_lock(&s->m);

    if (s->poly_closed) {
        /* The user declared the outline finished, so this vertex begins a new
         * one rather than extending the shape they just closed. */
        s->poly_n      = 0;
        s->poly_closed = 0;
    } else if (s->poly_n >= DYT_POLYGON_MAX_VTX) {
        rc = -1;                /* full: refuse, never recycle (session.h) */
    }

    if (rc == 0) {
        s->poly[s->poly_n].x = x;
        s->poly[s->poly_n].y = y;
        s->poly_n++;
    }

    pthread_mutex_unlock(&s->m);
    return rc;
}

int dyt_session_polygon_undo(dyt_session_t *s)
{
    int n;

    if (!s)
        return -1;

    pthread_mutex_lock(&s->m);
    /* Reopening is part of the undo: the user is stepping back through the
     * shape, so it has to accept vertices again. */
    s->poly_closed = 0;
    if (s->poly_n > 0)
        s->poly_n--;
    n = s->poly_n;
    pthread_mutex_unlock(&s->m);
    return n;
}

void dyt_session_set_polygon_closed(dyt_session_t *s, int closed)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->m);
    /* A region needs three vertices, so closing a shorter outline would claim
     * a shape that cannot exist. */
    s->poly_closed = (closed && s->poly_n >= 3) ? 1 : 0;
    pthread_mutex_unlock(&s->m);
}

void dyt_session_set_polygon_pts(dyt_session_t *s, const dyt_point_t *pts, int n)
{
    if (!s)
        return;
    if (n < 0 || n > DYT_POLYGON_MAX_VTX)
        return;                     /* refused, not truncated */
    if (n > 0 && !pts)
        return;

    pthread_mutex_lock(&s->m);
    if (n > 0)
        memcpy(s->poly, pts, (size_t)n * sizeof s->poly[0]);
    s->poly_n = n;
    /* A whole new outline is not the finished shape a previous close
     * declared. */
    s->poly_closed = 0;
    pthread_mutex_unlock(&s->m);
}

int dyt_session_profile(dyt_session_t *s, float *out, int cap)
{
    int n;

    if (!s || !out || cap < 0)
        return -1;

    pthread_mutex_lock(&s->m);
    if (!s->have) {
        pthread_mutex_unlock(&s->m);
        return -1;
    }
    if (s->tool != DYT_TOOL_LINE) {
        pthread_mutex_unlock(&s->m);
        return 0;               /* no line placed: an empty profile */
    }

    n = dyt_measure_line(s->temps, s->width, s->height,
                         s->p0.x, s->p0.y, s->p1.x, s->p1.y, out, cap);
    pthread_mutex_unlock(&s->m);
    return n;
}

/* ------------------------------------------------------------------ alarms */

void dyt_session_set_alarm(dyt_session_t *s, float lo, float hi, float hyst)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->m);
    dyt_alarm_set(&s->alarm, lo, hi, hyst);
    pthread_mutex_unlock(&s->m);
}

void dyt_session_alarm_disable(dyt_session_t *s)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->m);
    dyt_alarm_disable(&s->alarm);
    pthread_mutex_unlock(&s->m);
}

void dyt_session_alarm_reset(dyt_session_t *s)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->m);
    dyt_alarm_reset(&s->alarm);
    pthread_mutex_unlock(&s->m);
}

void dyt_session_set_isotherm(dyt_session_t *s, int on)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->m);
    s->iso_on = on ? 1 : 0;
    pthread_mutex_unlock(&s->m);
}
