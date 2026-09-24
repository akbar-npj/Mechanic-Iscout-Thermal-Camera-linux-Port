/*
 * frame.h — frame layout, calibration-record accessors, and the
 * frame→temperature pipeline for the DYT thermal camera Linux port.
 *
 * The frame layout is fully recovered (RE Docs 04 §4.5, verified across
 * all four sensor widths): a flat row-major uint16 array of
 * width × (active_height + 4) samples, stride == width, where the last
 * 4 rows are a reference/shutter band carrying the per-unit calibration
 * record.  The thermometry functions (thermometry.h) consume this layout
 * directly; this header provides the accessors and a thin pipeline that
 * wires thermometryT4Line + thermometrySearch behind a mode-dispatch seam.
 *
 * Modes (RE Docs 04 §4.10, derived from VID/PID):
 *   0x44c  full radiometric — 4 ref rows, LUT + corr, w×(h-4) output
 *   1000   direct AD — raw/64 - 273.15 over w×h, no ref band
 *   0x3eb  third variant — not yet implemented (stub)
 *   0      unsupported
 */
#ifndef DYT_FRAME_H
#define DYT_FRAME_H

#include <stddef.h>   /* size_t */
#include <stdint.h>

#include "thermometry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* device mode, derived from VID/PID (RE Docs 04 §4.10) */
typedef enum {
    DYT_MODE_0    = 0,
    DYT_MODE_44C  = 0x44c,   /* full radiometric */
    DYT_MODE_1000 = 1000,    /* direct AD: raw/64 - 273.15 */
    DYT_MODE_3EB  = 0x3eb    /* third variant — TODO */
} dyt_mode_t;

/* Where the thermal plane sits inside the streamed payload.
 *
 * The 0bda:5840 unit has two output modes, and they differ in geometry as
 * well as in content (RE Docs 04, measured 2026-09-24):
 *
 *   default mode  256x384 — the top half is the device's grayscale visible
 *                 image and the bottom half is the thermal plane.  Selected
 *                 without sending any vendor order.
 *   AD mode       256x192 — the whole payload is thermal.  Requires
 *                 setTinyCOutputADValue once the stream is running.
 *
 * The two modes carry the *same* thermal data (A-B-A interleave, 2026-09-24),
 * so this is purely a sub-rectangle selection — the conversion is identical. */
typedef enum {
    DYT_PLANE_FULL = 0,      /* whole payload is thermal (AD mode) */
    DYT_PLANE_BOTTOM_HALF    /* bottom half is thermal, top half is visible */
} dyt_plane_t;

/* A raw thermal frame.  raw is width*total_height uint16 samples,
 * caller-owned.  For mode 0x44c, total_height = active + REF_ROWS;
 * for mode 1000, total_height == active (no reference band). */
typedef struct {
    int       width;
    int       total_height;   /* active + REF_ROWS (mode 0x44c) */
    int       rec_base;       /* 0x1e0 | 0x200 | 0x900 | 0xf00 */
    uint16_t *raw;            /* width*total_height samples, caller-owned */
} frame_t;

/* Parsed calibration parameters from the frame's reference-band record.
 * Mirrors the outputs thermometryT4Line writes via its pointer args.
 * sensor_mode and fix_mode are INPUTS (caller-set); the float fields
 * and distance are OUTPUTS. */
typedef struct {
    float amb, corr, refl, air, humi, emiss;
    uint16_t distance;
    int sensor_mode;   /* input: 0x44 or 0x82 */
    int fix_mode;      /* input: 0x78 enables GetFix, else off */
} dyt_frame_params;

/* The reference/shutter band: last REF_ROWS rows of the frame buffer. */
uint8_t *dyt_user_area(const frame_t *f);

/* The calibration record inside the reference band: user_area + rec_base. */
uint8_t *dyt_cal_record(const frame_t *f);

/* Per-width geometry table (RE Docs 04 §4.5.1).  Looks up width
 * (0xf0/0x100/0x180/0x280).  Returns 0 on success with *active,
 * *total, *rec_base, *scale, *ambadv filled; -1 if width is unknown.
 * Any of the output pointers may be NULL. */
int dyt_geometry(int width, int *active, int *total, int *rec_base,
                 float *scale, int *ambadv);

/* Resolve the frame geometry from a received UVC payload size.
 *
 * The stream descriptor may advertise either the active height or
 * active+REF_ROWS, so the authoritative total row count is derived from
 * the byte count rather than trusted from the descriptor.  Returns 0 on
 * success, -1 if the width is unknown, the payload is not a whole number
 * of rows, or (mode 0x44c) the row count is not active+REF_ROWS.
 *
 * For mode 1000 there is no reference band, so active == total and
 * rec_base is 0.  Any output pointer may be NULL. */
int dyt_frame_resolve(dyt_mode_t mode, int width, size_t data_bytes,
                      int *active_out, int *total_out, int *rec_base_out);

/* Parse the calibration parameters from the frame's record.  Uses a
 * scratch LUT internally (thermometryT4Line fills the params as a
 * side effect of building the LUT).  p->sensor_mode and p->fix_mode
 * are inputs; the remaining fields are outputs. */
void dyt_frame_parse_params(const frame_t *f, float t_amb_in,
                            dyt_frame_params *p);

