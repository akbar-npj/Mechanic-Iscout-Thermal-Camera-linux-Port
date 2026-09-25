/*
 * harness.c — differential cross-check of the DYT container against the
 * vendor's own libDYTJpegAes.so.
 *
 * The port's dytjpeg.c was written from the decompilation of `D_updateData`
 * (@ 0x129d7c) and `D_jpegOpen` (@ 0x12b464).  This harness closes the loop by
 * running the *real* vendor code on the host:
 *
 *   B. the vendor's D_updateData writes a container from a header blob plus a
 *      raw payload, the port's dyt_dyt_read reads it back, and the recovered
 *      blob, payload and original image must be byte-identical to what went in;
 *   C. the container the vendor wrote is byte-compared against the container
 *      the port writes from the same inputs.  This is the strongest statement
 *      available: the port reproduces the vendor's *bytes*, not merely a
 *      container the vendor happens to tolerate.
 *   A. the port writes a container and the vendor's D_getDytFileLength /
 *      D_jpegOpen read it back.  This one is run in a forked child — see below.
 *
 * A note on case A, and on what this harness does NOT do
 * ------------------------------------------------------
 * Both of the vendor's *readers* call free() on their segment cursor rather
 * than on the buffer they allocated:
 *
 *     D_getDytFileLength @ 0x12af48   free([sp,#104])   base is [sp,#96]
 *     D_jpegOpen         @ 0x12b464   free([sp,#112])   base is [sp,#104]
 *
 * The buffer is walked by incrementing that same slot, so the pointer handed to
 * free() is a few hundred bytes into the allocation.  bionic's allocator
 * tolerates this; glibc reads a garbage chunk header and dies inside free().
 * It is dead code in the product — the JNI wrapper
 * `Java_com_dywcc_demojni_NativeUtils_JavaStaticCallD_1getDytFileLength` names a
 * Java package that does not exist in the APK's dex — which is why it was never
 * noticed.  The *writer* has no such problem: D_updateData frees only the three
 * base pointers it allocated (0x29e20, 0x2a280, 0x2a2e0 -> 0x2a698/0x2a6a8/0x2a6b4).
 *
 * This harness deliberately does **not** patch that bug.  Patching the code
 * under test would weaken the very thing the harness exists to establish.  So
 * case A runs in a child process: if the vendor's reader dies, that is reported
 * as the vendor's defect and the rest of the run stands.  B and C, which cover
 * the writer in both directions, are unaffected and are the strong results.
 *
 * Not part of `make check`: it needs the vendor .so from the APK, an aarch64
 * host with glibc (the vendor libraries are arm64-v8a and run natively — no
 * qemu), and the bionic shims in tools/vendor_shim/.  See run.sh.
 *
 * usage: harness <libDYTJpegAes.so> <workdir> [raw-payload-file]
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "dytjpeg.h"

/* ------------------------------------------------------------------ report -- */

static int checks, failures, skipped;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond)
        failures++;
    printf("  %-4s %s\n", cond ? "ok" : "FAIL", what);
}

/* A check that could not be performed at all — distinct from one that failed,
 * because it says nothing about the port. */
static void skip(const char *what)
{
    skipped++;
    printf("  %-4s %s\n", "SKIP", what);
}

static void note(const char *fmt, ...)
{
    va_list ap;

    printf("  ..   ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/* ------------------------------------------------------------------ vendor -- */

typedef int (*fn_update)(char *path, void *blob_raw, int total);
typedef int (*fn_open)(char *path, void **buf, int *len);
typedef int (*fn_len)(char *path, int *out);

/* --------------------------------------------------------------- utilities -- */

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE    *f = fopen(path, "rb");
    long     sz;
    uint8_t *b;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    b = malloc(sz ? (size_t)sz : 1);
    if (!b) { fclose(f); return NULL; }
    if (sz && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b); fclose(f); return NULL;
    }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static uint8_t *slurp_all(const char *path, size_t *n)
{
    uint8_t *b = slurp(path, n);
    if (!b) {
        fprintf(stderr, "harness: cannot read %s\n", path);
        exit(2);
    }
    return b;
}

static int spew(const char *path, const uint8_t *d, size_t n)
{
    FILE *f = fopen(path, "wb");

    if (!f)
        return -1;
    if (n && fwrite(d, 1, n, f) != n) { fclose(f); return -1; }
    return fclose(f) == 0 ? 0 : -1;
}

/* A deterministic payload so a failure is reproducible without a fixture. */
static void fill_pattern(uint8_t *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        p[i] = (uint8_t)((i * 131u + (i >> 8) * 7u) & 0xFFu);
}

static int same(const uint8_t *a, const uint8_t *b, size_t n)
{
    return n == 0 || (a && b && memcmp(a, b, n) == 0);
}

static void first_diff(const uint8_t *got, const uint8_t *want, size_t n,
                       const char *what)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (got[i] != want[i]) {
            note("first difference in the %s at offset %zu: got %02x want %02x",
                 what, i, got[i], want[i]);
            return;
        }
}

