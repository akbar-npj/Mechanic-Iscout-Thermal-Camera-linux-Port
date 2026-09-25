/*
 * dytjpeg.c — the DYT still container (dytjpeg.h).
 *
 * The byte layout is recovered from the vendor's own writer and reader,
 * `D_updateData` @ 0x129d7c and `D_jpegOpen` @ 0x12b464 in
 * libDYTJpegAes.so (RE Docs 06 §2.1).  Three details are easy to get wrong and
 * are pinned by dytjpeg_test.c rather than trusted:
 *
 *  1. **The length fields are big-endian.**  The vendor builds them with
 *     CONCAT11(high, low) — the JPEG convention, not the blob's own
 *     little-endian size field.
 *  2. **A full chunk is written as 0xFFFF, not 0xFFFD + 2.**  They are equal
 *     (0xFFFD + 2 = 0xFFFF), but the vendor writes the literal 0xFFFF, so a
 *     reader that expects the sum still agrees.  A *partial* chunk writes the
 *     real length.
 *  3. **The insertion point is the first segment after the APP0/APP1 chain**,
 *     so the raw thermal data sits before the DQT/SOF of the JPEG rather than
 *     at the end of the file.
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -I. -c dytjpeg.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dytjpeg.h"

/* ------------------------------------------------------------- small reads -- */

static unsigned read_be16(const uint8_t *p)
{
    return ((unsigned)p[0] << 8) | (unsigned)p[1];
}

static void write_be16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)((v >> 8) & 0xFFu);
    p[1] = (uint8_t)(v & 0xFFu);
}

