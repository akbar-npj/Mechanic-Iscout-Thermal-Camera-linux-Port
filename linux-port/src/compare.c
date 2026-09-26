/*
 * compare.c — two-board comparison (see compare.h).
 *
 * One or two passes over the caller's grids; nothing allocated, nothing
 * retained.  The pure-C, caller-owns-scratch discipline is the same as
 * measure.c.
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -I. -c compare.c
 */
#include <math.h>
#include <string.h>

#include "compare.h"

/* Same saturation clamp as fusion.c — kept local so compare.c has no
 * dependency on fusion's internals, only on its documented formula. */
static uint8_t sat8(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v);
}

/* ------------------------------------------------------------- diff stats */

int dyt_compare_diff(const float *a, const float *b, int w, int h, float *out)
{
    size_t i, npix;

    if (!a || !b || !out || w <= 0 || h <= 0)
        return -1;

    npix = (size_t)w * (size_t)h;
    for (i = 0; i < npix; i++) {
        float va = a[i], vb = b[i];
        out[i] = (isfinite(va) && isfinite(vb)) ? va - vb : NAN;
    }
    return 0;
}

int dyt_compare_stats(const float *a, const float *b, int w, int h,
                      float threshold, dyt_compare_stats_t *out)
{
    size_t i, npix;
    long   n = 0, beyond = 0;
    double sum = 0.0;
    float  mn = 0.f, mx = 0.f, mxabs = 0.f;
    int    mxabs_x = -1, mxabs_y = -1;

    if (!a || !b || !out || w <= 0 || h <= 0)
        return -1;

    out->min = out->max = out->mean = out->max_abs = NAN;
    out->max_abs_x = out->max_abs_y = -1;
    out->beyond = 0;
    out->n = 0;

    npix = (size_t)w * (size_t)h;
    for (i = 0; i < npix; i++) {
        float va = a[i], vb = b[i], d;

        if (!isfinite(va) || !isfinite(vb))
            continue;                       /* non-physical marker */

        d = va - vb;

        /* First-extreme-wins on ties, matching dyt_measure_roi()'s strict
         * </> and dyt_frame_stats(). */
        if (n == 0 || d < mn) mn = d;
        if (n == 0 || d > mx) mx = d;

        {
            float ad = d < 0 ? -d : d;
            if (n == 0 || ad > mxabs) {
                mxabs = ad;
                mxabs_x = (int)(i % (size_t)w);
                mxabs_y = (int)(i / (size_t)w);
            }
            if (ad > threshold)
                beyond++;
        }

        sum += d;
        n++;
    }

    out->n = n;
    out->beyond = beyond;
    if (n == 0)
        return 0;                           /* NaNs already written */

    out->min     = mn;
    out->max     = mx;
    out->mean    = (float)(sum / (double)n);
    out->max_abs = mxabs;
    out->max_abs_x = mxabs_x;
    out->max_abs_y = mxabs_y;
    return 0;
}

/* ------------------------------------------------------------- blend */

int dyt_compare_blend_rgb(const uint8_t *a_rgb, const uint8_t *b_rgb,
                          int w, int h, uint8_t *out_rgb)
{
    size_t i, npix;

    if (!a_rgb || !b_rgb || !out_rgb || w <= 0 || h <= 0)
        return -1;

    npix = (size_t)w * (size_t)h * 3;
    for (i = 0; i < npix; i++)
        out_rgb[i] = sat8(((int)a_rgb[i] + (int)b_rgb[i] + 1) / 2);
    return 0;
}
