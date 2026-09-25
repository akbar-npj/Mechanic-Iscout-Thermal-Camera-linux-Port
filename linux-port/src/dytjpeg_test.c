/*
 * dytjpeg_test.c — unit tests for the DYT still container (dytjpeg.c).
 *
 * The container is a *byte format*, so this test does not ask dytjpeg.c to
 * grade its own homework.  It carries its own segment walker and checks the
 * produced bytes directly:
 *
 *   - the APP2 run really starts where the APP0/APP1 chain ends, and the APP0
 *     and the JPEG body are left byte-identical either side of it;
 *   - the length fields are big-endian;
 *   - a full chunk is written as 0xFFFF and carries exactly 0xFFFD bytes, and
 *     a short one carries its true length;
 *   - the blob's own size at +0x06 agrees with the segment that carries it.
 *
 * Then it round-trips a real file — the frozen dual-half frame, whose payload
 * is four chunks — and byte-compares the raw thermal data, which is the thing
 * the format exists to preserve.
 *
 * usage:  ./dytjpeg_test [dual-half.raw]
 * build:  via the Makefile (make check)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dytjpeg.h"

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

static void sizecheck(const char *what, size_t got, size_t want)
{
    if (got == want) {
        ok(what);
    } else {
        char d[96];
        snprintf(d, sizeof d, "got %zu want %zu", got, want);
        fail(what, d);
    }
}

/* ------------------------------------------------------------ test helpers -- */

static uint8_t *load_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    long  sz;
    uint8_t *b;

    if (!f) { perror(path); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    b = malloc(sz ? (size_t)sz : 1);
    if (!b) { fclose(f); return NULL; }
    if (sz && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static void put_be16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)((v >> 8) & 0xFF);
    p[1] = (uint8_t)(v & 0xFF);
}

/* A structurally valid JPEG: SOI, one APP0, `app1` APP1 segments, then a
 * non-APP segment (standing in for DQT/SOF) and EOI.  `*ins` receives the
 * offset the APP2 run must start at — the first segment after the chain. */
static uint8_t *make_jpeg(int app1, size_t *len, size_t *ins)
{
    size_t   cap = 2 + (2 + 2 + 14) + (size_t)app1 * (2 + 2 + 10) + (2 + 2 + 6) + 2;
    uint8_t *d = malloc(cap);
    size_t   p = 0;
    int      i;

    if (!d)
        return NULL;

    d[p++] = 0xFF; d[p++] = 0xD8;                       /* SOI */

    d[p++] = 0xFF; d[p++] = 0xE0;                       /* APP0, len 16 */
    put_be16(d + p, 16); p += 2;
    memcpy(d + p, "JFIF\0\x01\x02\x00\x00\x01\x00\x01\x00\x00", 14); p += 14;

    for (i = 0; i < app1; i++) {                        /* APP1, len 12 */
        d[p++] = 0xFF; d[p++] = 0xE1;
        put_be16(d + p, 12); p += 2;
        memset(d + p, 0xA0 + i, 10); p += 10;
    }

    *ins = p;                                           /* APP2 goes here */

    d[p++] = 0xFF; d[p++] = 0xFE;                       /* a "body" segment */
    put_be16(d + p, 8); p += 2;
    memset(d + p, 0x5A, 6); p += 6;

    d[p++] = 0xFF; d[p++] = 0xD9;                       /* EOI */

    *len = p;
    return d;
}

/* ------------------------------------------------- the produced bytes, checked */

/* Walk the APP2 run and report what it found, so the test can compare against
 * the format directly.  Everything here is written from RE Docs 06 §2.1, not
 * from dytjpeg.c. */
typedef struct {
    size_t   ins;        /* where the run starts */
    size_t   nseg;       /* segments in the run */
    size_t   blob_len;   /* first segment's payload */
    size_t   raw_len;    /* total payload of the remaining segments */
    size_t   full;       /* how many used the 0xFFFF length field */
    size_t   tail_off;   /* first byte after the run */
    int      be_ok;      /* every length field read big-endian and in range */
    int      full_ok;    /* every 0xFFFF segment carried exactly 0xFFFD */
} run_t;

