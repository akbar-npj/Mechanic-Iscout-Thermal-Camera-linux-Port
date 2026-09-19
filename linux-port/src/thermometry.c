/*
 * thermometry.c — byte-verified portable C reimplementation of the
 * vendor's libthermometry.so.
 *
 * The six function bodies below are transcribed VERBATIM from the
 * disassembly at the indicated VAs.  Constants are the byte-verified
 * rodata values (see RE Docs 05).  Every fused multiply-add is written
 * as fmaf()/fma() and the file is compiled with -ffp-contract=off so
 * GCC does not auto-fuse anywhere else; this is what makes the LUT and
 * output image bit-identical to the vendor across all four sensor
 * widths (240/256/384/640) and both fix_mode configurations.
 *
 * The differential harness in tools/thermometry_diff/ (diff.py) is the
 * regression gate: it compares the port's port_lut.bin / port_out.bin
 * against the frozen vendor ground truth (in_lut_in.bin / out_image.bin)
 * byte for byte.  All 8 frozen cases must PASS or the port has drifted.
 *
 * build:  cc -O2 -g -Wall -Wextra -Wno-unused-parameter -ffp-contract=off \
 *           -c thermometry.c -o thermometry.o
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "thermometry.h"

/* ====================================================================== helpers
 * Transcribed from the disassembly at the indicated VAs.  Constants are the
 * byte-verified rodata values (see RE Docs §5.6). */

/* GetTempEvn @ 0x9bc — single-precision between double pow() calls. */
float GetTempEvn(float T_C, float A, float B)
{
    float t = T_C + 273.15f;                /* 0x9d0: fadd s0, s0, s8            */
    double d = pow((double)t, 4.0);        /* 0x9dc..0x9e0: fcvt; bl pow        */
    float t2 = (float)d;                    /* 0x9e4: fcvt s0, d0                */
    t2 = t2 - A;                           /* 0x9f0: fsub s0, s0, s9            */
    t2 = t2 * B;                           /* 0x9f4: fmul s0, s0, s2            */
    double d2 = pow((double)t2, 0.25);      /* 0x9f8..0x9fc: fcvt; bl pow        */
    float t3 = (float)d2;                  /* 0xa00: fcvt s0, d0                */
    return t3 - 273.15f;                   /* 0xa08: fsub s0, s0, s8            */
}

/* InitTempParam @ 0xa68 — completing the square.  half = b/(2a), quarter = half². */
void InitTempParam(float a, float b, float *half, float *quarter)
{
    *half    = b / (2.0f * a);
    *quarter = (b * b) / (4.0f * a * a);    /* == half*half, but match the disasm */
}

/* GetFix @ 0xa1c — reference-pixel offset, only for fix_mode == 0x78. */
int GetFix(float T, int fix_mode, int width)
{
    if (fix_mode != 0x78) return 0;
    if (width != 0x100) {
        /* 0xa3c: fmsub s0, s0, s2, s1  →  s1 - s0*s2  =  390.0 - 7.05*T  */
        float v = fmaf(-7.05f, T, 390.0f);
        int   iv = (int)v;                 /* 0xa40: fcvtzs w0, s0              */
        short sv = (short)iv;              /* 0xa44: sxth w0, w0                */
        return sv < 0 ? 0 : sv;            /* 0xa48..0xa54: csel                */
    }
    return 0xAA;                           /* 0xa58: mov w0, #0xaa              */
}

/* CalcFixRaw @ 0xa90.
 * out: tau, tau_eff, inv_tau_emiss, bg  (all single-precision). */
