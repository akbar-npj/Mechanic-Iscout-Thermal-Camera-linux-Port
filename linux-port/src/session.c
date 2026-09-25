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
     * and grown here on demand. */
    dyt_tool_t   tool;
    dyt_point_t  p0, p1;
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
};

/* --------------------------------------------------------------- lifecycle */

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
     * with, even before the caller points us at palettes/. */
    dyt_display_init(&s->disp, 0.0f, 0.0f, 0);
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
    out->unit      = s->unit;
    out->palette   = s->disp.palette;
    out->palette_n = s->pal_n;
    out->xform     = s->xform;
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

    if (s->tool == DYT_TOOL_BOX) {
        int need = s->width * s->height;   /* the largest possible box */
        int rc;

        /* dyt_measure_roi() only computes the median when it is given a
         * buffer, so the session must always own one — passing NULL would
         * silently report NaN.  Size it for the whole frame once, and reuse
         * it for every smaller box. */
        if (s->roi_cap < need) {
            float *grown = realloc(s->roi_scratch,
                                   (size_t)need * sizeof *grown);
            if (grown) {
                s->roi_scratch = grown;
                s->roi_cap     = need;
            }
        }

        rc = dyt_measure_roi(s->temps, s->width, s->height,
                             s->p0.x, s->p0.y, s->p1.x, s->p1.y,
                             s->roi_scratch, s->roi_cap, &out->roi);
        out->roi_ok = (rc == 0 && out->roi.n > 0);
    }

    /* The isotherm is the alarm band: the pixels that would trip it. */
    if (s->iso_on)
        dyt_measure_isotherm(s->temps, s->width, s->height,
                             out->iso_lo, out->iso_hi, &out->iso);

    pthread_mutex_unlock(&s->m);
    return 0;
}

int dyt_session_render_rgb(dyt_session_t *s, uint8_t *out_rgb, int rgb_cap,
                           int *w, int *h)
{
    int npix, rc;

    if (!s || !out_rgb)
        return -1;

    pthread_mutex_lock(&s->m);

    if (!s->have) {
        pthread_mutex_unlock(&s->m);
        return -1;
    }

    npix = s->width * s->height;
    if (rgb_cap < npix * 3) {
        pthread_mutex_unlock(&s->m);
        return -2;
    }

    rc = dyt_render_rgb(s->temps, s->width, s->height,
                        &s->pal[s->disp.palette], s->disp.lo, s->disp.hi,
                        out_rgb);
    if (rc == 0) {
        /* Fusion runs on the rendered RGB, in place: dyt_fusion_apply()
         * validates its arguments before writing anything, so a pattern that
         * cannot be honoured (no visible plane in the AD output mode) leaves
         * the thermal render intact and this falls back to plain thermal
         * rather than failing the whole render. */
        if (s->fusion.mode != DYT_FUSION_INFRARED) {
            const uint8_t *grey =
                (s->have_grey && s->grey_w == s->width &&
                 s->grey_h == s->height) ? s->grey : NULL;
            dyt_fusion_apply(&s->fusion, out_rgb, grey, s->width, s->height,
                             out_rgb);
        }
        if (w) *w = s->width;
        if (h) *h = s->height;
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
