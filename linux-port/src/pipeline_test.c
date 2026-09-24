/*
 * pipeline_test.c — regression test for the live capture pipeline.
 *
 * The live path (capture.c) cannot run without hardware, but everything it
 * does per frame except the libuvc plumbing is device-independent and now
 * lives in frame.c behind dyt_pipeline_*.  This test drives that exact
 * sequence over frozen raw frames:
 *
 *   mode 1000, real     testdata/mode1000_256x192.raw
 *                       — captured from the 0bda:5840 unit (see testdata/README.md)
 *   mode 0x44c, real    tools/thermometry_diff/out/256/in_frame.bin
 *                       — the vendor ground-truth frame already in the tree
 *
 * What it pins:
 *   1. geometry resolution for both modes;
 *   2. the mode-1000 arithmetic (raw/64 - 273.15) over a real 256x192 frame;
 *   3. the LUT policy.  This is the latent bug the test exists for: the port
 *      used to build the calibration LUT from the very first frame, which on
 *      a real 0x44c unit is the flat 0x8000 placeholder.  A filler frame must
 *      leave the LUT unbuilt; the first valid frame must build it.
 *
 * usage:  ./pipeline_test <mode1000.raw> <0x44c-in_frame.bin>
 * build:  via the Makefile (make pipeline-test)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frame.h"

#define PLACEHOLDER 0x8000   /* the device's pre-bring-up filler sample */

static int fails;

static void check_int(const char *what, int got, int want)
{
    if (got != want) {
        printf("  FAIL %-46s got %d want %d\n", what, got, want);
        fails++;
    } else {
        printf("  ok   %-46s %d\n", what, got);
    }
}

static void check_true(const char *what, int cond)
{
    check_int(what, cond ? 1 : 0, 1);
}