void CalcFixRaw(float air, float humi, float dist, float emiss,
                       float refl, float *tau, float *tau_eff,
                       float *inv_te, float *bg)
{
    /* --- 1. humidity-scaled exponential of an air-temperature polynomial --- */
    /* s5 = 1.5587 + 0.06939*air  (fmadd) */
    float p  = fmaf(air, 0.06939f, 1.5587f);
    /* s0 = air*2.7816e-4 ; s8 = air² * 6.8455e-7 */
    float s0 = air * 2.7816e-4f;
    float s8 = air * 6.8455e-7f;
    s8 = s8 * air;
    /* s0 = p - s0*air  (fmsub)  = 1.5587+0.06939*air - 2.7816e-4*air² */
    s0 = fmaf(-s0, air, p);
    /* s0 += s8*air  (fmadd)  →  + 6.8455e-7*air³ */
    s0 = fmaf(s8, air, s0);

    double d1 = exp((double)s0);           /* 0xafc..0xb00: fcvt; bl exp        */
    *tau = (float)((double)humi * d1);     /* 0xb0c..0xb14: fmul d; fcvt s      */

    /* --- 2. range-dependent transmittance via two exponentials --- */
    float sq_dist = sqrtf(dist);           /* 0xb04: fsqrt s8, s13              */
    float sq_tau  = sqrtf(*tau);           /* 0xb2c: fsqrt s1, s0               */

    /* e1 = exp(-sq_dist * (0.006569 - 0.002276*sq_tau))  [single inside] */
    float a1 = fmaf(sq_tau, -0.002276f, 0.006569f);   /* 0xb44: fmadd s0,s1,s2,s0  */
    a1 = (-sq_dist) * a1;                              /* 0xb48: fmul s0,s12,s0     */
    double e1 = exp((double)a1);                        /* 0xb4c..0xb50             */

    /* e2 = exp(-sq_dist * (0.012620 - 0.006670*sq_tau)) */
    float a2 = fmaf(sq_tau, -0.006670f, 0.012620f);   /* 0xb7c: fmadd              */
    a2 = (-sq_dist) * a2;                              /* 0xb84: fmul               */
    double e2 = exp((double)a2);

    /* tau_eff = 1.9*e1 - 0.9*e2   (double fmadd, then trunc to float) */
    double te_d = fma(e1, 1.9, e2 * (-0.9));           /* 0xba8: fmadd d8,d12,d8,d0 */
    *tau_eff = (float)te_d;                            /* 0xbb0: fcvt s2, d8        */

    /* inv_te = 1.0 / (tau_eff * emiss)  [single] */
    *inv_te = 1.0f / (*tau_eff * emiss);               /* 0xbb4..0xbbc              */

    /* --- 3. background radiance term --- */
    /* s8  = (refl+273.15)^4   (single add, then double pow, then trunc) */
    float refl_k = refl + 273.15f;                     /* 0xba4: fadd s11,s11,s14   */
    float l_refl = (float)pow((double)refl_k, 4.0);    /* 0xbac..0xbc4..0xbc8       */
    /* s0  = (air +273.15)^4   */
    float air_k  = air + 273.15f;                       /* 0xbcc: fadd s0,s9,s14     */
    float l_air  = (float)pow((double)air_k, 4.0);     /* 0xbd8..0xbdc..0xbe8       */

    float one_m_te   = 1.0f - *tau_eff;               /* 0xbec: fsub s1,s13,s1     */
    float one_m_emis = 1.0f - emiss;                   /* 0xbe4: fsub s10,s13,s10   */

    /* bg = tau_eff * l_refl * (1-emiss)  +  (1-tau_eff) * l_air   (fmadd, single) */
    float term_r = l_refl * one_m_emis;               /* 0xbf8: fmul s8,s8,s10     */
    float term_a = l_air  * one_m_te;                  /* 0xbf4: fmul s1,s0,s1      */
    *bg = fmaf(term_r, *tau_eff, term_a);             /* 0xbfc: fmadd s8,s8,s11,s1 */
}

/* ====================================================================== main LUT builder
 * thermometryT4Line @ 0x1370.  Signature verified against the disassembly:
 *   s0 = T_ambient_in (float),  w0 = width,  w1 = height (unused),
 *   x2 = lut,  x3 = ref_band,  x4 = &amb,  x5 = &corr,  x6 = &refl,
 *   x7 = &air,  stack0=&humi, stack1=&emiss, stack2=&user_area(u16*),
 *   stack3 = sensor_mode,  stack4 = fix_mode (NOT distance — see RE Docs §5.8).
 */
