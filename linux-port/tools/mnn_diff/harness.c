/*
 * harness.c — drive the vendor's own MNN super-resolution path on this host.
 *
 * The vendor's zoom is reached through libmnnmodel.so, whose entry points are
 * plain C symbols (no C++ ABI to reproduce):
 *
 *     init_mnn_model_module(JNIEnv*, jobject)   -- a flag-setter; both args
 *                                                  are ignored, so NULLs do
 *     mnn_run_1(mnn_mode_name, int key)         -- load + decrypt the model
 *     mnn_run_2(mnn_mode_name, uint16_t *in, uint16_t *out)
 *     mnn_run_3(mnn_mode_name)                  -- teardown
 *
 * `mnn_mode_name` is a 32-bit enum passed by value; MNN_ZOOM_X2 is 1 (mnn_run_1
 * rejects anything else with -4 after the flag check).
 *
 * libmnnmodel.so NEEDs libMNN.so, libMNN_Express.so and the APK's
 * libc++_shared.so, all prepared by ../../vendor_shim/prep_vendor_so.py, plus
 * the bionic shims from ../../vendor_shim/build_shims.sh.
 *
 * build: cc -O2 -g -Wall -Wextra -o build/harness harness.c -ldl
 * run:   LD_LIBRARY_PATH=build/shim:build ./build/harness [in.raw] [out.raw]
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IN_W 256
#define IN_H 192
#define OUT_W 512
#define OUT_H 384
#define IN_N  (IN_W * IN_H)
#define OUT_N (OUT_W * OUT_H)

#define MODE_ZOOM_X2 1

typedef int  (*fn_init)(void *, void *);
typedef int  (*fn_run1)(int, int);
typedef int  (*fn_run2)(int, uint16_t *, uint16_t *);
typedef int  (*fn_run3)(int);

static void *must_sym(void *h, const char *name)
{
    void *p = dlsym(h, name);
    if (!p) {
        fprintf(stderr, "harness: cannot resolve %s: %s\n", name, dlerror());
        exit(1);
    }
    return p;
}

int main(int argc, char **argv)
{
    const char *lib = argc > 3 ? argv[3] : "libmnnmodel.so";
    void       *h;
    fn_init     init;
    fn_run1     run1;
    fn_run2     run2;
    fn_run3     run3;
    uint16_t   *in, *out;
    long        i;
    int         rc;

    /* RTLD_LAZY is the point: the vendor libraries import ~137 libc symbols
     * (locale, dirent, most of pthread) that this inference path never calls,
     * and the shim does not define them.  See prep_vendor_so.py, fix 4. */
    h = dlopen(lib, RTLD_LAZY | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "harness: dlopen(%s) failed: %s\n", lib, dlerror());
        return 1;
    }
    printf("dlopen: OK (%s)\n", lib);

    init = (fn_init)must_sym(h, "_Z21init_mnn_model_moduleP7_JNIEnvP8_jobject");
    run1 = (fn_run1)must_sym(h, "_Z9mnn_run_113mnn_mode_namei");
    run2 = (fn_run2)must_sym(h, "_Z9mnn_run_213mnn_mode_namePtS0_");
    run3 = (fn_run3)must_sym(h, "_Z9mnn_run_313mnn_mode_name");

    printf("init(NULL, NULL) -> %d\n", init(NULL, NULL));

    rc = run1(MODE_ZOOM_X2, 2);
    printf("run_1(%d, 2)     -> %d\n", MODE_ZOOM_X2, rc);
    if (rc != 0) {
        fprintf(stderr, "harness: model load failed (rc=%d)\n", rc);
        return 1;
    }

    in  = calloc(IN_N,  sizeof *in);
    out = calloc(OUT_N, sizeof *out);
    if (!in || !out) {
        fprintf(stderr, "harness: out of memory\n");
        return 1;
    }

    if (argc > 1 && strcmp(argv[1], "-") != 0) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 1; }
        if (fread(in, sizeof *in, IN_N, f) != IN_N) {
            fprintf(stderr, "harness: %s is short of %d uint16\n", argv[1], IN_N);
            return 1;
        }
        fclose(f);
    } else {
        /* A deterministic ramp in the 8-bit range the app would supply. */
        for (i = 0; i < IN_N; i++)
            in[i] = (uint16_t)((i * 7) & 0xFF);
    }

    rc = run2(MODE_ZOOM_X2, in, out);
    printf("run_2            -> %d\n", rc);

    {
        uint16_t mn = 0xFFFF, mx = 0;
        double   sum = 0;
        long     uniq_hi = 0;
        int      seen[65536] = {0};

        for (i = 0; i < OUT_N; i++) {
            if (out[i] < mn) mn = out[i];
            if (out[i] > mx) mx = out[i];
            sum += out[i];
            if (!seen[out[i]]) { seen[out[i]] = 1; uniq_hi++; }
        }
        printf("out min %u max %u mean %.2f distinct %ld\n",
               mn, mx, sum / OUT_N, uniq_hi);
        printf("out[0:8] :");
        for (i = 0; i < 8; i++) printf(" %u", out[i]);
        printf("\n");
        printf("in [0:8] :");
        for (i = 0; i < 8; i++) printf(" %u", in[i]);
        printf("\n");
    }

    if (argc > 2 && strcmp(argv[2], "-") != 0) {
        FILE *f = fopen(argv[2], "wb");
        if (!f) { perror(argv[2]); return 1; }
        fwrite(out, sizeof *out, OUT_N, f);
        fclose(f);
        printf("wrote %s (%d uint16)\n", argv[2], OUT_N);
    }

    printf("run_3(%d)        -> %d\n", MODE_ZOOM_X2, run3(MODE_ZOOM_X2));
    free(in);
    free(out);
    return 0;
}
