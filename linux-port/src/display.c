/*
 * display.c — display mapping state: range, palette index, mirror/zoom,
 * and the start-up filler rule.
 *
 * See display.h for why this is a library module.  Pure: no camera, no GUI.
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -I. -c display.c -o display.o
 */
#include <math.h>

#include "display.h"

/* ------------------------------------------------------ frame statistics */

int dyt_frame_stats(const float *temps, int w, int h, dyt_frame_stats_t *out)
{
    double sum   = 0.0;
    float  lo    = 0.0f, hi = 0.0f;
    int    valid = 0;
    int    filler = 1;      /* cleared by the first sample outside the window */
    int    hot_i = -1, cold_i = -1;
    int    n, i;

    if (!temps || !out || w <= 0 || h <= 0)
        return -1;

    n = w * h;

    for (i = 0; i < n; i++) {
        float x = temps[i];

        if (isnan(x))
            continue;       /* the pipeline's non-physical marker */

        if (valid == 0) {
            lo = hi = x;
            hot_i = cold_i = i;
        } else {
            if (x < lo) { lo = x; cold_i = i; }
            if (x > hi) { hi = x; hot_i  = i; }
        }
        sum += x;
        valid++;

        if (fabsf(x - DYT_FILLER_C) > DYT_FILLER_TOL)
            filler = 0;
    }

    out->lo         = lo;
    out->hi         = hi;
    out->mean       = valid ? (float)(sum / (double)valid) : 0.0f;
    out->n_valid    = valid;
    /* No finite samples at all also counts as filler — an all-NaN frame must
     * not be mistaken for a reading. */
    out->all_filler = filler;

    out->hot_x  = hot_i  >= 0 ? hot_i  % w : -1;
    out->hot_y  = hot_i  >= 0 ? hot_i  / w : -1;
    out->cold_x = cold_i >= 0 ? cold_i % w : -1;
    out->cold_y = cold_i >= 0 ? cold_i / w : -1;
    return 0;
}

/* ------------------------------------------------------------- range state */

/* Guarantee the renderer gets a finite, non-empty span [lo, hi] with hi > lo.
 *
 * Three caller errors reach here and each would otherwise produce a silently
 * broken image rather than a visible failure: an inverted pair (hi < lo), a
 * flat pair (hi == lo, which makes dyt_palette_index divide by zero), and a
 * non-finite bound.  The inverted case must be swapped *before* the flat
 * check — widening an inverted pair by ±1 leaves it inverted. */
static void widen(float *lo, float *hi)
{
    float a = *lo, b = *hi, t;

    if (!isfinite(a) || !isfinite(b)) {
        *lo = 0.0f;
        *hi = 1.0f;
        return;
    }

    if (b < a) { t = a; a = b; b = t; }        /* inverted pair — swap */
    if (!(b > a)) { a -= 1.0f; b += 1.0f; }    /* flat — give it a span */

    *lo = a;
    *hi = b;
}

void dyt_display_init(dyt_display_t *d, float lo, float hi, int lock)
{
    if (!d)
        return;

    d->mode      = lock ? DYT_RANGE_FIXED : DYT_RANGE_AUTO;
    d->fixed_lo  = lo;
    d->fixed_hi  = hi;
    d->lo        = lo;
    d->hi        = hi;
    widen(&d->lo, &d->hi);
    d->palette   = 0;
    d->palette_n = 1;
}

void dyt_display_set_mode(dyt_display_t *d, dyt_range_mode_t m)
{
    if (d)
        d->mode = m;
}

const char *dyt_range_mode_name(dyt_range_mode_t m)
{
    switch (m) {
    case DYT_RANGE_AUTO:  return "auto";
    case DYT_RANGE_FIXED: return "fixed";
    default:              return "?";
    }
}

void dyt_display_toggle_mode(dyt_display_t *d)
{
    if (!d)
        return;

    if (d->mode == DYT_RANGE_AUTO) {
        /* Latch the range currently shown so the image does not jump the
         * instant the user locks it. */
        d->fixed_lo = d->lo;
        d->fixed_hi = d->hi;
        d->mode     = DYT_RANGE_FIXED;
    } else {
        d->mode = DYT_RANGE_AUTO;
    }
}

