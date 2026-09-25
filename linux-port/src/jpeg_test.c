/*
 * jpeg_test.c — unit tests for the JPEG codec behind jpeg.h.
 *
 * JPEG is lossy and the two backends (stb, libjpeg) do not produce identical
 * bytes, so nothing here pins the encoder's output.  What it does pin:
 *
 *  1. **The structure of a JPEG.**  A file that starts with FF D8 and ends
 *     with FF D9 is a JPEG whatever encoder made it.  The DYT container
 *     depends on that, because it splices its APP2 segments in by walking the
 *     segment chain — an encoder that emitted something else would be spliced
 *     in the wrong place.
 *
 *  2. **A round trip is close.**  Encoded then decoded, a smooth image must
 *     come back within a small tolerance.  This is the only thing that catches
 *     an encoder and decoder that are wrong *together* (e.g. a channel swap
 *     that both directions agree on).
 *
 *  3. **Bad input fails, it does not abort.**  libjpeg's stock error handler
 *     calls exit(); a corrupt file must return -1 instead.  The garbage-decode
 *     case below is what keeps that honest.
 *
 * build:  via the Makefile (make check)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpeg.h"

static int fails;

static void ok(const char *what) { printf("  ok   %s\n", what); }

static void fail(const char *what, const char *detail)
{
    printf("  FAIL %-46s %s\n", what, detail);
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

/* --------------------------------------------------------------- synthetic */

/* A smooth two-axis ramp with a few hard blocks.  The ramp catches banding and
 * DC offset; the blocks catch a codec that smears everything. */
static uint8_t *make_image(int w, int h)
{
    uint8_t *p = malloc((size_t)w * h * 3);
    int x, y;

    if (!p)
        return NULL;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint8_t *q = p + ((size_t)y * w + x) * 3;
            q[0] = (uint8_t)((x * 255) / (w > 1 ? w - 1 : 1));
            q[1] = (uint8_t)((y * 255) / (h > 1 ? h - 1 : 1));
            q[2] = (uint8_t)(((x + y) * 255) / ((w + h) > 2 ? w + h - 2 : 1));
        }
    }
    /* Two flat blocks, so a codec cannot pass by blurring uniformly. */
    for (y = h / 4; y < h / 2 && y < h; y++)
        for (x = w / 4; x < w / 2 && x < w; x++) {
            uint8_t *q = p + ((size_t)y * w + x) * 3;
            q[0] = 250; q[1] = 20; q[2] = 20;
        }
    for (y = h / 2; y < (3 * h) / 4 && y < h; y++)
        for (x = w / 2; x < (3 * w) / 4 && x < w; x++) {
            uint8_t *q = p + ((size_t)y * w + x) * 3;
            q[0] = 20; q[1] = 20; q[2] = 250;
        }
    return p;
}

static void test_backend(void)
{
    dyt_jpeg_backend_t b = dyt_jpeg_backend();
    const char        *n = dyt_jpeg_backend_name();

    printf("\n-- backend --\n");
    intcheck("a backend is selected", b != DYT_JPEG_NONE, 1);
    intcheck("name is non-empty", n && n[0] != '\0', 1);
    intcheck("name matches the enum",
             (b == DYT_JPEG_STB     && strcmp(n, "stb") == 0) ||
             (b == DYT_JPEG_LIBJPEG && strcmp(n, "libjpeg") == 0), 1);
    printf("       backend: %s\n", n ? n : "(null)");
}

static void test_roundtrip(int w, int h, int quality)
{
    uint8_t *src = make_image(w, h);
    uint8_t *jpg = NULL, *back = NULL;
    size_t   jlen = 0;
    int      bw = 0, bh = 0;
    long     sum = 0;
    int      i, n, worst = 0, far = 0;
    char     what[64];

    snprintf(what, sizeof what, "round trip %dx%d q%d", w, h, quality);
    printf("\n-- %s --\n", what);
    if (!src) { fail("allocate", "out of memory"); return; }

    if (dyt_jpeg_encode(src, w, h, quality, &jpg, &jlen) != 0) {
        fail("encode", "returned -1");
        free(src);
        return;
    }
    ok("encode");

    /* A JPEG file is a segment chain bracketed by SOI and EOI. */
    intcheck("starts with SOI (FF D8)",
             jlen >= 2 && jpg[0] == 0xFF && jpg[1] == 0xD8, 1);
    intcheck("ends with EOI (FF D9)",
             jlen >= 2 && jpg[jlen - 2] == 0xFF && jpg[jlen - 1] == 0xD9, 1);

    if (dyt_jpeg_decode(jpg, jlen, &back, &bw, &bh) != 0) {
        fail("decode", "returned -1");
        free(src); free(jpg);
        return;
    }
    intcheck("decoded width",  bw, w);
    intcheck("decoded height", bh, h);

    n = w * h * 3;
    for (i = 0; i < n; i++) {
        int d = (int)src[i] - (int)back[i];
        if (d < 0) d = -d;
        sum += d;
        if (d > worst) worst = d;
        if (d > 32)    far++;
    }
    printf("       mean |err| %.2f, worst %d, %d/%d px over 32\n",
           (double)sum / n, worst, far, n);

    intcheck("mean abs error is small", (int)((double)sum / n + 0.5) <= 8, 1);
    intcheck("almost no pixel is far off", far <= n / 50, 1);

    free(src);
    free(jpg);
    free(back);
}

