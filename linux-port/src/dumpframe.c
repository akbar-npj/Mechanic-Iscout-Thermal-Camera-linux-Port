/*
 * dumpframe.c — CLI: read a frozen thermal frame, run the frame→temperature
 * pipeline, and (with --check) byte-compare against the vendor ground truth.
 *
 * This is the no-hardware proof that the frame.c API (dyt_frame_build_lut +
 * dyt_frame_convert_mode) reproduces the vendor end-to-end: it reads the
 * same frozen in_frame.bin + meta.txt that the differential harness froze
 * against the real libthermometry.so, runs it through frame.c, and checks
 * the output bytes match out_image.bin + in_lut_in.bin exactly.
 *
 * usage:
 *   ./dumpframe <dir>            # run pipeline, write dumpframe_out.bin + dumpframe_lut.bin
 *   ./dumpframe --check <dir>    # run pipeline, cmp vs out_image.bin + in_lut_in.bin; exit 0/1
 *
 * <dir> layout (frozen by tools/thermometry_diff/run.sh + harness):
 *   in_frame.bin   raw uint16 frame, width*total_height samples
 *   meta.txt       "key value" lines: width, total_height, rec_base,
 *                  t_amb, sensor_mode, fix_mode, out_n
 *   in_lut_in.bin  vendor LUT (16384 float) — --check reference
 *   out_image.bin  vendor output (10 + width*(h-4) float) — --check reference
 *
 * build:  via the Makefile (links thermometry.o + frame.o + control.o)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frame.h"

/* ----- meta/file helpers (same format as tools/thermometry_diff/port.c) ----- */

static long meta_int(const char *dir, const char *key)
{
    char path[1024], line[256];
    FILE *f;
    long v = 0;

    snprintf(path, sizeof path, "%s/meta.txt", dir);
    f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, strlen(key)) == 0 && line[strlen(key)] == ' ') {
            v = strtol(line + strlen(key) + 1, NULL, 0);
            break;
        }
    }
    fclose(f);
    return v;
}

static float meta_float(const char *dir, const char *key)
{
    char path[1024], line[256];
    FILE *f;
    float v = 0.0f;

    snprintf(path, sizeof path, "%s/meta.txt", dir);
    f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, strlen(key)) == 0 && line[strlen(key)] == ' ') {
            v = strtof(line + strlen(key) + 1, NULL);
            break;
        }
    }
    fclose(f);
    return v;
}

static uint8_t *read_file(const char *path, size_t *out_n)
{
    FILE *f = fopen(path, "rb");
    long sz;
    uint8_t *p;
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    p = malloc(sz ? sz : 1);
    if (!p) { perror("malloc"); exit(1); }
    if (sz && fread(p, 1, sz, f) != (size_t)sz) { perror("fread"); exit(1); }
    fclose(f);
    *out_n = (size_t)sz;
    return p;
}

static void write_file(const char *path, const void *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    if (n && fwrite(p, 1, n, f) != n) { perror("fwrite"); exit(1); }
    fclose(f);
}

/* ----- byte comparison for --check ----- */

static int cmp_bytes(const char *what, const char *vendor_path,
                     const void *port_buf, size_t port_len)
{
    size_t vlen;
    uint8_t *vbuf = read_file(vendor_path, &vlen);
    const uint8_t *p = port_buf;
    const uint8_t *v = vbuf;
    size_t n = port_len < vlen ? port_len : vlen;
    size_t i = 0;

    if (port_len == vlen && memcmp(p, v, port_len) == 0) {
        printf("  [PASS] %s: %zu bytes identical\n", what, port_len);
        free(vbuf);
        return 0;
    }
    while (i < n && p[i] == v[i]) i++;
    printf("  [FAIL] %s: first diff at byte %zu (0x%zx); "
           "len vendor=%zu port=%zu\n", what, i, i, vlen, port_len);
    {
        size_t off4 = i - (i % 4);
        size_t lo = off4 > 8 ? off4 - 8 : 0;
        size_t hi = off4 + 16 < n ? off4 + 16 : n;
        printf("         vendor [0x%zx..]: ", lo);
        for (size_t k = lo; k < hi && k < vlen; k++) printf("%02x", v[k]);
        printf("\n         port   [0x%zx..]: ", lo);
        for (size_t k = lo; k < hi && k < port_len; k++) printf("%02x", p[k]);
        printf("\n");
    }
    free(vbuf);
    return 1;
}