/* Read a whole file into a uint16 buffer.  Returns NULL on any error. */
static uint16_t *load(const char *path, size_t *n_samples)
{
    FILE *f = fopen(path, "rb");
    long sz;
    uint16_t *buf;

    if (!f) { perror(path); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
        fprintf(stderr, "%s: cannot size file\n", path);
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = malloc(sz ? (size_t)sz : 1);
    if (!buf) { perror("malloc"); fclose(f); return NULL; }
    if (sz && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "%s: short read\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *n_samples = (size_t)sz / 2;
    return buf;
}

static uint16_t *fill(size_t n_samples, uint16_t value)
{
    uint16_t *b = malloc(n_samples * sizeof(uint16_t));
    if (b)
        for (size_t i = 0; i < n_samples; i++)
            b[i] = value;
    return b;
}

/* ------------------------------------------------------- mode 1000, real */

static void test_mode1000_live(const char *path)
{
    size_t n = 0;
    uint16_t *raw = load(path, &n);
    float *lut = malloc(LUT_N * sizeof(float));
    float *out = NULL;
    dyt_pipeline_t p;
    int bad = 0, i;

    printf("\n-- mode 1000, real frame (%s) --\n", path);
    if (!raw || !lut) { printf("  FAIL cannot load fixture\n"); fails++; goto out; }

    dyt_pipeline_init(&p, DYT_MODE_1000, 25.0f, 0x82, 0, lut);
    check_int("resolve rc", dyt_pipeline_resolve(&p, 256, n), 0);
    check_int("width",      p.width,    256);
    check_int("active",     p.active,   192);
    check_int("total",      p.total,    192);   /* no reference band */
    check_int("rec_base",   p.rec_base, 0);
    check_int("n_pix",      p.n_pix,    256 * 192);
    check_int("out_n",      p.out_n,    256 * 192);   /* no 10-float header */
    check_int("LUT not built", p.lut_built, 0);       /* mode 1000 never needs it */

    out = malloc((size_t)p.out_n * sizeof(float));
    if (!out) { printf("  FAIL out of memory\n"); fails++; goto out; }

    check_int("frame rc", dyt_pipeline_frame(&p, raw, n, out), 0);
    check_int("LUT still not built", p.lut_built, 0);

    /* The whole point: every sample is exactly raw/64 - 273.15. */
    for (i = 0; i < p.n_pix; i++) {
        float want = (float)raw[i] / 64.0f - 273.15f;
        if (out[i] != want) {
            if (!bad)
                printf("  FAIL out[%d]=%.6f want %.6f (raw=0x%04x)\n",
                       i, out[i], want, raw[i]);
            bad++;
        }
    }
    check_int("samples == raw/64-273.15", bad, 0);

    /* The fixture must be real data, not the pre-bring-up filler — a fixture
     * that silently regressed to the filler would make the test above pass
     * vacuously. */
    {
        int filler = 1;
        for (size_t k = 0; k < n; k++)
            if (raw[k] != PLACEHOLDER) { filler = 0; break; }
        check_int("fixture is real data (not filler)", filler, 0);
    }

    /* And the temperatures land in a physical range (product spec:
     * -15..+600 C), so a units/sign error cannot slip through. */
    {
        float lo = out[0], hi = out[0];
        for (i = 1; i < p.n_pix; i++) {
            if (out[i] < lo) lo = out[i];
            if (out[i] > hi) hi = out[i];
        }
        printf("       frame range: %.2f .. %.2f C\n", lo, hi);
        check_true("range within -15..600 C", lo > -15.0f && hi < 600.0f);
        check_true("range is plausible (10..60 C)",
                   lo > 10.0f && hi < 60.0f);
    }

out:
    free(out);
    free(lut);
    free(raw);
}

/* ------------------------------------------- mode 1000, filler (no sentinel) */

static void test_mode1000_filler(void)
{
    const size_t n = 256u * 192u;
    uint16_t *filler = fill(n, PLACEHOLDER);
    float *lut = malloc(LUT_N * sizeof(float));
    float *out = malloc(n * sizeof(float));
    dyt_pipeline_t p;
    int i, bad = 0;

    printf("\n-- mode 1000, all-0x8000 filler --\n");
    if (!filler || !lut || !out) { printf("  FAIL out of memory\n"); fails++; goto out; }

    dyt_pipeline_init(&p, DYT_MODE_1000, 25.0f, 0x82, 0, lut);
    if (dyt_pipeline_resolve(&p, 256, n) != 0) {
        printf("  FAIL resolve\n"); fails++; goto out;
    }

    /* Mode 1000 has no 0x4000 sentinel, so the filler is not rejected — it
     * decodes to a constant 238.85 C.  This characterises the behaviour and
     * guards against someone applying the 0x44c gate to mode 1000, which
     * would silently drop every frame in a scene above -17 C. */
    check_int("filler rc", dyt_pipeline_frame(&p, filler, n, out), 0);
    check_int("LUT not built", p.lut_built, 0);

    for (i = 0; i < (int)n; i++)
        if (out[i] != (float)PLACEHOLDER / 64.0f - 273.15f) bad++;
    check_int("filler decodes to a constant", bad, 0);
    printf("       filler temperature: %.2f C\n", out[0]);

out:
    free(out);
    free(lut);
    free(filler);
}

/* ------------------------------------------------ mode 0x44c, LUT policy */

static void test_mode44c_lut_policy(const char *path)
{
    size_t n = 0;
    uint16_t *real = load(path, &n);
    uint16_t *filler = NULL;
    float *lut = malloc(LUT_N * sizeof(float));
    float *out = NULL;
    dyt_pipeline_t p;
    frame_t ft;

    printf("\n-- mode 0x44c, LUT policy (%s) --\n", path);
    if (!real || !lut) { printf("  FAIL cannot load fixture\n"); fails++; goto out; }
    if (n != 256u * 196u) {
        printf("  FAIL fixture is %zu samples, expected %u\n",
               n, 256u * 196u);
        fails++;
        goto out;
    }

    dyt_pipeline_init(&p, DYT_MODE_44C, 25.0f, 0x82, 0, lut);
    check_int("resolve rc", dyt_pipeline_resolve(&p, 256, n), 0);
    check_int("width",      p.width,    256);
    check_int("active",     p.active,   192);
    check_int("total",      p.total,    196);      /* active + 4 ref rows */
    check_int("rec_base",   p.rec_base, 0x200);
    check_int("n_pix",      p.n_pix,    256 * 192);
    check_int("out_n",      p.out_n,    10 + 256 * 192);  /* 10-float header */

    out = malloc((size_t)p.out_n * sizeof(float));
    if (!out) { printf("  FAIL out of memory\n"); fails++; goto out; }

    /* The filler: rejected by the validity gate, and the LUT must stay
     * unbuilt.  This is the regression — the old code built the LUT from
     * this very frame. */
    filler = fill(n, PLACEHOLDER);
    if (!filler) { printf("  FAIL out of memory\n"); fails++; goto out; }

    ft.width = 256; ft.total_height = 196; ft.rec_base = 0x200; ft.raw = filler;
    check_int("filler is_valid", dyt_frame_is_valid(&ft), 0);
    check_int("filler rc", dyt_pipeline_frame(&p, filler, n, out), -1);
    check_int("filler left LUT unbuilt", p.lut_built, 0);

    /* A second filler frame must still not build it. */
    check_int("filler rc (again)", dyt_pipeline_frame(&p, filler, n, out), -1);
    check_int("LUT still unbuilt", p.lut_built, 0);

    /* The first real frame builds the LUT and converts. */
    ft.raw = real;
    check_int("real is_valid", dyt_frame_is_valid(&ft), 1);
    check_int("real rc", dyt_pipeline_frame(&p, real, n, out), 0);
    check_int("real built LUT", p.lut_built, 1);

    /* Once built, a later filler frame is still skipped by the gate but the
     * LUT is no longer rebuilt. */
    check_int("filler after real rc", dyt_pipeline_frame(&p, filler, n, out), -1);
    check_int("LUT unchanged", p.lut_built, 1);

out:
    free(out);
    free(lut);
    free(filler);
    free(real);
}

/* --------------------------------------------------------------- geometry */

static void test_resolve_rejects(void)
{
    dyt_pipeline_t p;
    float lut[LUT_N];

    printf("\n-- resolve rejects malformed geometry --\n");

    dyt_pipeline_init(&p, DYT_MODE_1000, 25.0f, 0x82, 0, lut);
    /* Mode 1000 is width-agnostic: no reference band, so no per-width
     * geometry (scale/rec_base) is needed and any whole-row payload
     * resolves.  frame_test covers the same property via dyt_frame_resolve. */
    check_int("1000 width-agnostic 320", dyt_pipeline_resolve(&p, 320, 320u * 244u), 0);
    check_int("1000 partial row",       dyt_pipeline_resolve(&p, 256, 256u * 192u - 1u), -1);
    check_int("1000 zero samples",      dyt_pipeline_resolve(&p, 256, 0), -1);

    dyt_pipeline_init(&p, DYT_MODE_44C, 25.0f, 0x82, 0, lut);
    check_int("0x44c unknown width 320",
              dyt_pipeline_resolve(&p, 320, 320u * 244u), -1);
    check_int("0x44c missing ref band",
              dyt_pipeline_resolve(&p, 256, 256u * 192u), -1);

    /* Feeding a frame before resolve must be refused, not read past the
     * (unallocated) geometry. */
    dyt_pipeline_init(&p, DYT_MODE_1000, 25.0f, 0x82, 0, lut);
    {
        uint16_t one = 0;
        float o = 0;
        check_int("frame before resolve", dyt_pipeline_frame(&p, &one, 1, &o), -1);
    }
}

int main(int argc, char **argv)
{
    const char *f1000 = argc > 1 ? argv[1] : "testdata/mode1000_256x192.raw";
    const char *f44c  = argc > 2 ? argv[2]
                                 : "tools/thermometry_diff/out/256/in_frame.bin";

    printf("=== pipeline_test (live capture pipeline) ===\n");

    test_mode1000_live(f1000);
    test_mode1000_filler();
    test_mode44c_lut_policy(f44c);
    test_resolve_rejects();

    printf("\n=== %s ===\n", fails ? "FAIL" : "ALL PASS");
    return fails ? 1 : 0;
}
