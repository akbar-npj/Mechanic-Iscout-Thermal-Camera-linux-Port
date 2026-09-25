/*
 * sr_test.c — unit tests for the super-resolution policy helpers (sr.c).
 *
 * The helper module is pure, so everything here runs with no model, no MNN
 * runtime and no camera.  Three things are worth pinning hard:
 *
 *  1. **Which 8 bits the thermal mode feeds the model.**  The thermal plane is
 *     14-bit, so its low byte is a sawtooth, not a picture.  A test that only
 *     checked "the grey plane has a wide range" would pass for the raw low
 *     byte too, because a sawtooth also spans 0..255.  So the check is
 *     *monotonicity*: a rising scene must give a rising display grey, and the
 *     raw low byte must be shown to wrap.  That is the specific mistake this
 *     catches (feeding `raw & 0xff` instead of the display domain).
 *
 *  2. **That the display grey is the palette's own normalisation.**  Mode B
 *     renders by inverting it and calling dyt_render_rgb(), so if the two ever
 *     disagree the super-resolved picture would show a different colour for a
 *     temperature than the normal render.  Every one of the 256 levels is
 *     round-tripped through dyt_palette_index() and must come back unchanged.
 *
 *  3. **The packing.**  The model reads in[i*2] and its +32768 bias restores a
 *     0x80 high byte, so the packed form must put the plane at the even
 *     offsets and 0x80 at the odd ones — and unpack must read them back.
 *
 * usage:  ./sr_test
 * build:  via the Makefile (make check)
 */
#include <stdio.h>
#include <string.h>

#include "palette.h"
#include "sr.h"

static int fails;

static void ok(const char *what) { printf("  ok   %s\n", what); }

static void fail(const char *what, const char *detail)
{
    printf("  FAIL %-52s %s\n", what, detail);
    fails++;
}

static void intcheck(const char *what, int got, int want)
{
    if (got == want) {
        ok(what);
    } else {
        char d[96];
        snprintf(d, sizeof d, "got %d want %d", got, want);
        fail(what, d);
    }
}

/* ------------------------------------------------------------------ names */

static void test_names(void)
{
    printf("\n-- names --\n");
    intcheck("off",     strcmp(dyt_sr_name(DYT_SR_OFF),     "off") == 0, 1);
    intcheck("visible", strcmp(dyt_sr_name(DYT_SR_VISIBLE), "visible") == 0, 1);
    intcheck("thermal", strcmp(dyt_sr_name(DYT_SR_THERMAL), "thermal") == 0, 1);
    intcheck("unknown", strcmp(dyt_sr_name((dyt_sr_t)99),   "?") == 0, 1);
}

/* ------------------------------------------------- display-domain encoding */

/* The scene this test uses: a mode-1000 thermal plane.  The device sends raw
 * counts, and frame.h converts mode 1000 as `raw/64 - 273.15`, so a raw count
 * is 1/64 C.  The low byte of a raw count cycles every 256 counts = 4 C. */
static uint16_t raw_at(int i) { return (uint16_t)(19000 + i * 32); }
static float    temp_at(int i) { return (float)raw_at(i) / 64.0f - 273.15f; }

static void test_thermal_is_display_domain(void)
{
    enum { N = 32 };
    float    temps[N];
    uint16_t raw[N];
    uint8_t  grey[N];
    const uint8_t *raw_low = (const uint8_t *)raw;
    float    lo = 20.0f, hi = 30.0f;
    int      i, non_monotonic = 0, wraps = 0, differs = 0;

    printf("\n-- the thermal mode feeds the display domain, not the raw low byte --\n");

    for (i = 0; i < N; i++) {
        raw[i]   = raw_at(i);
        temps[i] = temp_at(i);
    }

    intcheck("thermal grey rc", dyt_sr_thermal_grey(temps, N, lo, hi, grey), 0);

    /* The scene rises by 0.5 C per pixel, so the display grey must rise too
     * (until it clamps at the top of the range). */
    for (i = 1; i < N; i++)
        if (grey[i] < grey[i - 1])
            non_monotonic++;
    intcheck("display grey follows the rising scene", non_monotonic, 0);

    /* The raw low byte is a sawtooth: it wraps every 4 C. */
    for (i = 1; i < N; i++)
        if (raw_low[2 * i] < raw_low[2 * (i - 1)])
            wraps++;
    intcheck("the raw low byte wraps (it is not a picture)", wraps > 0, 1);

    /* And they are not the same bytes.  A sawtooth and a ramp can coincide at
     * a point or two by chance (they do here, once), so this is a majority
     * rather than an equality — the monotonicity above is the real
     * discriminator; this only catches the case where they are the same
     * *sequence* wearing different names. */
    for (i = 0; i < N; i++)
        if (grey[i] != raw_low[2 * i])
            differs++;
    printf("       differs from the raw low byte at %d/%d samples\n", differs, N);
    intcheck("display grey is not the raw low byte",
             differs > N * 3 / 4, 1);

    /* Spot-check the ends against the range, not just monotonicity. */
    printf("       temps %.2f..%.2f C, grey %u..%u, raw low %u..%u\n",
           temps[0], temps[N - 1], (unsigned)grey[0], (unsigned)grey[N - 1],
           (unsigned)raw_low[0], (unsigned)raw_low[2 * (N - 1)]);
    intcheck("coldest sample is in-range grey", grey[0] > 0 && grey[0] < 255, 1);
    intcheck("hottest sample clamps at the top", grey[N - 1], 255);
}

