/*
 * display.h — temperature→display mapping state for the DYT Linux port.
 *
 * The viewer today open-codes four things that are not viewer-specific and
 * that the Qt6 front-end will need identically: which temperature range is
 * shown (auto vs locked), which palette is active, how the image is mirrored
 * and zoomed, and whether the frame is still the device's start-up filler.
 * Keeping them in the tool means the second front-end reimplements them —
 * and the filler rule in particular is subtle enough to get wrong.  So they
 * live here instead.
 *
 * Pure: no camera, no GUI, no image library.  The renderer itself stays in
 * palette.c (dyt_render_rgb); this module only decides the parameters that
 * renderer is called with.
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -I. -c display.c
 */
#ifndef DYT_DISPLAY_H
#define DYT_DISPLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- the filler
 *
 * Before the device has real data it streams a flat 0x8000 on every pixel.
 * In mode 1000 that decodes to a perfectly legitimate-looking 238.85 C —
 * unlike mode 0x44c, where the pipeline's 0x4000 gate rejects the filler
 * outright — so a client has to recognise it or it will confidently paint a
 * 238.85 C "scene" and collapse the colour scale to one value.  Real scenes
 * on this unit read ~30 C, so the window below is deliberately narrow; the
 * filler→live transition overshoots it by only a few degrees for a handful
 * of frames.  See src/capture.c send_ad_order and dytview's original note.
 */
#define DYT_FILLER_C    (32768.0f / 64.0f - 273.15f)   /* 238.85 C */
#define DYT_FILLER_TOL  12.0f

/* --------------------------------------------------------- frame statistics */

typedef struct {
    float lo, hi;      /* min/max over finite samples; undefined if n_valid==0 */
    float mean;        /* mean over finite samples;    undefined if n_valid==0 */
    int   n_valid;     /* count of non-NaN samples */
    int   all_filler;  /* every finite sample is within the filler window */

    /* Coordinates of the extremes, in source pixels.  -1 when there are no
     * finite samples.  The vendor app draws on-screen markers for these
     * (openFeaturePoints(0/1/2), RE Docs 03 §3.5), so they are part of the
     * same single pass rather than a second scan. */
    int   hot_x, hot_y;    /* hottest pixel  (argmax) */
    int   cold_x, cold_y;  /* coldest pixel  (argmin) */
} dyt_frame_stats_t;

/* One pass over a `w` x `h` temperature image.  NaN samples are skipped
 * everywhere (they are the pipeline's non-physical marker, not a reading).
 *
 * `all_filler` is true when there are no finite samples at all, or when
 * every finite sample sits within DYT_FILLER_TOL of DYT_FILLER_C.  An
 * all-NaN frame therefore counts as filler, so it is never mistaken for a
 * measurement.
 *
 * Returns 0 on success, -1 on a bad argument (NULL, w <= 0, h <= 0). */
int dyt_frame_stats(const float *temps, int w, int h, dyt_frame_stats_t *out);

/* ------------------------------------------------------------ range state */

typedef enum {
    DYT_RANGE_AUTO = 0,   /* re-fit lo/hi to every frame */
    DYT_RANGE_FIXED       /* hold fixed_lo/fixed_hi (the vendor's "locked" bar) */
} dyt_range_mode_t;

typedef struct {
    dyt_range_mode_t mode;
    float fixed_lo, fixed_hi;   /* used while mode == DYT_RANGE_FIXED */
    float lo, hi;               /* the range resolved for the last frame */
    int   palette;              /* active palette index */
    int   palette_n;            /* how many palettes are available */
} dyt_display_t;

/* Initialise with an auto range over the default palette.  If `lock` is
 * non-zero the range starts FIXED to [lo, hi] — this is what the viewer's
 * --lo/--hi do at startup. */
void dyt_display_init(dyt_display_t *d, float lo, float hi, int lock);

void dyt_display_set_mode(dyt_display_t *d, dyt_range_mode_t m);

/* AUTO <-> FIXED.  Entering FIXED latches the range last resolved, so the
 * image does not jump; entering AUTO resumes per-frame fitting.  Backs the
 * viewer's "r" key. */
