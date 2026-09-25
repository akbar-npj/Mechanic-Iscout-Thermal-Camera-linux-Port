/*
 * imgwrite_test.c — unit tests for the image writers (imgwrite.c).
 *
 * imgwrite had no test of its own: the PPM path was only exercised indirectly,
 * as the vehicle for palette_test's and fusion_test's golden comparisons.  A
 * broken PPM writer would therefore have shown up as a golden mismatch rather
 * than as "the writer is wrong".
 *
 * The PNG writer is checked structurally, because the port has no PNG reader
 * and deliberately does not grow one: the file must carry the PNG signature,
 * an IHDR chunk of the right shape whose CRC verifies, and a final IEND.  The
 * CRC is computed here from the PNG polynomial, so a writer that emitted a
 * plausible-looking but malformed chunk would be caught.
 *
 * build:  via the Makefile (make check)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgwrite.h"

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

static uint8_t *load_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    long  sz;
    uint8_t *b;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) { fclose(f); return NULL; }
    rewind(f);
    b = malloc(sz ? (size_t)sz : 1);
    if (!b) { fclose(f); return NULL; }
    if (sz && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static int write_file(const char *path, const void *d, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (n && fwrite(d, 1, n, f) != n) { fclose(f); return -1; }
    return fclose(f) == 0 ? 0 : -1;
}

/* --------------------------------------------------------------- PPM (P6) -- */

static void test_ppm(void)
{
    const char *path = "build/imgwrite_test.ppm";
    const int   w = 5, h = 3;
    uint8_t     src[5 * 3 * 3];
    uint8_t    *back = NULL;
    int         bw = 0, bh = 0, i;

    printf("\n-- PPM (P6) --\n");

    for (i = 0; i < w * h * 3; i++)
        src[i] = (uint8_t)(i * 11 + 5);

    intcheck("write rc", dyt_write_ppm(path, src, w, h), 0);

    intcheck("read rc", dyt_read_ppm(path, &back, &bw, &bh), 0);
    intcheck("width",  bw, w);
    intcheck("height", bh, h);
    intcheck("pixels are byte-identical",
             back && memcmp(back, src, sizeof src) == 0, 1);

    /* The header must be exactly "P6\n<w> <h>\n255\n" and nothing else: the
     * raster has to start immediately after it, or every reader disagrees. */
    {
        size_t n = 0;
        uint8_t *raw = load_file(path, &n);
        intcheck("header is P6, w, h, 255",
                 raw && n == (size_t)(11 + w * h * 3) &&
                 memcmp(raw, "P6\n5 3\n255\n", 11) == 0, 1);
        free(raw);
    }

    free(back);
    remove(path);
}

static void test_ppm_reject(void)
{
    const char *junk = "build/imgwrite_test_junk.bin";
    uint8_t  buf[32];
    uint8_t *rgb = NULL;
    int      w = 0, h = 0;

    printf("\n-- PPM rejects --\n");

    intcheck("write NULL path", dyt_write_ppm(NULL, buf, 2, 2), -1);
    intcheck("write NULL rgb",  dyt_write_ppm("build/x.ppm", NULL, 2, 2), -1);
    intcheck("write zero width",  dyt_write_ppm("build/x.ppm", buf, 0, 2), -1);
    intcheck("write zero height", dyt_write_ppm("build/x.ppm", buf, 2, 0), -1);

    intcheck("read NULL path", dyt_read_ppm(NULL, &rgb, &w, &h), -1);
    intcheck("read missing file",
             dyt_read_ppm("build/imgwrite_test_absent.ppm", &rgb, &w, &h), -1);

    memset(buf, 0x41, sizeof buf);
    if (write_file(junk, buf, sizeof buf) != 0) {
        fail("write a junk file", "I/O error");
        return;
    }
    intcheck("read a non-PPM", dyt_read_ppm(junk, &rgb, &w, &h), -1);
    remove(junk);
}

/* ------------------------------------------------------------------- PNG --- */

static uint32_t crc_table[256];

static void crc_init(void)
{
    uint32_t i, j, c;

    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++)
            c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
        crc_table[i] = c;
    }
}

/* The PNG chunk CRC: over the chunk type and data, reflected, 0xEDB88320. */
static uint32_t crc_png(const uint8_t *d, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    size_t   i;

    for (i = 0; i < n; i++)
        c = crc_table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static unsigned be32(const uint8_t *p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
           ((unsigned)p[2] << 8)  | (unsigned)p[3];
}

static void test_png(void)
{
    const char *path = "build/imgwrite_test.png";
    const int   w = 7, h = 4;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    uint8_t  src[7 * 4 * 3];
    uint8_t *raw = NULL;
    size_t   n = 0;
    int      i;

    printf("\n-- PNG --\n");
    crc_init();

    for (i = 0; i < w * h * 3; i++)
        src[i] = (uint8_t)(255 - (i * 7) % 256);

    if (dyt_write_png(path, src, w, h) != 0) {
        fail("write rc", "returned -1");
        return;
    }
    ok("write rc");

    raw = load_file(path, &n);
    if (!raw || n < 8 + 25 + 12) {
        fail("read back", "file too short");
        free(raw);
        return;
    }

    intcheck("PNG signature", memcmp(raw, sig, 8) == 0, 1);

    /* First chunk: IHDR, 13 bytes of data. */
    intcheck("IHDR length is 13", be32(raw + 8) == 13, 1);
    intcheck("first chunk is IHDR", memcmp(raw + 12, "IHDR", 4) == 0, 1);
    intcheck("width",  (int)be32(raw + 16), w);
    intcheck("height", (int)be32(raw + 20), h);
    intcheck("bit depth is 8",        raw[24], 8);
    intcheck("colour type is 2 (truecolour)", raw[25], 2);
    intcheck("compression method is 0", raw[26], 0);
    intcheck("filter method is 0",      raw[27], 0);
    intcheck("interlace is 0",          raw[28], 0);

    {
        uint32_t want = crc_png(raw + 12, 4 + 13);
        uint32_t got  = (uint32_t)be32(raw + 29);
        char     d[96];
        if (want == got) {
            ok("IHDR CRC verifies");
        } else {
            snprintf(d, sizeof d, "crc %08x, computed %08x", got, want);
            fail("IHDR CRC verifies", d);
        }
    }

    /* And the file must actually end with IEND. */
    intcheck("ends with the IEND chunk",
             n >= 12 && memcmp(raw + n - 8, "IEND", 4) == 0 &&
             be32(raw + n - 12) == 0, 1);

    free(raw);
    remove(path);
}

static void test_png_reject(void)
{
    uint8_t buf[32];

    printf("\n-- PNG rejects --\n");

    memset(buf, 0, sizeof buf);
    intcheck("write NULL path", dyt_write_png(NULL, buf, 2, 2), -1);
    intcheck("write NULL rgb",  dyt_write_png("build/x.png", NULL, 2, 2), -1);
    intcheck("write zero width",  dyt_write_png("build/x.png", buf, 0, 2), -1);
    intcheck("write zero height", dyt_write_png("build/x.png", buf, 2, 0), -1);
}

int main(void)
{
    printf("=== imgwrite_test (PPM and PNG writers) ===\n");

    test_ppm();
    test_ppm_reject();
    test_png();
    test_png_reject();

    printf("\n=== %s ===\n", fails ? "FAIL" : "ALL PASS");
    return fails ? 1 : 0;
}
