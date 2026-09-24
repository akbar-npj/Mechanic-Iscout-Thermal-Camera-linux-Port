/*
 * frame.c — frame layout accessors and frame→temperature pipeline.
 *
 * Implements the accessors and mode-dispatch from frame.h.  The heavy
 * lifting (LUT construction and pixel→temperature conversion) is in
 * thermometry.c (byte-verified against the vendor library); this file
 * wires those functions behind the frame_t / dyt_mode_t seam.
 *
 * Geometry values (scale/ambadv/rec_base per width) MUST match
 * thermometry.c's switch exactly — they are the byte-verified rodata
 * values from libthermometry.so (RE Docs 05 §5.6).
 *
 * build:  cc -O2 -g -Wall -Wextra -Wno-unused-parameter -ffp-contract=off \
 *           -I. -c frame.c -o frame.o
 */
#include <stdlib.h>     /* abs */
#include <string.h>     /* memset */
#include "frame.h"

/* Per-width geometry (RE Docs 04 §4.5.1 + thermometry.c switch).
 * The width keys are the sensor width in pixels (0xf0=240 etc.). */
static const struct {
    int    width;
    int    active;
    int    total;
    int    rec_base;
    float  scale;
    int    ambadv;
} geom_table[] = {
    { 0x0f0, 180, 184, 0x1e0, 36.0f,   0x1E78 },   /* 240 × 180 */
    { 0x100, 192, 196, 0x200, 37.682f, 0x21A9 },   /* 256 × 192 */
    { 0x180, 288, 292, 0x900, 36.0f,   0x1E78 },   /* 384 × 288 */
    { 0x280, 476, 480, 0xf00, 33.8f,   0x1AD3 },   /* 640 × 476 */
};
#define GEOM_N (sizeof(geom_table) / sizeof(geom_table[0]))

uint8_t *dyt_user_area(const frame_t *f)
{
    /* The reference band is the last REF_ROWS rows of the frame buffer.
     * (RE Docs 04 §4.5.1; verified in thermometrySearch's rb computation.) */
    return (uint8_t *)f->raw +
           (size_t)f->width * (f->total_height - REF_ROWS) * 2;
}

uint8_t *dyt_cal_record(const frame_t *f)
{
    return dyt_user_area(f) + f->rec_base;
}

int dyt_geometry(int width, int *active, int *total, int *rec_base,
                 float *scale, int *ambadv)
{
    for (unsigned i = 0; i < GEOM_N; i++) {
        if (geom_table[i].width == width) {
            if (active)   *active   = geom_table[i].active;
            if (total)    *total    = geom_table[i].total;
            if (rec_base) *rec_base = geom_table[i].rec_base;
            if (scale)    *scale    = geom_table[i].scale;
            if (ambadv)   *ambadv   = geom_table[i].ambadv;
            return 0;
        }
    }
    return -1;
}

int dyt_frame_resolve(dyt_mode_t mode, int width, size_t data_bytes,
                      int *active_out, int *total_out, int *rec_base_out)
{
    size_t n_samp;
    int total, active, rec_base;

    if (width <= 0)
        return -1;

    n_samp = data_bytes / 2;
    if (n_samp == 0 || n_samp % (size_t)width)
        return -1;                    /* not a whole number of rows */
    total = (int)(n_samp / (size_t)width);

    if (mode == DYT_MODE_44C) {
        if (dyt_geometry(width, &active, NULL, &rec_base, NULL, NULL) != 0)
            return -1;                /* unknown sensor width */
        if (total != active + REF_ROWS)
            return -1;                /* payload inconsistent with the band */
    } else {
        active = total;               /* no reference band */
        rec_base = 0;
    }

    if (active_out)   *active_out   = active;
    if (total_out)    *total_out    = total;
    if (rec_base_out) *rec_base_out = rec_base;
    return 0;
}

void dyt_frame_parse_params(const frame_t *f, float t_amb_in,
                            dyt_frame_params *p)
{
    /* thermometryT4Line fills the params as a side effect of building
     * the LUT; use a scratch LUT so we don't require the caller to
     * allocate one just to inspect the parameters. */
    float scratch[LUT_N];
    float amb, corr, refl, air, humi, emiss;
    uint16_t ua = 0;
    thermometryT4Line(t_amb_in, f->width, f->total_height, scratch,
                      dyt_user_area(f), &amb, &corr, &refl, &air, &humi,
                      &emiss, &ua, p->sensor_mode, p->fix_mode);
    p->amb      = amb;
    p->corr     = corr;
    p->refl     = refl;
    p->air      = air;
    p->humi     = humi;
    p->emiss    = emiss;
    p->distance = ua;   /* rec+0x112, the u16 the model uses (not param_14) */
}

void dyt_frame_build_lut(const frame_t *f, float t_amb_in,
                         int sensor_mode, int fix_mode, float *lut)
{
    float amb, corr, refl, air, humi, emiss;
    uint16_t ua = 0;
    thermometryT4Line(t_amb_in, f->width, f->total_height, lut,
                      dyt_user_area(f), &amb, &corr, &refl, &air, &humi,
                      &emiss, &ua, sensor_mode, fix_mode);
}

