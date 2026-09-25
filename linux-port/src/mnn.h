/*
 * mnn.h — the optional super-resolution seam (engine Phase 8).
 *
 * The vendor ships a 2x zoom model, recovered into models/zoom2.mnn (see
 * models/README.md for the mechanism and the shape contract).  Running it needs
 * an MNN runtime.  This port does not require one and the development host has
 * none, so this module is deliberately three things and nothing more:
 *
 *   1. the shape contract, as data — so the geometry is pinned in one place and
 *      asserted by a test rather than repeated as literals;
 *   2. a validator for the extracted model file, which needs no runtime at all
 *      (a flatbuffer's structure and names can be checked by hand);
 *   3. an upscale call that reports "unavailable" when no runtime is linked.
 *
 * It is **optional and non-default**.  Nothing in the engine calls it unless a
 * front-end asks, so a build that never touches this module behaves exactly the
 * same — which is the plan's requirement for Phase 8.
 *
 * The runtime call is the one piece deliberately left out: this host has no MNN
 * runtime, and unbuildable code is worse than a documented gap.  mnn.c records
 * the exact sequence to add (it is six calls, taken from the vendor's own sr1).
 *
 * Note what this module does *not* claim: the model has never been executed
 * here, so its numeric behaviour is unverified (models/README.md says so).
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -I. -c mnn.c
 */
#ifndef DYT_MNN_H
#define DYT_MNN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returned by dyt_mnn_zoom2() when the build has no MNN runtime.  Distinct
 * from a plain -1 so a caller can tell "this build cannot do it" from "your
 * arguments are wrong". */
#define DYT_MNN_UNAVAILABLE (-100)

/* The 2x zoom contract, as data.
 *
 * Every number is derived in models/README.md, from three sources that agree:
 * the dims vector sr1 passes to MNN::Interpreter::resizeTensor ([1,1,192,256]),
 * the 393216-byte output buffer the app allocates, and the DepthToSpace block
 * size of 2 in the graph.  Sizes are in bytes, at two bytes per sample, which
 * is what the app's own buffer arithmetic implies. */
typedef struct {
    int in_w, in_h;          /* 256 x 192 */
    int out_w, out_h;        /* 512 x 384 */
    int factor;              /* 2 — the DepthToSpace block size */
    int in_samples;          /* in_w * in_h */
    int out_samples;         /* out_w * out_h */
    int in_bytes;            /* in_samples  * 2 */
    int out_bytes;           /* out_samples * 2 — 393216, the app's buffer */
} dyt_mnn_contract_t;

/* The contract above.  Never NULL. */
const dyt_mnn_contract_t *dyt_mnn_contract(void);

/* 1 when this build can actually run the model, 0 otherwise.  Always 0 in this
 * configuration, since no MNN runtime is linked. */
int dyt_mnn_available(void);

/* Validate an image of the extracted model (models/zoom2.mnn).
 *
 * Structural only — it does not run anything, so it works on a host with no MNN:
 * it checks that the flatbuffer's root offset is sane and that the identifiers
 * the app's model carries (its asset UUID, the image_input / image_output
 * tensors, and representative op names) are present.  That is enough to catch a
 * truncated file, a wrong decryption key, or a different model.
 *
 * Returns 0 if it looks like the expected model, -1 if not (reason on stderr),
 * -2 on a NULL argument or a buffer too short to be anything. */
int dyt_mnn_model_check(const uint8_t *data, size_t n);

/* Load the model from `path` and release it.  Both are no-ops here: without a
 * runtime there is nothing to load into, so dyt_mnn_load() reports
 * DYT_MNN_UNAVAILABLE rather than pretending.  Safe to call repeatedly;
 * unload is safe when nothing is loaded. */
int  dyt_mnn_load(const char *path);
void dyt_mnn_unload(void);

/* 2x upscale one 256x192 plane into 512x384.
 *
 * `in` is the contract's in_bytes; `out` must hold at least out_bytes, and
 * `out_cap` is how many bytes it really has.  Returns 0 and writes *out_n on
 * success, DYT_MNN_UNAVAILABLE when this build has no runtime (nothing is
 * written), -1 on a bad argument or an out_cap smaller than out_bytes.
 *
 * It never writes a partial or plausible-looking result when it cannot run the
 * model: the caller either gets the real upscale or a refusal. */
int dyt_mnn_zoom2(const uint8_t *in, uint8_t *out, int out_cap, int *out_n);

#ifdef __cplusplus
}
#endif

#endif /* DYT_MNN_H */
