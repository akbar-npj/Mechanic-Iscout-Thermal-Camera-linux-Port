/*
 * sr.h — the super-resolution *policy*, with no runtime and no device.
 *
 * The 2x zoom model (models/zoom2.mnn) is a plain 8-bit-plane upscaler.  The
 * vendor's own per-frame code proves what it eats and emits
 * (mnn_runtime.cpp, pinned against libmnnmodel.so's `sr2`):
 *
 *   input   reads in[i*2]  — the LOW byte of each little-endian uint16 sample,
 *           normalised by 255;
 *   output  writes (uint16_t)(f + 32768.0f) — so byte 2i is the upscaled
 *           8-bit value and byte 2i+1 is a constant 0x80.
 *
 * That bias is not arbitrary: it restores the high byte of the *visible*
 * plane, which is YUYV with neutral chroma — low byte = luma, high byte =
 * 0x80 (visible.h:10-15).  So the model's packing and the visible plane's
 * packing are the same thing, which is why the vendor feeds `onYUVtoJava`.
 *
 * It also means the model is a generic 8-bit-plane 2x upscaler, and this
 * module can serve both planes the port offers:
 *
 *   DYT_SR_VISIBLE  the vendor's plane — the dual-half grey picture, [V];
 *   DYT_SR_THERMAL  a documented extension — the thermal picture encoded
 *                   through the *display* range, [I].
 *
 * The thermal plane cannot be fed raw: it is 14-bit (mode 1000 reads ~19000
 * at 25 C), so its low byte is not a picture at all.  The extension maps
 * temperatures to 8 bits with the same normalisation the palette uses
 * (dyt_palette_index), so the super-resolved picture is the one the user was
 * already looking at, only larger.
 *
 * Everything here is a pure buffer transform, so `make check` covers it with
 * no model, no runtime and no camera (sr_test.c).  The model call itself
 * stays behind mnn.h; the session owns the buffers and the lock.
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -I. -c sr.c
 */
#ifndef DYT_SR_H
#define DYT_SR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which plane the model upscales.  OFF is the start-up state and the value a
 * build without a runtime must always report. */
typedef enum {
    DYT_SR_OFF     = 0,
    DYT_SR_VISIBLE = 1,   /* the vendor's plane (dual-half grey picture) */
    DYT_SR_THERMAL = 2,   /* the documented extension (display-domain grey) */
    DYT_SR_N       = 3
} dyt_sr_t;

/* Stable short name for the status line: "off", "visible", "thermal".  Never
 * NULL; an unknown value yields "?". */
const char *dyt_sr_name(dyt_sr_t m);

/* The high byte of a packed sample.  It is the visible plane's neutral chroma
 * and the value the model's +32768 bias reproduces, so packing with it and
 * unpacking a real model output agree byte for byte. */
#define DYT_SR_PACK_HIGH 0x80

/* The 8-bit value a non-physical (NaN) sample maps to, matching
 * dyt_render_rgb()'s grey for a pixel with no reading. */
#define DYT_SR_NAN_GREY  0x80

/* Temperatures -> the 8-bit plane the model upscales, using the display range:
 *
 *     g = clamp((t - lo) / (hi - lo), 0, 1) * 255
 *
 * which is exactly dyt_palette_index()'s normalisation, so feeding the result
 * back through dyt_sr_grey_to_temps() and dyt_render_rgb() reproduces the
 * palette entry the temperature had.  A NaN sample becomes DYT_SR_NAN_GREY.
 * A degenerate range (hi <= lo) has no scale to normalise against, so every
 * sample becomes DYT_SR_NAN_GREY rather than a division by zero.
 *
 * Returns 0, or -1 on a NULL pointer or non-positive n. */
int dyt_sr_thermal_grey(const float *temps, int n, float lo, float hi,
                        uint8_t *out);

/* The inverse of the normalisation above, so a super-resolved grey plane can
 * be rendered by the existing dyt_render_rgb():
 *
 *     t = lo + (g / 255) * (hi - lo)
 *
 * A degenerate range yields `lo` (there is no span to spread over).
 *
 * Returns 0, or -1 on a NULL pointer or non-positive n. */
int dyt_sr_grey_to_temps(const uint8_t *grey, int n, float lo, float hi,
                         float *out);

/* grey (n samples) -> the model's packed input (2*n bytes): the grey at every
 * even offset, DYT_SR_PACK_HIGH at every odd one.  Returns 0, or -1 on a bad
 * argument. */
int dyt_sr_pack(const uint8_t *grey, int n, uint8_t *out);

/* The model's packed output -> a plain grey plane: out[i] = packed[i*2].
 * Returns 0, or -1 on a bad argument. */
int dyt_sr_unpack(const uint8_t *packed, int n, uint8_t *out);

/* Nearest-neighbour 2x of an 8-bit plane (the channel the model did *not*
 * upscale, so the two planes fused at 2x are the same size).  `out` holds
 * (2w)*(2h) bytes.  Returns 0, or -1 on a bad argument. */
int dyt_sr_nearest2(const uint8_t *in, int w, int h, uint8_t *out);

/* Nearest-neighbour 2x of a float plane (the thermal plane in the
 * visible-channel mode).  `out` holds (2w)*(2h) floats.  Returns 0, or -1 on
 * a bad argument. */
int dyt_sr_nearest2_f(const float *in, int w, int h, float *out);

#ifdef __cplusplus
}
#endif

#endif /* DYT_SR_H */
