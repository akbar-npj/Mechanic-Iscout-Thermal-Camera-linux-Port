/*
 * imgwrite.h — minimal image writers.
 *
 * PPM is the dependency-free one, on purpose: it is what the render core's
 * byte-for-byte golden comparisons in `make check` use, and it must keep
 * working on a machine with no image libraries at all.
 *
 * PNG goes through the vendored stb (third_party/stb), which is source rather
 * than a system library, so it too is available on every build.  It exists so
 * the port's own tools do not need OpenCV just to write a picture — see
 * tools/dytview.cpp, whose headless `--png` path uses it.
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

/* Write an interleaved RGB8 buffer (w*h*3 bytes) as a PNG (8-bit truecolour,
 * no alpha).  Returns 0 on success, -1 on a bad argument or I/O error.
 * There is no PNG *reader* here: nothing in the port needs to read one. */
int dyt_write_png(const char *path, const uint8_t *rgb, int w, int h);

#ifdef __cplusplus
}
#endif

#endif /* DYT_IMGWRITE_H */
