/*
 * fusion.c — the six fusion patterns.  See fusion.h for the derivation and
 * the two caveats about fidelity.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "fusion.h"

/* ------------------------------------------------------------------ helpers */

/* The vendor's edge pipeline parameters, straight from the decompilation
 * (RE Docs 03 §3.5.2):
 *   bilateralFilter(d = 7, sigmaColor = 11.0, sigmaSpace = 11.0, REFLECT_101)
 *   Sobel(ksize = 3, scale = 1, delta = 0) then convertScaleAbs
 *   threshold(70.0, 255.0, THRESH_TOZERO) for the black variant
 * `d` is OpenCV's diameter, so the window radius is d/2. */
#define DYT_EDGE_BILATERAL_D     7
#define DYT_EDGE_BILATERAL_SIGMA 11.0
#define DYT_EDGE_BLACK_THRESH    70

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static uint8_t sat8(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v);
}

/* OpenCV's BORDER_REFLECT_101: fedcba|abcdefgh|hgfedcb (the edge pixel is not
 * repeated).  The while loop covers a shift larger than one period, which the
 * alignment offset can produce. */
static int reflect101(int i, int n)
{
    if (n <= 1)
        return 0;
    while (i < 0 || i >= n) {
        if (i < 0)
            i = -i;
        else
            i = 2 * (n - 1) - i;
    }
    return i;
}

/* Sample the visible plane at (x, y) with the alignment applied, clamped at
 * the edges so a shift never introduces a black border. */
static uint8_t grey_at(const uint8_t *g, int w, int h, int x, int y, int dx, int dy)
{
    x = clampi(x + dx, 0, w - 1);
    y = clampi(y + dy, 0, h - 1);
    return g[y * w + x];
}

/* ------------------------------------------------------- edge pipeline pieces */

/* Bilateral filter, the standard form: a Gaussian in space times a Gaussian in
 * intensity.  This is what the vendor runs on the visible grey before looking
 * for edges — it smooths flat regions while keeping edges sharp. */
static void bilateral(const uint8_t *src, int w, int h, uint8_t *dst)
{
    const int r = DYT_EDGE_BILATERAL_D / 2;
    const double s2 = 2.0 * DYT_EDGE_BILATERAL_SIGMA * DYT_EDGE_BILATERAL_SIGMA;
    double spatial[DYT_EDGE_BILATERAL_D][DYT_EDGE_BILATERAL_D];
    double range[256];
    int i, j, x, y;

    for (j = -r; j <= r; j++)
        for (i = -r; i <= r; i++)
            spatial[j + r][i + r] = exp(-(double)(i * i + j * j) / s2);
    for (i = 0; i < 256; i++)
        range[i] = exp(-(double)(i * i) / s2);

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int    p = y * w + x;
            int    c = src[p];
            double sum = 0.0, wsum = 0.0;

            for (j = -r; j <= r; j++) {
                int sy = reflect101(y + j, h);
                for (i = -r; i <= r; i++) {
                    int sx = reflect101(x + i, w);
                    int q  = src[sy * w + sx];
                    int d  = q - c;
                    double wgt;

                    if (d < 0) d = -d;
                    wgt   = spatial[j + r][i + r] * range[d];
                    sum  += wgt * (double)q;
                    wsum += wgt;
                }
            }
            dst[p] = wsum > 0.0 ? (uint8_t)(sum / wsum + 0.5) : (uint8_t)c;
        }
    }
}

/* Sobel with OpenCV's ksize=3 kernels, with convertScaleAbs folded in so the
 * result is |gradient| saturated to a byte.  `dx`/`dy` select the direction. */
static void sobel_abs(const uint8_t *src, int w, int h, int dx, uint8_t *out)
{
    static const int kx[3][3] = { { -1, 0, 1 }, { -2, 0, 2 }, { -1, 0, 1 } };
    static const int ky[3][3] = { { -1, -2, -1 }, { 0, 0, 0 }, { 1, 2, 1 } };
    const int (*k)[3] = dx ? kx : ky;
    int x, y, i, j;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int acc = 0;
            for (j = -1; j <= 1; j++) {
                int sy = reflect101(y + j, h);
                for (i = -1; i <= 1; i++) {
                    int sx = reflect101(x + i, w);
                    acc += k[j + 1][i + 1] * (int)src[sy * w + sx];
                }
            }
            out[y * w + x] = sat8(acc < 0 ? -acc : acc);
        }
    }
}

/* 3x3 maximum filter.  Stands in for the vendor's two `dilate` calls in case
 * 5, whose structuring elements are not recoverable (RE Docs 03 §3.5.2). */