void dyt_display_set_fixed(dyt_display_t *d, float lo, float hi)
{
    if (!d)
        return;

    d->fixed_lo = lo;
    d->fixed_hi = hi;
    d->mode     = DYT_RANGE_FIXED;
    d->lo       = lo;
    d->hi       = hi;
    widen(&d->lo, &d->hi);
}

void dyt_display_set_palette(dyt_display_t *d, int idx)
{
    if (!d || d->palette_n <= 0)
        return;

    if (idx < 0)                idx = 0;
    if (idx >= d->palette_n)    idx = d->palette_n - 1;
    d->palette = idx;
}

void dyt_display_cycle_palette(dyt_display_t *d, int dir)
{
    if (!d || d->palette_n <= 0)
        return;

    /* Double-modulo so a negative step wraps rather than going negative. */
    d->palette = ((d->palette + dir) % d->palette_n + d->palette_n)
                 % d->palette_n;
}

void dyt_display_update(dyt_display_t *d, const dyt_frame_stats_t *st)
{
    if (!d || !st)
        return;

    if (d->mode == DYT_RANGE_FIXED) {
        d->lo = d->fixed_lo;
        d->hi = d->fixed_hi;
    } else {
        if (st->n_valid <= 0)
            return;             /* nothing to fit — keep the last good range */
        d->lo = st->lo;
        d->hi = st->hi;
    }
    widen(&d->lo, &d->hi);
}

/* --------------------------------------------------------- mirror and zoom */

void dyt_view_transform_init(dyt_view_transform_t *t)
{
    if (!t)
        return;

    t->flip_h = 0;
    t->flip_v = 0;
    t->rot    = DYT_ROT_NONE;
    t->zoom   = DYT_ZOOM_MIN;
    t->sr     = DYT_SR_MIN;
}

void dyt_view_transform_toggle_flip_h(dyt_view_transform_t *t)
{
    if (t)
        t->flip_h = !t->flip_h;
}

void dyt_view_transform_toggle_flip_v(dyt_view_transform_t *t)
{
    if (t)
        t->flip_v = !t->flip_v;
}

/* Any degree value a caller may have stored, reduced to a quarter turn 0..3:
 * 45 reads as 1 (rounds up), -90 as 3, 359 as 0.  Every function below then
 * works in quarter turns, so a field set by hand rather than by
 * dyt_view_transform_rotate() still behaves. */
static int rot_quarter(int deg)
{
    int q = (deg % 360 + 360) % 360;

    return ((q + 45) / 90) % 4;
}

void dyt_view_transform_rotate(dyt_view_transform_t *t, int delta)
{
    /* The four fields the transform holds, named rather than derived from one
     * of them, so the mapping from a quarter turn back to degrees is explicit. */
    static const int kDeg[4] = { DYT_ROT_NONE, DYT_ROT_90,
                                 DYT_ROT_180,  DYT_ROT_270 };
    int steps, q;

    if (!t)
        return;

    /* Quarter turns, rounded away from zero, so a fractional request moves the
     * picture rather than being dropped. */
    steps = (delta >= 0) ? (delta + 45) / 90 : (delta - 45) / 90;

    q = ((rot_quarter(t->rot) + steps) % 4 + 4) % 4;   /* double modulo: wraps */
    t->rot = kDeg[q];
}

void dyt_view_transform_zoom(dyt_view_transform_t *t, int delta)
{
    int z;

    if (!t)
        return;

    z = t->zoom + delta;
    if (z < DYT_ZOOM_MIN) z = DYT_ZOOM_MIN;
    if (z > DYT_ZOOM_MAX) z = DYT_ZOOM_MAX;
    t->zoom = z;
}

void dyt_view_transform_set_sr(dyt_view_transform_t *t, int sr)
{
    if (!t)
        return;

    if (sr < DYT_SR_MIN) sr = DYT_SR_MIN;
    if (sr > DYT_SR_MAX) sr = DYT_SR_MAX;
    t->sr = sr;
}

