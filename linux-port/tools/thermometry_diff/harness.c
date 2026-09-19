/*
 * harness.c — drive the vendor's real libthermometry.so and capture its output
 * as ground truth for the Linux port.
 *
 * Why this exists
 * ---------------
 * `thermometryT4Line` and `thermometrySearch` were recovered by decompilation.
 * Rather than trusting that reading, we call the actual vendor binary and dump
 * exactly what it computes.  A portable C port can then be diffed against these
 * outputs byte-for-byte.
 *
 * The inputs do not need to be physically realistic — only *identical* for both
 * implementations.  We therefore synthesise a deterministic frame and
 * calibration record, and compare raw bytes (not floats) so that NaN payloads
 * and signed zeros are compared exactly too.
 *
 * This also settles the function signatures empirically: if the recovered
 * argument order were wrong, the LUT would not come out monotonic.
 *
 * usage: harness <outdir> [width] [total_height] [rec_base]
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LUT_N 16384
#define REF_ROWS 4

/* ------------------------------------------------------------------ types --
 * Signatures as recovered from the call sites in libUVCCamera.so.
 * `thermometryT4Line` takes a float first argument (passed in v0 on AArch64),
 * then integer/pointer arguments in x0..x7 and on the stack. */
/* param_14 is fix_mode (0x78 enables GetFix), NOT distance — see RE Docs
 * §5.8.  The distance the model actually uses is read from the frame at
 * rec+0x112 (u16) inside the library.  We pass fix_mode here. */
typedef void (*t4line_fn)(float t_amb_in, int width, int height,
                          float *lut, void *ref_band,
                          float *amb, float *corr, float *refl,
                          float *air, float *humi, float *emiss,
                          uint16_t *user_area, int sensor_mode, int fix_mode);

/* thermometrySearch returns void — verified at 0x1d38..0x1d44 (no x0 set
 * before ret).  Results land in `out` (10 header floats + width*(h-4) pixels). */
typedef void (*search_fn)(int width, int height, float *lut,
                          uint16_t *raw, float *out,
                          void *unused, int ref_rows);

/* ------------------------------------------------------- deterministic rng -- */

static uint32_t rng_state;