static int scan_run(const uint8_t *d, size_t n, run_t *r)
{
    size_t pos = 2;

    memset(r, 0, sizeof *r);
    r->be_ok = 1;
    r->full_ok = 1;

    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8)
        return -1;

    /* skip the APP0/APP1 chain exactly as the format says */
    for (;;) {
        int    marker;
        size_t len;

        if (pos + 2 > n || d[pos] != 0xFF)
            return -1;
        marker = d[pos + 1];
        if (marker != 0xE0 && marker != 0xE1)
            break;
        if (pos + 4 > n)
            return -1;
        len = ((size_t)d[pos + 2] << 8) | d[pos + 3];
        if (len < 2 || pos + 2 + len > n)
            return -1;
        pos += 2 + len;
    }
    r->ins = pos;

    /* the APP2 run */
    while (pos + 2 <= n && d[pos] == 0xFF && d[pos + 1] == 0xE2) {
        size_t seg, plen;

        if (pos + 4 > n) { r->be_ok = 0; return -1; }
        seg = ((size_t)d[pos + 2] << 8) | d[pos + 3];
        if (seg < 2 || pos + 2 + seg > n) { r->be_ok = 0; return -1; }
        plen = seg - 2;

        if (seg == 0xFFFF) {
            r->full++;
            if (plen != DYT_DYT_CHUNK_MAX)
                r->full_ok = 0;
        }

        if (r->nseg == 0) r->blob_len = plen;
        else               r->raw_len += plen;

        r->nseg++;
        pos += 2 + seg;
    }
    r->tail_off = pos;
    return 0;
}

/* ------------------------------------------------------------------- cases -- */