/* The magnification the transform actually applies: the user's zoom times the
 * super-resolution factor.  Both fields are defended the same way — a value
 * below its own minimum means the field was never set, so it reads as 1 — which
 * is what keeps a zero-initialised transform behaving as the identity rather
 * than as a division by zero. */
static int transform_scale(const dyt_view_transform_t *t)
{
    int z = (t->zoom >= DYT_ZOOM_MIN) ? t->zoom : DYT_ZOOM_MIN;
    int s = (t->sr   >= DYT_SR_MIN)   ? t->sr   : DYT_SR_MIN;

    return z * s;
}

void dyt_view_transform_size(const dyt_view_transform_t *t,
                             int src_w, int src_h, int *dst_w, int *dst_h)
{
    int z = t ? transform_scale(t) : DYT_ZOOM_MIN;
    int q = t ? rot_quarter(t->rot) : 0;
    /* A quarter turn swaps the axes: at 90° the output is as wide as the
     * source is tall.  This is why the front end must ask for the size rather
     * than assume src * zoom. */
    int rw = (q & 1) ? src_h : src_w;
    int rh = (q & 1) ? src_w : src_h;

    if (dst_w) *dst_w = rw * z;
    if (dst_h) *dst_h = rh * z;
}

int dyt_view_transform_map(const dyt_view_transform_t *t,
                           int src_w, int src_h, int dst_w, int dst_h,
                           int ox, int oy, int *sx, int *sy)
{
    int z, q, x, y, rw, rh, ux, uy;

    if (!t || src_w <= 0 || src_h <= 0)
        return -1;
    if (ox < 0 || oy < 0 || ox >= dst_w || oy >= dst_h)
        return -1;

    z = transform_scale(t);
    q = rot_quarter(t->rot);

    /* The output is the source rotated, magnified and then mirrored, so undo
     * that in reverse: the mirror in output space first, then divide by the
     * magnification (nearest-neighbour), which leaves a pixel of the *rotated*
     * image. */
    x = t->flip_h ? (dst_w - 1 - ox) : ox;
    y = t->flip_v ? (dst_h - 1 - oy) : oy;
    x /= z;
    y /= z;

    rw = (q & 1) ? src_h : src_w;
    rh = (q & 1) ? src_w : src_h;
    if (x < 0 || y < 0 || x >= rw || y >= rh)
        return -1;

    /* Finally undo the rotation.  The forward form is in project(); each case
     * here is its exact inverse. */
    switch (q) {
    case 1:  ux = y;             uy = src_h - 1 - x; break;   /* 90 cw */
    case 2:  ux = src_w - 1 - x; uy = src_h - 1 - y; break;   /* 180 */
    case 3:  ux = src_w - 1 - y; uy = x;             break;   /* 270 cw */
    default: ux = x;             uy = y;             break;   /* 0 */
    }

    if (sx) *sx = ux;
    if (sy) *sy = uy;
    return 0;
}

int dyt_view_transform_project(const dyt_view_transform_t *t,
                               int src_w, int src_h, int dst_w, int dst_h,
                               int sx, int sy, int *ox, int *oy)
{
    int z, q, x, y;

    if (!t || src_w <= 0 || src_h <= 0)
        return -1;
    if (sx < 0 || sy < 0 || sx >= src_w || sy >= src_h)
        return -1;

    z = transform_scale(t);
    q = rot_quarter(t->rot);

    /* Rotate first, into the rotated image's own coordinates.  A source
     * pixel (sx, sy) lands at the position below; map() inverts each case. */
    switch (q) {
    case 1:  x = src_h - 1 - sy; y = sx;             break;   /* 90 cw */
    case 2:  x = src_w - 1 - sx; y = src_h - 1 - sy; break;   /* 180 */
    case 3:  x = sy;             y = src_w - 1 - sx; break;   /* 270 cw */
    default: x = sx;             y = sy;             break;   /* 0 */
    }

    /* Then the magnification — the centre of the block — and the mirror last,
     * in output space.  The exact inverse of map()'s "undo mirror, then
     * divide". */
    x = x * z + z / 2;
    y = y * z + z / 2;
    if (t->flip_h) x = dst_w - 1 - x;
    if (t->flip_v) y = dst_h - 1 - y;

    if (x < 0 || y < 0 || x >= dst_w || y >= dst_h)
        return -1;

    if (ox) *ox = x;
    if (oy) *oy = y;
    return 0;
}

