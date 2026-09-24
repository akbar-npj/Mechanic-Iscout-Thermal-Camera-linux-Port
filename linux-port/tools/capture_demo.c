/*
 * capture_demo.c — stream live thermal frames and report temperatures.
 *
 * The end-to-end bring-up tool: opens the camera, streams N frames
 * through the verified pipeline, prints per-frame temperature statistics,
 * and optionally dumps the first raw frame and its temperatures to disk.
 *
 *   capture_demo                          # 10 frames, first matching device
 *   capture_demo --frames 100
 *   capture_demo --diag                   # dump every format libuvc parsed
 *   capture_demo --format-index 1 --width 256 --height 192 --fps 30
 *   capture_demo --out /tmp/frame0        # writes frame0_raw.bin + frame0_temps.bin
 *
 * build:  via the Makefile (make)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "capture.h"

typedef struct {
    int    frames_wanted;
    volatile int frames_seen;   /* written by the libuvc callback thread */
    char   out_prefix[512];
    int    wrote;
    int    save_last;        /* dump the final frame instead of the first */
    dyt_capture_t *cap;      /* for the raw-frame accessor */
} demo_state;

static int write_file(const char *path, const void *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    if (n && fwrite(p, 1, n, f) != n) { perror("fwrite"); fclose(f); return -1; }
    fclose(f);
    return 0;
}

static void save_frame(demo_state *s, const float *temps, int n)
{
    char path[600];
    int nraw = 0;
    const uint16_t *raw = dyt_capture_last_raw(s->cap, &nraw);

    snprintf(path, sizeof path, "%s_temps.bin", s->out_prefix);
    if (write_file(path, temps, (size_t)n * sizeof(float)) == 0)
        printf("           wrote %s (%d floats)\n", path, n);

    if (raw && nraw) {
        snprintf(path, sizeof path, "%s_raw.bin", s->out_prefix);
        if (write_file(path, raw, (size_t)nraw * sizeof(uint16_t)) == 0)
            printf("           wrote %s (%d uint16 samples)\n", path, nraw);
    }
}

static void on_frame(const float *temps, int n, int width, int active_height,
                     void *user)
{
    demo_state *s = user;
    /* mode 0x44c prepends a 10-float header; mode 1000 has no header. */
    int off = (dyt_capture_mode(s->cap) == DYT_MODE_44C && n > 10) ? 10 : 0;
    int i, npix = n - off;
    double tmin = 1e30, tmax = -1e30, tsum = 0.0;

    for (i = 0; i < npix; i++) {
        double t = temps[off + i];
        if (t != t) continue;               /* skip NaN (non-physical pixels) */
        if (t < tmin) tmin = t;
        if (t > tmax) tmax = t;
        tsum += t;
    }

    printf("frame %3d  %dx%d  min %.2f C  max %.2f C  mean %.2f C\n",
           s->frames_seen, width, active_height, tmin, tmax,
           npix ? tsum / npix : 0.0);

    /* Dump either the first frame or, with --save-last, the final one.  The
     * final one matters here: the device streams a flat 0x8000 placeholder
     * for several seconds after setTinyCOutputADValue before real data
     * appears (see send_ad_order in src/capture.c), so the first frame is
     * almost always the placeholder. */
    if (s->out_prefix[0] && !s->wrote) {
        int last = s->frames_seen + 1 >= s->frames_wanted;
        if (!s->save_last || last) {
            save_frame(s, temps, n);
            s->wrote = 1;
        }
    }

    s->frames_seen++;
}

int main(int argc, char **argv)
{
    dyt_capture_opts o;
    dyt_capture_t *cap = NULL;
    demo_state st;
    int i, want_diag = 0;

    memset(&st, 0, sizeof st);
    st.frames_wanted = 10;
    dyt_capture_opts_default(&o);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            st.frames_wanted = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            snprintf(st.out_prefix, sizeof st.out_prefix, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--diag"))
            want_diag = 1;
        else if (!strcmp(argv[i], "--vid") && i + 1 < argc)
            o.vid = (uint16_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--pid") && i + 1 < argc)
            o.pid = (uint16_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--format-index") && i + 1 < argc)
            o.format_index = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--width") && i + 1 < argc)
            o.width = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc)
            o.height = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc)
            o.fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--t-amb") && i + 1 < argc)
            o.t_amb = strtof(argv[++i], NULL);
        else if (!strcmp(argv[i], "--sensor-mode") && i + 1 < argc)
            o.sensor_mode = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--fix-mode") && i + 1 < argc)
            o.fix_mode = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--start-orders"))
            o.send_start_orders = 1;
        else if (!strcmp(argv[i], "--save-last"))
            st.save_last = 1;
        else {
            fprintf(stderr,
                "usage: %s [--frames N] [--out PREFIX] [--diag] [--save-last]\n"
                "          [--vid 0xXXXX] [--pid 0xXXXX]\n"
                "          [--format-index N] [--width W] [--height H] [--fps F]\n"
                "          [--t-amb C] [--sensor-mode 0x82] [--fix-mode 0x78]\n"
                "          [--start-orders]   send setTinyCOutputADValue once the\n"
                "                             stream is running — required on\n"
                "                             0bda:5840, which otherwise streams a\n"
                "                             flat 0x8000 placeholder\n",
                argv[0]);
            return 2;
        }
    }

    if (dyt_capture_open(&cap, &o) != 0)
        return 1;
    st.cap = cap;

    if (want_diag)
        dyt_capture_print_diag(cap);

    printf("mode %s — streaming %d frame(s)\n",
           dyt_capture_mode(cap) == DYT_MODE_44C  ? "0x44c radiometric" :
           dyt_capture_mode(cap) == DYT_MODE_1000 ? "1000 direct-AD" : "?",
           st.frames_wanted);

    if (dyt_capture_start(cap, on_frame, &st) != 0) {
        dyt_capture_close(cap);
        return 1;
    }

    while (st.frames_seen < st.frames_wanted) {
        struct timespec ts = { 0, 100 * 1000 * 1000 };   /* 100 ms */
        nanosleep(&ts, NULL);
    }

    dyt_capture_stop(cap);
    dyt_capture_close(cap);

    printf("done: %d frame(s)\n", st.frames_seen);
    return st.frames_seen ? 0 : 1;
}