/* Build the 16384-entry float LUT from the frame's calibration record.
 * Thin wrapper over thermometryT4Line.  lut must hold LUT_N floats. */
void dyt_frame_build_lut(const frame_t *f, float t_amb_in,
                         int sensor_mode, int fix_mode, float *lut);

/* Convert the frame to temperatures (mode 0x44c path).  out must hold
 * 10 + width*(total_height-REF_ROWS) floats.  Returns 0 on success,
 * -1 if any pixel is saturated/corrupt (>= 0x4000).  Thin wrapper
 * over thermometrySearch (which writes the 10-float header + image). */
int dyt_frame_convert(const frame_t *f, const float *lut, float *out);

/* Mode-dispatched convert.  0x44c → dyt_frame_convert; 1000 → direct
 * raw/64-273.15 over width*total_height pixels; 0x3eb/0 → -1 (TODO).
 * For mode 1000, out must hold width*total_height floats (no header);
 * for 0x44c, see dyt_frame_convert. */
int dyt_frame_convert_mode(dyt_mode_t mode, const frame_t *f,
                           const float *lut, float *out);

/* Auto-shutter (flat-field correction) trigger (RE Docs 04 §4.5.5).
 * Reads raw[width*(total-REF_ROWS)+1] (the 2nd sample of the reference
 * band).  Returns 1 if a shutter command is needed (drift >= 15 from
 * last_ref), 0 otherwise.  *cur_ref_out is always set to the current
 * reference sample (the caller stores it as last_ref for next frame).
 * A last_ref of 0 means "first frame" — initialises without triggering. */
int dyt_frame_needs_shutter(const frame_t *f, uint16_t last_ref,
                            uint16_t *cur_ref_out);

/* The mode-0x44c validity gate: true when no image sample and none of the
 * six reference pixels thermometrySearch inspects sits at/above the 0x4000
 * sentinel.  dyt_frame_convert applies it before converting.
 *
 * This is also what keeps calibration out of the device's start-up filler.
 * The device streams a flat 0x8000 for its first ~6 s, which fails this
 * gate — so a LUT built only from valid frames can never be seeded from the
 * filler.  Mode 1000 has no sentinel (0x4000 decodes to a legitimate
 * -17.15 C), so it never consults this. */
int dyt_frame_is_valid(const frame_t *f);

/* -------------------------------------------------------------------------
 * Live capture pipeline
 *
 * This is the device-independent glue the capture layer runs per frame:
 * resolve the geometry from the first payload, build the calibration LUT
 * from the first frame that actually carries calibration data, then convert.
 * It lives here rather than inside capture.c so that the exact sequence the
 * live path executes can be regression-tested without hardware — see
 * src/pipeline_test.c.
 * ------------------------------------------------------------------------- */

typedef struct {
    dyt_mode_t  mode;
    dyt_plane_t plane;     /* configured at init; where the thermal rows are */
    float       t_amb;
    int         sensor_mode, fix_mode;

    int        width;      /* sensor width in pixels */
    int        active;     /* rows the caller receives: the thermal plane
                            * (active == plane_h for mode 1000, and
                            * total - REF_ROWS for mode 0x44c) */
    int        total;      /* rows in the *payload* (the whole streamed
                            * frame, including the visible half and, for
                            * mode 0x44c, the reference band) */
    int        plane_y;    /* first thermal row within the payload */
    int        plane_h;    /* number of thermal rows in the payload */
    int        rec_base;   /* calibration-record offset in the reference band */
    int        n_pix;      /* samples converted */
    int        out_n;      /* floats dyt_pipeline_frame writes to out */
    int        ready;      /* geometry resolved */
    int        lut_built;  /* LUT built from a valid 0x44c frame */

    float     *lut;        /* LUT_N floats, caller-owned */
} dyt_pipeline_t;

/* Initialise.  `lut` must hold LUT_N floats and outlive the pipeline.
 * `plane` selects where the thermal rows are inside the payload; mode 0x44c
 * always uses DYT_PLANE_FULL. */
void dyt_pipeline_init(dyt_pipeline_t *p, dyt_mode_t mode, dyt_plane_t plane,
                       float t_amb, int sensor_mode, int fix_mode, float *lut);

/* Resolve the geometry from the first payload: `width` pixels wide,
 * `n_samples` uint16 samples.  Returns 0 on success (p->ready set), -1 if
 * the geometry is unusable — unknown width, not a whole number of rows, or
 * inconsistent with the mode's reference band or the requested plane (a
 * DYT_PLANE_BOTTOM_HALF payload must be exactly twice the sensor's active
 * height). */
int dyt_pipeline_resolve(dyt_pipeline_t *p, int width, size_t n_samples);

/* Run one frame.  raw holds n_samples uint16 samples; out holds at least
 * p->out_n floats.  Requires a prior successful dyt_pipeline_resolve().
 * Builds the LUT lazily from the first valid frame (mode 0x44c only) and
 * then converts.  Returns 0 when out holds a temperature image, -1 when the
 * frame was skipped (unresolved geometry, short payload, or rejected by the
 * mode's validity gate). */
int dyt_pipeline_frame(dyt_pipeline_t *p, const uint16_t *raw,
                       size_t n_samples, float *out);

#ifdef __cplusplus
}
#endif

#endif /* DYT_FRAME_H */