/* ------------------------------------------------- grayscale mapping
 *
 * The algorithm is the vendor's `linearPlatKernel` (RE Docs 03 §3.5.1):
 *
 *   linearOut = (v - (lo+hi)/2) * 128 / Deta + 128   -- [lo,hi] -> [64,192]
 *   platOut   = 255 * acc[v] / total                 -- histogram CDF
 *   gray      = clamp(linearOut*linearPercent
 *                     + platOut*platPercent + detail, 0, 255)
 *
 * Everything is integer arithmetic, as in the kernel, so the port reproduces
 * the vendor's rounding rather than approximating it.
 */

void dyt_gray_params_default(dyt_gray_params_t *p)
{
    if (!p)
        return;

    p->lo             = 0;
    p->hi             = DYT_GRAY_LEVELS - 1;
    p->linear_percent = 0.5f;
    p->plat_percent   = 0.5f;
    p->detail_clamp   = DYT_GRAY_DETAIL_CLAMP;
}

int dyt_gray_cdf_build(const uint8_t *grey, int n, int *acc, long *total)
{
    int hist[DYT_GRAY_LEVELS];
    int i;
    long sum = 0;

    if (!grey || !acc || !total || n < 0)
        return -1;

    for (i = 0; i < DYT_GRAY_LEVELS; i++)
        hist[i] = 0;

    for (i = 0; i < n; i++)
        hist[grey[i]]++;

    /* acc[L] = number of samples <= L, which is what the kernel indexes. */
    for (i = 0; i < DYT_GRAY_LEVELS; i++) {
        sum += hist[i];
        acc[i] = (int)sum;
    }

    *total = sum;
    return 0;
}

int dyt_gray_plateau_window(const int *acc, long total,
                            int low_pct, int high_pct, int *lo, int *hi)
{
    long want_lo, want_hi;
    int  l, h;

    if (!acc || !lo || !hi || total <= 0)
        return -1;

    if (low_pct  < 0)   low_pct  = 0;
    if (high_pct > 100) high_pct = 100;
    if (low_pct > high_pct)
        low_pct = high_pct;

    want_lo = (long)((double)total * low_pct  / 100.0);
    want_hi = (long)((double)total * high_pct / 100.0);

    /* First level whose cumulative count reaches the requested percentile. */
    for (l = 0; l < DYT_GRAY_LEVELS - 1 && acc[l] < want_lo; l++)
        ;
    for (h = DYT_GRAY_LEVELS - 1; h > 0 && acc[h - 1] >= want_hi; h--)
        ;

    /* A flat plane puts both percentiles on the same level; widen so the
     * stretch never divides by zero. */
    if (h <= l)
        h = l + 1 < DYT_GRAY_LEVELS ? l + 1 : l;

    *lo = l;
    *hi = h;
    return 0;
}

float dyt_gray_linear(int v, const dyt_gray_params_t *p)
{
    int deta;

    if (!p)
        return 0.0f;

    deta = p->hi - p->lo;
    if (deta <= 0)
        return 128.0f;          /* nothing to stretch: neutral mid-grey */

    /* Clamp to the window exactly as the kernel does before the arithmetic,
     * so an out-of-window sample yields the same endpoint value. */
    if (v > p->hi) v = p->hi;
    if (v < p->lo) v = p->lo;

    return (float)((v - (p->hi + p->lo) / 2) * 128 / deta + 128);
}

float dyt_gray_plateau(int v, const int *acc, long total,
                       const dyt_gray_params_t *p)
{
    (void)p;

    if (!acc || total <= 0)
        return 0.0f;

    if (v < 0) v = 0;
    if (v >= DYT_GRAY_LEVELS) v = DYT_GRAY_LEVELS - 1;

    return (float)(255L * acc[v] / total);
}

int dyt_gray_map(int v, const int *acc, long total,
                 const dyt_gray_params_t *p, int detail)
{
    float lin, plat;
    int   gray;

    if (!p)
        return 0;

    lin  = dyt_gray_linear(v, p);
    plat = dyt_gray_plateau(v, acc, total, p);

    gray = (int)(lin * p->linear_percent + plat * p->plat_percent +
                 (float)detail);

    if (gray > 255) gray = 255;
    if (gray < 0)   gray = 0;
    return gray;
}

