/*
 * mnn_test.c — unit tests for mnn.c (the Phase 8 super-resolution seam).
 *
 * Four things are worth guarding:
 *
 *  1. **The shape contract.** It is derived from the vendor's disassembly and
 *     the app's buffer arithmetic (models/README.md), so it must not drift
 *     silently.  The strongest single assertion is that the contract's
 *     out_bytes equals the literal 393216 the app allocates.
 *
 *  2. **The validator.** Its whole job is to tell a correctly decrypted model
 *     from ciphertext or a truncated file, so the negative cases matter more
 *     than the positive one: a wrong-key decrypt must be rejected.  XORing the
 *     file's first word stands in for "wrong key", because that is exactly the
 *     failure a wrong key produces — a random root offset.
 *
 *  3. **The refusal.** With no runtime linked, the upscale must report
 *     unavailable and write nothing.  A stub that returned a plausible-looking
 *     frame would be far worse than one that refuses.  This test is therefore
 *     compiled only when the build has no MNN runtime; with a runtime present
 *     the differential below takes its place.
 *
 *  4. **The differential.** With a runtime linked, upscale the frozen input
 *     plane in tools/mnn_diff/out/ and compare against the vendor's own
 *     mnn_run_2 output for the same plane.  This is the test that catches a
 *     layout regression: the session tensor is NC4HW4, so writing samples
 *     straight into host<float>() produces garbage with a peak error of 255,
 *     while the correct path agrees with the vendor to within one LSB.  A
 *     constant plane would not catch it — see src/mnn_runtime.cpp.
 *
 * 1 and 2 need no runtime and always run.  3 and 4 are mutually exclusive.
 *
 * build:  via the Makefile (make check)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mnn.h"

/* Where tools/mnn_diff/ froze the vendor's answer.  Overridable so the test can
 * be pointed at a different reference tree. */
#define REF_DIR_DEFAULT "tools/mnn_diff/out"

static int fails;

static void ok(const char *what)
{
    printf("  ok   %s\n", what);
}

static void fail(const char *what, const char *detail)
{
    printf("  FAIL %-46s %s\n", what, detail);
    fails++;
}

static void check_int(const char *what, long got, long want)
{
    char buf[128];

    if (got == want) {
        ok(what);
        return;
    }
    snprintf(buf, sizeof buf, "got %ld, want %ld", got, want);
    fail(what, buf);
}