int dyt_frame_is_valid(const frame_t *f)
{
    int n = f->width * (f->total_height - REF_ROWS);
    const uint16_t *r;

    for (int k = 0; k < n; k++)
        if (f->raw[k] >= 0x4000)
            return 0;

    /* Also gate the six reference pixels thermometrySearch checks before
     * the image loop (disasm 0x1c78..0x1cac). */
    r = f->raw + n;
    if (r[4]  >= 0x4000 || r[7]  >= 0x4000 || r[8]  >= 0x4000 ||
        r[12] >= 0x4000 || r[13] >= 0x4000 || r[14] >= 0x4000)
        return 0;

    return 1;
}

int dyt_frame_convert(const frame_t *f, const float *lut, float *out)
{
    /* Pre-scan for the 0x4000 validity gate (thermometrySearch aborts on
     * the first offending pixel, printing "thermometrySearch err data" and
     * returning void).  Mirror it here so the mode-dispatch API has a
     * usable return value. */
    if (!dyt_frame_is_valid(f))
        return -1;

    thermometrySearch(f->width, f->total_height, lut, f->raw, out,
                      NULL, REF_ROWS);
    return 0;
}

int dyt_frame_convert_mode(dyt_mode_t mode, const frame_t *f,
                           const float *lut, float *out)
{
    switch (mode) {
      case DYT_MODE_44C:
        return dyt_frame_convert(f, lut, out);

      case DYT_MODE_1000: {
        /* Direct AD conversion (RE Docs 04 §4.5.4): raw sample is
         * Kelvin×64 (Q6 fixed point); output is w×h floats, no header,
         * no reference band.  Uses the *separate* dimension fields
         * (FrameImage+0x53c/+0x540), so total_height == active here. */
        int n = f->width * f->total_height;
        for (int k = 0; k < n; k++)
            out[k] = (float)f->raw[k] / 64.0f - 273.15f;
        return 0;
      }

      case DYT_MODE_3EB:
      case DYT_MODE_0:
      default:
        return -1;   /* TODO — needs live capture to identify the path */
    }
}

int dyt_frame_needs_shutter(const frame_t *f, uint16_t last_ref,
                            uint16_t *cur_ref_out)
{
    /* RE Docs 04 §4.5.5: monitor the 2nd sample of the reference band
     * (raw[width*(h-4)+1]); when it drifts >= 15 counts, command a
     * shutter via uvc_set_zoom_abs(cam, 0xffff8000).  The caller owns
     * last_ref and updates it from *cur_ref_out each frame. */
    int n = f->width * (f->total_height - REF_ROWS);
    uint16_t cur = f->raw[n + 1];
    if (cur_ref_out) *cur_ref_out = cur;
    if (last_ref == 0) return 0;   /* first frame: initialise, no trigger */
    return (abs((int)cur - (int)last_ref) >= 15) ? 1 : 0;
}

/* ------------------------------------------------------------ live pipeline */

void dyt_pipeline_init(dyt_pipeline_t *p, dyt_mode_t mode, float t_amb,
                       int sensor_mode, int fix_mode, float *lut)
{
    memset(p, 0, sizeof *p);
    p->mode        = mode;
    p->t_amb       = t_amb;
    p->sensor_mode = sensor_mode;
    p->fix_mode    = fix_mode;
    p->lut         = lut;
}

int dyt_pipeline_resolve(dyt_pipeline_t *p, int width, size_t n_samples)
{
    int active, total, rec_base;

    if (dyt_frame_resolve(p->mode, width, n_samples * 2,
                          &active, &total, &rec_base) != 0)
        return -1;

    p->width    = width;
    p->active   = active;
    p->total    = total;
    p->rec_base = rec_base;
    p->n_pix    = width * (p->mode == DYT_MODE_44C ? active : total);
    p->out_n    = p->n_pix + (p->mode == DYT_MODE_44C ? 10 : 0);
    p->ready    = 1;
    return 0;
}

int dyt_pipeline_frame(dyt_pipeline_t *p, const uint16_t *raw,
                       size_t n_samples, float *out)
{
    frame_t ft;

    if (!p->ready || !raw || !out)
        return -1;
    if (n_samples < (size_t)p->width * p->total)
        return -1;                    /* short frame */

    ft.width        = p->width;
    ft.total_height = p->total;
    ft.rec_base     = p->rec_base;
    ft.raw          = (uint16_t *)raw;

    /* The calibration record is per-unit and static, so the LUT is built
     * once — but only from a frame that actually carries calibration data.
     * Until the AD-output order lands the device streams a flat 0x8000
     * filler (MechaniscoutPcap/4.pcapng); building the LUT from that would
     * seed every later frame with garbage.  Only mode 0x44c consumes the
     * LUT, so mode 1000 never builds one. */
    if (p->mode == DYT_MODE_44C && !p->lut_built && dyt_frame_is_valid(&ft)) {
        dyt_frame_build_lut(&ft, p->t_amb, p->sensor_mode, p->fix_mode, p->lut);
        p->lut_built = 1;
    }

    return dyt_frame_convert_mode(p->mode, &ft, p->lut, out);
}