static void test_layout(int app1, size_t raw_len)
{
    uint8_t *jpg = NULL, *blob = NULL, *raw = NULL, *out = NULL;
    size_t   jlen = 0, ins = 0, blen = DYT_DYT_BLOB_SIZE, olen = 0;
    size_t   want_chunks, want_total, i;
    run_t    r;
    char     what[80];

    snprintf(what, sizeof what, "layout, %d APP1, raw %zu", app1, raw_len);
    printf("\n-- %s --\n", what);

    jpg = make_jpeg(app1, &jlen, &ins);
    blob = calloc(1, blen);
    raw  = malloc(raw_len ? raw_len : 1);
    if (!jpg || !blob || !raw) { fail("allocate", "out of memory"); goto out; }

    if (dyt_dyt_blob_init(blob, blen, 256, 192, 192,
                          DYT_DYT_FLAG_DUAL_HALF) != 0) {
        fail("blob_init", "returned -1");
        goto out;
    }
    for (i = 0; i < raw_len; i++)
        raw[i] = (uint8_t)(i * 31 + 7);          /* deterministic, non-constant */

    if (dyt_dyt_build(jpg, jlen, blob, blen, raw, raw_len, &out, &olen) != 0) {
        fail("build", "returned -1");
        goto out;
    }
    ok("build");

    /* --- the container's size is exactly the format's arithmetic --- */
    want_chunks = raw_len / DYT_DYT_CHUNK_MAX + (raw_len % DYT_DYT_CHUNK_MAX ? 1 : 0);
    want_total  = jlen + 4 + blen + want_chunks * 4 + raw_len;
    sizecheck("total size", olen, want_total);

    if (scan_run(out, olen, &r) != 0) {
        fail("walk the APP2 run", "malformed");
        goto out;
    }

    intcheck("big-endian lengths are in range", r.be_ok, 1);
    sizecheck("insertion point is after the APP0/APP1 chain", r.ins, ins);
    sizecheck("segment count", r.nseg, want_chunks + 1);
    sizecheck("blob segment payload", r.blob_len, blen);
    sizecheck("raw payload, reassembled", r.raw_len, raw_len);
    intcheck("every full chunk used 0xFFFF for 0xFFFD bytes", r.full_ok, 1);
    sizecheck("full chunks", r.full, raw_len / DYT_DYT_CHUNK_MAX);
    sizecheck("tail starts after the run", r.tail_off, ins + 4 + blen +
              want_chunks * 4 + raw_len);

    /* --- the pieces around the run are untouched --- */
    intcheck("APP0 is byte-identical", memcmp(out, jpg, ins) == 0, 1);
    intcheck("the JPEG body follows the run intact",
             memcmp(out + r.tail_off, jpg + ins, jlen - ins) == 0, 1);

    /* --- the blob segment carries the blob, verbatim --- */
    intcheck("first segment payload == the blob",
             memcmp(out + ins + 4, blob, blen) == 0, 1);

    /* --- the length field of the blob segment is big-endian --- */
    intcheck("blob segment length field is BE",
             out[ins + 2] == (uint8_t)((blen + 2) >> 8) &&
             out[ins + 3] == (uint8_t)((blen + 2) & 0xFF), 1);

    /* --- the blob is self-describing: its size at +0x06 equals the segment
     *     that carries it.  The payload starts 4 bytes into the segment, so
     *     the field sits at ins + 4 + DYT_DYT_OFF_SIZE. --- */
    {
        const uint8_t *b = out + ins + 4;
        unsigned sz = (unsigned)b[DYT_DYT_OFF_SIZE] |
                      ((unsigned)b[DYT_DYT_OFF_SIZE + 1] << 8);
        sizecheck("blob size at +0x06 matches its segment", sz, blen);
        intcheck("dyt_dyt_blob_size agrees",
                 dyt_dyt_blob_size(blob, blen) == blen, 1);
    }

    /* --- the raw chunks carry the payload, in order --- */
    {
        size_t pos = ins + 4 + blen, done = 0;
        int    seg_bad = 0;

        for (i = 0; i < want_chunks; i++) {
            size_t seg = ((size_t)out[pos + 2] << 8) | out[pos + 3];
            size_t plen = seg - 2;

            if (memcmp(out + pos + 4, raw + done, plen) != 0)
                seg_bad++;
            done += plen;
            pos  += 2 + seg;
        }
        intcheck("every chunk payload matches the raw data", seg_bad, 0);
        sizecheck("chunks cover the payload exactly", done, raw_len);
    }

out:
    free(jpg); free(blob); free(raw); free(out);
}

