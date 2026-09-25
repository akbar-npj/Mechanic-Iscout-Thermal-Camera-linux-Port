/*
 * mnn.c — the super-resolution seam.  See mnn.h.
 *
 * This file is plain C and runtime-free: the shape contract, the structural
 * validator for the extracted model, and the dispatch to the runtime.  The
 * runtime itself is C++ and lives in mnn_runtime.cpp, compiled only under
 * DYT_HAVE_MNN; without it the entry points below refuse rather than invent a
 * result (see the #else branch further down).
 *
 * The model itself is real and in the tree: models/zoom2.mnn, recovered from
 * libmnnmodel.so by RE Workspace/tools/extract_mnn_model.py.
 */
#include <stdio.h>
#include <string.h>

#include "mnn.h"

/* The contract, derived in models/README.md from the reshape sr1 performs
 * ([1,1,192,256]), the app's 393216-byte output buffer, and the graph's
 * DepthToSpace block size of 2.  Two bytes per sample is what the app's own
 * buffer arithmetic implies. */
static const dyt_mnn_contract_t kContract = {
    .in_w = 256, .in_h = 192,
    .out_w = 512, .out_h = 384,
    .factor = 2,
    .in_samples = 256 * 192,          /* 49152 */
    .out_samples = 512 * 384,         /* 196608 */
    .in_bytes = 256 * 192 * 2,        /* 98304 */
    .out_bytes = 512 * 384 * 2        /* 393216 */
};

/* The identifiers the app's zoom2 model carries.  The UUID is the flatbuffer's
 * asset UUID; the rest are the tensors and a representative sample of the
 * ONNX-derived op names.  A wrong key or a truncated file loses these. */
static const char *const kMarkers[] = {
    "7006ec85-d318-4f47-9508-fbbe5f08ec82",
    "image_input",
    "image_output",
    "onnx::Conv_22",
    "onnx::Add_25",
    "onnx::DepthToSpace_26",
    "onnx::Clip_27"
};

#define MARKER_N ((int)(sizeof kMarkers / sizeof kMarkers[0]))

const dyt_mnn_contract_t *dyt_mnn_contract(void)
{
    return &kContract;
}

/* ------------------------------------------------------------ the runtime --
 *
 * The C++ half lives in mnn_runtime.cpp, which the Makefile only compiles when
 * an MNN runtime is present (DYT_HAVE_MNN).  Without it these fall back to the
 * refusal described in mnn.h, so a host with no MNN still builds and still
 * gets the contract and the validator. */
#ifdef DYT_HAVE_MNN

int  dyt_mnn_runtime_available(void);
int  dyt_mnn_runtime_load(const char *path);
void dyt_mnn_runtime_unload(void);
int  dyt_mnn_runtime_zoom2(const uint8_t *in, uint8_t *out, int out_cap,
                           int *out_n);

int dyt_mnn_available(void)
{
    return dyt_mnn_runtime_available();
}

int dyt_mnn_load(const char *path)
{
    if (!path)
        return -1;
    return dyt_mnn_runtime_load(path);
}

void dyt_mnn_unload(void)
{
    dyt_mnn_runtime_unload();
}

int dyt_mnn_zoom2(const uint8_t *in, uint8_t *out, int out_cap, int *out_n)
{
    if (!in || !out || !out_n || out_cap < 0)
        return -1;
    if (out_cap < kContract.out_bytes) {
        fprintf(stderr, "mnn: output buffer is %d bytes, need %d\n",
                out_cap, kContract.out_bytes);
        return -1;
    }
    return dyt_mnn_runtime_zoom2(in, out, out_cap, out_n);
}

#else /* no MNN runtime in this build */

/* No runtime is linked in this configuration, so nothing can run. */
int dyt_mnn_available(void)
{
    return 0;
}

int dyt_mnn_load(const char *path)
{
    (void)path;
    fprintf(stderr, "mnn: this build has no MNN runtime; cannot load a model\n");
    return DYT_MNN_UNAVAILABLE;
}

void dyt_mnn_unload(void)
{
}

int dyt_mnn_zoom2(const uint8_t *in, uint8_t *out, int out_cap, int *out_n)
{
    if (!in || !out || !out_n || out_cap < 0)
        return -1;
    if (out_cap < kContract.out_bytes) {
        fprintf(stderr, "mnn: output buffer is %d bytes, need %d\n",
                out_cap, kContract.out_bytes);
        return -1;
    }

    /* Refuse rather than invent.  A caller that gets 0 has a real upscale; a
     * caller that gets this has a build that cannot produce one. */
    fprintf(stderr, "mnn: this build has no MNN runtime; %dx%d -> %dx%d "
                    "is unavailable\n", kContract.in_w, kContract.in_h,
            kContract.out_w, kContract.out_h);
    return DYT_MNN_UNAVAILABLE;
}

#endif /* DYT_HAVE_MNN */

/* Does `needle` (a NUL-terminated string) appear in [data, data+n)?  A plain
 * byte search is the right tool here: the model is a flatbuffer, and the
 * identifiers live in its string table as plain bytes. */
static int contains(const uint8_t *data, size_t n, const char *needle)
{
    size_t len = strlen(needle);
    size_t i;

    if (len == 0 || len > n)
        return 0;
    for (i = 0; i + len <= n; i++)
        if (data[i] == (uint8_t)needle[0] &&
            memcmp(data + i, needle, len) == 0)
            return 1;
    return 0;
}

int dyt_mnn_model_check(const uint8_t *data, size_t n)
{
    uint32_t root;
    int      i;

    if (!data || n < 8)
        return -2;

    /* The first word of a flatbuffer is the root table's offset.  Checking it
     * is in range and 4-byte aligned is what separates "decrypted correctly"
     * from "still ciphertext": a wrong key gives a random offset that fails
     * this immediately. */
    root = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    if (root < 4 || root % 4 != 0 || (size_t)root + 4 > n) {
        fprintf(stderr, "mnn: not a flatbuffer (root offset 0x%x of %zu bytes)\n",
                (unsigned)root, n);
        return -1;
    }

    for (i = 0; i < MARKER_N; i++) {
        if (!contains(data, n, kMarkers[i])) {
            fprintf(stderr, "mnn: model is missing the marker \"%s\"\n",
                    kMarkers[i]);
            return -1;
        }
    }
    return 0;
}