static uint32_t rng_next(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

/* uniform in [lo, hi) with 24 bits of mantissa resolution */
static float rng_float(float lo, float hi) __attribute__((unused));
static float rng_float(float lo, float hi)
{
    double u = (double)(rng_next() >> 8) / 16777216.0;
    return (float)(lo + (hi - lo) * u);
}

/* --------------------------------------------------- env-driven parameters --
 * The synthetic calibration coefficients have to sit in the range the model
 * expects, or `sqrt()` goes negative and most of the LUT becomes NaN.  Rather
 * than guess, every input is overridable so a sweep can find a valid set. */
static float envf(const char *name, float dflt)
{
    const char *s = getenv(name);
    return s ? strtof(s, NULL) : dflt;
}

static unsigned long envu(const char *name, unsigned long dflt)
{
    const char *s = getenv(name);
    return s ? strtoul(s, NULL, 0) : dflt;
}

static void wr(const char *dir, const char *name, const void *p, size_t n)
{
    char path[1024];
    FILE *f;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    if (n && fwrite(p, 1, n, f) != n) {
        perror(path);
        exit(1);
    }
    fclose(f);
    printf("  wrote %-18s %8zu bytes\n", name, n);
}

int main(int argc, char **argv)
{
    const char *outdir;
    int width, height, rec_base;
    int active_rows, n_pixels, frame_bytes;
    uint8_t *frame;
    uint16_t *raw;
    uint8_t *ref_band;
    uint8_t *rec;
    float *lut;
    float amb = 0, corr = 0, refl = 0, air = 0, humi = 0, emiss = 0;
    float *out;
    int out_n;
    void *h;
    t4line_fn t4line;
    search_fn search;
    FILE *meta;
    char path[1024];

    if (argc < 2) {
        fprintf(stderr, "usage: %s <outdir> [width] [total_height] [rec_base]\n", argv[0]);
        return 2;
    }
    outdir   = argv[1];
    width    = argc > 2 ? atoi(argv[2]) : 256;
    height   = argc > 3 ? atoi(argv[3]) : 196;      /* 192 active + 4 reference */
    rec_base = argc > 4 ? (int)strtol(argv[4], NULL, 0) : 0x200;

    active_rows = height - REF_ROWS;
    n_pixels    = width * active_rows;
    frame_bytes = width * height * 2;

    printf("=== harness: %dx%d (active %d), rec_base=%#x, frame=%d bytes ===\n",
           width, height, active_rows, rec_base, frame_bytes);

    /* ---------------------------------------------------- load the library -- */
    h = dlopen("./build/libthermometry.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    t4line = (t4line_fn)dlsym(h, "thermometryT4Line");
    search = (search_fn)dlsym(h, "thermometrySearch");
    if (!t4line || !search) {
        fprintf(stderr, "dlsym failed: t4line=%p search=%p\n", (void *)t4line, (void *)search);
        return 1;
    }

    /* ------------------------------------------------------ build the frame -- */
    frame = calloc(1, frame_bytes);
    if (!frame) { perror("calloc"); return 1; }
    raw = (uint16_t *)frame;

    ref_band = frame + (size_t)width * active_rows * 2;
    rec      = ref_band + rec_base;

    rng_state = 0x12345678u;                        /* fixed seed */

    /* Active image: deterministic 14-bit values. */
    for (int i = 0; i < n_pixels; i++)
        raw[i] = (uint16_t)(rng_next() & 0x3FFF);

    /* Reference band: leave mostly zero, then place the calibration record. */
    {
        /* rec+0x00: lower bound of the LUT sweep.
         * ref_band+2: reference ADC sample -> ambient estimate.
         * rec+0x02: Kelvin*10 code -> adds (T_code/10 - 273.15) to t_amb_in. */
        uint16_t lo     = (uint16_t)envu("DYT_LO", 0x2000);
        uint16_t ambadv = (uint16_t)envu("DYT_AMBADV", 0x21A9);   /* -> amb = 20.0 */
        uint16_t tcode  = (uint16_t)envu("DYT_TCODE", 2932);      /* -> +20.0 C */

        float a = envf("DYT_A", 0.001f);
        float b = envf("DYT_B", 0.05f);
        float c = envf("DYT_C", 0.0f);
        float d = envf("DYT_D", 0.0f);
        /* e>0 makes q2>0 so the LUT is non-constant and (with humi as a
         * fraction) monotonically increasing — the physically correct shape. */
        float e = envf("DYT_E", 0.0001f);

        float f_corr  = envf("DYT_CORR",  -1.583236f);
        float f_refl  = envf("DYT_REFL",  29.798769f);
        float f_air   = envf("DYT_AIR",   27.925001f);
        /* humi is a 0..1 fraction (relative humidity), NOT percent.  The
         * formula uses it raw: tau = humi * exp(poly(air)).  With humi>1 the
         * atmospheric transmittance tau_eff goes negative, flipping the sign
         * of GetTempEvn and inverting the LUT.  A real device stores ~0.5. */
        float f_humi  = envf("DYT_HUMI",  0.5f);
        float f_emiss = envf("DYT_EMISS", 0.901972f);
        uint16_t ua0 = (uint16_t)envu("DYT_UA0", 0x1A3);
        uint16_t ua1 = (uint16_t)envu("DYT_UA1", 0xFAA6);

        memcpy(rec + 0x00, &lo,     2);
        memcpy(rec + 0x02, &tcode,  2);
        memcpy(rec + 0x06, &a, 4);
        memcpy(rec + 0x0a, &b, 4);
        memcpy(rec + 0x0e, &c, 4);
        memcpy(rec + 0x12, &d, 4);
        memcpy(rec + 0x16, &e, 4);
        memcpy(rec + 0xfe, &f_corr,  4);
        memcpy(rec + 0x102, &f_refl, 4);
        memcpy(rec + 0x106, &f_air,  4);
        memcpy(rec + 0x10a, &f_humi, 4);
        memcpy(rec + 0x10e, &f_emiss, 4);
        memcpy(rec + 0x112, &ua0, 2);
        memcpy(rec + 0x114, &ua1, 2);
        for (int i = 0; i < 32; i++) rec[0x40 + i] = (uint8_t)rng_next();
        for (int i = 0; i < 16; i++) rec[0x162 + i] = (uint8_t)rng_next();

        /* The absolute userArea[1] sample drives the ambient estimate. */
        memcpy(ref_band + 2, &ambadv, 2);

        printf("  record: a=%.6g b=%.6g c=%.6g d=%.6g e=%.6g\n", a, b, c, d, e);
        printf("          corr=%.4f refl=%.4f air=%.4f humi=%.4f emiss=%.4f\n",
               f_corr, f_refl, f_air, f_humi, f_emiss);
        printf("          lo=%u ambadv=%u (0x%04x) tcode=%u\n",
               lo, ambadv, ambadv, tcode);
    }

    /* --------------------------------------------------- call thermometryT4Line -- */
    lut = calloc(LUT_N, sizeof(float));
    if (!lut) { perror("calloc"); return 1; }

    printf("\n  calling thermometryT4Line(t_amb=%.4g, w=%d, h=%d, ...)\n",
           (double)envf("DYT_TAMB", 25.0f), width, height);
    /* fix_mode: 0x78 enables GetFix's width-dependent offset; any other value
     * disables it (GetFix returns 0).  Default 400 keeps GetFix off, matching
     * the prior runs.  Set DYT_FIXMODE=0x78 to exercise that branch too. */
    t4line(envf("DYT_TAMB", 25.0f), width, height, lut, ref_band,
           &amb, &corr, &refl, &air, &humi, &emiss,
           (uint16_t *)ref_band, (int)envu("DYT_MODE", 0x82),
           (int)envu("DYT_FIXMODE", envu("DYT_DIST", 400)));

    printf("  outputs: amb=%.6f corr=%.6f refl=%.6f air=%.6f humi=%.6f emiss=%.6f\n",
           amb, corr, refl, air, humi, emiss);

    /* Report the LUT's shape — this is the empirical signature check. */
    int lut_nan = 0, lut_neg = 0, lut_inc = 1;
    {
        float prev = lut[0];
        for (int i = 0; i < LUT_N; i++) {
            if (lut[i] != lut[i]) lut_nan++;
            if (lut[i] < 0.0f) lut_neg++;
            if (i && lut[i] < prev) lut_inc = 0;
            prev = lut[i];
        }
        printf("  LUT: [0]=%.6f [%d]=%.6f  monotonic_increasing=%s  nan=%d  negative=%d\n",
               lut[0], LUT_N - 1, lut[LUT_N - 1], lut_inc ? "YES" : "no", lut_nan, lut_neg);
    }

    /* -------------------------------------------------- call thermometrySearch -- */
    out_n = n_pixels + 10;
    out = calloc((size_t)out_n, sizeof(float));
    if (!out) { perror("calloc"); return 1; }

    printf("\n  calling thermometrySearch(w=%d, h=%d, lut, raw, out, 400, %d)\n",
           width, height, REF_ROWS);
    search(width, height, lut, raw, out, (void *)400, REF_ROWS);
    printf("  returned (void)\n");

    int out_nan = 0;
    {
        for (int i = 0; i < out_n; i++)
            if (out[i] != out[i]) out_nan++;
        printf("  out[0]=%.4f out[1..6]=%.1f %.1f %.4f %.1f %.1f %.4f  out[10]=%.4f out[%d]=%.4f  nan=%d\n",
               out[0], out[1], out[2], out[3], out[4], out[5], out[6],
               out[10], out_n - 1, out[out_n - 1], out_nan);
    }

    /* Machine-readable one-liner, for sweeps and for the diff script. */
    printf("\nSUMMARY width=%d height=%d rec_base=%#x lut_nan=%d lut_neg=%d "
           "lut_monotonic=%d lut0=%.9g lut_last=%.9g amb=%.9g out_nan=%d\n",
           width, height, rec_base, lut_nan, lut_neg, lut_inc,
           lut[0], lut[LUT_N - 1], amb, out_nan);

    /* --------------------------------------------------------------- dump -- */
    printf("\n  dumping ground truth to %s/\n", outdir);
    wr(outdir, "in_frame.bin",  frame, (size_t)frame_bytes);
    wr(outdir, "in_lut_in.bin", lut,   LUT_N * sizeof(float));   /* post-call LUT */
    wr(outdir, "out_image.bin", out,   (size_t)out_n * sizeof(float));

    snprintf(path, sizeof path, "%s/meta.txt", outdir);
    meta = fopen(path, "w");
    if (meta) {
        /* Inputs the port must replay exactly to reproduce the ground truth. */
        float t_amb   = envf("DYT_TAMB", 25.0f);
        int   mode    = (int)envu("DYT_MODE", 0x82);
        int   fixmode = (int)envu("DYT_FIXMODE", envu("DYT_DIST", 400));
        fprintf(meta, "width %d\n", width);
        fprintf(meta, "total_height %d\n", height);
        fprintf(meta, "active_rows %d\n", active_rows);
        fprintf(meta, "rec_base %#x\n", rec_base);
        fprintf(meta, "ref_band_offset %d\n", width * active_rows * 2);
        fprintf(meta, "n_pixels %d\n", n_pixels);
        fprintf(meta, "out_n %d\n", out_n);
        fprintf(meta, "t_amb %.9g\n", t_amb);
        fprintf(meta, "sensor_mode %d\n", mode);
        fprintf(meta, "fix_mode %d\n", fixmode);
        /* Outputs (for the port to sanity-check against). */
        fprintf(meta, "amb %.9g\ncorr %.9g\nrefl %.9g\nair %.9g\nhumi %.9g\nemiss %.9g\n",
                amb, corr, refl, air, humi, emiss);
        fprintf(meta, "lut0 %.9g\nlut_last %.9g\n", lut[0], lut[LUT_N - 1]);
        fclose(meta);
        printf("  wrote %-18s\n", "meta.txt");
    }

    return 0;
}
