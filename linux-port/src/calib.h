/*
 * calib.h — Windows calibration tables (tau_*.bin, MILI6_*.bin).
 *
 * The Windows build ships two families of atmospheric-transmittance
 * table that the Android build does not (RE Docs 10):
 *
 *   tau_H.bin / tau_L.bin      7168 B = uint16[56][64], no header
 *   MILI6_{H,L}.bin            7424 B = 256-byte header + uint16[56][64]
 *
 * SCOPE — this module is deliberately NOT wired into the frame pipeline.
 * The shipping Android stack computes tau analytically in CalcFixRaw
 * (thermometry.c) and ships no table at all, and tools/thermometry_diff is
 * byte-verified against that path.  The Windows SDK uses these tables
 * instead, via a differently-structured inversion, so this is a second
 * thermometry backend rather than a drop-in for CalcFixRaw.  Which model
 * better matches a given unit needs a two-distance hardware comparison;
 * until then the analytic path stays the default.  See RE Docs 10 §4.5.
 *
 * Both store transmittance in Q14 fixed point (16384 == 1.0) and are
 * indexed [target_temperature_band][distance_band].  MILI6 additionally
 * carries a header whose high 16 bits select a layout; versions 0x40 and
 * above widen the table to 42x88 and 45x88 (RE Docs 10 §3.2).
 *
 * Everything here is transcribed from
 *   Tiny1CDll.dll!read_tau_with_target_temp_and_dist   @ 0x1001a2f0
 *   Tiny1CDll.dll!FUN_10018ee0 (get_dist_read_index)   @ 0x10018ee0
 *   libirtemp.dll!read_compatible_tau_with_target_temp_and_dist
 *                                                      @ 0x10006660
 * and the axis constants in those DLLs' .data.  See RE Docs 10 for the
 * evidence and for the two deliberate divergences from the vendor code
 * (both are vendor out-of-bounds bugs, marked VENDOR-BUG below).
 *
 * build: cc -O2 -g -Wall -Wextra -Wno-unused-parameter -ffp-contract=off \
 *          -c calib.c -o calib.o
 */
#ifndef DYT_CALIB_H
#define DYT_CALIB_H

#include <stddef.h>
#include <stdint.h>

/* Table layouts, keyed by the MILI6 header's high 16 bits. */
typedef enum {
    DYT_CALIB_V1 = 1,   /* 56 x 64, header absent or version <= 0x3F  */
    DYT_CALIB_V2 = 2,   /* 42 x 88, version 0x40 .. 0x100             */
    DYT_CALIB_V3 = 3    /* 45 x 88, version >= 0x101                  */
} dyt_calib_layout_t;

typedef struct {
    uint16_t         *data;        /* rows*cols entries, little-endian */
    int               rows;
    int               cols;
    int               has_header;  /* 256-byte header present          */
    uint32_t          header;      /* raw dword 0 (0 if absent)        */
    uint32_t          version;     /* header >> 16 (0 if absent)       */
    dyt_calib_layout_t layout;
} dyt_calib_table_t;

/* Q14 scale: value 16384 == 1.0 (libirtemp.dll DAT_1000b4b0 = 6.103515625e-05). */
#define DYT_CALIB_Q14  16384.0f

/* ----- lifecycle ---------------------------------------------------- */

/* Load a whole table file.  Returns 0 on success, -1 on I/O or format
 * error (in which case *t is left zeroed).  Recognises the 256-byte
 * 0xFFFFFFFF header; a file that is neither 7168 nor 7424/7648/8176
 * bytes long, and has no valid header, is rejected. */
int  dyt_calib_load(dyt_calib_table_t *t, const char *path);

/* Same, from an in-memory blob.  The bytes are copied. */
int  dyt_calib_load_mem(dyt_calib_table_t *t, const void *buf, size_t len);

void dyt_calib_free(dyt_calib_table_t *t);

/* ----- accessors ---------------------------------------------------- */

/* Build the conventional filename ("tau_H.bin", "MILI6_L.bin", ...).
 * `kind` is 't' for tau, 'm' for MILI6.  Returns dst. */
char *dyt_calib_name(char *dst, size_t n, char kind, int high_gain);

/* ----- lookup ------------------------------------------------------- */

/* Bilinear transmittance lookup, exactly as the vendor readers do it.
 * Returns 0 on success, -1 if the table is empty.  *out receives the
 * raw Q14 value, rounded with +0.5 then truncated (the vendor's
 * `_DAT_10009580` / `_DAT_10028a58` constant).
 *
 * target_temp_c: target temperature in degrees Celsius (the vendor adds
 *                273.15 internally to reach the Kelvin axis).
 * distance_m:    distance in metres. */
int dyt_calib_tau_read(const dyt_calib_table_t *t,
                       float target_temp_c, float distance_m, uint16_t *out);

/* As above, but returns the value as a float fraction (raw / 16384). */
int dyt_calib_tau_read_f(const dyt_calib_table_t *t,
                         float target_temp_c, float distance_m, float *out);

/* Exposed for testing: the vendor's band-selection rule.
 *   axis[0] > x            -> 0
 *   x >= axis[n-1]         -> n-1
 *   otherwise              -> (first i with x <= axis[i] - eps) - 1, clamped to >= 0
 * Returns the index; never fails. */
int dyt_calib_band_index(const double *axis, int n, double x, double eps);

/* The axis arrays, exposed for testing and for callers that want to
 * display the band edges.  `layout` selects the pair. */
const double *dyt_calib_temp_axis(dyt_calib_layout_t layout, int *n);
const double *dyt_calib_dist_axis(dyt_calib_layout_t layout, int *n);

#endif /* DYT_CALIB_H */