static void test_deterministic(void)
{
    uint8_t *src = make_image(64, 48);
    uint8_t *a = NULL, *b = NULL;
    size_t   alen = 0, blen = 0;

    printf("\n-- encode is deterministic --\n");
    if (!src) { fail("allocate", "out of memory"); return; }

    if (dyt_jpeg_encode(src, 64, 48, 85, &a, &alen) != 0 ||
        dyt_jpeg_encode(src, 64, 48, 85, &b, &blen) != 0) {
        fail("encode twice", "returned -1");
        free(src); free(a); free(b);
        return;
    }

    intcheck("same length", (int)alen, (int)blen);
    intcheck("same bytes",
             alen == blen && memcmp(a, b, alen) == 0, 1);

    free(src); free(a); free(b);
}

static void test_quality_matters(void)
{
    uint8_t *src = make_image(128, 96);
    uint8_t *lo = NULL, *hi = NULL;
    size_t   llen = 0, hlen = 0;

    printf("\n-- quality changes the output --\n");
    if (!src) { fail("allocate", "out of memory"); return; }

    if (dyt_jpeg_encode(src, 128, 96, 30, &lo, &llen) != 0 ||
        dyt_jpeg_encode(src, 128, 96, 95, &hi, &hlen) != 0) {
        fail("encode at two qualities", "returned -1");
        free(src); free(lo); free(hi);
        return;
    }
    printf("       q30 %zu bytes, q95 %zu bytes\n", llen, hlen);
    intcheck("q95 is larger than q30", hlen > llen, 1);

    free(src); free(lo); free(hi);
}

static void test_reject(void)
{
    uint8_t  buf[64];
    uint8_t *out = NULL;
    size_t   olen = 0;
    int      w = 0, h = 0;

    printf("\n-- reject bad arguments --\n");

    intcheck("encode NULL rgb",   dyt_jpeg_encode(NULL, 8, 8, 85, &out, &olen), -1);
    intcheck("encode NULL out",   dyt_jpeg_encode(buf, 8, 8, 85, NULL, &olen), -1);
    intcheck("encode NULL len",   dyt_jpeg_encode(buf, 8, 8, 85, &out, NULL), -1);
    intcheck("encode zero width", dyt_jpeg_encode(buf, 0, 8, 85, &out, &olen), -1);
    intcheck("encode zero height",dyt_jpeg_encode(buf, 8, 0, 85, &out, &olen), -1);

    intcheck("decode NULL jpeg", dyt_jpeg_decode(NULL, 16, &out, &w, &h), -1);
    intcheck("decode NULL rgb",  dyt_jpeg_decode(buf, 16, NULL, &w, &h), -1);
    intcheck("decode zero len",  dyt_jpeg_decode(buf, 0, &out, &w, &h), -1);
}

/* Malformed input must return -1 rather than abort the process.  With libjpeg
 * these are exactly the paths that would exit() if the error handler were left
 * at its default, so this is the test that keeps the longjmp honest.
 *
 * A *truncated* but otherwise valid stream is deliberately not asserted on:
 * stb tolerates it and fills what it can, while libjpeg reports a premature
 * end.  Both are legitimate, so the two backends would disagree. */
static void test_garbage(void)
{
    uint8_t  junk[512];
    uint8_t *out = NULL;
    int      w = 0, h = 0, i;

    printf("\n-- malformed input fails cleanly --\n");

    for (i = 0; i < (int)sizeof junk; i++)
        junk[i] = (uint8_t)(i * 7 + 3);
    intcheck("random bytes", dyt_jpeg_decode(junk, sizeof junk, &out, &w, &h), -1);

    /* Looks like a JPEG — SOI, then noise where a segment should be. */
    junk[0] = 0xFF; junk[1] = 0xD8;
    intcheck("SOI then noise", dyt_jpeg_decode(junk, sizeof junk, &out, &w, &h), -1);

    /* A segment whose declared length runs off the end of the buffer. */
    junk[0] = 0xFF; junk[1] = 0xD8;
    junk[2] = 0xFF; junk[3] = 0xE0;
    junk[4] = 0xFF; junk[5] = 0xFF;
    intcheck("segment length past the end",
             dyt_jpeg_decode(junk, sizeof junk, &out, &w, &h), -1);

    /* Just a marker and nothing else. */
    junk[0] = 0xFF; junk[1] = 0xD8;
    intcheck("SOI alone", dyt_jpeg_decode(junk, 2, &out, &w, &h), -1);
}

static void test_tiny(void)
{
    uint8_t  px[3] = { 200, 100, 50 };
    uint8_t *jpg = NULL, *back = NULL;
    size_t   jlen = 0;
    int      w = 0, h = 0;

    printf("\n-- 1x1 --\n");

    if (dyt_jpeg_encode(px, 1, 1, 90, &jpg, &jlen) != 0) {
        fail("encode 1x1", "returned -1");
        return;
    }
    if (dyt_jpeg_decode(jpg, jlen, &back, &w, &h) != 0) {
        fail("decode 1x1", "returned -1");
        free(jpg);
        return;
    }
    intcheck("1x1 dims", w == 1 && h == 1, 1);
    free(jpg);
    free(back);
}

int main(void)
{
    printf("=== jpeg_test (JPEG codec) ===\n");

    test_backend();
    test_roundtrip(128, 96, 90);
    test_roundtrip(256, 192, 85);
    test_deterministic();
    test_quality_matters();
    test_reject();
    test_garbage();
    test_tiny();

    printf("\n=== %s ===\n", fails ? "FAIL" : "ALL PASS");
    return fails ? 1 : 0;
}
