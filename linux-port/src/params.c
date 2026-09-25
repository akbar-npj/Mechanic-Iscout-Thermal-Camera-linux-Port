/*
 * params.c — device stored-parameter encodings and the read-back map.
 *
 * Transcribed from two independent sources that agree (RE Docs 04 §4.2):
 *
 *   - UVCPreviewIR::sendTinyCParamsModification (@ 0x167760) builds
 *     `14 c5 00 <type> 00 00 <hi><lo>`, casting a float to the uint16
 *     payload — (int)(v + 273.15) for the temperature types and
 *     (int)(v * 128.0) for the ratio types.
 *   - MechaniscoutPcap/4.pcapng frames 4502/5178/5856/6532 carry the
 *     resulting bytes: `14 c5 00 03 … 00 7f`, `14 c5 00 01 … 01 2c`,
 *     `14 c5 00 02 … 01 2c`, `14 c5 00 04 … 00 7f`.
 *
 * The index map is measured, not decompiled: reading every slot 0..15 of
 * the getTinyCParams read-back against the reference unit (2026-09-25)
 * returns two bytes for each, and slots 1..5 match the values the app
 * writes.  Slots 6..15 answer with plausible calibration words whose
 * meaning is not established — they are exposed raw and named "slotN"
 * rather than guessed at.
 *
 * build:  cc -O2 -g -Wall -Wextra -Wno-unused-parameter -ffp-contract=off \
 *           -I. -c params.c -o params.o
 */
#include <math.h>
#include <string.h>

#include "params.h"
/* ------------------------------------------------------------------ encoding */

uint16_t dyt_param_encode_kelvin(float celsius)
{
    /* The vendor's C cast truncates toward zero, which is *not* floor():
     * -0.5 °C encodes to 272, not 273.  Write the truncation explicitly so
     * the two never drift apart. */
    return (uint16_t)(int)(celsius + 273.15f);
}

float dyt_param_decode_kelvin(uint16_t kelvin)
{
    return (float)kelvin - 273.15f;
}

uint16_t dyt_param_encode_ratio(float value)
{
    return (uint16_t)(int)(value * 128.0f);
}

float dyt_param_decode_ratio(uint16_t raw)
{
    return (float)raw / 128.0f;
}

/* ------------------------------------------------------- runtime write order */

/* Every sendOrder carries this on 0x1d08 after the 0x9d00 command.  It is
 * the same "result length" pre-fill the read path uses (control.c:
 * dyt_read_param), fixed at 2 here because a write reads nothing back. */
const uint8_t dyt_order_prefill[8] = { 0, 0, 0, 0, 0, 0, 0, 0x02 };

int dyt_order_type_ok(int type)
{
    return type >= DYT_ORDER_REFLECTED && type <= DYT_ORDER_DISTANCE;
}

int dyt_params_build_cmd(uint8_t cmd[8], int type, float value)
{
    long raw;

    if (!cmd || !dyt_order_type_ok(type))
        return -1;

    if (type == DYT_ORDER_REFLECTED || type == DYT_ORDER_AMBIENT) {
        if (!isfinite(value))
            return -1;
        raw = (long)(int)(value + 273.15f);
    } else {
        if (!isfinite(value))
            return -1;
        raw = (long)(int)(value * 128.0f);
    }
    if (raw < 0 || raw > 0xffffL)
        return -1;

    cmd[0] = 0x14;
    cmd[1] = 0xc5;
    cmd[2] = 0x00;
    cmd[3] = (uint8_t)type;
    cmd[4] = 0x00;
    cmd[5] = 0x00;
    cmd[6] = (uint8_t)((raw >> 8) & 0xff);   /* big-endian */
    cmd[7] = (uint8_t)(raw & 0xff);
    return 0;
}

/* --------------------------------------------------------------- index map */

/* Names for the slots the port interprets; everything else falls through
 * to "slotN".  Kept in index order so a reader can line this up against a
 * read-back dump. */
static const struct {
    int         index;
    const char *name;
    const char *unit;    /* NULL = no unit claimed */
} known[] = {
    { DYT_PARAM_SLOT0,      "slot0",       NULL  },
    { DYT_PARAM_REFLECTED,  "reflected",   "C"   },
    { DYT_PARAM_AMBIENT,    "ambient",     "C"   },
    { DYT_PARAM_EMISSIVITY, "emissivity",  NULL  },
    { DYT_PARAM_DISTANCE,   "distance",    "m"   },
    { DYT_PARAM_MACHINE,    "machine",     NULL  },
};
#define KNOWN_N ((int)(sizeof(known) / sizeof(known[0])))

static const char *lookup_name(int index)
{
    int i;
    for (i = 0; i < KNOWN_N; i++)
        if (known[i].index == index)
            return known[i].name;
    return NULL;
}

static const char *lookup_unit(int index)
{
    int i;
    for (i = 0; i < KNOWN_N; i++)
        if (known[i].index == index)
            return known[i].unit;
    return NULL;
}