void thermometryT4Line(float t_amb_in, int width, int height,
                              float *lut, void *ref_band,
                              float *amb, float *corr, float *refl,
                              float *air, float *humi, float *emiss,
                              uint16_t *user_area, int sensor_mode, int fix_mode)
{
    uint8_t *rb = ref_band;
    uint16_t raw_ref = *(uint16_t *)(rb + 2);          /* 0x13e8: ldrh w0,[x3,#2]  */

    float scale, ambadv;
    uint32_t rec_base;
    switch (width) {
      case 0x100: scale = 37.682f; ambadv = 0x21A9; rec_base = 0x200; break;
      case 0x0f0: scale = 36.0f;   ambadv = 0x1E78; rec_base = 0x1e0; break;
      case 0x180: scale = 36.0f;   ambadv = 0x1E78; rec_base = 0x900; break;
      case 0x280: scale = 33.8f;   ambadv = 0x1AD3; rec_base = 0xf00; break;
      default:    scale = 0.0f;    ambadv = 0.0f;    rec_base = 0;      break;
    }
    *amb = 20.0f - (float)((int)raw_ref - (int)(uint32_t)ambadv) / scale; /* 0x1970..0x19c0 */

    uint8_t *rec = rb + rec_base;
    float a = *(float *)(rec + 0x06);
    float b = *(float *)(rec + 0x0a);
    float c = *(float *)(rec + 0x0e);
    float d = *(float *)(rec + 0x12);
    float e = *(float *)(rec + 0x16);
    *corr  = *(float *)(rec + 0xfe);
    *refl  = *(float *)(rec + 0x102);
    *air   = *(float *)(rec + 0x106);
    *humi  = *(float *)(rec + 0x10a);
    *emiss = *(float *)(rec + 0x10e);
    uint16_t dist_u16 = *(uint16_t *)(rec + 0x112);
    *user_area = dist_u16;                              /* 0x14d8: strh w0,[x23]    */

    /* distance the model uses (frame value, NOT param_14) */
    float distv = (sensor_mode == 0x44) ? (float)dist_u16 * 3.0f
               : (float)dist_u16;

    /* InitTempParam → half, quarter */
    float half, quarter;
    InitTempParam(a, b, &half, &quarter);

    float tau, tau_eff, inv_te, bg;
    CalcFixRaw(*air, *humi, distv, *emiss, *refl,
               &tau, &tau_eff, &inv_te, &bg);

    /* T_ambient adjustment: T_code/10 - 273.15 + t_amb_in  (0x14cc, 0x1624) */
    uint16_t tcode = *(uint16_t *)(rec + 0x02);
    float T_adj = (float)tcode / 10.0f - 273.15f;
    T_adj = T_adj + t_amb_in;

    float amb2 = *amb;
    int fix = GetFix(amb2, fix_mode, width);           /* 0x1620..0x1630          */

    /* base = (userArea[0] - fix) & 0xFFFF  (0x1634: sub w0,w22,w0; 0x1644: uxth)
     * userArea[0] is the u16 at rec+0x00 (loaded into w22 at 0x1490). */
    uint16_t lo = *(uint16_t *)(rec + 0x00);
    uint32_t base = (uint32_t)((int)lo - fix) & 0xFFFF;

    /* q1 = a*T_adj² + b*T_adj   (0x1654, 0x1664: two fmuls then fmadd)
     *   s2 = b * T_adj ;  s3 = a * T_adj ;  s11 = fmaf(s3, T_adj, s2) */
    float q1 = fmaf(a * T_adj, T_adj, b * T_adj);

    /* q2 = c*amb2² + d*amb2 + e   (0x1648..0x1668)
     *   s1 = amb2*c ;  s0 = amb2*d ;  s0 = fmaf(s1, amb2, s0);  s10 = s0 + e */
    float q2 = fmaf(amb2 * c, amb2, amb2 * d) + e;

    /* fill the LUT  (0x1694..0x16fc)
     *   i from -base to 0x4000-base (exclusive of the end sentinel) */
    int i = -(int)base;
    int end = 0x4000 - (int)base;
    float *out = lut;
    do {
        /* inner = (q1 + q2*i)/a + quarter   [all single] */
        float inner = fmaf((float)i, q2, q1) / a + quarter;   /* 0x16a4,0x16a8,0x16ac */
        double s = sqrt((double)inner);                       /* 0x16b0,0x16b4         */
        float T = GetTempEvn((float)(s - (double)half), bg, inv_te);  /* 0x16c0..0x16dc */

        /* distance correction, mode 0x82 dist>=20 branch (0x1684..0x16f8) */
        if (sensor_mode == 0x44) {
            if (distv < 20.0f)
                T = T + (fmaf(distv * 0.85f, 1.0f, 1.125f) * (T - *air)) / 100.0f;
            else
                T = T + (18.125 * (T - *air)) / 100.0;
        } else if (sensor_mode == 0x82) {
            if (distv < 20.0f)
                T = T + (fmaf(0.85f, distv, -1.125f) * (T - *air)) / 100.0f;
            else
                T = T + (15.875f * (T - *air)) / 100.0f;     /* 0x16ec..0x16f4 */
        } else {
            if (distv < 20.0f)
                T = T + (fmaf(0.85f, distv, -1.125f) * (T - *air)) / 100.0f;
            else
                T = T + (15.875f * (T - *air)) / 100.0f;
        }
        *out++ = T;
        i++;
    } while (i != end);
}