static void test_roundtrip_file(const uint8_t *fixture, size_t fix_len)
{
    const char *path = "build/dytjpeg_test_tmp.jpg";
    uint8_t *jpg = NULL, *blob = NULL;
    uint8_t *rblob = NULL, *rraw = NULL, *rjpg = NULL;
    size_t   jlen = 0, ins = 0, blen = DYT_DYT_BLOB_SIZE, olen = 0;
    size_t   rblen = 0, rrlen = 0, rjlen = 0;
    int      w = 0, ar = 0, tr = 0;
    unsigned fl = 0;

    printf("\n-- write a file, read it back (%zu-byte payload) --\n", fix_len);

    jpg = make_jpeg(1, &jlen, &ins);
    blob = calloc(1, blen);
    if (!jpg || !blob) { fail("allocate", "out of memory"); goto out; }

    if (dyt_dyt_blob_init(blob, blen, 256, 192, 384,
                          DYT_DYT_FLAG_DUAL_HALF) != 0) {
        fail("blob_init", "returned -1");
        goto out;
    }

    if (dyt_dyt_write(path, jpg, jlen, blob, blen, fixture, fix_len) != 0) {
        fail("dyt_dyt_write", "returned -1");
        goto out;
    }
    ok("dyt_dyt_write");

    if (dyt_dyt_read(path, &rblob, &rblen, &rraw, &rrlen, &rjpg, &rjlen) != 0) {
        fail("dyt_dyt_read", "returned -1");
        goto out;
    }
    ok("dyt_dyt_read");

    sizecheck("blob length", rblen, blen);
    sizecheck("raw length", rrlen, fix_len);
    intcheck("raw payload is byte-identical",
             rrlen == fix_len && memcmp(rraw, fixture, fix_len) == 0, 1);
    intcheck("blob is byte-identical",
             rblen == blen && memcmp(rblob, blob, blen) == 0, 1);
    intcheck("the extracted JPEG is the original",
             rjlen == jlen && memcmp(rjpg, jpg, jlen) == 0, 1);

    /* The extension record survives the trip. */
    intcheck("geometry record round-trips",
             dyt_dyt_blob_geometry(rblob, rblen, &w, &ar, &tr, &fl) == 1, 1);
    intcheck("width", w, 256);
    intcheck("active rows", ar, 192);
    intcheck("total rows", tr, 384);
    intcheck("flags", (int)fl, DYT_DYT_FLAG_DUAL_HALF);

    /* Rebuild from the read-back parts and compare to what is on disk. */
    {
        uint8_t *again = NULL;
        size_t   alen = 0;
        if (dyt_dyt_build(rjpg, rjlen, rblob, rblen, rraw, rrlen,
                          &again, &alen) != 0) {
            fail("rebuild", "returned -1");
        } else {
            uint8_t *disk = load_file(path, &olen);
            intcheck("rebuild is byte-identical to the file",
                     disk && alen == olen && memcmp(again, disk, alen) == 0, 1);
            free(disk);
            free(again);
        }
    }

out:
    remove(path);
    free(jpg); free(blob);
    free(rblob); free(rraw); free(rjpg);
}

static void test_blob_helpers(void)
{
    uint8_t blob[DYT_DYT_BLOB_SIZE];
    int     w = 0, ar = 0, tr = 0;
    unsigned fl = 0;
    uint8_t vendor[DYT_DYT_HDR_FIXED];

    printf("\n-- header blob helpers --\n");

    intcheck("init rc", dyt_dyt_blob_init(blob, sizeof blob, 384, 288, 292,
                                          0), 0);
    sizecheck("blob_size", dyt_dyt_blob_size(blob, sizeof blob),
              DYT_DYT_BLOB_SIZE);
    intcheck("geometry present",
             dyt_dyt_blob_geometry(blob, sizeof blob, &w, &ar, &tr, &fl), 1);
    intcheck("geometry width", w, 384);
    intcheck("geometry active rows", ar, 288);
    intcheck("geometry total rows", tr, 292);
    intcheck("geometry flags", (int)fl, 0);

    /* A vendor blob has the fixed part but no extension: the size is readable,
     * the geometry is not — and that is reported, not guessed. */
    memset(vendor, 0, sizeof vendor);
    vendor[DYT_DYT_OFF_SIZE]     = (uint8_t)(DYT_DYT_HDR_FIXED & 0xFF);
    vendor[DYT_DYT_OFF_SIZE + 1] = (uint8_t)(DYT_DYT_HDR_FIXED >> 8);
    sizecheck("vendor blob size", dyt_dyt_blob_size(vendor, sizeof vendor),
              DYT_DYT_HDR_FIXED);
    intcheck("vendor blob has no geometry record",
             dyt_dyt_blob_geometry(vendor, sizeof vendor, &w, &ar, &tr, &fl), 0);

    /* A size field that runs past what we were handed is not trusted. */
    vendor[DYT_DYT_OFF_SIZE]     = 0xFF;
    vendor[DYT_DYT_OFF_SIZE + 1] = 0xFF;
    sizecheck("size larger than the blob is rejected",
              dyt_dyt_blob_size(vendor, sizeof vendor), 0);

    /* A zero size is "absent", not "zero-length blob". */
    vendor[DYT_DYT_OFF_SIZE] = 0;
    vendor[DYT_DYT_OFF_SIZE + 1] = 0;
    sizecheck("zero size is absent", dyt_dyt_blob_size(vendor, sizeof vendor), 0);

    intcheck("init rejects a short blob",
             dyt_dyt_blob_init(blob, DYT_DYT_HDR_FIXED, 1, 1, 1, 0), -1);
    intcheck("init rejects NULL", dyt_dyt_blob_init(NULL, sizeof blob, 1, 1, 1, 0), -1);
    intcheck("init rejects a negative dimension",
             dyt_dyt_blob_init(blob, sizeof blob, -1, 1, 1, 0), -1);
    intcheck("geometry rejects NULL",
             dyt_dyt_blob_geometry(NULL, 0, &w, &ar, &tr, &fl), -1);
}

