/*
 * measure.c — measurement geometry over a temperature plane (see measure.h).
 *
 * All of it is one or two passes over the caller's plane; nothing is
 * allocated, and nothing is retained.  The ROI median is the only operation
 * that needs memory, and it borrows the caller's scratch buffer.
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -I. -c measure.c -o measure.o
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "measure.h"

/* ------------------------------------------------------------- point probe */

int dyt_measure_point(const float *temps, int w, int h, int x, int y, float *out)
{
    float v;

    if (!temps || !out || w <= 0 || h <= 0)
        return -1;
    if (x < 0 || y < 0 || x >= w || y >= h)
        return -1;

    v = temps[(size_t)y * w + x];
    if (!isfinite(v))
        return -1;

    *out = v;
    return 0;
}

/* ------------------------------------------------------------- rectangle ROI */

static int cmp_float(const void *a, const void *b)
{
    float fa = *(const float *)a, fb = *(const float *)b;
    /* NaN never reaches the sort (finite samples only), but order them last
     * anyway so the comparator stays a strict weak ordering. */
    if (fa < fb) return -1;
    if (fa > fb) return  1;
    return 0;
}

int dyt_measure_roi(const float *temps, int w, int h,
                    int x0, int y0, int x1, int y1,
                    float *scratch, int scratch_cap,
                    dyt_roi_stats_t *out)
{
    int ax0, ay0, ax1, ay1, rw, rh, x, y, need;
    long n = 0;
    double sum = 0.0;
    float  mn = 0.f, mx = 0.f;
    int    mnx = -1, mny = -1, mxx = -1, mxy = -1;

    if (!temps || !out || w <= 0 || h <= 0)
        return -1;

    /* Accept the corners in any order and clip to the image. */
    ax0 = x0 < x1 ? x0 : x1;
    ax1 = x0 < x1 ? x1 : x0;
    ay0 = y0 < y1 ? y0 : y1;
    ay1 = y0 < y1 ? y1 : y0;

    if (ax0 < 0) ax0 = 0;
    if (ay0 < 0) ay0 = 0;
    if (ax1 > w - 1) ax1 = w - 1;
    if (ay1 > h - 1) ay1 = h - 1;

    rw = ax1 - ax0 + 1;
    rh = ay1 - ay0 + 1;

    out->n = 0;
    if (rw <= 0 || rh <= 0) {
        /* Entirely outside the image: a valid "nothing selected" result. */
        out->min = out->max = out->mean = out->median = NAN;
        out->min_x = out->min_y = out->max_x = out->max_y = -1;
        return 0;
    }

    need = rw * rh;
    if (scratch && scratch_cap < need) {
        out->n = need;          /* tell the caller how much to grow to */
        return -2;
    }

    for (y = ay0; y <= ay1; y++) {
        const float *row = temps + (size_t)y * w;
        for (x = ax0; x <= ax1; x++) {
            float v = row[x];
            if (!isfinite(v))
                continue;                       /* non-physical marker */
            if (n == 0 || v < mn) { mn = v; mnx = x; mny = y; }
            if (n == 0 || v > mx) { mx = v; mxx = x; mxy = y; }
            sum += v;
            if (scratch)
                scratch[n] = v;
            n++;
        }
    }

    out->n = (int)n;
    if (n == 0) {
        out->min = out->max = out->mean = out->median = NAN;
        out->min_x = out->min_y = out->max_x = out->max_y = -1;
        return 0;
    }

    out->min = mn;
    out->max = mx;
    out->mean = (float)(sum / (double)n);
    out->min_x = mnx; out->min_y = mny;
    out->max_x = mxx; out->max_y = mxy;

    if (!scratch) {
        out->median = NAN;                      /* caller did not ask for it */
        return 0;
    }

    qsort(scratch, (size_t)n, sizeof scratch[0], cmp_float);
    out->median = (n & 1) ? scratch[n / 2]
                          : 0.5f * (scratch[n / 2 - 1] + scratch[n / 2]);
    return 0;
}

/* ------------------------------------------------------------- line profile */

int dyt_measure_line(const float *temps, int w, int h,
                     int x0, int y0, int x1, int y1,
                     float *out, int cap)
{
    int dx, dy, steps, n, i;

    if (!temps || !out || w <= 0 || h <= 0)
        return -1;

    dx = x1 - x0;
    dy = y1 - y0;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;

    steps = dx > dy ? dx : dy;
    n = steps + 1;                              /* inclusive of both ends */

    if (cap < n)
        return -n;                              /* negated required count */

    for (i = 0; i < n; i++) {
        float t = (n > 1) ? (float)i / (float)(n - 1) : 0.0f;
        int   x = (int)lroundf((float)x0 + (float)(x1 - x0) * t);
        int   y = (int)lroundf((float)y0 + (float)(y1 - y0) * t);

        /* Out-of-image points stay in the array as NaN so the caller's
         * x-axis keeps its spacing instead of silently shortening. */
        out[i] = (x >= 0 && y >= 0 && x < w && y < h)
                     ? temps[(size_t)y * w + x]
                     : NAN;
    }

    return n;
}

/* ------------------------------------------------------------- polygon ROI */