/* thermometrySearch @ 0x1bf0.  Returns void; writes 10 header floats +
 * width*(height-4) pixel floats to `out`.
 *
 * The "base value" added to every LUT lookup is `corr` — the float at
 * `rec+0xFE` — NOT a LUT entry.  The vendor reaches it via a width-dependent
 * bit-twiddled index that, for every height, resolves to exactly
 * `width*(h-4)*2 + rec_base + 0xFE` bytes from `raw` (verified symbolically
 * for w=0x100 and w=0x180).  We read it directly. */
void thermometrySearch(int width, int height, const float *lut,
                              const uint16_t *raw, float *out,
                              void *unused, int ref_rows)
{
    (void)unused;
    int n = (height - ref_rows) * width;       /* 0x1bf8,0x1c0c              */

    /* record base mirrors thermometryT4Line's switch */
    uint32_t rec_base;
    switch (width) {
      case 0x100: rec_base = 0x200; break;
      case 0x0f0: rec_base = 0x1e0; break;
      case 0x180: rec_base = 0x900; break;
      case 0x280: rec_base = 0xf00; break;
      default:    rec_base = 0;      break;
    }
    const uint8_t *rb = (const uint8_t *)raw + (size_t)width * (height - ref_rows) * 2;
    float corr = *(const float *)(rb + rec_base + 0xFE);   /* base_val  */

    /* reference pixels live in the last `ref_rows` rows of the frame, starting
     * at raw[n].  The header reads six of them through the LUT and four as
     * raw u16→float.  Disasm map (0x1c20..0x1d1c), indices from raw[n]:  */
    const uint16_t *r = raw + n;             /* r[0] == raw[n]              */
    /* validity gate on the six LUT-indexed reference pixels (0x1c78..0x1cac):
     * any >= 0x4000 aborts the whole call (puts + return). */
    if (r[4]  >= 0x4000 || r[7]  >= 0x4000 || r[8]  >= 0x4000 ||
        r[12] >= 0x4000 || r[13] >= 0x4000 || r[14] >= 0x4000) {
        fputs("thermometrySearch err data\n", stdout);
        return;
    }

    /* 10-float header.  out[1,2,4,5] are raw u16→float; the rest are
     * corr + lut[raw[...]].  The duplicates (out[7] == out[9]) are in the
     * vendor binary, not a typo. */
    out[0] = corr + lut[r[12]];              /* s17 = lut[r[12]] + corr     */
    out[1] = (float)(unsigned)r[2];          /* s3  = (float)r[2]           */
    out[2] = (float)(unsigned)r[3];          /* s2  = (float)r[3]           */
    out[3] = corr + lut[r[4]];               /* s16 = lut[r[4]]  + corr     */
    out[4] = (float)(unsigned)r[5];          /* s0  = (float)r[5]           */
    out[5] = (float)(unsigned)r[6];          /* s8  = (float)r[6]           */
    out[6] = corr + lut[r[7]];               /* s7  = lut[r[7]]  + corr     */
    out[7] = corr + lut[r[8]];               /* s6  = lut[r[8]]  + corr     */
    out[8] = corr + lut[r[14]];              /* s5  = lut[r[14]] + corr     */
    out[9] = corr + lut[r[8]];               /* s4  = lut[r[8]]  + corr (dup) */

    /* image: out[10+k] = lut[raw[k]] + corr, with the 0x4000 validity gate.
     * (0x1dc8..0x1e08: the loop aborts on the first offending pixel.) */
    for (int k = 0; k < n; k++) {
        if (raw[k] >= 0x4000) {
            fputs("thermometrySearch err data\n", stdout);
            return;
        }
        out[10 + k] = lut[raw[k]] + corr;
    }
}