/* Read a whole file.  Returns NULL on failure (message on stderr). */
static uint8_t *slurp(const char *path, size_t *n_out)
{
    FILE    *f = fopen(path, "rb");
    long     sz;
    uint8_t *buf;

    if (!f) {
        perror(path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "mnn_test: cannot size %s\n", path);
        fclose(f);
        return NULL;
    }
    buf = malloc(sz ? (size_t)sz : 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (sz && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "mnn_test: short read on %s\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *n_out = (size_t)sz;
    return buf;
}

static void test_contract(void)
{
    const dyt_mnn_contract_t *c = dyt_mnn_contract();

    printf("-- contract --\n");

    if (!c) {
        fail("dyt_mnn_contract() is never NULL", "returned NULL");
        return;
    }
    check_int("input is 256x192",        c->in_w * c->in_h, 256 * 192);
    check_int("output is 512x384",       c->out_w * c->out_h, 512 * 384);
    check_int("the factor is 2",         c->factor, 2);
    check_int("out_w is in_w * factor",  c->out_w, c->in_w * c->factor);
    check_int("out_h is in_h * factor",  c->out_h, c->in_h * c->factor);
    check_int("in_samples",              c->in_samples, 256 * 192);
    check_int("out_samples",             c->out_samples, 512 * 384);

    /* The load-bearing one: the app allocates exactly this many bytes
     * (AbstractUVCCameraHandler: new byte[393216]). */
    check_int("out_bytes is the app's 393216", c->out_bytes, 393216);
    check_int("in_bytes",                      c->in_bytes, 256 * 192 * 2);
    check_int("out_bytes is out_samples * 2",  c->out_bytes, c->out_samples * 2);
    check_int("in_bytes is in_samples * 2",    c->in_bytes, c->in_samples * 2);
}

static void test_validator(const uint8_t *model, size_t n)
{
    uint8_t *copy;
    int      rc;

    printf("-- the extracted model --\n");

    if (!model) {
        fail("models/zoom2.mnn loads", "could not read the file");
        return;
    }

    check_int("the model is 12928 bytes", (long)n, 12928);

    rc = dyt_mnn_model_check(model, n);
    check_int("the model validates", rc, 0);

    /* A wrong decryption key yields ciphertext, whose first word is a random
     * root offset.  That is the case this validator exists to catch. */
    copy = malloc(n);
    if (!copy) {
        fail("wrong-key detection", "out of memory");
        return;
    }
    memcpy(copy, model, n);
    copy[0] ^= 0xFF;
    copy[1] ^= 0xFF;
    copy[2] ^= 0xFF;
    copy[3] ^= 0xFF;
    rc = dyt_mnn_model_check(copy, n);
    if (rc != 0)
        ok("a wrong-key (ciphertext) image is rejected");
    else
        fail("a wrong-key image is rejected", "was accepted");

    /* A truncated file must not pass either. */
    rc = dyt_mnn_model_check(model, 4);
    check_int("a 4-byte buffer is refused (-2)", rc, -2);

    rc = dyt_mnn_model_check(NULL, n);
    check_int("a NULL buffer is refused (-2)", rc, -2);

    free(copy);
}

/* Bad arguments are -1, distinct from "unavailable".  These hold whether or not
 * a runtime is linked, because the checks live in the mnn.c wrapper. */
static void test_bad_args(void)
{
    const dyt_mnn_contract_t *c = dyt_mnn_contract();
    uint8_t *in, *out;
    int      out_n = -1;

    printf("-- argument checking --\n");

    in  = calloc(1, (size_t)c->in_bytes);
    out = malloc((size_t)c->out_bytes);
    if (!in || !out) {
        fail("argument-check buffers", "out of memory");
        free(in);
        free(out);
        return;
    }

    check_int("a short output buffer is -1",
              dyt_mnn_zoom2(in, out, c->out_bytes - 1, &out_n), -1);
    check_int("a NULL input is -1",
              dyt_mnn_zoom2(NULL, out, c->out_bytes, &out_n), -1);
    check_int("a NULL output is -1",
              dyt_mnn_zoom2(in, NULL, c->out_bytes, &out_n), -1);

    free(in);
    free(out);
}

#ifndef DYT_HAVE_MNN

static void test_refusal(void)
{
    const dyt_mnn_contract_t *c = dyt_mnn_contract();
    uint8_t *in, *out;
    int      out_n = -1, rc, i, untouched;

    printf("-- gating (no runtime linked) --\n");

    check_int("dyt_mnn_available() is 0", dyt_mnn_available(), 0);

    rc = dyt_mnn_load("models/zoom2.mnn");
    check_int("dyt_mnn_load reports unavailable", rc, DYT_MNN_UNAVAILABLE);

    in  = calloc(1, (size_t)c->in_bytes);
    out = malloc((size_t)c->out_bytes);
    if (!in || !out) {
        fail("gating buffers", "out of memory");
        free(in);
        free(out);
        return;
    }
    memset(out, 0xA5, (size_t)c->out_bytes);

    rc = dyt_mnn_zoom2(in, out, c->out_bytes, &out_n);
    check_int("zoom2 reports unavailable", rc, DYT_MNN_UNAVAILABLE);

    /* The refusal must not have written anything. */
    untouched = 1;
    for (i = 0; i < c->out_bytes; i++)
        if (out[i] != 0xA5) {
            untouched = 0;
            break;
        }
    if (untouched)
        ok("the refusal wrote nothing to the output buffer");
    else
        fail("the refusal writes nothing", "the buffer was modified");

    free(in);
    free(out);
}

#else /* DYT_HAVE_MNN */

/*
 * The differential.  The reference is the vendor's own mnn_run_2 on the same
 * plane (tools/mnn_diff/out/vendor_out.raw, provenance in meta.txt).
 *
 * What this can and cannot catch:
 *
 *   - A layout regression is caught loudly.  Feeding the plane the naive way
 *     (writing samples straight into the NC4HW4 session tensor) misses 188657
 *     of 196608 samples with a peak error of 255, so the <= 1 bound fails.
 *   - A wrong normalisation, bias or model is caught: any of them shifts the
 *     output far more than one LSB.
 *   - It does NOT pin the last bit.  The vendor ran MNN 2.5.0 and this port
 *     runs upstream MNN, and the two differ by one LSB on a minority of
 *     samples.  The bound is deliberately 1 rather than 0 for that reason; see
 *     tools/mnn_diff/out/meta.txt.
 */
static void test_differential(const char *model_path, const char *refdir)
{
    const dyt_mnn_contract_t *c = dyt_mnn_contract();
    char     path[512];
    uint8_t *plane = NULL, *ref = NULL, *out = NULL;
    size_t   plane_n = 0, ref_n = 0;
    uint16_t *p, *r, *o;
    int      rc, out_n = -1, i;
    long     exact = 0, maxd = 0;

    printf("-- differential vs the vendor's mnn_run_2 --\n");

    if (!model_path) {
        fail("the differential runs", "no model file");
        return;
    }

    snprintf(path, sizeof path, "%s/in_plane.raw", refdir);
    plane = slurp(path, &plane_n);
    snprintf(path, sizeof path, "%s/vendor_out.raw", refdir);
    ref = slurp(path, &ref_n);

    if (!plane || !ref) {
        fail("the frozen vendor reference loads",
             "tools/mnn_diff/out/ is missing — see tools/mnn_diff/run.sh");
        free(plane);
        free(ref);
        return;
    }
    if (plane_n != (size_t)c->in_bytes || ref_n != (size_t)c->out_bytes) {
        fail("the reference files are the right size",
             "in_plane.raw must be 98304 B, vendor_out.raw 393216 B");
        free(plane);
        free(ref);
        return;
    }

    rc = dyt_mnn_load(model_path);
    check_int("dyt_mnn_load succeeds", rc, 0);
    if (rc != 0) {
        free(plane);
        free(ref);
        return;
    }
    check_int("dyt_mnn_available() is 1", dyt_mnn_available(), 1);

    out = malloc((size_t)c->out_bytes);
    if (!out) {
        fail("the differential runs", "out of memory");
        free(plane);
        free(ref);
        return;
    }

    rc = dyt_mnn_zoom2(plane, out, c->out_bytes, &out_n);
    check_int("dyt_mnn_zoom2 succeeds", rc, 0);
    check_int("zoom2 reports the full frame", out_n, c->out_bytes);

    p = (uint16_t *)plane;
    r = (uint16_t *)ref;
    o = (uint16_t *)out;
    for (i = 0; i < c->out_samples; i++) {
        long d = (long)o[i] - (long)r[i];
        if (d < 0)
            d = -d;
        if (d == 0)
            exact++;
        if (d > maxd)
            maxd = d;
    }

    printf("       %ld/%d bit-exact, %ld differ, max|delta| %ld\n",
           exact, c->out_samples, c->out_samples - exact, maxd);

    if (maxd <= 1)
        ok("every sample is within one LSB of the vendor");
    else
        fail("every sample is within one LSB of the vendor",
             "a layout or convention regression looks like this");

    /* Guards against a systematic shift that still lands within one LSB. */
    if (exact * 100 >= (long)c->out_samples * 85)
        ok("at least 85% of samples are bit-exact");
    else
        fail("at least 85% of samples are bit-exact",
             "the two runtimes have drifted further apart than expected");

    /* A wrong normalisation or bias shifts the whole frame, which shows up in
     * the means even when the per-sample bound happens to hold. */
    {
        double sp = 0, so = 0, sr = 0;

        for (i = 0; i < c->in_samples; i++)
            sp += p[i];
        for (i = 0; i < c->out_samples; i++) {
            so += o[i];
            sr += r[i];
        }
        printf("       means: input %.2f, ours %.2f, vendor %.2f\n",
               sp / c->in_samples, so / c->out_samples, sr / c->out_samples);

        if ((so - sr) / c->out_samples < 0.5 &&
            (sr - so) / c->out_samples < 0.5)
            ok("the output mean matches the vendor to within half a step");
        else
            fail("the output mean matches the vendor",
                 "a normalisation or bias change looks like this");
    }

    dyt_mnn_unload();
    check_int("dyt_mnn_available() is 0 after unload", dyt_mnn_available(), 0);

    free(plane);
    free(ref);
    free(out);
}

#endif /* DYT_HAVE_MNN */

int main(int argc, char **argv)
{
    const char *path   = argc > 1 ? argv[1] : "models/zoom2.mnn";
    const char *refdir = argc > 2 ? argv[2] : REF_DIR_DEFAULT;
    uint8_t    *model = NULL;
    size_t      n = 0;

    printf("== mnn (super-resolution seam) ==\n");

    model = slurp(path, &n);
    if (!model)
        fprintf(stderr, "mnn_test: cannot read %s — run from the port root\n",
                path);

    test_contract();
    test_validator(model, n);
    test_bad_args();
#ifdef DYT_HAVE_MNN
    test_differential(model ? path : NULL, refdir);
#else
    test_refusal();
#endif

    free(model);

    printf("=== %s ===\n", fails ? "FAIL" : "ALL PASS");
    return fails ? 1 : 0;
}
