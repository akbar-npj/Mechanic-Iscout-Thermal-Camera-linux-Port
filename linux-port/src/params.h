/*
 * params.h — the device's stored-parameter vocabulary for the DYT thermal
 * camera Linux port.
 *
 * Two independent things live here:
 *
 *  1. The *fixed-point encodings* the vendor uses on the wire.  Every
 *     runtime parameter is a single big-endian uint16, but the unit
 *     depends on the slot:
 *
 *       types 1, 2 (reflected, ambient)  — whole kelvin
 *                                          write: (int)(celsius + 273.15)
 *       types 3, 4 (emissivity, distance) — 1/128 units
 *                                          write: (int)(value * 128.0)
 *
 *     Verified two ways: the sendTinyCParamsModification decompilation
 *     (RE Docs 04 §4.2) and MechaniscoutPcap/4.pcapng, where the app
 *     writes `14 c5 00 03 … 00 7f` (127/128 = 0.992 emissivity) and
 *     `14 c5 00 01 … 01 2c` (300 K reflected).
 *
 *  2. The *index map* of the getTinyCParams read-back.  Reading slot N
 *     returns slot N's raw uint16; the port only claims to understand
 *     slots 0..5, which are the ones either written by the app or
 *     corroborated by a read-back after a write.
 *
 * Nothing here talks to hardware — the transfer lives in control.c and
 * the wiring in capture.c, so this module is testable with no device.
 *
 * build:  cc -O2 -g -Wall -Wextra -Wno-unused-parameter -ffp-contract=off \
 *           -I. -c params.c -o params.o
 */
#ifndef DYT_PARAMS_H
#define DYT_PARAMS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ encoding */

/* (int)(celsius + 273.15) — the vendor's whole-kelvin encoding.  The cast
 * truncates toward zero, so it is not a round-trip: 26.85 °C encodes to
 * 300 and decodes back to 26.85 °C, but 26.9 °C also encodes to 300. */
uint16_t dyt_param_encode_kelvin(float celsius);
float    dyt_param_decode_kelvin(uint16_t kelvin);

/* (int)(value * 128.0) — the vendor's 1/128 encoding for emissivity and
 * distance.  Emissivity is dimensionless (127 -> 0.9922); distance is
 * metres (127 -> 0.992 m). */
uint16_t dyt_param_encode_ratio(float value);
float    dyt_param_decode_ratio(uint16_t raw);

/* ------------------------------------------------------- runtime write order
 *
 * `sendOrder(float, type)` is the vendor's *runtime* parameter write — the
 * one device write this port implements.  It is a runtime setting, not
 * calibration: the Windows app sends these orders during normal operation
 * (MechaniscoutPcap/4.pcapng).  The persistent-calibration writers
 * (`setMachineSetting`, `setTinySaveCameraParams`) are deliberately NOT
 * implemented.
 *
 * Wire form — two OUT transfers, **no status poll** (unlike every read):
 *
 *   OUT 0x9d00 <- 14 c5 00 <type> 00 00 <hi> <lo>
 *   OUT 0x1d08 <- 00 00 00 00 00 00 00 02      (result-length pre-fill)
 *
 * The transfer itself lives in control.c (`dyt_write_param`); this module
 * only builds the bytes, so it stays device-free and testable.
 */

/* The sendOrder type byte.  Type N writes the same index N that
 * dyt_read_param() reads back, which is how a write is verified. */
typedef enum {
    DYT_ORDER_REFLECTED  = 1,   /* Celsius -> whole kelvin */
    DYT_ORDER_AMBIENT    = 2,   /* Celsius -> whole kelvin */
    DYT_ORDER_EMISSIVITY = 3,   /* dimensionless -> 1/128 */
    DYT_ORDER_DISTANCE   = 4    /* metres -> 1/128 */
} dyt_order_type_t;

/* The fixed 8-byte pre-fill that follows every sendOrder on 0x1d08. */
extern const uint8_t dyt_order_prefill[8];

/* Is `type` one of the four runtime parameters this port will write? */
int dyt_order_type_ok(int type);

/* Encode sendOrder(type, value) into its 8-byte 0x9d00 payload.  `value`
 * is in the type's natural unit (Celsius for types 1/2, dimensionless /
 * metres for 3/4).
 *
 * Returns 0, or -1 for an unknown type or a value the encoding cannot
 * represent (non-finite, negative, or above 65535 after scaling). */
int dyt_params_build_cmd(uint8_t cmd[8], int type, float value);

/* --------------------------------------------------------------- index map */

