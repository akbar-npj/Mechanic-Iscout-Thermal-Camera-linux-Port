/*
 * calib.c — see calib.h for provenance and for the vendor VAs.
 *
 * Two deliberate divergences from the vendor code, both to avoid
 * out-of-bounds reads that exist in the original:
 *
 *   VENDOR-BUG 1  read_compatible_tau_with_target_temp_and_dist selects
 *                 the last distance band when the query is at or beyond
 *                 the top of the axis, then bilinearly reads column
 *                 col+1 -- one column past the end of the row.  We take
 *                 the single-cell path instead, which is what the same
 *                 function already does for the temperature axis.
 *
 *   VENDOR-BUG 2  read_tau_with_target_temp_and_dist computes the row as
 *                 (ushort)(i-1) where i can be 0, giving 0xFFFF, then
 *                 masks to 0x3FF -- an index far past the table.  We
 *                 clamp instead.
 *
 * Neither can be triggered by a sane (target temperature, distance)
 * pair, so behaviour on real inputs is identical.
 */

#include "calib.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Constants, all read out of the vendor DLLs' .data (RE Docs 10 §2.2) */
/* ------------------------------------------------------------------ */

#define CALIB_HEADER_BYTES  256      /* 0x40 dwords, skipped when magic present */
#define CALIB_MAGIC         0xFFFFFFFFu

/* libirtemp.dll DAT_1000b570 / Tiny1CDll.dll _DAT_10025330 */
#define CALIB_CELSIUS_TO_K  273.15
/* libirtemp.dll DAT_1000b4d8 = Tiny1CDll.dll DAT_10028a40 */
#define CALIB_BAND_EPS      0.001
/* libirtemp.dll _DAT_10009580 = Tiny1CDll.dll _DAT_10028a58 */
#define CALIB_ROUND_HALF    0.5

/* Target-temperature axis, Kelvin: 248.15 + 25*i. */
static const double k_temp_v1[56] = {
    248.15, 273.15, 298.15, 323.15, 348.15, 373.15, 398.15, 423.15, 448.15,
    473.15, 498.15, 523.15, 548.15, 573.15, 598.15, 623.15, 648.15, 673.15,
    698.15, 723.15, 748.15, 773.15, 798.15, 823.15, 848.15, 873.15, 898.15,
    923.15, 948.15, 973.15, 998.15, 1023.15, 1048.15, 1073.15, 1098.15,
    1123.15, 1148.15, 1173.15, 1198.15, 1223.15, 1248.15, 1273.15, 1298.15,
    1323.15, 1348.15, 1373.15, 1398.15, 1423.15, 1448.15, 1473.15, 1498.15,
    1523.15, 1548.15, 1573.15, 1598.15, 1623.15
};

/* libirtemp.dll DAT_1000d6f8: same series, truncated at 42 entries. */
static const double k_temp_v2[42] = {
    248.15, 273.15, 298.15, 323.15, 348.15, 373.15, 398.15, 423.15, 448.15,
    473.15, 498.15, 523.15, 548.15, 573.15, 598.15, 623.15, 648.15, 673.15,
    698.15, 723.15, 748.15, 773.15, 798.15, 823.15, 848.15, 873.15, 898.15,
    923.15, 948.15, 973.15, 998.15, 1023.15, 1048.15, 1073.15, 1098.15,
    1123.15, 1148.15, 1173.15, 1198.15, 1223.15, 1248.15, 1273.15
};

/* libirtemp.dll DAT_1000db08: 25 K steps to 823.15 K, then 50 K steps. */
static const double k_temp_v3[45] = {
    248.15, 273.15, 298.15, 323.15, 348.15, 373.15, 398.15, 423.15, 448.15,
    473.15, 498.15, 523.15, 548.15, 573.15, 598.15, 623.15, 648.15, 673.15,
    698.15, 723.15, 748.15, 773.15, 798.15, 823.15, 873.15, 923.15, 973.15,
    1023.15, 1073.15, 1123.15, 1173.15, 1223.15, 1273.15, 1323.15, 1373.15,
    1423.15, 1473.15, 1523.15, 1573.15, 1623.15, 1673.15, 1723.15, 1773.15,
    1823.15, 1873.15
};

/* Distance axis, metres: 0.25 .. 50 (libirtemp.dll DAT_1000d008). */
static const double k_dist_v1[64] = {
    0.25, 0.30, 0.35, 0.40, 0.45, 0.50, 0.55, 0.60, 0.65, 0.70, 0.75, 0.80,
    0.85, 0.90, 0.95, 1.00, 1.05, 1.10, 1.15, 1.20, 1.30, 1.40, 1.50, 1.60,
    1.70, 1.80, 1.90, 2.00, 2.20, 2.40, 2.60, 2.80, 3.00, 3.20, 3.40, 3.60,
    3.80, 4.00, 4.50, 5.00, 5.50, 6.00, 6.50, 7.00, 7.50, 8.00, 9.00, 10.0,
    11.0, 12.0, 13.0, 14.0, 16.0, 18.0, 20.0, 22.0, 24.0, 26.0, 28.0, 30.0,
    35.0, 40.0, 45.0, 50.0
};