static void dilate3(const uint8_t *src, int w, int h, uint8_t *out)
{
    int x, y, i, j;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int mx = 0;
            for (j = -1; j <= 1; j++) {
                int sy = reflect101(y + j, h);
                for (i = -1; i <= 1; i++) {
                    int sx = reflect101(x + i, w);
                    int v  = src[sy * w + sx];
                    if (v > mx) mx = v;
                }
            }
            out[y * w + x] = (uint8_t)mx;
        }
    }
}

/* blur -> |Sobel_x| + |Sobel_y|, i.e. the vendor's case-2/5 edge image.
 * `scratch` must hold 3*w*h bytes. */
static void edge_magnitude(const uint8_t *grey, int w, int h,
                           uint8_t *scratch, uint8_t *mag_out)
{
    size_t n = (size_t)w * (size_t)h;
    uint8_t *blur = scratch;
    uint8_t *sx   = scratch + n;
    uint8_t *sy   = scratch + 2 * n;
    size_t i;

    bilateral(grey, w, h, blur);
    sobel_abs(blur, w, h, 1, sx);
    sobel_abs(blur, w, h, 0, sy);

    for (i = 0; i < n; i++)
        mag_out[i] = sat8((int)sx[i] + (int)sy[i]);
}

/* The picture-in-picture inset.  The vendor pastes a 240x240 thermal ROI at
 * (200,120) in a 640x480 visible frame; the port keeps the same *proportions*
 * (x = 5/16 W, y = 1/4 H, w = 3/8 W, h = 1/2 H), which at 256x192 is a 96x96
 * inset at (80,48). */
static void pip_rect(int w, int h, int *rx, int *ry, int *rw, int *rh)
{
    *rx = (5 * w) / 16;
    *ry = (1 * h) / 4;
    *rw = (3 * w) / 8;
    *rh = (1 * h) / 2;
    if (*rx < 0) *rx = 0;
    if (*ry < 0) *ry = 0;
    if (*rw < 0) *rw = 0;
    if (*rh < 0) *rh = 0;
    if (*rx + *rw > w) *rw = w - *rx;
    if (*ry + *rh > h) *rh = h - *ry;
}

/* ------------------------------------------------------------------ the API */

void dyt_fusion_cfg_default(dyt_fusion_cfg_t *cfg)
{
    if (!cfg)
        return;
    cfg->mode = DYT_FUSION_INFRARED;
    cfg->dx   = 0;
    cfg->dy   = 0;
}

const char *dyt_fusion_name(dyt_fusion_t f)
{
    switch (f) {
    case DYT_FUSION_INFRARED:   return "ir";
    case DYT_FUSION_VISIBLE:    return "visible";
    case DYT_FUSION_EDGE:       return "edge";
    case DYT_FUSION_BLEND:      return "blend";
    case DYT_FUSION_PIP:        return "pip";
    case DYT_FUSION_EDGE_BLACK: return "edge-black";
    default:                    return "?";
    }
}

int dyt_fusion_clamp_align(int v)
{
    return clampi(v, -DYT_FUSION_ALIGN_MAX, DYT_FUSION_ALIGN_MAX);
}