/* The read-back slots.  DYT_PARAM_N is the width of the read-back: the
 * device answers every index 0..15 with two bytes (measured 2026-09-25). */
typedef enum {
    DYT_PARAM_SLOT0      = 0,   /* 32 on the reference unit — meaning [I] */
    DYT_PARAM_REFLECTED  = 1,   /* kelvin — sendOrder type 1 */
    DYT_PARAM_AMBIENT    = 2,   /* kelvin — sendOrder type 2 */
    DYT_PARAM_EMISSIVITY = 3,   /* 1/128 — sendOrder type 3 */
    DYT_PARAM_DISTANCE   = 4,   /* 1/128 m — sendOrder type 4 */
    DYT_PARAM_MACHINE    = 5,   /* setMachineSetting coefficient */
    DYT_PARAM_N          = 16
} dyt_param_id_t;

/* Stable short name for a slot ("reflected", "emissivity", …), or
 * "slot<N>" for the ones the port does not interpret.  Never NULL. */
const char *dyt_param_name(int index);

/* Unit suffix for a slot's *decoded* value ("K", "", "m"), or NULL when the
 * slot has no known unit. */
const char *dyt_param_unit(int index);

/* Does dyt_param_decode_ratio() apply to this slot? */
int dyt_param_is_ratio(int index);

/* Does dyt_param_decode_kelvin() apply to this slot? */
int dyt_param_is_kelvin(int index);

/* ------------------------------------------------------------------ snapshot */

/* A full read-back.  `raw[i]` is slot i as the device returned it;
 * `valid` is a bitmask — bit i set means the read of slot i succeeded.
 * A slot whose bit is clear must be ignored, not treated as 0. */
typedef struct {
    uint16_t raw[DYT_PARAM_N];
    uint32_t valid;
} dyt_params_t;

/* Clear every slot. */
void dyt_params_init(dyt_params_t *p);

/* Record a successful read of `index`.  Returns 0, or -1 if `index` is
 * out of range (the snapshot is left untouched). */
int dyt_params_set(dyt_params_t *p, int index, uint16_t value);

/* Mark a slot's read as failed.  Returns 0, or -1 if out of range. */
int dyt_params_fail(dyt_params_t *p, int index);

/* Was slot `index` read successfully?  Returns 0 for an out-of-range
 * index, so it doubles as a range check. */
int dyt_params_ok(const dyt_params_t *p, int index);

/* Decoded value of slot `index` in the slot's natural unit, or NaN when
 * the slot was not read or has no known unit.  Needs <math.h> for NAN. */
float dyt_params_value(const dyt_params_t *p, int index);

/* How many slots were read successfully. */
int dyt_params_count(const dyt_params_t *p);

/* The four runtime radiometric parameters the app exposes, as the device
 * stores them.  `ok` is a bitmask of the same four slots, in the order
 * reflected/ambient/emissivity/distance (bits 0..3). */
typedef struct {
    uint16_t reflected_k;
    uint16_t ambient_k;
    uint16_t emissivity;
    uint16_t distance;
    uint32_t ok;
} dyt_radiometry_t;

/* Bit positions in dyt_radiometry_t.ok, and the matching mask helpers. */
#define DYT_RADIO_REFLECTED  (1u << 0)
#define DYT_RADIO_AMBIENT    (1u << 1)
#define DYT_RADIO_EMISSIVITY (1u << 2)
#define DYT_RADIO_DISTANCE   (1u << 3)
#define DYT_RADIO_ALL        0x0fu

/* Copy the four radiometric slots out of a snapshot.  Unread slots keep
 * their encoded default (see dyt_radiometry_default) and clear their ok
 * bit.  Returns the ok mask. */
uint32_t dyt_params_radiometry(const dyt_params_t *p, dyt_radiometry_t *out);

/* The values the reference unit ships with (measured 2026-09-25):
 * reflected 300 K, ambient 300 K, emissivity 127/128, distance 127/128. */
void dyt_radiometry_default(dyt_radiometry_t *out);

/* Convenience decoders for a dyt_radiometry_t.  All four are NaN when the
 * corresponding ok bit is clear. */
float dyt_radiometry_reflected_c(const dyt_radiometry_t *r);
float dyt_radiometry_ambient_c(const dyt_radiometry_t *r);
float dyt_radiometry_emissivity(const dyt_radiometry_t *r);
float dyt_radiometry_distance_m(const dyt_radiometry_t *r);

#ifdef __cplusplus
}
#endif

#endif /* DYT_PARAMS_H */
