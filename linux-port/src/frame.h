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

/* device mode, derived from VID/PID (RE Docs 04 §4.10) */
typedef enum {
    DYT_MODE_0    = 0,
    DYT_MODE_44C  = 0x44c,   /* full radiometric */
    DYT_MODE_1000 = 1000,    /* direct AD: raw/64 - 273.15 */
    DYT_MODE_3EB  = 0x3eb    /* third variant — TODO */
} dyt_mode_t;

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

#endif /* DYT_FRAME_H */