static void test_reject(void)
{
    uint8_t *jpg = NULL, *blob = NULL, *out = NULL;
    size_t   jlen = 0, ins = 0, blen = DYT_DYT_BLOB_SIZE, olen = 0;
    uint8_t  big[DYT_DYT_CHUNK_MAX + 8];

    printf("\n-- reject bad arguments --\n");

    jpg = make_jpeg(0, &jlen, &ins);
    blob = calloc(1, blen);
    if (!jpg || !blob) { fail("allocate", "out of memory"); goto out; }
    dyt_dyt_blob_init(blob, blen, 1, 1, 1, 0);

    intcheck("build NULL jpeg",  dyt_dyt_build(NULL, jlen, blob, blen, NULL, 0, &out, &olen), -1);
    intcheck("build NULL blob",  dyt_dyt_build(jpg, jlen, NULL, blen, NULL, 0, &out, &olen), -1);
    intcheck("build NULL out",   dyt_dyt_build(jpg, jlen, blob, blen, NULL, 0, NULL, &olen), -1);
    intcheck("build NULL len",   dyt_dyt_build(jpg, jlen, blob, blen, NULL, 0, &out, NULL), -1);
    intcheck("build zero jpeg",  dyt_dyt_build(jpg, 0, blob, blen, NULL, 0, &out, &olen), -1);
    intcheck("build zero blob",  dyt_dyt_build(jpg, jlen, blob, 0, NULL, 0, &out, &olen), -1);
    intcheck("build raw with no pointer",
             dyt_dyt_build(jpg, jlen, blob, blen, NULL, 16, &out, &olen), -1);

    /* A blob that cannot fit a segment length field must be refused, not
     * silently truncated. */
    intcheck("build oversized blob",
             dyt_dyt_build(jpg, jlen, big, sizeof big, NULL, 0, &out, &olen), -1);

    /* Not a JPEG: no SOI. */
    intcheck("build on a non-JPEG",
             dyt_dyt_build((const uint8_t *)"not a jpeg at all", 17,
                           blob, blen, NULL, 0, &out, &olen), -1);

    /* A chain that walks off the end. */
    {
        uint8_t bad[8] = { 0xFF, 0xD8, 0xFF, 0xE0, 0xFF, 0xF0, 0x00, 0x00 };
        intcheck("build on a broken chain",
                 dyt_dyt_build(bad, sizeof bad, blob, blen, NULL, 0,
                               &out, &olen), -1);
    }

    intcheck("read NULL path", dyt_dyt_read(NULL, NULL, NULL, NULL, NULL, NULL, NULL), -1);
    intcheck("read a missing file",
             dyt_dyt_read("build/dytjpeg_test_does_not_exist.jpg",
                          NULL, NULL, NULL, NULL, NULL, NULL), -1);
    intcheck("write NULL path",
             dyt_dyt_write(NULL, jpg, jlen, blob, blen, NULL, 0), -1);

out:
    free(jpg);
    free(blob);
    free(out);
}

/* A plain JPEG with no APP2 run is not a container, and must be reported as
 * such rather than yielding an empty payload. */
