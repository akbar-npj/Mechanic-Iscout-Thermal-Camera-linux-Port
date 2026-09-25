/*
 * fusion.h — the vendor's six image-fusion patterns, device-free.
 *
 * The vendor app calls them "patterns" (`myPattern`, JNI message 57) and
 * exposes an X/Y alignment (`myCoefficient`, message 58).  Both are recovered
 * from the decompiled stack — RE Docs 03 §3.5.2 has the full derivation and
 * the two caveats that shape what "faithful" can mean here:
 *
 *   1. The vendor fuses a **separate 640x480 MJPEG visible stream**, not the
 *      256x192 grayscale top half of the dual-half payload.  This port fuses
 *      the top half with the thermal plane; both are 256x192, so they are
 *      already 1:1 and no ROI rectangle is needed.
 *   2. Case 5's mask depends on three quantities that are **not** recoverable
 *      from `fusionFunction`: the two `dilate` structuring elements (built
 *      outside it, at `+0x870`/`+0x810`) and a constant Mat added at
 *      `+0x7b0`.  The bias matters more than the kernels — with it at zero the
 *      recovered `threshold(70)` passes almost every pixel and case 5
 *      collapses onto case 2 — so inventing a value would not be more
 *      faithful, it would just be a different guess.  The port therefore
 *      applies the one recovered constant (the threshold of 70) to a
 *      3x3-dilated edge magnitude and documents that as a **simplification**
 *      of the dataflow.  Measured live, it renders almost entirely black on
 *      this unit's low-contrast visible picture.
 *
 * So the *shape* of each pattern is [V]; the exact pixel output at this
 * geometry is [I].  Cases 2/3/4 are a faithful reimplementation of the same
 * algorithm with the same parameters as the vendor's OpenCV calls (not a
 * byte-identical reproduction of OpenCV itself); case 5 is a documented
 * simplification, for the reason in caveat 2.
 *
 * Nothing here touches the device or the filesystem, so `make check` covers
 * it (fusion_test.c).
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -I. -c fusion.c -lm
 */
#ifndef DYT_FUSION_H
#define DYT_FUSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The six patterns, in the vendor's own order (RE Docs 03 §3.5.2).  The APK
 * rejects anything outside 0..5, so this enum is the whole set. */
typedef enum {
    DYT_FUSION_INFRARED   = 0,  /* thermal only */
    DYT_FUSION_VISIBLE    = 1,  /* visible only */
    DYT_FUSION_EDGE       = 2,  /* thermal + visible edges */
    DYT_FUSION_BLEND      = 3,  /* 50/50 thermal and visible */
    DYT_FUSION_PIP        = 4,  /* picture in picture */
    DYT_FUSION_EDGE_BLACK = 5,  /* visible edges over black */
    DYT_FUSION_N          = 6
} dyt_fusion_t;

/* The vendor clamps each alignment coefficient to this range
 * (`PreviewFragment.java:1939-1960`, ±40). */
#define DYT_FUSION_ALIGN_MAX 40

/* Which pattern, and how far the visible plane is offset against the thermal
 * one.  Positive dx samples the visible plane dx columns to the right, so the
 * visible *content* appears dx pixels to the left — the same sense as the
 * vendor's `X_Coefficient`, which shifts the thermal ROI inside the visible
 * frame.  Out-of-range samples are edge-clamped.  The offset is applied to
 * every pattern, including the two edge modes (the edges are computed from
 * the aligned plane). */
typedef struct {
    dyt_fusion_t mode;
    int          dx, dy;
} dyt_fusion_cfg_t;

/* Infrared (thermal only), zero alignment — the port's start-up state. */
void dyt_fusion_cfg_default(dyt_fusion_cfg_t *cfg);

/* Stable short name for the status line: "ir", "visible", "edge", "blend",
 * "pip", "edge-black".  Never NULL; an unknown value yields "?". */
const char *dyt_fusion_name(dyt_fusion_t f);

/* Clamp an alignment offset into ±DYT_FUSION_ALIGN_MAX. */
int dyt_fusion_clamp_align(int v);

/* Combine a thermal render with the grey visible plane.
 *
 *   therm_rgb  w*h*3 bytes, RGB — what dyt_session_render_rgb() produces
 *   grey       w*h bytes, the visible plane at the same size.  Required for
 *              every mode except INFRARED, which ignores it.
 *   out_rgb    w*h*3 bytes; may alias therm_rgb.
 *
 * Returns 0, or -1 on a bad argument (NULL out, non-positive size, an
 * unusable mode, or a missing `grey` where the mode needs it).  The edge
 * modes allocate a small scratch buffer internally. */
int dyt_fusion_apply(const dyt_fusion_cfg_t *cfg,
                     const uint8_t *therm_rgb, const uint8_t *grey,
                     int w, int h, uint8_t *out_rgb);

#ifdef __cplusplus
}
#endif

#endif /* DYT_FUSION_H */
