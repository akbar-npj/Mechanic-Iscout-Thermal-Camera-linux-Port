/*
 * visible_test.c — unit tests for the visible-plane extraction (visible.c).
 *
 * The dual-half payload's top half is YUYV with neutral chroma, so its luma
 * is the *first* byte of each pair.  Two things are worth pinning hard:
 *
 *  1. Which byte is the luma.  A test that builds the plane through a
 *     uint16_t array and then checks the extraction would pass even if the
 *     code read the wrong byte of the pair, because it would be checking the
 *     same wrong assumption twice.  So the synthetic cases write the payload
 *     through an explicit byte view (`Y U Y V`) and compare against the Y
 *     bytes — that is the only way to catch an endianness or pair-offset bug.
 *
 *  2. That the real frozen fixture's two halves are distinguishable.  The
 *     top half must satisfy dyt_visible_is_grey() and the bottom half — the
 *     actual thermal plane — must not.  One predicate, opposite answers: a
 *     regression that read the wrong half would flip it.
 *
 * usage:  ./visible_test <dual-half.raw>
 * build:  via the Makefile (make check)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "visible.h"

#define FIX_W   256
#define FIX_ROWS 192        /* the fixture's per-half height */

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

static void rangecheck(const char *what, int got, int lo, int hi)
{
    if (got >= lo && got <= hi) {
        ok(what);
    } else {
        char d[96];
        snprintf(d, sizeof d, "got %d, want %d..%d", got, lo, hi);
        fail(what, d);
    }
}

/* --------------------------------------------- synthetic, byte-explicit YUYV */

/* Build a width*rows YUYV payload where pixel i has luma `lum(i)` and chroma
 * `chroma`.  Written as bytes so the layout is stated, not assumed. */
static uint16_t *make_yuyv(int width, int rows, int (*lum)(int), uint8_t chroma)
{
    uint16_t *p = malloc((size_t)width * rows * sizeof *p);
    uint8_t  *b = (uint8_t *)p;
    int       i, n = width * rows;

    if (!p)
        return NULL;
    for (i = 0; i < n; i++) {
        b[2 * i]     = (uint8_t)lum(i);
        b[2 * i + 1] = chroma;
    }
    return p;
}

static int ramp_lum(int i) { return i % 256; }

static void test_synthetic_roundtrip(void)
{
    const int w = 8, rows = 4;
    uint16_t *p = make_yuyv(w, rows, ramp_lum, DYT_VISIBLE_CHROMA_NEUTRAL);
    uint8_t   grey[8 * 4];
    int       i, bad = 0;

    printf("\n-- synthetic YUYV round-trip --\n");
    if (!p) { fail("allocate", "out of memory"); return; }

    intcheck("is_grey", dyt_visible_is_grey(p, w, rows), 1);
    intcheck("extract rc", dyt_visible_extract(p, w, rows, grey), 0);

    for (i = 0; i < w * rows; i++)
        if (grey[i] != (uint8_t)ramp_lum(i))
            bad++;
    intcheck("luma == the first byte of each pair", bad, 0);

    /* Reading the *second* byte instead would give a constant 0x80 plane;
     * that is the specific mistake this checks for. */
    bad = 0;
    for (i = 0; i < w * rows; i++)
        if (grey[i] == DYT_VISIBLE_CHROMA_NEUTRAL)
            bad++;
    intcheck("luma is not the chroma byte", bad, 0);

    free(p);
}

static void test_not_grey(void)
{
    const int w = 4, rows = 2;
    uint16_t *p = make_yuyv(w, rows, ramp_lum, 0x40);
    uint8_t  *b;
    uint8_t   grey[4 * 2];

    printf("\n-- non-neutral chroma is rejected --\n");
    if (!p) { fail("allocate", "out of memory"); return; }

    intcheck("uniformly non-neutral", dyt_visible_is_grey(p, w, rows), 0);

    /* A single bad chroma byte is enough: the predicate is "every pixel". */
    b = (uint8_t *)p;
    b[2 * 3 + 1] = 0x7f;
    intcheck("one wrong chroma byte", dyt_visible_is_grey(p, w, rows), 0);

    /* The luma still extracts — is_grey() is a gate, not a precondition. */
    intcheck("extract still works", dyt_visible_extract(p, w, rows, grey), 0);

    free(p);
}

/* ------------------------------------------------------------ stats + reject */

