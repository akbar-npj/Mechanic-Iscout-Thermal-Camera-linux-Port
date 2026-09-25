/*
 * visible.h — the grayscale visible plane of the DYT camera's dual-half frame.
 *
 * In its default output mode the device streams a 256x384 payload whose
 * bottom half is the thermal plane and whose **top half is a grayscale
 * visible picture** (RE Docs 04 §4.10).  This module turns that top half into
 * a plain 8-bit grey plane so the fusion module can combine it with the
 * thermal render.
 *
 * The top half is YUYV: each pixel is a byte pair `Y U` / `Y V`, and the
 * device sends it with the chroma neutral (U = V = 0x80), so the luma is
 * simply the **first byte of each pair**.  Read as the port's usual
 * little-endian uint16 payload that is the *low* byte of every sample, the
 * high byte being a constant 0x80 — measured on the frozen fixture, every odd
 * byte of the top half is exactly 0x80 (RE Docs 04 §4.10).
 *
 * Nothing here talks to hardware, so it is fully covered by `make check`.
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -I. -c visible.c
 */
#ifndef DYT_VISIBLE_H
#define DYT_VISIBLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The chroma byte the device uses for a neutral (colourless) pixel. */
#define DYT_VISIBLE_CHROMA_NEUTRAL 0x80

/* Extract the luma of the first `rows` payload rows into `grey_out`
 * (width*rows bytes).  `payload` holds width*total uint16 samples; only the
 * first width*rows are read, so a caller can pass the whole frame and the
 * number of visible rows.
 *
 * Returns 0 on success, -1 on a NULL pointer or a non-positive width/rows. */
int dyt_visible_extract(const uint16_t *payload, int width, int rows,
                        uint8_t *grey_out);

/* Does the first `rows` rows really look like a neutral-chroma visible
 * picture?  True when every chroma byte (the odd byte of each YUYV pair)
 * equals DYT_VISIBLE_CHROMA_NEUTRAL.  A front-end uses this to decide
 * whether fusion is meaningful: if the device ever put something else in the
 * top half, fusing it would silently produce nonsense rather than fail.
 *
 * Returns 1 (grey), 0 (not grey), or -1 on a bad argument. */
int dyt_visible_is_grey(const uint16_t *payload, int width, int rows);

/* Summary of a grey plane, for tests and the viewer's status line. */
typedef struct {
    uint8_t min, max;
    double  mean;
    int     n;      /* pixels summarised */
} dyt_visible_stats_t;

/* Fill *out from `n` grey bytes.  Returns 0, or -1 on a bad argument. */
int dyt_visible_stats(const uint8_t *grey, int n, dyt_visible_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DYT_VISIBLE_H */
