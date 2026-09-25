/*
 * jpeg.h — the one JPEG codec the DYT still writer needs, behind a tiny
 * interface so the container in dytjpeg.c stays codec-free.
 *
 * The DYT container (RE Docs 06 §2.1) is an ordinary JPEG with APP2 segments
 * spliced into it.  Splicing needs no codec at all — only *producing* the JPEG
 * does — so the container is tested without one and this module is tested on
 * its own.
 *
 * Two backends, exactly one compiled in:
 *
 *   - **stb** (default).  Vendored in third_party/stb, so a bare compiler with
 *     no image libraries still gets a working encoder and `make check` stays
 *     green.  This is the same trade the project made for libuvc.
 *   - **libjpeg** (optional, `DYT_HAVE_LIBJPEG`).  Detected like
 *     `DYT_HAVE_LIBUSB`; an accelerator, never a requirement.
 *
 * JPEG is lossy, so a round trip is compared with a tolerance rather than
 * byte-for-byte (see jpeg_test.c).  The container test, by contrast, compares
 * the raw thermal payload exactly — which is why the two concerns are split.
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -I. -Ithird_party/stb -c jpeg.c
 */
#ifndef DYT_JPEG_H
#define DYT_JPEG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DYT_JPEG_NONE    = 0,   /* no codec compiled in (not expected: stb ships) */
    DYT_JPEG_STB     = 1,
    DYT_JPEG_LIBJPEG = 2
} dyt_jpeg_backend_t;

/* Which backend this build selected.  Never DYT_JPEG_NONE in a normal build,
 * because stb is vendored; the value exists so callers can report it. */
dyt_jpeg_backend_t dyt_jpeg_backend(void);

/* "stb" or "libjpeg" (or "none").  Static storage. */
const char *dyt_jpeg_backend_name(void);

/* Encode an interleaved RGB8 buffer (w*h*3 bytes) as baseline JPEG.
 * `quality` is 1..100 (the vendor app writes 85).  On success *out receives a
 * malloc'd buffer the caller frees, and *out_len its size.
 * Returns 0, or -1 on a bad argument, a missing codec, or encoder failure. */
int dyt_jpeg_encode(const uint8_t *rgb, int w, int h, int quality,
                    uint8_t **out, size_t *out_len);

/* Decode a JPEG to a malloc'd interleaved RGB8 buffer (*w x *h), which the
 * caller frees.  Returns 0, or -1 on a bad argument, a missing codec, or a
 * decode failure. */
int dyt_jpeg_decode(const uint8_t *jpeg, size_t len,
                    uint8_t **rgb, int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif /* DYT_JPEG_H */
