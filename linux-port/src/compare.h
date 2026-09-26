/*
 * compare.h — two-board comparison: difference grid, stats, and a blend
 * preview, all device-free.
 *
 * The vendor app's "Comparison" feature (the Comparison tab in the Windows
 * panel) overlays a saved reference still against the live frame and reports
 * where they differ.  This module is the pure engine behind that tab:
 *
 *   - dyt_compare_diff()      per-pixel signed difference (Celsius-delta)
 *   - dyt_compare_stats()     min/max/mean of the difference, the location
 *                             of the largest absolute change, and how many
 *                             pixels exceed a threshold
 *   - dyt_compare_blend_rgb() 50/50 average of two RGB renders for the preview
 *
 * It follows the same pure-C discipline as measure.c: no allocation of its
 * own, nothing retained, no camera, no GUI.  The caller owns every buffer.
 * NaN samples (the pipeline's non-physical marker) propagate to the diff
 * grid and are skipped by the stats, so a region with no finite overlap
 * reports n == 0 and NaN statistics rather than a bogus 0 C-delta.
 *
 * The blend reuses fusion.c's DYT_FUSION_BLEND rounding ((a + b + 1) / 2,
 * sat8-clamped) so the preview is the same average the visible/thermal
 * blend produces — a real code-level reuse of the formula, not a second
 * invention.  The only difference is that the two inputs are both RGB
 * renders (reference + live) rather than RGB + grey.
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -I. -c compare.c
 */
#ifndef DYT_COMPARE_H
#define DYT_COMPARE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- diff stats */

typedef struct {
    float min;        /* most negative finite difference; NaN when n == 0 */
    float max;        /* most positive finite difference; NaN when n == 0 */
    float mean;       /* arithmetic mean of the finite differences */
    float max_abs;    /* the largest |difference| seen; sign-collapsed */
    int   max_abs_x;  /* where it is; -1 when n == 0 */
    int   max_abs_y;
    long  beyond;     /* pixels whose |difference| > threshold */
    long  n;          /* finite pairs actually counted */
} dyt_compare_stats_t;

/* Per-pixel signed difference: out[i] = a[i] - b[i].  A pair is skipped
 * (out[i] = NaN) when either sample is not finite, so a NaN never reaches
 * the stats.  Returns 0 on success, -1 on a bad argument. */
int dyt_compare_diff(const float *a, const float *b, int w, int h, float *out);

/* Statistics over the signed difference of two grids, plus a count of the
 * pixels whose absolute difference exceeds `threshold` (a > test, so a
 * threshold of 0 still excludes exact matches).  NaN pairs are skipped.
 * Returns 0 on success, -1 on a bad argument. */
int dyt_compare_stats(const float *a, const float *b, int w, int h,
                      float threshold, dyt_compare_stats_t *out);

/* 50/50 average of two RGB renders, channel-wise, with the same
 * (a + b + 1) / 2 rounding and sat8 clamp as DYT_FUSION_BLEND (fusion.c).
 * `out_rgb` may alias either `a_rgb` or `b_rgb`.  Returns 0 on success,
 * -1 on a bad argument. */
int dyt_compare_blend_rgb(const uint8_t *a_rgb, const uint8_t *b_rgb,
                          int w, int h, uint8_t *out_rgb);

#ifdef __cplusplus
}
#endif

#endif /* DYT_COMPARE_H */
