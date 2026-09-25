/*
 * measure.h — measurement geometry over a temperature plane.
 *
 * The vendor app's measurement tools (point probe, line profile, rectangle
 * ROI, isotherm/area check — RE Docs 03 §3.5 rows 33/34) are pure geometry
 * over the Celsius plane: no camera, no GUI.  They live here rather than in
 * a front-end so the OpenCV viewer and the Qt6 app report the *same* numbers
 * for the same pixels, and so they can be tested against a synthetic ramp
 * with no hardware.
 *
 * Everything is in source-pixel coordinates (the untransformed plane), which
 * is what the session stores.  A front-end that has applied mirror/zoom maps
 * its pointer through dyt_view_transform_map() first (display.h).
 *
 * NaN samples are the pipeline's non-physical marker, not a reading, so every
 * statistic here skips them.  A region with no finite samples reports
 * n == 0 and NaN statistics rather than a bogus 0 C.
 *
 * Pure: no camera, no GUI, no allocation of its own (the ROI median needs a
 * scratch buffer, which the caller owns and may reuse across frames).
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -I. -c measure.c
 */
#ifndef DYT_MEASURE_H
#define DYT_MEASURE_H

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- point probe */

typedef struct {
    int x, y;
} dyt_point_t;

/* The temperature at one pixel.  Returns 0 and writes *out on success, -1 if
 * the point is outside the image or the sample is not finite — so a caller
 * never displays a NaN as a measurement. */
int dyt_measure_point(const float *temps, int w, int h, int x, int y, float *out);

/* ------------------------------------------------------------- rectangle ROI */

typedef struct {
    float min, max, mean, median;
    int   min_x, min_y;   /* where the extremes are; -1 when n == 0 */
    int   max_x, max_y;
    int   n;              /* finite samples actually counted */
} dyt_roi_stats_t;

/* Statistics over the inclusive rectangle spanned by the two corners.  The
 * corners may be given in any order; the rectangle is clipped to the image.
 *
 * `scratch` (scratch_cap floats) is used for the median's sort and is only
 * touched if it is large enough — pass NULL/0 to get everything except the
 * median (which is then NaN).  Returns 0 on success, -1 on a bad argument,
 * -2 if scratch is non-NULL but too small for the (clipped) region, in which
 * case *out->n reports how many floats are needed. */
int dyt_measure_roi(const float *temps, int w, int h,
                    int x0, int y0, int x1, int y1,
                    float *scratch, int scratch_cap,
                    dyt_roi_stats_t *out);

/* ------------------------------------------------------------- line profile */

/* Sample the inclusive line (x0,y0)..(x1,y1) into `out` (cap floats).
 *
 * Points are spaced one pixel apart along the major axis, so the count is
 * max(|dx|,|dy|) + 1 — the same convention the vendor's profile ruler uses.
 * Out-of-image points are written as NaN rather than dropped, so the caller's
 * x-axis stays linear.
 *
 * Returns the number of points written (>= 1) on success, -1 on a bad
 * argument, -2 if cap is too small — in which case the return value is the
 * negated required count. */
int dyt_measure_line(const float *temps, int w, int h,
                     int x0, int y0, int x1, int y1,
                     float *out, int cap);

/* --------------------------------------------------------- isotherm / area */

typedef struct {
    long  count;      /* pixels inside [lo, hi] */
    long  total;      /* finite pixels considered */
    float fraction;   /* count / total, 0 when total == 0 */
    float min, max;   /* extremes *within* the band; NaN when count == 0 */
} dyt_isotherm_t;

/* Count the pixels whose temperature lies in [lo, hi] (inclusive), and report
 * the extremes of just those pixels.  This is the vendor's area check: the
 * highlighted area and its statistics.  Returns 0 on success, -1 on a bad
 * argument or if hi < lo. */
int dyt_measure_isotherm(const float *temps, int w, int h,
                         float lo, float hi, dyt_isotherm_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DYT_MEASURE_H */