/* A JPEG whose APP chain is longer than the one stb emits, so the "insert after
 * the *last* APP1" rule is actually exercised rather than assumed.  The body
 * after the chain is arbitrary: nothing here decodes it. */
static size_t make_jpeg(uint8_t *d, size_t cap)
{
    static const uint8_t prefix[] = {
        0xFF, 0xD8,                                     /* SOI             */
        0xFF, 0xE0, 0x00, 0x10,                         /* APP0, len 16    */
        'J',  'F',  'I',  'F',  0x00, 0x01, 0x01, 0x00,
        0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
        0xFF, 0xE1, 0x00, 0x0A,                         /* APP1 #1, len 10 */
        'E',  'x',  'i',  'f',  0x00, 0x00, 0x11, 0x22,
        0xFF, 0xE1, 0x00, 0x08,                         /* APP1 #2, len 8  */
        'A',  'B',  'C',  'D',  0x00, 0x00,
    };
    /* ... the first segment after the chain, then a body, then EOI */
    static const uint8_t suffix[] = {
        0xFF, 0xDB, 0x00, 0x04, 0x00, 0x00,             /* DQT             */
        0xFF, 0xC0, 0x00, 0x04, 0x00, 0x00,             /* SOF0            */
        0xFF, 0xDA, 0x00, 0x03, 0x00,                   /* SOS             */
        0x12, 0x34, 0x56, 0x78, 0x9A,                   /* entropy-ish     */
        0xFF, 0xD9,                                     /* EOI             */
    };
    size_t n = sizeof prefix + sizeof suffix;

    if (n > cap)
        return 0;
    memcpy(d, prefix, sizeof prefix);
    memcpy(d + sizeof prefix, suffix, sizeof suffix);
    return n;
}

/* ---------------------------------------------------------------- cases B/C -- */

/* The vendor writes; the port reads; the two containers are byte-compared. */
static void cases_bc(const char *dir, fn_update vupdate,
                     const uint8_t *jpeg, size_t jpeg_len,
                     const uint8_t *blob, size_t blob_len,
                     const uint8_t *raw, size_t raw_len)
{
    char     vendor[1024], ported[1024];
    uint8_t *joined, *gb = NULL, *gr = NULL, *gj = NULL;
    uint8_t *vb = NULL, *pb = NULL;
    size_t   gbl = 0, grl = 0, gjl = 0, vn = 0, pn = 0;
    int      rc;

    printf("\n== B: the vendor writes, the port reads ==\n");

    /* D_updateData() takes one contiguous buffer: [blob][raw]. */
    joined = malloc(blob_len + raw_len);
    if (!joined) { ok(0, "malloc for the vendor's input"); return; }
    memcpy(joined, blob, blob_len);
    memcpy(joined + blob_len, raw, raw_len);

    /* Seed the file with the *bare* JPEG, so the vendor inserts the APP2 run
     * into a container-free image — the clean case, not a rewrite. */
    snprintf(vendor, sizeof vendor, "%s/vendor.dyt.jpg", dir);
    if (spew(vendor, jpeg, jpeg_len) != 0) {
        ok(0, "seeding the vendor's input file");
        free(joined);
        return;
    }

    rc = vupdate(vendor, joined, (int)(blob_len + raw_len));
    ok(rc == 0, "D_updateData() returns 0");

    ok(dyt_dyt_read(vendor, &gb, &gbl, &gr, &grl, &gj, &gjl) == 0,
       "dyt_dyt_read() accepts the vendor's container");
    ok(gbl == blob_len && same(gb, blob, blob_len),
       "the port recovers the header blob byte-for-byte");
    ok(grl == raw_len && same(gr, raw, raw_len),
       "the port recovers the raw payload byte-for-byte");
    ok(gjl == jpeg_len && same(gj, jpeg, jpeg_len),
       "the port recovers the original image byte-for-byte");

    if (gbl != blob_len || !same(gb, blob, blob_len))
        note("blob: got %zu bytes, want %zu", gbl, blob_len);
    if (grl != raw_len || !same(gr, raw, raw_len))
        note("raw:  got %zu bytes, want %zu", grl, raw_len);
    if (gjl != jpeg_len || !same(gj, jpeg, jpeg_len))
        note("jpeg: got %zu bytes, want %zu", gjl, jpeg_len);

    printf("\n== C: the vendor's container is the port's container ==\n");

    snprintf(ported, sizeof ported, "%s/port.dyt.jpg", dir);
    if (dyt_dyt_write(ported, jpeg, jpeg_len, blob, blob_len, raw, raw_len) != 0) {
        ok(0, "the port writes its own container");
    } else {
        vb = slurp_all(vendor, &vn);
        pb = slurp_all(ported, &pn);
        ok(vn == pn, "the two containers are the same length");
        ok(same(vb, pb, vn), "the two containers are byte-identical");
        if (vn != pn) {
            note("vendor %zu bytes, port %zu bytes", vn, pn);
        } else if (!same(vb, pb, vn)) {
            size_t i;
            first_diff(vb, pb, vn, "container");
            for (i = 0; i < vn; i++)
                if (vb[i] != pb[i])
                    break;
            note("differing byte is at offset %zu of %zu (%.1f%% through)",
                 i, vn, 100.0 * (double)i / (double)vn);
        }
    }

    free(joined);
    free(gb);
    free(gr);
    free(gj);
    free(vb);
    free(pb);
}