int dyt_measure_polygon(const float *temps, int w, int h,
                        const dyt_point_t *verts, int nvtx,
                        float *scratch, int scratch_cap,
                        dyt_roi_stats_t *out)
{
    float  xc[DYT_POLYGON_MAX_VTX];   /* this row's edge crossings */
    int    bx0, bx1, by0, by1, x, y, i, k;
    long   n = 0;
    double sum = 0.0;
    float  mn = 0.f, mx = 0.f;
    int    mnx = -1, mny = -1, mxx = -1, mxy = -1;
    int    short_scratch = 0;

    if (!temps || !out || !verts || w <= 0 || h <= 0)
        return -1;
    if (nvtx < 3 || nvtx > DYT_POLYGON_MAX_VTX)
        return -1;

    out->n = 0;

    /* The bounding box, clipped to the image: a row or column outside it
     * cannot hold a covered pixel, so it is never scanned.  This is also what
     * keeps a polygon drawn far off the frame cheap. */
    bx0 = bx1 = verts[0].x;
    by0 = by1 = verts[0].y;
    for (i = 1; i < nvtx; i++) {
        if (verts[i].x < bx0) bx0 = verts[i].x;
        if (verts[i].x > bx1) bx1 = verts[i].x;
        if (verts[i].y < by0) by0 = verts[i].y;
        if (verts[i].y > by1) by1 = verts[i].y;
    }

    if (bx1 < 0 || by1 < 0 || bx0 > w - 1 || by0 > h - 1) {
        /* Entirely outside the image: a valid "nothing selected" result. */
        out->min = out->max = out->mean = out->median = NAN;
        out->min_x = out->min_y = out->max_x = out->max_y = -1;
        return 0;
    }
    if (by0 < 0) by0 = 0;
    if (by1 > h - 1) by1 = h - 1;

    for (y = by0; y <= by1; y++) {
        int nc = 0;

        /* Where the outline crosses this row.  The half-open test
         * ((a.y > y) != (b.y > y)) counts a vertex once rather than twice,
         * which is what makes the span fill and a point-in-polygon test agree
         * on the boundary — and why the fill matches the drawn outline. */
        for (i = 0; i < nvtx; i++) {
            const dyt_point_t *a = &verts[i];
            const dyt_point_t *b = &verts[(i + 1) % nvtx];

            if ((a->y > y) != (b->y > y))
                xc[nc++] = (float)a->x +
                           (float)(y - a->y) * (float)(b->x - a->x) /
                           (float)(b->y - a->y);
        }
        if (nc < 2)
            continue;                 /* a tangent row covers nothing */

        /* Insertion sort: nc is bounded by the vertex count, and the list is
         * nearly ordered from one row to the next. */
        for (i = 1; i < nc; i++) {
            float v = xc[i];
            for (k = i - 1; k >= 0 && xc[k] > v; k--)
                xc[k + 1] = xc[k];
            xc[k + 1] = v;
        }

        /* Even-odd: the spans between crossing pairs are inside.  Pixel x is
         * covered for x in [ceil(lo), ceil(hi) - 1], the integer form of
         * "lo <= x < hi" — the same half-open rule the crossings used. */
        for (i = 0; i + 1 < nc; i += 2) {
            int sx = (int)ceilf(xc[i]);
            int ex = (int)ceilf(xc[i + 1]) - 1;
            const float *row;

            if (sx < 0) sx = 0;
            if (ex > w - 1) ex = w - 1;
            if (sx > ex)
                continue;

            row = temps + (size_t)y * w;
            for (x = sx; x <= ex; x++) {
                float v = row[x];
                if (!isfinite(v))
                    continue;                       /* non-physical marker */
                if (n == 0 || v < mn) { mn = v; mnx = x; mny = y; }
                if (n == 0 || v > mx) { mx = v; mxx = x; mxy = y; }
                sum += v;
                /* Fill the scratch only while it fits, and remember that it
                 * stopped: the median cannot be taken over a partial sample,
                 * so the caller is told to grow rather than handed a wrong
                 * one.  The count still completes, so the requirement is the
                 * true one. */
                if (scratch && n < scratch_cap)
                    scratch[n] = v;
                else if (scratch)
                    short_scratch = 1;
                n++;
            }
        }
    }

    out->n = (int)n;

    if (short_scratch)
        return -2;                    /* nothing was written past the end */

    if (n == 0) {
        out->min = out->max = out->mean = out->median = NAN;
        out->min_x = out->min_y = out->max_x = out->max_y = -1;
        return 0;
    }

    out->min = mn;
    out->max = mx;
    out->mean = (float)(sum / (double)n);
    out->min_x = mnx; out->min_y = mny;
    out->max_x = mxx; out->max_y = mxy;

    if (!scratch) {
        out->median = NAN;                      /* caller did not ask for it */
        return 0;
    }

    qsort(scratch, (size_t)n, sizeof scratch[0], cmp_float);
    out->median = (n & 1) ? scratch[n / 2]
                          : 0.5f * (scratch[n / 2 - 1] + scratch[n / 2]);
    return 0;
}

/* --------------------------------------------------------- isotherm / area */

int dyt_measure_isotherm(const float *temps, int w, int h,
                         float lo, float hi, dyt_isotherm_t *out)
{
    long  count = 0, total = 0;
    float mn = 0.f, mx = 0.f;
    size_t i, npix;

    if (!temps || !out || w <= 0 || h <= 0 || hi < lo)
        return -1;

    npix = (size_t)w * h;
    for (i = 0; i < npix; i++) {
        float v = temps[i];
        if (!isfinite(v))
            continue;
        total++;
        if (v < lo || v > hi)
            continue;
        if (count == 0 || v < mn) mn = v;
        if (count == 0 || v > mx) mx = v;
        count++;
    }

    out->count    = count;
    out->total    = total;
    out->fraction = total > 0 ? (float)((double)count / (double)total) : 0.0f;
    out->min      = count > 0 ? mn : NAN;
    out->max      = count > 0 ? mx : NAN;
    return 0;
}