static void test_plain_jpeg_is_not_a_container(void)
{
    const char *path = "build/dytjpeg_test_plain.jpg";
    uint8_t *jpg = NULL, *blob = NULL, *raw = NULL;
    size_t   jlen = 0, ins = 0, blen = 0, rlen = 0;
    FILE    *f;

    printf("\n-- a plain JPEG is not a container --\n");

    jpg = make_jpeg(1, &jlen, &ins);
    if (!jpg) { fail("allocate", "out of memory"); return; }

    f = fopen(path, "wb");
    if (!f || fwrite(jpg, 1, jlen, f) != jlen) {
        if (f) fclose(f);
        fail("write a plain JPEG", "I/O error");
        free(jpg);
        return;
    }
    fclose(f);

    intcheck("read refuses it",
             dyt_dyt_read(path, &blob, &blen, &raw, &rlen, NULL, NULL), -1);

    remove(path);
    free(jpg);
    free(blob);
    free(raw);
}

/* A container written from a *vendor* blob: the vendor's 0x668-byte fixed part
 * and nothing else.  The port's reader has to accept it — it is a perfectly
 * valid container — and report the one thing the blob does not carry (the
 * thermal geometry) as absent rather than guessing it.  This is the fallback a
 * gallery needs: a still that can be displayed from its JPEG even when it
 * cannot be re-rendered thermally.
 *
 * The blob's own size field is what makes the container readable at all: it is
 * the raw-data offset, and it is the same field a vendor file carries. */