static void test_thermal_grey_range(void)
{
    const float temps[5] = { 20.0f, 25.0f, 30.0f, 19.0f, 31.0f };
    uint8_t grey[5];

    printf("\n-- display grey normalisation --\n");

    intcheck("rc", dyt_sr_thermal_grey(temps, 5, 20.0f, 30.0f, grey), 0);
    intcheck("at lo is 0",        grey[0], 0);
    intcheck("at mid is ~128",    grey[1] >= 127 && grey[1] <= 128, 1);
    intcheck("at hi is 255",      grey[2], 255);
    intcheck("below lo clamps",   grey[3], 0);
    intcheck("above hi clamps",   grey[4], 255);

    /* A flat frame has no span: mid-grey, not a division by zero. */
    {
        const float flat[3] = { 21.0f, 21.0f, 21.0f };
        uint8_t g2[3];
        intcheck("flat rc", dyt_sr_thermal_grey(flat, 3, 21.0f, 21.0f, g2), 0);
        intcheck("flat -> non-physical grey", g2[0], DYT_SR_NAN_GREY);
        intcheck("flat is uniform", g2[0] == g2[1] && g2[1] == g2[2], 1);
    }

    /* A NaN sample has no reading. */
    {
        const float withnan[2] = { 25.0f, 0.0f / 0.0f };
        uint8_t g2[2];
        intcheck("nan rc", dyt_sr_thermal_grey(withnan, 2, 20.0f, 30.0f, g2), 0);
        intcheck("nan -> non-physical grey", g2[1], DYT_SR_NAN_GREY);
    }
}

static void test_palette_roundtrip(void)
{
    const float lo = -10.0f, hi = 40.0f;
    int g, bad = 0, chain_bad = 0;

    printf("\n-- the display grey is the palette's own normalisation --\n");

    /* Every grey level, back to a temperature, must land on that same palette
     * index.  This is what lets mode B render through dyt_render_rgb() and
     * still agree with the normal render. */
    for (g = 0; g < 256; g++) {
        uint8_t grey = (uint8_t)g;
        float   t;
        int     idx;

        if (dyt_sr_grey_to_temps(&grey, 1, lo, hi, &t) != 0) { bad++; continue; }
        idx = dyt_palette_index(t, lo, hi);
        if (idx != g) bad++;
    }
    intcheck("every grey level round-trips to the same palette index", bad, 0);

    /* And the full chain a real frame takes: temps -> grey -> temps -> index
     * must equal temps -> index. */
    {
        int i;
        for (i = 0; i < 200; i++) {
            float   t = lo + (float)i * (hi - lo) / 199.0f;
            uint8_t grey;
            float   back;
            int     want, got;

            dyt_sr_thermal_grey(&t, 1, lo, hi, &grey);
            dyt_sr_grey_to_temps(&grey, 1, lo, hi, &back);
            want = dyt_palette_index(t, lo, hi);
            got  = dyt_palette_index(back, lo, hi);
            if (want != got) chain_bad++;
        }
    }
    intcheck("temps -> grey -> temps keeps the palette index", chain_bad, 0);

    /* Degenerate range: no span, so the inverse yields lo. */
    {
        uint8_t grey = 200;
        float   t = 0.0f;
        intcheck("degenerate inverse rc",
                 dyt_sr_grey_to_temps(&grey, 1, 5.0f, 5.0f, &t), 0);
        intcheck("degenerate inverse yields lo", t == 5.0f, 1);
    }
}

/* -------------------------------------------------------------- packing */