/* libirtemp.dll DAT_1000d438 / DAT_1000d848: extended to 1000 m. */
static const double k_dist_v2[88] = {
    0.25, 0.30, 0.35, 0.40, 0.45, 0.50, 0.55, 0.60, 0.65, 0.70, 0.75, 0.80,
    0.85, 0.90, 0.95, 1.00, 1.05, 1.10, 1.15, 1.20, 1.30, 1.40, 1.50, 1.60,
    1.70, 1.80, 1.90, 2.00, 2.20, 2.40, 2.60, 2.80, 3.00, 3.20, 3.40, 3.60,
    3.80, 4.00, 4.50, 5.00, 5.50, 6.00, 6.50, 7.00, 7.50, 8.00, 9.00, 10.0,
    11.0, 12.0, 13.0, 14.0, 16.0, 18.0, 20.0, 22.0, 24.0, 26.0, 28.0, 30.0,
    35.0, 40.0, 45.0, 50.0, 60.0, 70.0, 80.0, 90.0, 100.0, 120.0, 140.0,
    160.0, 180.0, 200.0, 220.0, 240.0, 260.0, 280.0, 300.0, 350.0, 400.0,
    450.0, 500.0, 600.0, 700.0, 800.0, 900.0, 1000.0
};

/* ------------------------------------------------------------------ */
/* Layout selection                                                    */
/* ------------------------------------------------------------------ */

static void layout_dims(dyt_calib_layout_t l, int *rows, int *cols)
{
    switch (l) {
    case DYT_CALIB_V2: *rows = 42; *cols = 88; break;
    case DYT_CALIB_V3: *rows = 45; *cols = 88; break;
    case DYT_CALIB_V1:
    default:           *rows = 56; *cols = 64; break;
    }
}

const double *dyt_calib_temp_axis(dyt_calib_layout_t layout, int *n)
{
    switch (layout) {
    case DYT_CALIB_V2: *n = 42; return k_temp_v2;
    case DYT_CALIB_V3: *n = 45; return k_temp_v3;
    case DYT_CALIB_V1:
    default:           *n = 56; return k_temp_v1;
    }
}

const double *dyt_calib_dist_axis(dyt_calib_layout_t layout, int *n)
{
    switch (layout) {
    case DYT_CALIB_V2:
    case DYT_CALIB_V3: *n = 88; return k_dist_v2;
    case DYT_CALIB_V1:
    default:           *n = 64; return k_dist_v1;
    }
}

static uint16_t rd_u16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------ */
/* Loading                                                             */
/* ------------------------------------------------------------------ */

void dyt_calib_free(dyt_calib_table_t *t)
{
    if (t == NULL)
        return;
    free(t->data);
    memset(t, 0, sizeof(*t));
}

int dyt_calib_load_mem(dyt_calib_table_t *t, const void *buf, size_t len)
{
    const uint8_t *b = (const uint8_t *)buf;
    size_t         off, need;
    int            i, n;
    dyt_calib_layout_t layout;
    uint32_t       header = 0;
    int            has_header = 0;

    if (t == NULL || buf == NULL)
        return -1;
    memset(t, 0, sizeof(*t));

    /* Header magic: dword at +4 is 0xFFFFFFFF.  The vendor tests the four
     * bytes individually (libirtemp.dll.c:3944), which is equivalent. */
    if (len >= CALIB_HEADER_BYTES && rd_u32le(b + 4) == CALIB_MAGIC) {
        has_header = 1;
        header = rd_u32le(b);
        off = CALIB_HEADER_BYTES;
    } else {
        off = 0;
    }

    if (has_header && (header >> 16) > 0x3Fu) {
        layout = ((header >> 16) < 0x101u) ? DYT_CALIB_V2 : DYT_CALIB_V3;
    } else {
        layout = DYT_CALIB_V1;
    }

    layout_dims(layout, &t->rows, &t->cols);
    n = t->rows * t->cols;
    need = off + (size_t)n * 2;
    if (len < need) {
        memset(t, 0, sizeof(*t));
        return -1;
    }

    t->data = (uint16_t *)malloc((size_t)n * sizeof(uint16_t));
    if (t->data == NULL) {
        memset(t, 0, sizeof(*t));
        return -1;
    }
    for (i = 0; i < n; i++)
        t->data[i] = rd_u16le(b + off + (size_t)i * 2);

    t->has_header = has_header;
    t->header = header;
    t->version = has_header ? (header >> 16) : 0u;
    t->layout = layout;
    return 0;
}

