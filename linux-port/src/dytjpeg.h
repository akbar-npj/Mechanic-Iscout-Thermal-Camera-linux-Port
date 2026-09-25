/*
 * dytjpeg.h — the vendor's DYT still container: a JPEG with APP2 segments.
 *
 * A DYT `.jpg` is an ordinary JPEG with a run of APP2 (`FF E2`) segments
 * spliced in immediately after the APP0/APP1 header chain (RE Docs 06 §2.1):
 *
 *     FF D8 SOI
 *     FF E0 APP0                      <- the normal JFIF header
 *     FF E1 APP1 *                    <- any number, e.g. EXIF
 *     FF E2 <len> <DYT header blob>   <- inserted here
 *     FF E2 <len> <raw chunk 0>       <- the thermal payload, in pieces
 *     FF E2 <len> <raw chunk 1>
 *     ...
 *     <the rest of the original JPEG, untouched>
 *
 * The container is **plaintext**.  The "Aes" in libDYTJpegAes.so is the
 * serial-number allow-list (RE Docs 06 §3), not the image payload, so nothing
 * here needs a key.
 *
 * This module deliberately knows nothing about JPEG *coding*.  It takes a
 * finished JPEG and splices; producing one is jpeg.h's job.  That split is
 * what lets the container's exact bytes be tested with no codec at all.
 *
 * Nothing here talks to hardware, so it is fully covered by `make check`.
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -I. -c dytjpeg.c
 */
#ifndef DYT_DYTJPEG_H
#define DYT_DYTJPEG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- geometry --
 * The vendor's header blob is a fixed table of structs (RE Docs 06 §2.2).
 * Only its own size, a little-endian u16 at +0x06, is load-bearing for the
 * container: it is what makes the blob self-describing, because the raw
 * thermal data begins immediately after it. */
#define DYT_DYT_HDR_FIXED   0x668   /* fixed part, before any shape geometry */
#define DYT_DYT_OFF_SIZE    0x006   /* blob size, LE u16, inside the file head */
#define DYT_DYT_OFF_SHAPE   0x65C   /* 4 x u8 counts: points/lines/rects/polys */
#define DYT_DYT_OFF_SHAPEDATA 0x668 /* shape geometry starts here */

/* Segment payload cap.  A full chunk carries 0xFFFD bytes and is written with
 * the length field 0xFFFF (= 0xFFFD + 2, the two length bytes included). */
#define DYT_DYT_CHUNK_MAX   0xFFFD

/* --------------------------------------------------- the port's extension --
 * The vendor's metadata fields past the container bookkeeping are recovered
 * only as offsets, not as contents (RE Docs 06 §2.2), so the port cannot fill
 * them faithfully and will not invent values for them.
 *
 * Instead it appends a small record *after* the vendor's fixed header, still
 * inside the same blob, and points the size field at the whole thing.  The
 * vendor's getters read fixed offsets from the start of the blob, so a longer
 * blob is invisible to them; the port's reader uses the record to recover the
 * thermal geometry without guessing.
 *
 * Layout, all little-endian:
 *   +0x00  u32  magic ('D','X','T','1')
 *   +0x04  u16  width        thermal plane width in pixels
 *   +0x06  u16  active_rows  thermal plane height in pixels
 *   +0x08  u16  total_rows   payload rows, including any reference band
 *   +0x0A  u16  flags        DYT_DYT_FLAG_* below
 *   +0x0C  u32  reserved     0
 */
#define DYT_DYT_EXT_MAGIC   0x31545844u  /* "DXT1" read as LE */
#define DYT_DYT_EXT_OFF     DYT_DYT_HDR_FIXED
#define DYT_DYT_EXT_SIZE    0x10

#define DYT_DYT_FLAG_DUAL_HALF 0x0001u   /* the payload carries a visible half */

/* The whole blob the port writes: vendor fixed part + the extension. */
#define DYT_DYT_BLOB_SIZE   (DYT_DYT_HDR_FIXED + DYT_DYT_EXT_SIZE)

/* ------------------------------------------------------------ header blob -- */

/* Fill a fresh `n`-byte blob: zeroed, with the size field at +0x06 set to `n`
 * and the extension record populated.  `flags` is a DYT_DYT_FLAG_* mask.
 * `n` must be DYT_DYT_BLOB_SIZE (the vendor's fixed part plus the extension).
 * Returns 0, or -1 on a bad argument. */
int dyt_dyt_blob_init(uint8_t *blob, size_t n,
                      int width, int active_rows, int total_rows,
                      unsigned flags);

/* The blob size recorded at +0x06, or 0 when it is absent or out of range. */
uint16_t dyt_dyt_blob_size(const uint8_t *blob, size_t blob_len);

/* Read the port's extension record.  Returns 1 when the record is present and
 * well formed (the outputs are then filled), 0 when it is not, -1 on a bad
 * argument.  A blob written by the vendor, which has no record, returns 0 and
 * leaves the outputs untouched. */
int dyt_dyt_blob_geometry(const uint8_t *blob, size_t blob_len,
                          int *width, int *active_rows, int *total_rows,
                          unsigned *flags);

/* --------------------------------------------------------------- container -- */

/* Splice the header blob and the raw thermal payload into `jpeg` as APP2
 * segments, producing the container in a malloc'd buffer (*out, *out_len).
 *
 * The insertion point is the first segment marker after the leading APP0/APP1
 * chain — where the vendor's own writer puts it for a JFIF-ordered file, which
 * is what both encoders here produce.
 *
 * `raw` may be NULL when `raw_len` is 0.  Returns 0, or -1 on a bad argument
 * (including a JPEG with no valid segment chain, or a blob too large for a
 * segment length field). */
int dyt_dyt_build(const uint8_t *jpeg, size_t jpeg_len,
                  const uint8_t *blob, size_t blob_len,
                  const uint8_t *raw,  size_t raw_len,
                  uint8_t **out, size_t *out_len);

/* dyt_dyt_build() written straight to `path`.  Returns 0 or -1. */
int dyt_dyt_write(const char *path,
                  const uint8_t *jpeg, size_t jpeg_len,
                  const uint8_t *blob, size_t blob_len,
                  const uint8_t *raw,  size_t raw_len);

/* Read a container back.  The first APP2 payload is the header blob and the
 * remaining ones are concatenated into the raw thermal payload.
 *
 * `blob`/`raw` and their length outputs are optional (pass NULL to skip one);
 * whatever is returned is malloc'd and the caller frees it.  `jpeg` optionally
 * receives a copy of the container with every APP2 segment removed — the
 * original image — which is what a viewer needs to display it.
 *
 * Returns 0, or -1 on a bad argument, an unreadable file, or a file that is
 * not a DYT container. */
int dyt_dyt_read(const char *path,
                 uint8_t **blob, size_t *blob_len,
                 uint8_t **raw,  size_t *raw_len,
                 uint8_t **jpeg, size_t *jpeg_len);

#ifdef __cplusplus
}
#endif

#endif /* DYT_DYTJPEG_H */