static void test_stats(void)
{
    const uint8_t g[5] = { 10, 200, 30, 0, 255 };
    dyt_visible_stats_t st;

    printf("\n-- stats --\n");

    intcheck("stats rc", dyt_visible_stats(g, 5, &st), 0);
    intcheck("min", st.min, 0);
    intcheck("max", st.max, 255);
    intcheck("n", st.n, 5);
    intcheck("mean", (int)(st.mean * 100 + 0.5), 9900);   /* 99.0 */

    /* A single value is a legal plane. */
    {
        const uint8_t one[1] = { 42 };
        intcheck("single pixel rc", dyt_visible_stats(one, 1, &st), 0);
        intcheck("single pixel min == max == value",
                 st.min == 42 && st.max == 42 && st.n == 1, 1);
    }

    intcheck("NULL grey", dyt_visible_stats(NULL, 5, &st), -1);
    intcheck("NULL out",  dyt_visible_stats(g, 5, NULL), -1);
    intcheck("zero n",    dyt_visible_stats(g, 0, &st), -1);
}

static void test_reject(void)
{
    uint16_t p[16];
    uint8_t  g[16];
    int      i;

    printf("\n-- reject bad arguments --\n");

    for (i = 0; i < 16; i++)
        p[i] = (uint16_t)(DYT_VISIBLE_CHROMA_NEUTRAL << 8);

    intcheck("extract NULL payload", dyt_visible_extract(NULL, 4, 4, g), -1);
    intcheck("extract NULL out",     dyt_visible_extract(p, 4, 4, NULL), -1);
    intcheck("extract zero width",   dyt_visible_extract(p, 0, 4, g), -1);
    intcheck("extract zero rows",    dyt_visible_extract(p, 4, 0, g), -1);

    intcheck("is_grey NULL",      dyt_visible_is_grey(NULL, 4, 4), -1);
    intcheck("is_grey zero rows", dyt_visible_is_grey(p, 4, 0), -1);
}

/* ------------------------------------------------------- the frozen fixture */

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

static void test_fixture(const char *path)
{
    size_t n = 0;
    uint16_t *raw = load(path, &n);
    const uint16_t *top, *bottom;
    uint8_t *grey = NULL;
    dyt_visible_stats_t st;
    int i, diff = 0, distinct = 0;
    int seen[256] = { 0 };

    printf("\n-- frozen dual-half fixture (%s) --\n", path);
    if (!raw) { fail("load", "cannot read fixture"); return; }

    if (n != (size_t)FIX_W * FIX_ROWS * 2) {
        char d[96];
        snprintf(d, sizeof d, "%zu samples, expected %d", n,
                 FIX_W * FIX_ROWS * 2);
        fail("fixture is 256x384", d);
        goto out;
    }

    top    = raw;
    bottom = raw + (size_t)FIX_W * FIX_ROWS;

    /* The predicate that defines the half: neutral chroma everywhere. */
    intcheck("top half is a grey plane",    dyt_visible_is_grey(top, FIX_W, FIX_ROWS), 1);
    intcheck("bottom half is not grey",     dyt_visible_is_grey(bottom, FIX_W, FIX_ROWS), 0);

    grey = malloc((size_t)FIX_W * FIX_ROWS);
    if (!grey) { fail("allocate", "out of memory"); goto out; }

    intcheck("extract rc", dyt_visible_extract(top, FIX_W, FIX_ROWS, grey), 0);
    intcheck("stats rc",   dyt_visible_stats(grey, FIX_W * FIX_ROWS, &st), 0);

    printf("       visible plane: %u..%u, mean %.2f\n",
           (unsigned)st.min, (unsigned)st.max, st.mean);
    rangecheck("visible min is a real grey level", st.min, 40, 70);
    rangecheck("visible max is a real grey level", st.max, 75, 110);
    rangecheck("visible mean is plausible", (int)(st.mean + 0.5), 55, 80);
    intcheck("visible plane has structure (min < max)", st.min < st.max, 1);

    for (i = 0; i < FIX_W * FIX_ROWS; i++)
        seen[grey[i]] = 1;
    for (i = 0; i < 256; i++)
        distinct += seen[i];
    rangecheck("distinct grey levels", distinct, 8, 256);

    /* "Is not the thermal plane": the visible luma must not just be the
     * thermal plane's low byte.  Most pixels differ, and the plane's spread
     * is the visible picture's, not the thermal scene's 16 raw values. */
    {
        const uint8_t *blow = (const uint8_t *)bottom;
        for (i = 0; i < FIX_W * FIX_ROWS; i++)
            if (grey[i] != blow[2 * i])
                diff++;
        printf("       differs from the thermal low byte at %d/%d pixels\n",
               diff, FIX_W * FIX_ROWS);
        intcheck("is not the thermal plane's bytes",
                 diff > FIX_W * FIX_ROWS / 2, 1);
    }

out:
    free(grey);
    free(raw);
}

int main(int argc, char **argv)
{
    const char *fixture = argc > 1 ? argv[1]
                                   : "testdata/mode1000_256x384_default.raw";

    printf("=== visible_test (visible-half extraction) ===\n");

    test_synthetic_roundtrip();
    test_not_grey();
    test_stats();
    test_reject();
    test_fixture(fixture);

    printf("\n=== %s ===\n", fails ? "FAIL" : "ALL PASS");
    return fails ? 1 : 0;
}
