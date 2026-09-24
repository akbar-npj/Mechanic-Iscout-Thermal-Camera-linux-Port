/*
 * thermometry.h — public API for the byte-verified thermometry port.
 *
 * These six functions are transcribed verbatim from the vendor
 * libthermometry.so disassembly (VAs noted on each function in
 * thermometry.c).  The differential harness in tools/thermometry_diff/
 * drives the real vendor library for byte-exact ground truth; this
 * header + thermometry.c is the from-scratch Linux port, verified
 * byte-for-byte against the vendor across all four sensor widths
 * (240/256/384/640) and both fix_mode configurations.
 *
 * Precision discipline (derived from disassembly, NOT pseudo-C):
 *   - every fused multiply-add is written as fmaf()/fma() and the
 *     translation unit is compiled with -ffp-contract=off so GCC
 *     does not auto-fuse anywhere else;
 *   - float between double pow()/exp() calls, matching the ARM
 *     fcvt d,s / fcvt s,d promotions/demotions in the disassembly;
 *   - glibc libm is used on both sides (harness forwards the vendor's
 *     pow/exp/sqrt to the same glibc libm), so transcendentals are
 *     bit-identical by construction.
 *
 * build: cc -O2 -g -Wall -Wextra -Wno-unused-parameter -ffp-contract=off \
 *          -c thermometry.c -o thermometry.o
 */
#ifndef DYT_THERMOMETRY_H
#define DYT_THERMOMETRY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LUT_N     16384   /* entries in the radiometric LUT (float) */
#define REF_ROWS  4       /* reference/shutter band rows at frame bottom */

/* ----- helpers (exposed for completeness / unit testing) ----- */

/* GetTempEvn @ 0x9bc — single-precision between double pow() calls. */
float GetTempEvn(float T_C, float A, float B);

/* InitTempParam @ 0xa68 — completing the square: half=b/(2a), quarter=half². */
void  InitTempParam(float a, float b, float *half, float *quarter);

/* GetFix @ 0xa1c — reference-pixel offset; only non-zero for fix_mode==0x78. */
int   GetFix(float T, int fix_mode, int width);

/* CalcFixRaw @ 0xa90 — out: tau, tau_eff, inv_tau_emiss, bg (all float). */
void  CalcFixRaw(float air, float humi, float dist, float emiss,
                float refl, float *tau, float *tau_eff,
                float *inv_te, float *bg);

/* ----- main entry points ----- */

/* thermometryT4Line @ 0x1370 — build the 16384-entry float LUT from the
 * per-unit calibration record embedded in the frame's reference band.
 * Signature verified against the disassembly:
 *   s0=t_amb_in, w0=width, w1=height(unused), x2=lut, x3=ref_band,
 *   x4=&amb, x5=&corr, x6=&refl, x7=&air, stack0=&humi, stack1=&emiss,
 *   stack2=&user_area(u16*), stack3=sensor_mode, stack4=fix_mode
 *   (fix_mode is NOT distance — see RE Docs 08 §5.8). */
void thermometryT4Line(float t_amb_in, int width, int height,
                       float *lut, void *ref_band,
                       float *amb, float *corr, float *refl,
                       float *air, float *humi, float *emiss,
                       uint16_t *user_area, int sensor_mode, int fix_mode);

/* thermometrySearch @ 0x1bf0 — convert pixels to temperatures.
 * Returns void; writes 10 header floats + width*(height-ref_rows)
 * pixel floats to `out`.  The "base value" added to every LUT lookup
 * is `corr` — the float at rec+0xFE — reached via a width-dependent
 * bit-twiddled index that resolves to width*(h-ref_rows)*2 + rec_base
 * + 0xFE bytes from `raw` for every height (verified symbolically). */
void thermometrySearch(int width, int height, const float *lut,
                       const uint16_t *raw, float *out,
                       void *unused, int ref_rows);

#ifdef __cplusplus
}
#endif

#endif /* DYT_THERMOMETRY_H */