void dyt_display_toggle_mode(dyt_display_t *d);

/* Pin the range and switch to FIXED.  A degenerate pair (hi <= lo) is
 * widened by ±1 so the renderer never divides by zero. */
void dyt_display_set_fixed(dyt_display_t *d, float lo, float hi);

void dyt_display_set_palette(dyt_display_t *d, int idx);

/* Step the palette by `dir` (±1), wrapping.  Backs the palette keys. */
void dyt_display_cycle_palette(dyt_display_t *d, int dir);

/* Resolve d->lo/hi for one frame from its statistics.
 *
 * AUTO adopts the frame's own min/max.  FIXED holds fixed_lo/fixed_hi.  In
 * both cases a degenerate result (hi <= lo — a flat frame, or a fixed pair
 * the caller got wrong) is widened by ±1 so the renderer has a non-zero
 * span to normalise against.  A frame with no finite samples leaves the
 * previous range untouched. */
void dyt_display_update(dyt_display_t *d, const dyt_frame_stats_t *st);

/* -------------------------------------------------------- mirror and zoom */

#define DYT_ZOOM_MIN 1
#define DYT_ZOOM_MAX 8

typedef struct {
    int flip_h;   /* mirror left<->right */
    int flip_v;   /* mirror top<->bottom */
    int zoom;     /* integer magnification, DYT_ZOOM_MIN..DYT_ZOOM_MAX */
} dyt_view_transform_t;

/* Identity: no flip, zoom 1. */
void dyt_view_transform_init(dyt_view_transform_t *t);

/* Toggle the mirrors.  Backs the viewer's "h"/"v" keys. */
void dyt_view_transform_toggle_flip_h(dyt_view_transform_t *t);
void dyt_view_transform_toggle_flip_v(dyt_view_transform_t *t);

/* Step the zoom by `delta`, clamped to DYT_ZOOM_MIN..DYT_ZOOM_MAX. */
void dyt_view_transform_zoom(dyt_view_transform_t *t, int delta);

/* Size of the transformed output for a given source size. */
void dyt_view_transform_size(const dyt_view_transform_t *t,
                             int src_w, int src_h, int *dst_w, int *dst_h);

/* Map output pixel (ox, oy) in a dst_w x dst_h image back to source pixel
 * coordinates in a src_w x src_h image.
 *
 * The output is the source magnified by `zoom` and then mirrored, so the
 * mapping undoes the mirror in output space and then divides by the zoom.
 * Nearest-neighbour semantics: every output pixel maps to exactly one source
 * pixel.  Returns 0 on success, -1 if the point falls outside the source
 * (which a caller iterating its own output should not see). */
int dyt_view_transform_map(const dyt_view_transform_t *t,
                           int src_w, int src_h, int dst_w, int dst_h,
                           int ox, int oy, int *sx, int *sy);

/* The inverse of dyt_view_transform_map, for placing overlays: where does
 * source pixel (sx, sy) land in the output?  The point is the centre of the
 * magnified block, so a marker sits on the pixel rather than at its corner.
 * Returns 0 on success, -1 if the source point is out of range or the result
 * falls outside the output. */
int dyt_view_transform_project(const dyt_view_transform_t *t,
                               int src_w, int src_h, int dst_w, int dst_h,
                               int sx, int sy, int *ox, int *oy);

/* ------------------------------------------------- grayscale mapping
 *
 * The *visible* half of the dual-half frame is an 8-bit grey image, and the
 * vendor processes it before showing it: a linear stretch blended with a
 * histogram equalisation ("plateau"), plus a detail layer from a bilateral
 * filter.  The algorithm below is recovered verbatim from the OpenCL kernels
 * embedded in libsimplePictureProcessing.so — see RE Docs 03 §3.5.1, which
 * also lists the tuning constants that are *not* statically recoverable.
 *
 * It lives in display.c because it is the same concern as the temperature
 * side: deciding what value a pixel is shown as.  Pure: no camera, no GUI.
 */