/* ----- main ----- */

int main(int argc, char **argv)
{
    int check = 0;
    const char *dir = NULL;

    if (argc >= 3 && strcmp(argv[1], "--check") == 0) {
        check = 1;
        dir = argv[2];
    } else if (argc >= 2) {
        dir = argv[1];
    } else {
        fprintf(stderr, "usage: %s [--check] <dir>\n", argv[0]);
        return 2;
    }

    int width    = (int)meta_int(dir, "width");
    int height   = (int)meta_int(dir, "total_height");
    int rec_base = (int)meta_int(dir, "rec_base");
    float t_amb  = meta_float(dir, "t_amb");
    int smode    = (int)meta_int(dir, "sensor_mode");
    int fixmode  = (int)meta_int(dir, "fix_mode");
    int out_n    = (int)meta_int(dir, "out_n");
    (void)rec_base;  /* dyt_geometry / thermometryT4Line derive it from width */

    size_t frame_n;
    char fpath[1024];
    snprintf(fpath, sizeof fpath, "%s/in_frame.bin", dir);
    uint8_t *frame = read_file(fpath, &frame_n);
    (void)frame_n;

    frame_t f = {
        .width        = width,
        .total_height = height,
        .rec_base     = rec_base,
        .raw          = (uint16_t *)frame,
    };

    float *lut = calloc(LUT_N, sizeof(float));
    float *out = calloc((size_t)out_n, sizeof(float));
    if (!lut || !out) { perror("calloc"); exit(1); }

    /* Build the LUT from the frame's calibration record. */
    dyt_frame_build_lut(&f, t_amb, smode, fixmode, lut);

    /* Convert the frame to temperatures through the mode dispatch.
     * All frozen cases are mode 0x44c (full radiometric). */
    int rc = dyt_frame_convert_mode(DYT_MODE_44C, &f, lut, out);
    if (rc != 0) {
        fprintf(stderr, "dumpframe: convert failed (rc=%d) — "
                "frame has saturated/corrupt pixels (>= 0x4000)\n", rc);
    }

    /* Write the pipeline output. */
    snprintf(fpath, sizeof fpath, "%s/dumpframe_lut.bin", dir);
    write_file(fpath, lut, LUT_N * sizeof(float));
    snprintf(fpath, sizeof fpath, "%s/dumpframe_out.bin", dir);
    write_file(fpath, out, (size_t)out_n * sizeof(float));

    printf("dumpframe: width=%d total_height=%d out_n=%d rc=%d\n",
           width, height, out_n, rc);
    printf("  lut[0]=%.7f lut[%d]=%.7f\n", lut[0], LUT_N - 1, lut[LUT_N - 1]);

    if (!check) {
        free(lut); free(out); free(frame);
        return 0;
    }

    /* --check: byte-compare against the frozen vendor ground truth. */
    int fails = 0;
    snprintf(fpath, sizeof fpath, "%s/in_lut_in.bin", dir);
    fails += cmp_bytes("LUT",   fpath, lut, LUT_N * sizeof(float));
    snprintf(fpath, sizeof fpath, "%s/out_image.bin", dir);
    fails += cmp_bytes("image", fpath, out, (size_t)out_n * sizeof(float));

    printf("  result: %s\n", fails ? "FAIL" : "PASS");

    free(lut); free(out); free(frame);
    return fails ? 1 : 0;
}
