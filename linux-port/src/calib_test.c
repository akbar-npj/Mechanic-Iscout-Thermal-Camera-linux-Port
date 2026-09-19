/*
 * calib_test.c — unit tests for the vendor calibration tables (calib.c).
 *
 * Two kinds of check, deliberately:
 *
 *   1. Structural invariants that hold for any correctly parsed table and
 *      do not depend on our reading of the vendor arithmetic at all.  These
 *      catch a wrong payload offset, a transposed table or a wrong stride.
 *
 *   2. Frozen values for eight interior points per file, produced by an
 *      independent Python transcription of the decompiled readers.  These
 *      pin the interpolation arithmetic and the Q14 rounding.
 *
 * The frozen values are regression anchors, not proof — they come from the
 * same reading of the disassembly that calib.c implements.  The structural
 * checks are what makes the pair meaningful.
 *
 * The vendor blobs live in calib/.  If they are absent the test reports SKIP
 * and exits 0, so `make check` still works on a checkout without them.
 *
 * build:  via the Makefile (make check)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "calib.h"

static int fails;
static int checks;

static void fail(const char *what, const char *detail)
{
    printf("  FAIL %-46s %s\n", what, detail);
    fails++;
}

static void expect_u16(const char *what, unsigned got, unsigned want)
{
    checks++;
    if (got != want) {
        char buf[96];
        snprintf(buf, sizeof buf, "got %u want %u", got, want);
        fail(what, buf);
    }
}

static void expect_int(const char *what, long got, long want)
{
    checks++;
    if (got != want) {
        char buf[96];
        snprintf(buf, sizeof buf, "got %ld want %ld", got, want);
        fail(what, buf);
    }
}

static char *join(const char *dir, const char *name)
{
    size_t n = strlen(dir) + strlen(name) + 2;
    char  *p = malloc(n);

    if (p == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    snprintf(p, n, "%s/%s", dir, name);
    return p;
}

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

static void test_parse(const char *dir)
{
    dyt_calib_table_t t;
    char             *path;

    printf("-- parsing --\n");

    /* A bare tau file: 7168 bytes, no header, version 1. */
    path = join(dir, "tau_H.bin");
    checks++;
    if (dyt_calib_load(&t, path) != 0) {
        fail("tau_H.bin loads", "returned non-zero");
    } else {
        expect_int("tau_H.bin layout", t.layout, DYT_CALIB_V1);
        expect_int("tau_H.bin has_header", t.has_header, 0);
        expect_int("tau_H.bin version", (long)t.version, 0);
        expect_int("tau_H.bin rows", t.rows, 56);
        expect_int("tau_H.bin cols", t.cols, 64);
        dyt_calib_free(&t);
    }
    free(path);

    /* A MILI6 file: 7424 bytes, 256-byte header, version 0x0b.  That is
     * <= 0x3F, so it keeps the version-1 axes even though it has a header. */
    path = join(dir, "MILI6_H.bin");
    checks++;
    if (dyt_calib_load(&t, path) != 0) {
        fail("MILI6_H.bin loads", "returned non-zero");
    } else {
        expect_int("MILI6_H.bin layout", t.layout, DYT_CALIB_V1);
        expect_int("MILI6_H.bin has_header", t.has_header, 1);
        expect_int("MILI6_H.bin version", (long)t.version, 0x0b);
        expect_int("MILI6_H.bin rows", t.rows, 56);
        expect_int("MILI6_H.bin cols", t.cols, 64);
        dyt_calib_free(&t);
    }
    free(path);

    /* Negative cases. */
    {
        static unsigned char junk[7424];

        memset(junk, 0, sizeof junk);
        expect_int("100-byte blob rejected",
                   dyt_calib_load_mem(&t, junk, 100), -1);
        expect_int("null out rejected",
                   dyt_calib_load_mem(NULL, junk, sizeof junk), -1);
        expect_int("null buffer rejected",
                   dyt_calib_load_mem(&t, NULL, sizeof junk), -1);

        path = join(dir, "does-not-exist.bin");
        expect_int("missing file rejected", dyt_calib_load(&t, path), -1);
        free(path);
    }

    /* Synthetic version-2 and version-3 images, to prove the dispatch is on
     * the header's high half rather than on the size alone. */
    {
        static unsigned char v2[256 + 42 * 88 * 2];
        static unsigned char v3[256 + 45 * 88 * 2];

        memset(v2, 0xff, sizeof v2);
        memset(v3, 0xff, sizeof v3);
        /* Write the whole dword: the surrounding bytes are the 0xFF magic,
         * so a partial store would leave 0xFFFF in the version field and
         * select the wrong layout.  The version lives in the *high* half. */
        v2[0] = 0x00; v2[1] = 0x00; v2[2] = 0x80; v2[3] = 0x00;  /* version 0x0080 */
        v3[0] = 0x00; v3[1] = 0x00; v3[2] = 0x00; v3[3] = 0x02;  /* version 0x0200 */
        memset(v2 + 256, 0, sizeof v2 - 256);
        memset(v3 + 256, 0, sizeof v3 - 256);

        checks++;
        if (dyt_calib_load_mem(&t, v2, sizeof v2) != 0) {
            fail("v2 image parses", "returned non-zero");
        } else {
            expect_int("v2 layout", t.layout, DYT_CALIB_V2);
            expect_int("v2 rows", t.rows, 42);
            expect_int("v2 cols", t.cols, 88);
            dyt_calib_free(&t);
        }

        checks++;
        if (dyt_calib_load_mem(&t, v3, sizeof v3) != 0) {
            fail("v3 image parses", "returned non-zero");
        } else {
            expect_int("v3 layout", t.layout, DYT_CALIB_V3);
            expect_int("v3 rows", t.rows, 45);
            expect_int("v3 cols", t.cols, 88);
            dyt_calib_free(&t);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Structural invariants                                               */
/* ------------------------------------------------------------------ */

static uint16_t cell(const dyt_calib_table_t *t, int row, int col)
{
    return t->data[row * t->cols + col];
}

/* For a target temperature landing exactly on a temperature breakpoint and a
 * distance landing exactly on a distance breakpoint, the bilinear weights
 * collapse to a single cell, so the result must be that cell.  True for every
 * row including the degenerate first and last.  This exercises the whole
 * index chain: band search, row/col derivation, stride and the single-cell
 * rule. */
static void test_grid_identity(const char *name, const dyt_calib_table_t *t)
{
    static const int rows[] = { 0, 1, 7, 20, 40, 54, 55 };
    static const int cols[] = { 0, 1, 15, 20, 38, 50, 61 };
    int              tr, tc, bad = 0;

    for (tr = 0; tr < (int)(sizeof rows / sizeof rows[0]); tr++) {
        for (tc = 0; tc < (int)(sizeof cols / sizeof cols[0]); tc++) {
            int      row = rows[tr], col = cols[tc];
            int      tn = 0, dn = 0;
            const double *taxis = dyt_calib_temp_axis(t->layout, &tn);
            const double *daxis = dyt_calib_dist_axis(t->layout, &dn);
            float    t_c = (float)(taxis[row] - 273.15);
            float    d_m = (float)daxis[col];
            uint16_t got = 0;
            char     what[80];

            snprintf(what, sizeof what, "%s grid[%d][%d]", name, row, col);
            if (dyt_calib_tau_read(t, t_c, d_m, &got) != 0) {
                fail(what, "read returned non-zero");
                bad++;
                continue;
            }
            checks++;
            if (got != cell(t, row, col)) {
                char buf[96];
                snprintf(buf, sizeof buf, "got %u want cell %u",
                         (unsigned)got, (unsigned)cell(t, row, col));
                fail(what, buf);
                bad++;
            }
        }
    }
    if (!bad)
        printf("  ok   %-46s %d/%d grid points\n", name,
               (int)(sizeof rows / sizeof rows[0]) * (int)(sizeof cols / sizeof cols[0]),
               (int)(sizeof rows / sizeof rows[0]) * (int)(sizeof cols / sizeof cols[0]));
}

/* At the exact centre of a cell the four bilinear weights are all 1/4, so the
 * result is the mean of the four corners.  Tolerance is one count, not zero:
 * the axis breakpoints are decimal (248.15 K, 0.25 m) and so are not exactly
 * representable in binary, and the implementation additionally narrows the
 * Celsius-to-Kelvin sum through a float.  Both the test and the implementation
 * therefore land a few ulps away from the true midpoint, and the +0.5
 * truncation can fall either side.  A wrong corner assignment or a transposed
 * table would be off by far more than one count. */
static void test_cell_centre(const char *name, const dyt_calib_table_t *t)
{
    static const int rows[] = { 1, 7, 20, 53 };
    static const int cols[] = { 0, 15, 38, 60 };
    int              tr, tc, bad = 0;
    int              tn = 0, dn = 0;
    const double    *taxis = dyt_calib_temp_axis(t->layout, &tn);
    const double    *daxis = dyt_calib_dist_axis(t->layout, &dn);

    for (tr = 0; tr < (int)(sizeof rows / sizeof rows[0]); tr++) {
        for (tc = 0; tc < (int)(sizeof cols / sizeof cols[0]); tc++) {
            int      row = rows[tr], col = cols[tc];
            float    t_c = (float)((taxis[row] + taxis[row + 1]) / 2.0 - 273.15);
            float    d_m = (float)((daxis[col] + daxis[col + 1]) / 2.0);
            double   mean = ((double)cell(t, row, col) +
                             (double)cell(t, row, col + 1) +
                             (double)cell(t, row + 1, col) +
                             (double)cell(t, row + 1, col + 1)) / 4.0;
            int      want = (int)(mean + 0.5);
            uint16_t got = 0;
            char     what[80];

            snprintf(what, sizeof what, "%s centre[%d][%d]", name, row, col);
            checks++;
            if (dyt_calib_tau_read(t, t_c, d_m, &got) != 0) {
                fail(what, "read returned non-zero");
                bad++;
                continue;
            }
            if ((int)got < want - 1 || (int)got > want + 1) {
                char buf[96];
                snprintf(buf, sizeof buf, "got %u want %d +/-1",
                         (unsigned)got, want);
                fail(what, buf);
                bad++;
            }
        }
    }
    if (!bad)
        printf("  ok   %-46s %d/%d cell centres\n", name,
               (int)(sizeof rows / sizeof rows[0]) * (int)(sizeof cols / sizeof cols[0]),
               (int)(sizeof rows / sizeof rows[0]) * (int)(sizeof cols / sizeof cols[0]));
}

/* Every row's nearest-distance cell is 1.0: at 0.25 m there is no atmosphere
 * between the camera and the target.  A transposed or mis-strided parse
 * breaks this immediately. */
static void test_nearest_distance_is_unity(const char *name, const dyt_calib_table_t *t)
{
    int row, bad = 0;

    for (row = 0; row < t->rows; row++) {
        checks++;
        if (cell(t, row, 0) != 16384) {
            char what[80];
            snprintf(what, sizeof what, "%s col0 row %d", name, row);
            fail(what, "nearest-distance transmittance is not 16384");
            bad++;
        }
    }
    if (!bad)
        printf("  ok   %-46s all %d rows start at 16384\n", name, t->rows);
}

/* Transmittance must stay inside a sane Q14 range.  The upper bound is not
 * exactly 1.0: tau_H.bin has 16 cells at 16399 (0.09 % over unity) in the
 * first two distance columns, where the high-gain fit runs slightly hot.
 * Anything further out than that means the table was parsed wrong. */
#define CELL_SANE_MAX 16448

static void test_bounded(const char *name, const dyt_calib_table_t *t)
{
    int row, col, bad = 0;

    for (row = 0; row < t->rows; row++) {
        for (col = 0; col < t->cols; col++) {
            int v = cell(t, row, col);
            checks++;
            if (v < 0 || v > CELL_SANE_MAX) {
                char what[80];
                snprintf(what, sizeof what, "%s range[%d][%d]", name, row, col);
                fail(what, "cell outside the sane Q14 range");
                bad++;
            }
        }
    }
    if (!bad)
        printf("  ok   %-46s all %d cells sane\n", name, t->rows * t->cols);
}

/* ------------------------------------------------------------------ */
/* Frozen values                                                       */
/* ------------------------------------------------------------------ */

struct vec {
    float    t_c;
    float    d_m;
    uint16_t want;
};

/* Generated by an independent Python transcription of
 * read_tau_with_target_temp_and_dist; see RE Docs 10 §8. */
static const struct vec VEC_TAU_H[] = {
    {  25.00f,   1.000f, 15723 },
    {  25.00f,   3.000f, 14732 },
    {  60.00f,   2.500f, 14796 },
    { 150.00f,   7.000f, 14024 },
    { 400.00f,  12.000f, 14499 },
    {1000.00f,  25.000f, 13696 },
    {  12.50f,   0.625f, 16068 },
    {  87.50f,   1.750f, 14928 },
};
static const struct vec VEC_TAU_L[] = {
    {  25.00f,   1.000f, 14859 },
    {  25.00f,   3.000f, 13225 },
    {  60.00f,   2.500f, 13413 },
    { 150.00f,   7.000f, 12910 },
    { 400.00f,  12.000f, 13169 },
    {1000.00f,  25.000f, 11401 },
    {  12.50f,   0.625f, 15503 },
    {  87.50f,   1.750f, 14282 },
};
static const struct vec VEC_MILI6_H[] = {
    {  25.00f,   1.000f, 15041 },
    {  25.00f,   3.000f, 13926 },
    {  60.00f,   2.500f, 14076 },
    { 150.00f,   7.000f, 11977 },
    { 400.00f,  12.000f, 10650 },
    {1000.00f,  25.000f,  8913 },
    {  12.50f,   0.625f, 15532 },
    {  87.50f,   1.750f, 14332 },
};
static const struct vec VEC_MILI6_L[] = {
    {  25.00f,   1.000f, 15237 },
    {  25.00f,   3.000f, 14139 },
    {  60.00f,   2.500f, 14348 },
    { 150.00f,   7.000f, 11960 },
    { 400.00f,  12.000f,  8667 },
    {1000.00f,  25.000f,  6234 },
    {  12.50f,   0.625f, 15647 },
    {  87.50f,   1.750f, 14733 },
};

static void test_vectors(const char *name, const dyt_calib_table_t *t,
                         const struct vec *v, int n)
{
    int i, bad = 0;

    for (i = 0; i < n; i++) {
        uint16_t got = 0;
        char     what[96];

        snprintf(what, sizeof what, "%s vec[%d] T=%.2f D=%.3f",
                 name, i, (double)v[i].t_c, (double)v[i].d_m);
        if (dyt_calib_tau_read(t, v[i].t_c, v[i].d_m, &got) != 0) {
            fail(what, "read returned non-zero");
            bad++;
            continue;
        }
        expect_u16(what, got, v[i].want);
    }
    if (!bad)
        printf("  ok   %-46s %d/%d frozen vectors\n", name, n, n);
}

/* ------------------------------------------------------------------ */
/* Range and rule behaviour                                            */
/* ------------------------------------------------------------------ */

/* Behaviour outside the axis ranges.
 *
 * Three of the four cases collapse to a single cell and can be asserted
 * against the table directly.  The fourth — a distance below 0.25 m — is not
 * clamped by the vendor's compatible reader: it selects band 0 and then
 * bilinearly extrapolates using the first cell, which yields a transmittance
 * *above* 1.0.  That is nonsense physically but it is what the code does, so
 * it gets a frozen literal rather than a plausible-looking assertion. */
static void test_clamping(const char *name, const dyt_calib_table_t *t,
                          unsigned want_dist_below)
{
    uint16_t got = 0;

    if (dyt_calib_tau_read(t, 25.0f, 0.0f, &got) == 0)
        expect_u16("dist below range (extrapolated)", got, want_dist_below);

    if (dyt_calib_tau_read(t, 25.0f, 1e6f, &got) == 0)
        expect_u16("dist above range -> last col", got, cell(t, 2, t->cols - 1));

    if (dyt_calib_tau_read(t, -200.0f, 1.0f, &got) == 0)
        expect_u16("target below range -> row 0", got, cell(t, 0, 15));

    if (dyt_calib_tau_read(t, 5000.0f, 1.0f, &got) == 0)
        expect_u16("target above range -> last row", got, cell(t, t->rows - 1, 15));

    printf("  ok   %-46s 4 range cases\n", name);
}

/* The vendor skips the interpolation when the temperature band is row 0, so
 * for a target between -25 C and 0 C the result is the nearest-lower distance
 * cell rather than a bilinear blend.  Reproduced deliberately, so assert it
 * explicitly rather than letting a future "cleanup" silently change the
 * numbers.  See RE Docs 10 §2.4. */
static void test_row0_degenerate(const char *name, const dyt_calib_table_t *t)
{
    const double *daxis;
    int           dn = 0;
    double        mid;
    uint16_t      got = 0;

    daxis = dyt_calib_dist_axis(t->layout, &dn);
    mid = (daxis[3] + daxis[4]) / 2.0;

    if (dyt_calib_tau_read(t, -25.0f, (float)mid, &got) != 0) {
        fail("row 0 degenerate read", "returned non-zero");
        return;
    }
    /* The plain cell, not the mean of the four corners. */
    expect_u16("row 0 skips distance interp", got, cell(t, 0, 3));

    /* Row 1 at the same distance is not degenerate, so it blends the row-1
     * and row-2 cells and must not come out as the plain cell. */
    got = 0;
    if (dyt_calib_tau_read(t, 0.0f, (float)mid, &got) == 0) {
        checks++;
        if (got == cell(t, 1, 3))
            fail("row 1 interpolates", "returned the plain cell");
    }

    printf("  ok   %-46s row-0 rule asserted\n", name);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    static const struct {
        const char       *file;
        const struct vec *v;
        int               n;
        unsigned          dist_below;   /* frozen extrapolation at 0 m */
    } cases[] = {
        { "tau_H.bin",   VEC_TAU_H,   (int)(sizeof VEC_TAU_H   / sizeof VEC_TAU_H[0]),   16749 },
        { "tau_L.bin",   VEC_TAU_L,   (int)(sizeof VEC_TAU_L   / sizeof VEC_TAU_L[0]),   17049 },
        { "MILI6_H.bin", VEC_MILI6_H, (int)(sizeof VEC_MILI6_H / sizeof VEC_MILI6_H[0]), 16549 },
        { "MILI6_L.bin", VEC_MILI6_L, (int)(sizeof VEC_MILI6_L / sizeof VEC_MILI6_L[0]), 17204 },
    };
    const char *dir = argc > 1 ? argv[1] : "calib";
    char       *probe = join(dir, "tau_H.bin");
    FILE       *f = fopen(probe, "rb");
    size_t      i;

    free(probe);
    if (f == NULL) {
        printf("calib_test: SKIP — no calibration blobs in '%s'\n", dir);
        printf("            see calib/README.md for how to extract them\n");
        return 0;
    }
    fclose(f);

    printf("calib_test: vendor calibration tables in '%s'\n", dir);

    test_parse(dir);

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        dyt_calib_table_t t;
        char             *path = join(dir, cases[i].file);

        printf("-- %s --\n", cases[i].file);
        checks++;
        if (dyt_calib_load(&t, path) != 0) {
            fail(cases[i].file, "load failed");
            free(path);
            continue;
        }
        free(path);

        test_nearest_distance_is_unity(cases[i].file, &t);
        test_bounded(cases[i].file, &t);
        test_grid_identity(cases[i].file, &t);
        test_cell_centre(cases[i].file, &t);
        test_vectors(cases[i].file, &t, cases[i].v, cases[i].n);
        test_clamping(cases[i].file, &t, cases[i].dist_below);
        test_row0_degenerate(cases[i].file, &t);
        dyt_calib_free(&t);
    }

    printf("\n%d checks, %d failures\n", checks, fails);
    if (fails) {
        printf("CALIB TEST FAILED\n");
        return 1;
    }
    printf("CALIB TEST PASSED\n");
    return 0;
}