#define DYT_GRAY_LEVELS      256   /* the visible plane is 8-bit */
#define DYT_GRAY_DETAIL_CLAMP 30   /* the vendor's hard ±30 on the detail term */

typedef struct {
    int   lo, hi;            /* input window; the kernel's min/max, Deta = hi-lo */
    float linear_percent;    /* weight of the linear stretch */
    float plat_percent;      /* weight of the histogram-CDF term */
    int   detail_clamp;      /* ± clamp on the detail layer */
} dyt_gray_params_t;

/* lo=0, hi=255, an equal linear/equalisation blend, and the vendor's ±30.
 * The blend weights have no matching vendor handler, so an even split is the
 * documented choice (RE Docs 03 §3.5.1). */
void dyt_gray_params_default(dyt_gray_params_t *p);

/* Cumulative histogram of an 8-bit grey plane: acc[L] = samples <= L, for
 * L in 0..DYT_GRAY_LEVELS-1.  `total` receives the sample count (0 for an
 * empty plane).  Returns 0 on success, -1 on a bad argument. */
int dyt_gray_cdf_build(const uint8_t *grey, int n, int *acc, long *total);

/* The vendor's "plateau" bounds (handleSetHighPlat/LowPlat): the input
 * window that clips the given percentages off each end of the histogram.
 * Returns 0 and writes *lo and *hi, or -1 on a bad argument or an empty
 * plane.  A degenerate result is widened by one level so hi > lo always
 * holds. */
int dyt_gray_plateau_window(const int *acc, long total,
                            int low_pct, int high_pct, int *lo, int *hi);

/* The linear stretch term for one sample, exactly the kernel's
 * `(v - (lo+hi)/2) * 128 / Deta + 128` in integer arithmetic: it maps
 * [lo, hi] onto [64, 192], deliberately using only the middle of the range. */
float dyt_gray_linear(int v, const dyt_gray_params_t *p);

/* The histogram-equalisation term for one sample, the kernel's
 * `255 * acc[v] / total`.  Returns 0 for an empty plane. */
float dyt_gray_plateau(int v, const int *acc, long total,
                       const dyt_gray_params_t *p);

/* The full mapping for one sample: the blended terms plus `detail`, clamped
 * to 0..255.  This is `linearPlatKernel`'s body with kindOfPalette == 0. */
int dyt_gray_map(int v, const int *acc, long total,
                 const dyt_gray_params_t *p, int detail);

/* Map a whole plane.  `detail` may be NULL for no detail term.  Returns 0 on
 * success, -1 on a bad argument. */
int dyt_gray_render(const uint8_t *grey, int n, const int *acc, long total,
                    const dyt_gray_params_t *p, const int *detail,
                    uint8_t *out);

/* The detail layer: a bilateral filter minus a 5x5 Gaussian blur of the same
 * plane, clamped to ±detail_clamp.  `sigma_d` is the spatial sigma and
 * `sigma_r` the range (intensity) sigma — the vendor's handleSetSigmaD/R.
 * The Gaussian weights are generated from sigma_d and normalised to sum to
 * 128, matching the kernel's fixed-point `>> 7`.
 *
 * `detail_out` is w*h ints and must be provided.  Returns 0 on success, -1
 * on a bad argument.  This is the expensive part of the pipeline: it is
 * O(w*h*ksize^2), so a front-end should call it per frame, not per repaint.
 *
 * Note one deliberate fidelity detail: the kernel computes
 * `Bil = (short)(numerator / denominator)`, a *truncating* cast, and a
 * perfectly flat plane then yields -1 rather than 0 — the float ratio comes
 * out just under the true integer (99.999… -> 99).  That is reproduced here
 * rather than rounded away, so a front-end should expect |detail| <= 1 on
 * flat regions instead of exactly 0. */
int dyt_gray_detail(const uint8_t *grey, int w, int h,
                    float sigma_d, float sigma_r, int ksize,
                    int detail_clamp, int *detail_out);

#ifdef __cplusplus
}
#endif

#endif /* DYT_DISPLAY_H */
