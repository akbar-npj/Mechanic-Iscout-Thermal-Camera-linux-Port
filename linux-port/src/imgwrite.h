/*
 * imgwrite.h — minimal image writers.
 *
 * PPM only, on purpose: it is dependency-free, so the render core can be
 * verified byte-for-byte in `make check` on a machine with no image
 * libraries.  The viewer writes PNG through OpenCV instead, which is why
 * there is no PNG writer here.
 */
#ifndef DYT_IMGWRITE_H
#define DYT_IMGWRITE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Write an interleaved RGB8 buffer (w*h*3 bytes) as a binary PPM (P6).
 * Returns 0 on success, -1 on a bad argument or I/O error. */
int dyt_write_ppm(const char *path, const uint8_t *rgb, int w, int h);

/* Read a binary PPM (P6, maxval 255) back.  The width and height outputs
 * receive the dimensions and the rgb output a malloc'd w*h*3 buffer the
 * caller must free.  Returns 0 on success.
 * Intended for tests and for the `--check` golden comparison. */
int dyt_read_ppm(const char *path, uint8_t **rgb, int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif /* DYT_IMGWRITE_H */
