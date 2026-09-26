/*
 * palette.h — colour palettes and the temperature→RGB render core.
 *
 * This is the device-independent half of the viewer: it turns a temperature
 * image into pixels, with no GUI, camera or image-library dependency.  That
 * keeps it unit-testable (see palette_test.c) and lets any front-end — the
 * OpenCV viewer today, a GTK front-end later — share exactly one renderer.
 *
 * Palette format (RE Docs 06 §1.1-1.2, verified against the vendor loader
 * CreateBitmap.toByteArray()): 768 bytes = 256 entries × 3 bytes, R then G
 * then B, no header.  **Index 0 is the coldest colour, 255 the hottest** —
 * the vendor loader's `255 - i3` write index is a bitmap-row flip, not a
 * ramp inversion, so do not invert it again.  The vendor's own six palettes
 * are tracked in linux-port/palettes/.
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -I. -c palette.c
 */
#ifndef DYT_PALETTE_H
#define DYT_PALETTE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DYT_PALETTE_N     256            /* entries per palette */
#define DYT_PALETTE_BYTES (DYT_PALETTE_N * 3)

/* The number of palettes the vendor's Android app exposes (RE Docs 06 §1.4).
 * This is also the count of built-in stand-in ramps, so it stays 6. */
#define DYT_PALETTE_BUILTIN_N 6

/* Storage bound for a loaded palette set — a compile-time array dimension,
 * so it is deliberately not the current count.  linux-port/palettes/ ships
 * 28 distinct ramps: the Android app's 6, plus the 22 Windows installer
 * palettes that are not byte-identical to one of those (the Windows set has
 * 27, but 5 duplicate an Android one).  See palettes/README.md. */
#define DYT_PALETTE_MAX 32

/* Storage bound for a palette's name (the file's basename, or a built-in's
 * stand-in name).  Named rather than repeated as a literal so the palette's
 * own field and the session's alias array cannot drift apart — an alias longer
 * than this is truncated, not overflowed. */
#define DYT_PALETTE_NAME_MAX 32

typedef struct {
    uint8_t rgb[DYT_PALETTE_BYTES];      /* 256 × R,G,B; index 0 = coldest */
    char    name[DYT_PALETTE_NAME_MAX];  /* for window titles / --list */
} dyt_palette_t;

/* Load a vendor palette file.  Requires exactly DYT_PALETTE_BYTES and no
 * trailing data, so a wrong path or a truncated asset is rejected rather
 * than silently zero-filled.  Returns 0 on success, -1 on failure. */
int dyt_palette_load(dyt_palette_t *p, const char *path);

/* Load every *.dat palette in `dir` into out[0..max-1], in name order, so
 * the front-end gets a stable palette list without hard-coding filenames.
 * Files that are not exactly DYT_PALETTE_BYTES are skipped rather than
 * rejected, so an unrelated .dat in the directory cannot break the load.
 * Returns the number loaded (possibly 0), or -1 if `dir` is unreadable.
 * The caller should fall back to dyt_palette_builtin() when this returns 0. */
int dyt_palette_load_dir(dyt_palette_t *out, int max, const char *dir);

/* Fill *p with one of the built-in ramps, 0..DYT_PALETTE_BUILTIN_N-1.
 * These are small stand-ins so the viewer still works when the vendor assets
 * are absent; the real files in linux-port/palettes/ are preferred.  The
 * iron-red ramp is interpolated through the sampled values of the vendor
 * 1.dat given in RE Docs 06 §1.2, so it is a close approximation, not a copy.
 * Returns 0, or -1 if id is out of range. */
int dyt_palette_builtin(dyt_palette_t *p, int id);

/* Name of a built-in ramp, e.g. "iron-red".  NULL if id is out of range. */
const char *dyt_palette_builtin_name(int id);

/* Map a temperature to a palette index in [0, DYT_PALETTE_N-1]:
 *
 *     idx = clamp((t - lo) / (hi - lo), 0, 1) * (DYT_PALETTE_N - 1)
 *
 * **The vendor's own normalisation is not documented** — RE Docs 06 §1 shows
 * that an automatic and a fixed display range both exist
 * (nativeRenderTempRangeChange / nativeLockRenderTempRange, and the Windows
 * config keys FixedTempMin=0 / FixedTempMax=80) but never states the formula.
 * This is the port's documented choice; index 0 = coldest matches the
 * palette's own ordering.
 *
 * Total by construction: a degenerate range (hi <= lo) returns the midpoint
 * rather than dividing by zero, and NaN maps to 0. */
int dyt_palette_index(float t, float lo, float hi);

/* Render a temperature image to interleaved RGB8.
 * `temps` is w*h floats row-major; `out_rgb` receives w*h*3 bytes.
 * Pixels outside [lo, hi] clamp to the end colours.  NaN — which the
 * thermometry pipeline uses for non-physical pixels — is drawn as mid-grey
 * so it is visually distinct from a genuinely cold reading.
 * Returns 0 on success, -1 on a bad argument. */
int dyt_render_rgb(const float *temps, int w, int h,
                   const dyt_palette_t *p, float lo, float hi,
                   uint8_t *out_rgb);

/* Min/max over a temperature image, skipping NaN.  Returns 0 if at least one
 * finite sample was seen (writing the lo and hi outputs), -1 if empty or
 * all-NaN.  Either output pointer may be NULL. */
int dyt_render_minmax(const float *temps, int n, float *lo, float *hi);

#ifdef __cplusplus
}
#endif

#endif /* DYT_PALETTE_H */