const char *dyt_param_name(int index)
{
    /* Static strings rather than a shared buffer, so the result stays valid
     * across calls (the viewer prints several names in one printf). */
    static const char *slot_name[DYT_PARAM_N] = {
        "slot0",  "slot1",  "slot2",  "slot3",
        "slot4",  "slot5",  "slot6",  "slot7",
        "slot8",  "slot9",  "slot10", "slot11",
        "slot12", "slot13", "slot14", "slot15"
    };
    const char *n = lookup_name(index);

    if (n)
        return n;
    if (index < 0 || index >= DYT_PARAM_N)
        return "?";
    return slot_name[index];
}

const char *dyt_param_unit(int index)
{
    return lookup_unit(index);
}

int dyt_param_is_kelvin(int index)
{
    return index == DYT_PARAM_REFLECTED || index == DYT_PARAM_AMBIENT;
}

int dyt_param_is_ratio(int index)
{
    return index == DYT_PARAM_EMISSIVITY || index == DYT_PARAM_DISTANCE;
}

/* ------------------------------------------------------------------ snapshot */

void dyt_params_init(dyt_params_t *p)
{
    if (!p)
        return;
    memset(p->raw, 0, sizeof p->raw);
    p->valid = 0;
}

int dyt_params_set(dyt_params_t *p, int index, uint16_t value)
{
    if (!p || index < 0 || index >= DYT_PARAM_N)
        return -1;
    p->raw[index] = value;
    p->valid |= (1u << index);
    return 0;
}

int dyt_params_fail(dyt_params_t *p, int index)
{
    if (!p || index < 0 || index >= DYT_PARAM_N)
        return -1;
    p->raw[index] = 0;
    p->valid &= ~(1u << index);
    return 0;
}

int dyt_params_ok(const dyt_params_t *p, int index)
{
    if (!p || index < 0 || index >= DYT_PARAM_N)
        return 0;
    return (p->valid >> index) & 1u ? 1 : 0;
}

float dyt_params_value(const dyt_params_t *p, int index)
{
    if (!dyt_params_ok(p, index))
        return NAN;
    if (dyt_param_is_kelvin(index))
        return dyt_param_decode_kelvin(p->raw[index]);
    if (dyt_param_is_ratio(index))
        return dyt_param_decode_ratio(p->raw[index]);
    /* slot0 and machine have no unit the port can claim; hand back the raw
     * count so callers that only want to display it need no special case. */
    if (index == DYT_PARAM_SLOT0 || index == DYT_PARAM_MACHINE)
        return (float)p->raw[index];
    return NAN;
}

int dyt_params_count(const dyt_params_t *p)
{
    int i, n = 0;
    if (!p)
        return 0;
    for (i = 0; i < DYT_PARAM_N; i++)
        n += dyt_params_ok(p, i);
    return n;
}

/* ------------------------------------------------------------------ radiometry */

uint32_t dyt_params_radiometry(const dyt_params_t *p, dyt_radiometry_t *out)
{
    static const struct { int slot; uint32_t bit; } map[] = {
        { DYT_PARAM_REFLECTED,  DYT_RADIO_REFLECTED  },
        { DYT_PARAM_AMBIENT,    DYT_RADIO_AMBIENT    },
        { DYT_PARAM_EMISSIVITY, DYT_RADIO_EMISSIVITY },
        { DYT_PARAM_DISTANCE,   DYT_RADIO_DISTANCE   },
    };
    uint16_t *dst[4];
    uint32_t ok = 0;
    int i;

    if (!out)
        return 0;

    dst[0] = &out->reflected_k;
    dst[1] = &out->ambient_k;
    dst[2] = &out->emissivity;
    dst[3] = &out->distance;

    for (i = 0; i < 4; i++) {
        if (dyt_params_ok(p, map[i].slot)) {
            *dst[i] = p->raw[map[i].slot];
            ok |= map[i].bit;
        } else {
            *dst[i] = 0;
        }
    }
    out->ok = ok;
    return ok;
}

void dyt_radiometry_default(dyt_radiometry_t *out)
{
    if (!out)
        return;
    /* Measured on the reference unit 2026-09-25, and independently what the
     * Android app writes at connect (MechaniscoutPcap/4.pcapng 4234-4269). */
    out->reflected_k = 300;
    out->ambient_k   = 300;
    out->emissivity  = 127;
    out->distance    = 127;
    out->ok          = DYT_RADIO_ALL;
}

float dyt_radiometry_reflected_c(const dyt_radiometry_t *r)
{
    if (!r || !(r->ok & DYT_RADIO_REFLECTED))
        return NAN;
    return dyt_param_decode_kelvin(r->reflected_k);
}

float dyt_radiometry_ambient_c(const dyt_radiometry_t *r)
{
    if (!r || !(r->ok & DYT_RADIO_AMBIENT))
        return NAN;
    return dyt_param_decode_kelvin(r->ambient_k);
}

float dyt_radiometry_emissivity(const dyt_radiometry_t *r)
{
    if (!r || !(r->ok & DYT_RADIO_EMISSIVITY))
        return NAN;
    return dyt_param_decode_ratio(r->emissivity);
}

float dyt_radiometry_distance_m(const dyt_radiometry_t *r)
{
    if (!r || !(r->ok & DYT_RADIO_DISTANCE))
        return NAN;
    return dyt_param_decode_ratio(r->distance);
}