int dyt_calib_load(dyt_calib_table_t *t, const char *path)
{
    FILE    *f;
    uint8_t *buf;
    long     sz;
    int      rc;

    if (t == NULL || path == NULL)
        return -1;
    memset(t, 0, sizeof(*t));

    f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
        fclose(f);
        return -1;
    }
    rewind(f);
    buf = (uint8_t *)malloc((size_t)sz);
    if (buf == NULL) {
        fclose(f);
        return -1;
    }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    rc = dyt_calib_load_mem(t, buf, (size_t)sz);
    free(buf);
    return rc;
}

char *dyt_calib_name(char *dst, size_t n, char kind, int high_gain)
{
    if (dst == NULL || n == 0)
        return dst;
    snprintf(dst, n, "%s_%c.bin", kind == 'm' ? "MILI6" : "tau",
             high_gain ? 'H' : 'L');
    return dst;
}

/* ------------------------------------------------------------------ */
/* Lookup                                                              */
/* ------------------------------------------------------------------ */

int dyt_calib_band_index(const double *axis, int n, double x, double eps)
{
    int i;

    if (axis == NULL || n < 2)
        return 0;
    if (x < axis[0])
        return 0;
    if (x >= axis[n - 1])
        return n - 1;            /* vendor: "too long" -> last band */

    for (i = 0; i < n; i++) {
        if (x <= axis[i] - eps)
            break;
    }
    i -= 1;
    if (i < 0)
        i = 0;
    if (i > n - 2)
        i = n - 2;
    return i;
}

int dyt_calib_tau_read(const dyt_calib_table_t *t,
                       float target_temp_c, float distance_m, uint16_t *out)
{
    const double *taxis, *daxis;
    int           tn, dn, row, col;
    double        tk, xd, dt, dd, gt, ft, gd, fd, den, acc;

    if (t == NULL || t->data == NULL || out == NULL)
        return -1;
    if (t->rows < 2 || t->cols < 2)
        return -1;

    taxis = dyt_calib_temp_axis(t->layout, &tn);
    daxis = dyt_calib_dist_axis(t->layout, &dn);
    if (tn != t->rows || dn != t->cols)
        return -1;

    /* The vendor adds 273.15 as a double, narrows to float, widens back. */
    tk = (double)(float)((double)target_temp_c + CALIB_CELSIUS_TO_K);
    xd = (double)distance_m;

    row = dyt_calib_band_index(taxis, tn, tk, CALIB_BAND_EPS);
    col = dyt_calib_band_index(daxis, dn, xd, CALIB_BAND_EPS);

    /* Single-cell path.  Two reasons to take it:
     *
     *   - the temperature band is the first or the last one.  This is the
     *     vendor's `row == 0 || row > rows-2` test, present verbatim in all
     *     three libirtemp builds and in Tiny1CDll (see RE Docs 10 §2.4).
     *     The row == 0 half is almost certainly a bug -- the author meant
     *     "below the first breakpoint", which the search already reports as
     *     row 0 -- but it is shipped behaviour and it changes the answer for
     *     every target between -25 C and 0 C, so we reproduce it.  Flip it
     *     here if the port should be more accurate than the vendor.
     *
     *   - the distance band is the last one.  The vendor instead bilinearly
     *     reads column col+1, one past the end of the row (VENDOR-BUG 1);
     *     we take the single-cell path. */
    if (row == 0 || row >= t->rows - 1 || col >= t->cols - 1) {
        *out = t->data[row * t->cols + col];
        return 0;
    }

    dt = taxis[row + 1] - taxis[row];
    dd = daxis[col + 1] - daxis[col];
    gt = taxis[row + 1] - tk;      /* weight for row   */
    ft = tk - taxis[row];          /* weight for row+1 */
    gd = daxis[col + 1] - xd;      /* weight for col   */
    fd = xd - daxis[col];          /* weight for col+1 */
    den = fabs(dt) * fabs(dd);
    if (den == 0.0) {
        *out = t->data[row * t->cols + col];
        return 0;
    }

    acc = (double)t->data[row * t->cols + col + 1] * ((gt * fd) / den)
        + (double)t->data[row * t->cols + col]     * ((gt * gd) / den)
        + (double)t->data[(row + 1) * t->cols + col]     * ((ft * gd) / den)
        + (double)t->data[(row + 1) * t->cols + col + 1] * ((ft * fd) / den)
        + CALIB_ROUND_HALF;

    *out = (uint16_t)(int)acc;
    return 0;
}

int dyt_calib_tau_read_f(const dyt_calib_table_t *t,
                         float target_temp_c, float distance_m, float *out)
{
    uint16_t raw;

    if (out == NULL)
        return -1;
    if (dyt_calib_tau_read(t, target_temp_c, distance_m, &raw) != 0)
        return -1;
    *out = (float)raw / DYT_CALIB_Q14;
    return 0;
}