static void test_vendor_blob_container(const uint8_t *fixture, size_t fix_len)
{
    const char *path = "build/dytjpeg_test_vendor.jpg";
    uint8_t *jpg = NULL, *blob = NULL, *rblob = NULL, *rraw = NULL, *rjpg = NULL;
    size_t   jlen = 0, ins = 0;
    size_t   rblen = 0, rrlen = 0, rjlen = 0;
    int      w = 123, ar = 123, tr = 123;
    unsigned fl = 0x1234u;

    printf("\n-- a vendor blob (fixed part only, no extension) --\n");

    jpg = make_jpeg(1, &jlen, &ins);
    blob = calloc(1, DYT_DYT_HDR_FIXED);
    if (!jpg || !blob) { fail("allocate", "out of memory"); goto out; }

    /* The vendor's fixed part: only the size field is set, exactly as the
     * port's own blob_init leaves the vendor half. */
    blob[DYT_DYT_OFF_SIZE]     = (uint8_t)(DYT_DYT_HDR_FIXED & 0xFF);
    blob[DYT_DYT_OFF_SIZE + 1] = (uint8_t)(DYT_DYT_HDR_FIXED >> 8);

    if (dyt_dyt_write(path, jpg, jlen, blob, DYT_DYT_HDR_FIXED,
                      fixture, fix_len) != 0) {
        fail("write a vendor-blob container", "returned -1");
        goto out;
    }
    ok("the container writes");

    if (dyt_dyt_read(path, &rblob, &rblen, &rraw, &rrlen, &rjpg, &rjlen) != 0) {
        fail("the reader accepts it", "returned -1");
        goto out;
    }
    ok("the reader accepts a vendor blob");

    sizecheck("vendor blob length", rblen, DYT_DYT_HDR_FIXED);
    intcheck("vendor blob is byte-identical",
             rblen == DYT_DYT_HDR_FIXED && memcmp(rblob, blob, rblen) == 0, 1);
    intcheck("the payload is byte-identical",
             rrlen == fix_len && memcmp(rraw, fixture, fix_len) == 0, 1);
    intcheck("the JPEG is the original",
             rjlen == jlen && memcmp(rjpg, jpg, jlen) == 0, 1);

    /* The size field still reads — it is the raw-data offset, and it is what a
     * vendor file carries instead of an extension. */
    sizecheck("the size field still reads",
              dyt_dyt_blob_size(rblob, rblen), DYT_DYT_HDR_FIXED);

    /* The geometry is *absent*, and reported as such: the outputs must not be
     * touched, so a caller cannot mistake a stale value for a real one. */
    intcheck("no geometry record",
             dyt_dyt_blob_geometry(rblob, rblen, &w, &ar, &tr, &fl), 0);
    intcheck("the width is untouched", w, 123);
    intcheck("the active rows are untouched", ar, 123);
    intcheck("the total rows are untouched", tr, 123);
    intcheck("the flags are untouched", (int)fl, 0x1234);

    /* A vendor blob may be longer than the fixed part — shape geometry follows
     * at 0x668 — so the extension check must be an exact magic match, not "the
     * blob is long enough".  A near-miss byte pattern must not be mistaken for
     * the record. */
    {
        uint8_t  *vb = NULL, *rb = NULL, *rr = NULL;
        size_t    vblen = DYT_DYT_HDR_FIXED + 0x34, rbl = 0, rrl = 0;
        int       gw = 0, gar = 0, gtr = 0;
        unsigned  gfl = 0;

        vb = calloc(1, vblen);
        if (!vb) { fail("allocate", "out of memory"); goto out; }
        memcpy(vb, blob, DYT_DYT_HDR_FIXED);
        vb[DYT_DYT_OFF_SIZE]     = (uint8_t)(vblen & 0xFF);
        vb[DYT_DYT_OFF_SIZE + 1] = (uint8_t)(vblen >> 8);
        /* One rect's worth of shape data, whose first four bytes are the magic
         * plus one — the check has to be exact. */
        vb[DYT_DYT_EXT_OFF + 0] = 0x45;
        vb[DYT_DYT_EXT_OFF + 1] = 0x58;
        vb[DYT_DYT_EXT_OFF + 2] = 0x54;
        vb[DYT_DYT_EXT_OFF + 3] = 0x31;

        if (dyt_dyt_write(path, jpg, jlen, vb, vblen, fixture, fix_len) != 0) {
            fail("write a longer vendor blob", "returned -1");
            free(vb);
            goto out;
        }
        if (dyt_dyt_read(path, &rb, &rbl, &rr, &rrl, NULL, NULL) != 0) {
            fail("read the longer vendor blob", "returned -1");
            free(vb);
            goto out;
        }
        sizecheck("the longer blob round-trips", rbl, vblen);
        intcheck("the near-miss is not the magic",
                 dyt_dyt_blob_geometry(rb, rbl, &gw, &gar, &gtr, &gfl), 0);
        free(vb); free(rb); free(rr);
    }

out:
    remove(path);
    free(jpg); free(blob);
    free(rblob); free(rraw); free(rjpg);
}

int main(int argc, char **argv)
{
    const char *fixture_path = argc > 1 ? argv[1]
                                        : "testdata/mode1000_256x384_default.raw";
    uint8_t *fixture = NULL;
    size_t   fix_len = 0;

    printf("=== dytjpeg_test (the DYT still container) ===\n");

    /* The interesting boundaries: no payload, under one chunk, exactly one
     * chunk, one byte over (two chunks), and a genuine multi-chunk payload. */
    test_layout(0, 0);
    test_layout(0, 100);
    test_layout(2, DYT_DYT_CHUNK_MAX);
    test_layout(1, DYT_DYT_CHUNK_MAX + 1);
    test_layout(3, 3 * DYT_DYT_CHUNK_MAX + 17);

    test_blob_helpers();
    test_reject();
    test_plain_jpeg_is_not_a_container();

    fixture = load_file(fixture_path, &fix_len);
    if (!fixture) {
        printf("\n  FAIL cannot read the fixture %s\n", fixture_path);
        fails++;
    } else {
        test_roundtrip_file(fixture, fix_len);
        test_vendor_blob_container(fixture, fix_len);
    }
    free(fixture);

    printf("\n=== %s ===\n", fails ? "FAIL" : "ALL PASS");
    return fails ? 1 : 0;
}