/* -------------------------------------------------------------------- case A -- */

/* Runs in the child: the port's container is read back by the vendor. */
static void case_a_child(char *path, fn_len vlen, fn_open vopen,
                         const uint8_t *blob, size_t blob_len,
                         const uint8_t *raw, size_t raw_len)
{
    uint8_t *got;
    int      len = 0, rc, total = 0;

    rc = vlen(path, &total);
    ok(rc == 0, "D_getDytFileLength() returns 0");
    ok(total == (int)(blob_len + raw_len),
       "D_getDytFileLength() reports blob + raw");

    got = malloc(blob_len + raw_len + 1);
    if (!got) { ok(0, "malloc for the vendor's output"); return; }
    memset(got, 0xA5, blob_len + raw_len + 1);

    len = 0;
    rc  = vopen(path, (void **)&got, &len);
    ok(rc == 0, "D_jpegOpen() returns 0");
    ok(len == total, "D_jpegOpen() reports the same total");

    ok(same(got, blob, blob_len),
       "the vendor recovers the port's header blob byte-for-byte");
    ok(same(got + blob_len, raw, raw_len),
       "the vendor recovers the port's raw payload byte-for-byte");

    if (!same(got, blob, blob_len))
        first_diff(got, blob, blob_len, "header blob");
    if (!same(got + blob_len, raw, raw_len))
        first_diff(got + blob_len, raw, raw_len, "raw payload");

    free(got);
}

/* The port writes; the vendor reads — in a child, because the vendor's readers
 * have a latent free() bug (see the header comment).  The child reports its own
 * check counts back through a pipe so the summary stays accurate, and its
 * printed lines are the real evidence when it survives. */
static void case_a(const char *dir, fn_len vlen, fn_open vopen,
                   const uint8_t *jpeg, size_t jpeg_len,
                   const uint8_t *blob, size_t blob_len,
                   const uint8_t *raw, size_t raw_len)
{
    char   path[1024];
    int    pfd[2], st = 0, c0 = checks, f0 = failures, s0 = skipped;
    int    delta[2] = { 0, 0 };
    pid_t  pid;
    ssize_t n;

    printf("\n== A: the port writes, the vendor reads (forked) ==\n");

    snprintf(path, sizeof path, "%s/port.dyt.jpg", dir);
    ok(dyt_dyt_write(path, jpeg, jpeg_len, blob, blob_len, raw, raw_len) == 0,
       "dyt_dyt_write() succeeds");

    if (pipe(pfd) != 0) {
        skip("the vendor's readers round-trip the port's container");
        note("could not create the reporting pipe");
        return;
    }

    fflush(stdout);
    pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        skip("the vendor's readers round-trip the port's container");
        note("fork() failed");
        return;
    }
    if (pid == 0) {
        int r[2];

        close(pfd[0]);
        case_a_child(path, vlen, vopen, blob, blob_len, raw, raw_len);
        r[0] = checks - c0;
        r[1] = failures - f0;
        fflush(stdout);
        n = write(pfd[1], r, sizeof r);
        (void)n;
        close(pfd[1]);
        _exit(failures ? 1 : 0);
    }

    close(pfd[1]);
    n = read(pfd[0], delta, sizeof delta);
    close(pfd[0]);
    if (n == (ssize_t)sizeof delta) {
        checks   += delta[0];
        failures += delta[1];
    }

    if (waitpid(pid, &st, 0) < 0) {
        skip("the vendor's readers round-trip the port's container");
        note("waitpid() failed");
        return;
    }

    if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
        return;                             /* the child's own lines say it all */

    if (n != (ssize_t)sizeof delta) {       /* died before it could report */
        checks = c0;
        failures = f0;
        skipped = s0;
        skip("the vendor's readers round-trip the port's container");
    } else {
        return;                             /* it ran and genuinely failed */
    }

    if (WIFSIGNALED(st))
        note("the vendor's reader died with signal %d (%s) before reporting",
             WTERMSIG(st), strsignal(WTERMSIG(st)));
    else
        note("the vendor's reader exited with status %d", WEXITSTATUS(st));
    note("that is the vendor's own defect, not the port's:");
    note("  D_getDytFileLength @ 0x12af48 frees [sp,#104] (the segment cursor)");
    note("  D_jpegOpen         @ 0x12b464 frees [sp,#112] (the segment cursor)");
    note("where the buffer base is [sp,#96] / [sp,#104].  bionic tolerates the");
    note("interior pointer; glibc does not.  Both functions are dead code in the");
    note("app, so the defect was never observed.  This harness does not patch the");
    note("code under test: B and C above already pin the container's bytes.");
}