static void test_pack_unpack(void)
{
    const uint8_t grey[4] = { 0, 1, 0x80, 0xff };
    uint8_t packed[8];
    uint8_t back[4];
    int     i, high_ok = 1;

    printf("\n-- the model's packing --\n");

    intcheck("pack rc", dyt_sr_pack(grey, 4, packed), 0);
    for (i = 0; i < 4; i++) {
        if (packed[2 * i] != grey[i])
            high_ok = 0;
        if (packed[2 * i + 1] != DYT_SR_PACK_HIGH)
            high_ok = 0;
    }
    intcheck("low byte is the plane, high byte is 0x80", high_ok, 1);

    intcheck("unpack rc", dyt_sr_unpack(packed, 4, back), 0);
    intcheck("pack/unpack round-trips", memcmp(grey, back, 4) == 0, 1);

    /* A real model output carries 0x80 in the high byte, so unpacking it must
     * give the plane back without any masking surprise. */
    {
        uint8_t model_out[8];
        for (i = 0; i < 4; i++) {
            model_out[2 * i]     = (uint8_t)(10 * (i + 1));
            model_out[2 * i + 1] = DYT_SR_PACK_HIGH;
        }
        intcheck("unpack a model-shaped buffer",
                 dyt_sr_unpack(model_out, 4, back) == 0 &&
                 back[0] == 10 && back[1] == 20 && back[2] == 30 &&
                 back[3] == 40, 1);
    }
}

/* ------------------------------------------------------------ nearest 2x */

static void test_nearest2(void)
{
    const uint8_t in[6] = { 1, 2, 3,
                            4, 5, 6 };      /* 3 x 2 */
    uint8_t out[24];
    const uint8_t want[24] = { 1, 1, 2, 2, 3, 3,
                               1, 1, 2, 2, 3, 3,
                               4, 4, 5, 5, 6, 6,
                               4, 4, 5, 5, 6, 6 };

    printf("\n-- nearest 2x --\n");

    intcheck("nearest2 rc", dyt_sr_nearest2(in, 3, 2, out), 0);
    intcheck("each pixel becomes a 2x2 block", memcmp(out, want, 24) == 0, 1);

    {
        const float fin[6] = { 1.5f, 2.5f, 3.5f,
                               4.5f, 5.5f, 6.5f };
        float fout[24];
        int   i, bad = 0;

        intcheck("nearest2_f rc", dyt_sr_nearest2_f(fin, 3, 2, fout), 0);
        for (i = 0; i < 6; i++) {
            int x = i % 3, y = i / 3;
            if (fout[(2 * y) * 6 + 2 * x]     != fin[i] ||
                fout[(2 * y) * 6 + 2 * x + 1] != fin[i] ||
                fout[(2 * y + 1) * 6 + 2 * x]     != fin[i] ||
                fout[(2 * y + 1) * 6 + 2 * x + 1] != fin[i])
                bad++;
        }
        intcheck("float plane doubles the same way", bad, 0);
    }
}

/* --------------------------------------------------------------- rejects */

static void test_reject(void)
{
    float   t[4] = { 1, 2, 3, 4 };
    uint8_t g[8], p[8];
    float   f[8];

    printf("\n-- reject bad arguments --\n");

    intcheck("thermal_grey NULL temps",
             dyt_sr_thermal_grey(NULL, 4, 0, 1, g), -1);
    intcheck("thermal_grey NULL out",
             dyt_sr_thermal_grey(t, 4, 0, 1, NULL), -1);
    intcheck("thermal_grey zero n",
             dyt_sr_thermal_grey(t, 0, 0, 1, g), -1);

    intcheck("grey_to_temps NULL grey",
             dyt_sr_grey_to_temps(NULL, 4, 0, 1, f), -1);
    intcheck("grey_to_temps NULL out",
             dyt_sr_grey_to_temps(g, 4, 0, 1, NULL), -1);

    intcheck("pack NULL",   dyt_sr_pack(NULL, 4, p), -1);
    intcheck("pack zero n", dyt_sr_pack(g, 0, p), -1);
    intcheck("unpack NULL", dyt_sr_unpack(NULL, 4, g), -1);

    intcheck("nearest2 NULL in",  dyt_sr_nearest2(NULL, 2, 2, g), -1);
    intcheck("nearest2 zero w",   dyt_sr_nearest2(g, 0, 2, p), -1);
    intcheck("nearest2_f NULL in", dyt_sr_nearest2_f(NULL, 2, 2, f), -1);
    intcheck("nearest2_f zero h",  dyt_sr_nearest2_f(f, 2, 0, f), -1);
}

int main(void)
{
    printf("=== sr_test (super-resolution policy helpers) ===\n");

    test_names();
    test_thermal_is_display_domain();
    test_thermal_grey_range();
    test_palette_roundtrip();
    test_pack_unpack();
    test_nearest2();
    test_reject();

    printf("\n=== %s ===\n", fails ? "FAIL" : "ALL PASS");
    return fails ? 1 : 0;
}