int dyt_fusion_apply(const dyt_fusion_cfg_t *cfg,
                     const uint8_t *therm_rgb, const uint8_t *grey,
                     int w, int h, uint8_t *out_rgb)
{
    dyt_fusion_t mode;
    size_t       n;
    int          dx, dy, x, y;

    if (!cfg || !therm_rgb || !out_rgb || w <= 0 || h <= 0)
        return -1;

    mode = cfg->mode;
    if (mode < 0 || mode >= DYT_FUSION_N)
        return -1;

    dx = dyt_fusion_clamp_align(cfg->dx);
    dy = dyt_fusion_clamp_align(cfg->dy);
    n  = (size_t)w * (size_t)h;

    /* Infrared is the identity: the thermal render *is* the picture.  The
     * caller may render in place (session.c does), so the copy is skipped
     * when the buffers already coincide — memcpy() forbids overlap even when
     * the pointers are equal. */
    if (mode == DYT_FUSION_INFRARED) {
        if (out_rgb != therm_rgb)
            memcpy(out_rgb, therm_rgb, n * 3);
        return 0;
    }

    if (!grey)
        return -1;

    if (mode == DYT_FUSION_VISIBLE) {
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                uint8_t g = grey_at(grey, w, h, x, y, dx, dy);
                uint8_t *o = out_rgb + ((size_t)y * w + x) * 3;
                o[0] = o[1] = o[2] = g;
            }
        }
        return 0;
    }

    if (mode == DYT_FUSION_BLEND) {
        /* addWeighted(visible, 0.5, thermal, 0.5, 0) */
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                size_t   p = (size_t)y * w + x;
                uint8_t  g = grey_at(grey, w, h, x, y, dx, dy);
                uint8_t *o = out_rgb + p * 3;
                const uint8_t *t = therm_rgb + p * 3;
                o[0] = sat8(((int)t[0] + g + 1) / 2);
                o[1] = sat8(((int)t[1] + g + 1) / 2);
                o[2] = sat8(((int)t[2] + g + 1) / 2);
            }
        }
        return 0;
    }

    if (mode == DYT_FUSION_PIP) {
        int rx, ry, rw, rh;

        /* Visible is the base, with the thermal image pasted into the inset.
         * Written as a single per-pixel choice rather than "fill, then paste"
         * so the render can be done in place — the caller may pass the same
         * buffer as therm_rgb (session.c does), and a fill-then-paste would
         * have already overwritten the thermal pixels it then copies. */
        pip_rect(w, h, &rx, &ry, &rw, &rh);
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                size_t   p = (size_t)y * w + x;
                uint8_t *o = out_rgb + p * 3;

                if (x >= rx && x < rx + rw && y >= ry && y < ry + rh) {
                    const uint8_t *t = therm_rgb + p * 3;
                    o[0] = t[0];
                    o[1] = t[1];
                    o[2] = t[2];
                } else {
                    uint8_t g = grey_at(grey, w, h, x, y, dx, dy);
                    o[0] = o[1] = o[2] = g;
                }
            }
        }
        return 0;
    }

    /* The two edge modes share the edge image.
     *
     * The alignment has to be applied *before* the edges are computed, not
     * after: the whole point of the coefficient is to register the visible
     * image with the thermal one (the vendor adds it to the thermal ROI's
     * origin inside the visible frame — RE Docs 03 §3.5.2), so an edge that
     * comes from the visible plane has to be sampled at the same offset as
     * the rest of that plane.  Fusing unshifted edges with a shifted picture
     * would leave this key doing nothing on two of the six patterns. */
    {
        uint8_t *buf     = malloc(n * 4);
        uint8_t *aligned = buf;
        uint8_t *scratch = buf ? buf + n : NULL;   /* 3n, for edge_magnitude */
        uint8_t *mag     = malloc(n);
        size_t   i;

        if (!buf || !mag) {
            free(buf);
            free(mag);
            return -1;
        }

        if (dx || dy) {
            for (y = 0; y < h; y++)
                for (x = 0; x < w; x++)
                    aligned[(size_t)y * w + x] =
                        grey_at(grey, w, h, x, y, dx, dy);
        } else {
            memcpy(aligned, grey, n);
        }

        edge_magnitude(aligned, w, h, scratch, mag);

        if (mode == DYT_FUSION_EDGE) {
            /* add(thermal, edges) — the edges lighten the thermal picture. */
            for (i = 0; i < n; i++) {
                const uint8_t *t = therm_rgb + i * 3;
                uint8_t       *o = out_rgb + i * 3;
                int            m = mag[i];
                o[0] = sat8((int)t[0] + m);
                o[1] = sat8((int)t[1] + m);
                o[2] = sat8((int)t[2] + m);
            }
        } else {
            /* Edge blending (black).  The vendor's dataflow is
             *   d1 = dilate(blur, K1); d2 = dilate(d1, K2); d = d2 - d1 + C
             *   m = threshold(d, 70, TOZERO); mask = ~m
             *   out = (thermal + mag) & mask
             * where K1/K2 (`+0x870`/`+0x810`) and the constant Mat C
             * (`+0x7b0`) are built outside the recovered function.  C is not a
             * detail: at C = 0 the threshold passes almost every pixel and the
             * mode collapses onto case 2, so any value here would be invented
             * rather than recovered.  The port keeps the one recovered
             * constant (the threshold of 70) and applies it to a 3x3-dilated
             * edge magnitude — a documented simplification of the dataflow,
             * not a reproduction of it (RE Docs 03 §3.5.2). */
            uint8_t *dil = scratch;          /* reuse: edge_magnitude is done */

            dilate3(mag, w, h, dil);
            for (i = 0; i < n; i++) {
                uint8_t *o = out_rgb + i * 3;
                if (dil[i] > DYT_EDGE_BLACK_THRESH) {
                    const uint8_t *t = therm_rgb + i * 3;
                    int            m = mag[i];
                    o[0] = sat8((int)t[0] + m);
                    o[1] = sat8((int)t[1] + m);
                    o[2] = sat8((int)t[2] + m);
                } else {
                    o[0] = o[1] = o[2] = 0;
                }
            }
        }

        free(mag);
        free(buf);
    }

    return 0;
}