int dyt_gray_render(const uint8_t *grey, int n, const int *acc, long total,
                    const dyt_gray_params_t *p, const int *detail,
                    uint8_t *out)
{
    int i;

    if (!grey || !out || !p || n < 0)
        return -1;

    for (i = 0; i < n; i++)
        out[i] = (uint8_t)dyt_gray_map(grey[i], acc, total, p,
                                       detail ? detail[i] : 0);
    return 0;
}

int dyt_gray_detail(const uint8_t *grey, int w, int h,
                    float sigma_d, float sigma_r, int ksize,
                    int detail_clamp, int *detail_out)
{
    float w5[25];               /* raw 5x5 Gaussian */
    int   gw[25];               /* the same, fixed point, summing to 128 */
    float wsum = 0.0f;
    int   x, y, K, L, i;
    int   half;

    if (!grey || !detail_out || w <= 0 || h <= 0)
        return -1;
    if (ksize < 1) ksize = 5;
    if (ksize > 5)  ksize = 5;      /* the kernel's gsPara is a fixed 5x5 */
    if (ksize % 2 == 0) ksize++;    /* need a centre */
    if (!(sigma_d > 0.0f) || !(sigma_r > 0.0f))
        return -1;

    half = ksize / 2;

    /* The vendor's gsPara table is fixed point with a >> 7, i.e. its weights
     * sum to 128.  Build the 5x5 Gaussian from sigma_d and normalise it
     * *before* scaling to 128 — scaling the raw exponentials would not sum to
     * 128 at all (a wide sigma makes their sum several thousand), and the
     * residue correction below would then drive the centre tap negative. */
    for (L = -2; L <= 2; L++) {
        for (K = -2; K <= 2; K++) {
            float d = (float)(K * K + L * L) / (2.0f * sigma_d * sigma_d);
            w5[(L + 2) * 5 + (K + 2)] = expf(-d);
            wsum += w5[(L + 2) * 5 + (K + 2)];
        }
    }
    if (!(wsum > 0.0f))
        return -1;

    for (i = 0; i < 25; i++)
        gw[i] = (int)(w5[i] / wsum * 128.0f + 0.5f);

    /* Absorb the rounding residue in the centre tap (index 12), so the
     * fixed-point sum is exactly 128 and `gs_sum >> 7` is unbiased. */
    {
        int diff = 128;
        for (i = 0; i < 25; i++)
            diff -= gw[i];
        gw[12] += diff;
    }

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int   fij = grey[y * w + x];
            float num = 0.0f, den = 0.0f;
            int   gs_sum = 0;

            for (L = -half; L <= half; L++) {
                for (K = -half; K <= half; K++) {
                    int fkl, kk, ll;

                    if (y + L < 0 || x + K < 0 ||
                        y + L >= h || x + K >= w) {
                        fkl = fij;              /* edge repeats the centre */
                    } else {
                        fkl = grey[(y + L) * w + (x + K)];
                    }

                    kk = K + 2; ll = L + 2;
                    {
                        float dkl = -(float)(K * K + L * L) /
                                    (2.0f * sigma_d * sigma_d);
                        float rkl = -(float)((fij - fkl) * (fij - fkl)) /
                                    (2.0f * sigma_r * sigma_r);
                        float wkl = expf(dkl + rkl);

                        num += (float)fkl * wkl;
                        den += wkl;
                    }

                    /* ksize is clamped to 5, so half == 2 and (kk, ll) are
                     * always inside the 5x5 table. */
                    gs_sum += fkl * gw[ll * 5 + kk];
                }
            }

            {
                int bil = den != 0.0f ? (int)(num / den) : 0;
                int gs  = gs_sum >> 7;
                int sub = bil - gs;

                if (sub < -detail_clamp) sub = -detail_clamp;
                if (sub >  detail_clamp) sub =  detail_clamp;
                detail_out[y * w + x] = sub;
            }
        }
    }

    return 0;
}
