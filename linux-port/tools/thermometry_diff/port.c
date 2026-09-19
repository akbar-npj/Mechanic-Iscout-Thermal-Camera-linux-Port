/*
 * port.c — driver for the byte-verified thermometry port.
 *
 * The six thermometry function bodies now live in src/thermometry.c
 * (extracted verbatim from this file — no reformatting of any
 * fmaf/pow/exp expression; only the `static` linkage was removed).
 * This file is the differential-harness counterpart: it reads a
 * frozen ground-truth directory (in_frame.bin + meta.txt, produced
 * by run.sh + harness against the real vendor libthermometry.so),
 * runs the same two entry points from the port, and writes
 * port_lut.bin / port_out.bin for diff.py to byte-compare against
 * the vendor's in_lut_in.bin / out_image.bin.
 *
 * Why byte-for-byte and not "close enough"
 * -----------------------------------------
 * Radiometric thermometry is a chain of fused multiply-adds, square
 * roots and fourth powers.  A single-precision rounding difference
 * anywhere in the chain propagates to every pixel.  The only honest
 * proof that the port is correct is that the raw bytes of its LUT and
 * output image equal the vendor's.  diff.py is that proof; it is the
 * CI-usable regression gate (exit 0 on PASS, non-zero on FAIL).
 *
 * build:  cc -O2 -g -Wall -Wextra -Wno-unused-parameter -ffp-contract=off \
 *           -I../../src -o build/port port.c -lm
 *
 * usage:  ./build/port <ground_truth_dir>      # reads in_frame.bin + meta.txt,
 *                                              # writes port_lut.bin, port_out.bin
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "thermometry.h"

/* ================================================================ driver */
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

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "out/256";
    int width   = (int)meta_int(dir, "width");
    int height  = (int)meta_int(dir, "total_height");
    int rec_base= (int)meta_int(dir, "rec_base");
    float t_amb = meta_float(dir, "t_amb");
    int mode    = (int)meta_int(dir, "sensor_mode");
    int fixmode = (int)meta_int(dir, "fix_mode");
    int out_n   = (int)meta_int(dir, "out_n");
    int active  = height - REF_ROWS;
    (void)rec_base;  /* the port reads the record from the frame directly */

    size_t frame_n;
    char fpath[1024];
    snprintf(fpath, sizeof fpath, "%s/in_frame.bin", dir);
    uint8_t *frame = read_file(fpath, &frame_n);
    (void)frame_n;

    uint8_t *ref_band = frame + (size_t)width * active * 2;
    float *lut = calloc(LUT_N, sizeof(float));
    float *out = calloc((size_t)out_n, sizeof(float));
    float amb, corr, refl, air, humi, emiss;
    uint16_t ua = 0;

    thermometryT4Line(t_amb, width, height, lut, ref_band,
                      &amb, &corr, &refl, &air, &humi, &emiss,
                      &ua, mode, fixmode);
    thermometrySearch(width, height, lut, (uint16_t *)frame, out,
                      (void *)400, REF_ROWS);

    snprintf(fpath, sizeof fpath, "%s/port_lut.bin", dir);
    write_file(fpath, lut, LUT_N * sizeof(float));
    snprintf(fpath, sizeof fpath, "%s/port_out.bin", dir);
    write_file(fpath, out, (size_t)out_n * sizeof(float));

    printf("port: amb=%.6f corr=%.6f refl=%.6f air=%.6f humi=%.6f emiss=%.6f\n",
           amb, corr, refl, air, humi, emiss);
    printf("port: lut[0]=%.7f lut[%d]=%.7f  (vendor: lut0=%.7f lut_last=%.7f)\n",
           lut[0], LUT_N - 1, lut[LUT_N - 1],
           meta_float(dir, "lut0"), meta_float(dir, "lut_last"));

    free(lut); free(out); free(frame);
    return 0;
}
