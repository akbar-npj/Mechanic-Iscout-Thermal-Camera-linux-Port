/*
 * visible.c — grayscale visible-plane extraction.  See visible.h.
 */
#include "visible.h"

int dyt_visible_extract(const uint16_t *payload, int width, int rows,
                        uint8_t *grey_out)
{
    const uint8_t *b;
    int n, i;

    if (!payload || !grey_out || width <= 0 || rows <= 0)
        return -1;

    /* YUYV packs two pixels per four bytes as `Y0 U Y1 V`, so the luma of
     * pixel i is byte 2i.  Read through a byte view rather than masking the
     * uint16 low byte: identical on a little-endian host, but it says what it
     * means instead of silently depending on the host's endianness. */
    b = (const uint8_t *)payload;
    n = width * rows;
    for (i = 0; i < n; i++)
        grey_out[i] = b[2 * i];

    return 0;
}

int dyt_visible_is_grey(const uint16_t *payload, int width, int rows)
{
    const uint8_t *b;
    int n, i;

    if (!payload || width <= 0 || rows <= 0)
        return -1;

    /* The chroma byte of pixel i is the second byte of its YUYV pair. */
    b = (const uint8_t *)payload;
    n = width * rows;
    for (i = 0; i < n; i++)
        if (b[2 * i + 1] != DYT_VISIBLE_CHROMA_NEUTRAL)
            return 0;

    return 1;
}

int dyt_visible_stats(const uint8_t *grey, int n, dyt_visible_stats_t *out)
{
    long sum = 0;
    int  lo = 255, hi = 0, i;

    if (!grey || !out || n <= 0)
        return -1;

    for (i = 0; i < n; i++) {
        int v = grey[i];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        sum += v;
    }

    out->min  = (uint8_t)lo;
    out->max  = (uint8_t)hi;
    out->mean = (double)sum / (double)n;
    out->n    = n;
    return 0;
}