static unsigned read_le16(const uint8_t *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned read_le32(const uint8_t *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static void write_le16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void write_le32(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* ------------------------------------------------------------ the JPEG scan -- */

/* Where the APP2 run belongs: the offset of the first segment marker after the
 * leading APP0/APP1 chain.  A file with no APPn segments at all yields 2 (just
 * after SOI), which is still a valid place to splice.
 *
 * Returns 0 on success, -1 if this is not a walkable JPEG segment chain. */
static int find_insertion(const uint8_t *d, size_t n, size_t *out)
{
    size_t pos = 2;

    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8)
        return -1;                          /* no SOI */

    for (;;) {
        int    marker;
        size_t len;

        if (pos + 2 > n)
            return -1;
        if (d[pos] != 0xFF)
            return -1;                      /* lost the segment chain */
        marker = d[pos + 1];

        if (marker != 0xE0 && marker != 0xE1)
            break;                          /* end of the header chain */

        if (pos + 4 > n)
            return -1;
        len = read_be16(d + pos + 2);
        if (len < 2 || pos + 2 + len > n)
            return -1;
        pos += 2 + len;                     /* advances >= 4: terminates */
    }

    *out = pos;
    return 0;
}

/* ------------------------------------------------------------- header blob -- */

int dyt_dyt_blob_init(uint8_t *blob, size_t n,
                      int width, int active_rows, int total_rows,
                      unsigned flags)
{
    if (!blob || n != DYT_DYT_BLOB_SIZE)
        return -1;
    if (width < 0 || active_rows < 0 || total_rows < 0)
        return -1;
    if (width > 0xFFFF || active_rows > 0xFFFF || total_rows > 0xFFFF)
        return -1;

    memset(blob, 0, n);

    /* The one field the container itself depends on: the blob's own size, so
     * that the raw thermal data begins immediately after it. */
    write_le16(blob + DYT_DYT_OFF_SIZE, (unsigned)n);

    write_le32(blob + DYT_DYT_EXT_OFF + 0x00, DYT_DYT_EXT_MAGIC);
    write_le16(blob + DYT_DYT_EXT_OFF + 0x04, (unsigned)width);
    write_le16(blob + DYT_DYT_EXT_OFF + 0x06, (unsigned)active_rows);
    write_le16(blob + DYT_DYT_EXT_OFF + 0x08, (unsigned)total_rows);
    write_le16(blob + DYT_DYT_EXT_OFF + 0x0A, flags & 0xFFFFu);
    write_le32(blob + DYT_DYT_EXT_OFF + 0x0C, 0);

    return 0;
}

uint16_t dyt_dyt_blob_size(const uint8_t *blob, size_t blob_len)
{
    unsigned v;

    if (!blob || blob_len < DYT_DYT_OFF_SIZE + 2)
        return 0;

    v = read_le16(blob + DYT_DYT_OFF_SIZE);
    if (v == 0 || (size_t)v > blob_len)
        return 0;                           /* absent, or larger than we hold */

    return (uint16_t)v;
}

int dyt_dyt_blob_geometry(const uint8_t *blob, size_t blob_len,
                          int *width, int *active_rows, int *total_rows,
                          unsigned *flags)
{
    if (!blob)
        return -1;

    /* A vendor blob has no record here; the magic is what tells them apart. */
    if (blob_len < (size_t)DYT_DYT_EXT_OFF + DYT_DYT_EXT_SIZE)
        return 0;
    if (read_le32(blob + DYT_DYT_EXT_OFF) != DYT_DYT_EXT_MAGIC)
        return 0;

    if (width)       *width       = (int)read_le16(blob + DYT_DYT_EXT_OFF + 0x04);
    if (active_rows) *active_rows = (int)read_le16(blob + DYT_DYT_EXT_OFF + 0x06);
    if (total_rows)  *total_rows  = (int)read_le16(blob + DYT_DYT_EXT_OFF + 0x08);
    if (flags)       *flags       = read_le16(blob + DYT_DYT_EXT_OFF + 0x0A);

    return 1;
}

/* --------------------------------------------------------------- container -- */

int dyt_dyt_build(const uint8_t *jpeg, size_t jpeg_len,
                  const uint8_t *blob, size_t blob_len,
                  const uint8_t *raw,  size_t raw_len,
                  uint8_t **out, size_t *out_len)
{
    size_t   ins, nchunks, total, off, done;
    uint8_t *buf;

    if (!jpeg || !blob || !out || !out_len)
        return -1;
    if (jpeg_len == 0 || blob_len == 0)
        return -1;
    if (blob_len > DYT_DYT_CHUNK_MAX)
        return -1;                          /* the length field is a u16 */
    if (raw_len != 0 && !raw)
        return -1;

    if (find_insertion(jpeg, jpeg_len, &ins) != 0)
        return -1;

    nchunks = raw_len / DYT_DYT_CHUNK_MAX;
    if (raw_len % DYT_DYT_CHUNK_MAX)
        nchunks++;

    total = jpeg_len + 4 + blob_len + nchunks * 4 + raw_len;
    buf = malloc(total);
    if (!buf)
        return -1;

    off = 0;

    /* everything before the insertion point */
    memcpy(buf + off, jpeg, ins);
    off += ins;

    /* the header blob, as one APP2 segment */
    buf[off + 0] = 0xFF;
    buf[off + 1] = 0xE2;
    write_be16(buf + off + 2, (unsigned)(blob_len + 2));
    memcpy(buf + off + 4, blob, blob_len);
    off += 4 + blob_len;

    /* the raw payload, in APP2 chunks */
    done = 0;
    while (done < raw_len) {
        size_t rem  = raw_len - done;
        size_t take = rem < DYT_DYT_CHUNK_MAX + 1 ? rem : DYT_DYT_CHUNK_MAX;

        buf[off + 0] = 0xFF;
        buf[off + 1] = 0xE2;
        if (rem < DYT_DYT_CHUNK_MAX + 1)
            write_be16(buf + off + 2, (unsigned)(take + 2));
        else
            write_be16(buf + off + 2, 0xFFFFu);   /* full chunk, as the vendor */

        memcpy(buf + off + 4, raw + done, take);
        off  += 4 + take;
        done += take;
    }

    /* the rest of the JPEG, untouched */
    memcpy(buf + off, jpeg + ins, jpeg_len - ins);
    off += jpeg_len - ins;

    *out     = buf;
    *out_len = off;                         /* == total */
    return 0;
}

int dyt_dyt_write(const char *path,
                  const uint8_t *jpeg, size_t jpeg_len,
                  const uint8_t *blob, size_t blob_len,
                  const uint8_t *raw,  size_t raw_len)
{
    uint8_t *buf = NULL;
    size_t   n   = 0;
    FILE    *f;

    if (!path)
        return -1;
    if (dyt_dyt_build(jpeg, jpeg_len, blob, blob_len, raw, raw_len,
                      &buf, &n) != 0)
        return -1;

    f = fopen(path, "wb");
    if (!f) {
        free(buf);
        return -1;
    }
    if (n && fwrite(buf, 1, n, f) != n) {
        fclose(f);
        free(buf);
        return -1;
    }
    free(buf);
    return fclose(f) == 0 ? 0 : -1;
}

/* ----------------------------------------------------------------- reading -- */

static int read_file(const char *path, uint8_t **out, size_t *n)
{
    FILE    *f = fopen(path, "rb");
    long     sz;
    uint8_t *b;

    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    rewind(f);

    b = malloc(sz ? (size_t)sz : 1);
    if (!b) { fclose(f); return -1; }
    if (sz && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        fclose(f);
        return -1;
    }
    fclose(f);

    *out = b;
    *n   = (size_t)sz;
    return 0;
}

int dyt_dyt_read(const char *path,
                 uint8_t **blob, size_t *blob_len,
                 uint8_t **raw,  size_t *raw_len,
                 uint8_t **jpeg, size_t *jpeg_len)
{
    uint8_t *file = NULL, *bbuf = NULL, *rbuf = NULL, *jbuf = NULL;
    size_t   n = 0, ins, pos, blen = 0, rlen = 0;
    int      rc = -1;

    if (!path)
        return -1;
    if (read_file(path, &file, &n) != 0)
        return -1;

    if (find_insertion(file, n, &ins) != 0)
        goto out;

    /* Walk the APP2 run: the first payload is the blob, the rest is the raw
     * thermal data.  This is the inverse of dyt_dyt_build() and matches what
     * the vendor's D_jpegOpen concatenates. */
    pos = ins;
    for (;;) {
        size_t seg, plen;
        const uint8_t *pay;

        if (pos + 2 > n || file[pos] != 0xFF || file[pos + 1] != 0xE2)
            break;                          /* end of the APP2 run */
        if (pos + 4 > n)
            goto out;
        seg = read_be16(file + pos + 2);
        if (seg < 2 || pos + 2 + seg > n)
            goto out;

        pay  = file + pos + 4;
        plen = seg - 2;

        if (!bbuf) {
            bbuf = malloc(plen ? plen : 1);
            if (!bbuf)
                goto out;
            memcpy(bbuf, pay, plen);
            blen = plen;
        } else {
            uint8_t *t = realloc(rbuf, rlen + plen);
            if (!t)
                goto out;
            rbuf = t;
            memcpy(rbuf + rlen, pay, plen);
            rlen += plen;
        }
        pos += 2 + seg;
    }

    if (!bbuf)
        goto out;                           /* no APP2 segment: not a DYT file */

    /* The original image, with the whole APP2 run cut out. */
    if (jpeg) {
        size_t jn = ins + (n - pos);

        jbuf = malloc(jn ? jn : 1);
        if (!jbuf)
            goto out;
        memcpy(jbuf, file, ins);
        memcpy(jbuf + ins, file + pos, n - pos);
        *jpeg     = jbuf;
        *jpeg_len = jn;
        jbuf      = NULL;
    }

    if (blob) { *blob = bbuf; *blob_len = blen; bbuf = NULL; }
    if (raw)  { *raw  = rbuf; *raw_len  = rlen; rbuf = NULL; }
    rc = 0;

out:
    free(file);
    free(bbuf);
    free(rbuf);
    free(jbuf);
    return rc;
}