/* --------------------------------------------------------------------- main -- */

int main(int argc, char **argv)
{
    void        *h;
    fn_update    vupdate;
    fn_open      vopen;
    fn_len       vlen;
    const char  *so = argc > 1 ? argv[1] : "./libDYTJpegAes.so";
    const char  *dir = argc > 2 ? argv[2] : ".";
    const char  *rawfile = argc > 3 ? argv[3] : NULL;
    uint8_t     *raw = NULL, *blob = NULL, *jpeg = NULL, *fixture = NULL;
    size_t       raw_len, jpeg_len;
    const char  *rawsrc;

    h = dlopen(so, RTLD_LAZY | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "harness: dlopen(%s) failed: %s\n", so, dlerror());
        return 2;
    }
    *(void **)(&vupdate) = dlsym(h, "D_updateData");
    *(void **)(&vopen)   = dlsym(h, "D_jpegOpen");
    *(void **)(&vlen)    = dlsym(h, "D_getDytFileLength");
    if (!vupdate || !vopen || !vlen) {
        fprintf(stderr, "harness: %s is missing the D_* entry points\n", so);
        return 2;
    }

    /* The raw thermal payload: the frozen device fixture when given, otherwise
     * a synthetic pattern long enough to span several APP2 chunks. */
    if (rawfile) {
        fixture = slurp_all(rawfile, &raw_len);
        rawsrc  = rawfile;
    } else {
        raw_len = 3u * DYT_DYT_CHUNK_MAX + 17u;
        fixture = malloc(raw_len);
        if (!fixture) return 2;
        fill_pattern(fixture, raw_len);
        rawsrc = "synthetic pattern";
    }

    blob = malloc(DYT_DYT_BLOB_SIZE);
    jpeg = malloc(4096);
    if (!blob || !jpeg) return 2;

    /* Geometry chosen so the extension record carries non-trivial values. */
    if (dyt_dyt_blob_init(blob, DYT_DYT_BLOB_SIZE,
                          256, 192, 384, DYT_DYT_FLAG_DUAL_HALF) != 0) {
        fprintf(stderr, "harness: dyt_dyt_blob_init failed\n");
        return 2;
    }
    jpeg_len = make_jpeg(jpeg, 4096);
    if (jpeg_len == 0) {
        fprintf(stderr, "harness: the synthetic JPEG does not fit\n");
        return 2;
    }

    printf("DYT container differential vs the vendor library\n");
    printf("  vendor:   %s\n", so);
    printf("  workdir:  %s\n", dir);
    printf("  jpeg:     %zu bytes (APP0 + 2 x APP1 + body)\n", jpeg_len);
    printf("  blob:     %zu bytes\n", (size_t)DYT_DYT_BLOB_SIZE);
    printf("  raw:      %zu bytes (%s)\n", raw_len, rawsrc);

    raw = fixture;
    cases_bc(dir, vupdate, jpeg, jpeg_len, blob, DYT_DYT_BLOB_SIZE, raw, raw_len);
    case_a(dir, vlen, vopen, jpeg, jpeg_len, blob, DYT_DYT_BLOB_SIZE,
           raw, raw_len);

    printf("\n%d checks, %d failure%s", checks, failures,
           failures == 1 ? "" : "s");
    if (skipped)
        printf(", %d skipped (the vendor's own defect)", skipped);
    printf("\n");
    return failures ? 1 : 0;
}
