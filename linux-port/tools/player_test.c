/*
 * player_test.c — the mp4 reader, pinned against the writer it mirrors.
 *
 * tools/recorder.c and tools/player.c are a pair: the writer swaps the port's
 * RGB to OpenCV's BGR on the way into the container and the reader swaps it
 * back on the way out.  Nothing else checks that the two swaps actually cancel.
 * If either were dropped — or if both were applied, which is the same mistake
 * seen from the other side — every clip the app records would play back with
 * red and blue exchanged, and no count or size check would notice.
 *
 * So this test writes a deliberately colour-skewed frame (red-dominant, with a
 * gradient so it is not uniform), reads it back, and asserts the skew survived.
 * A channel swap turns "mean R > mean B" into its opposite, which is decisive.
 * The synthetic frame also makes the test device-free: no fixture, no hardware.
 *
 * It then pins the reader's other contract — that it loops, so a caller playing
 * a short clip does not have to own the wrap-around.
 *
 * build:  via the Makefile (make check)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "player.h"
#include "recorder.h"

#define W 64
#define H 48
#define N 8

static int fails;

static void ok(const char *what) { printf("  ok   %s\n", what); }

static void fail(const char *what, const char *detail)
{
    printf("  FAIL %-52s %s\n", what, detail);
    fails++;
}

static void check(const char *what, int cond, const char *detail)
{
    if (cond)
        ok(what);
    else
        fail(what, detail);
}

static void llcheck(const char *what, long long got, long long want)
{
    if (got == want) {
        ok(what);
    } else {
        char d[96];
        snprintf(d, sizeof d, "got %lld want %lld", got, want);
        fail(what, d);
    }
}

/* The mean of one channel over the whole frame, for the channel-order check. */
static double channel_mean(const unsigned char *rgb, int ch)
{
    long sum = 0;
    for (int i = 0; i < W * H; i++)
        sum += rgb[(size_t)i * 3 + ch];
    return (double)sum / (W * H);
}

int main(void)
{
    const char    *path = "build/player_test.mp4";
    unsigned char *frame = malloc((size_t)W * H * 3);
    unsigned char *back  = malloc((size_t)W * H * 3);
    dyt_recorder_t *rec;
    dyt_player_t   *pl;
    int             w = 0, h = 0;
    long long       pos = -1, last = -1;
    double          mr, mg, mb;
    int             rc;

    if (!frame || !back) {
        printf("player_test: out of memory\n");
        return 1;
    }

    /* A red-dominant ramp: R falls left-to-right, G rises top-to-bottom, B is
     * flat.  Mean R (~160) is far above mean B (30), so a swap cannot hide. */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            unsigned char *px = frame + ((size_t)y * W + x) * 3;
            px[0] = (unsigned char)(220 - 120 * x / W);   /* R */
            px[1] = (unsigned char)(40 + 80 * y / H);     /* G */
            px[2] = 30;                                   /* B */
        }
    }

    printf("player_test: %dx%d, %d frames -> %s\n", W, H, N, path);

    /* ---------------------------------------------------------- write it */
    rec = dyt_recorder_open(path, 25.0, "avc1");
    if (!rec) {
        printf("  FAIL the recorder would not open %s\n", path);
        return 1;
    }
    rc = 0;
    for (int i = 0; i < N && rc == 0; i++)
        rc = dyt_recorder_write(rec, frame, W, H);
    llcheck("the recorder accepted every frame", rc == 0 ? N : 0, N);
    llcheck("the recorder wrote the frames it was given",
            dyt_recorder_close(rec), N);

    /* ---------------------------------------------------------- read it */
    pl = dyt_player_open(path);
    check("the reader opens the clip the writer produced", pl != NULL,
          "dyt_player_open returned NULL");
    if (!pl) {
        free(frame);
        free(back);
        return fails ? 1 : 0;
    }

    check("the reader reports the clip's geometry",
          dyt_player_size(pl, &w, &h) == 0 && w == W && h == H,
          "geometry differs");
    llcheck("the reader reports the clip's frame count",
            dyt_player_count(pl), N);
    check("the reader reports a positive frame rate",
          dyt_player_fps(pl) > 0.0, "fps is not positive");

    rc = dyt_player_next(pl, back, W * H * 3, &pos);
    check("the first frame decodes", rc == 0, "dyt_player_next returned -1");
    llcheck("the first frame is frame 0", pos, 0);

    mr = channel_mean(back, 0);
    mg = channel_mean(back, 1);
    mb = channel_mean(back, 2);

    /* The load-bearing assertion: the writer's RGB->BGR and the reader's
     * BGR->RGB cancel, so the red-dominant input stays red-dominant. */
    check("red and blue are not swapped (writer and reader are symmetric)",
          mr > mb + 50.0, "the red channel is not dominant — a swap occurred");
    check("the decoded frame carries real image content",
          mr - mb > 20.0 && mg > 20.0, "the frame is flat or blank");

    /* -------------------------------------------------------- it loops */
    last = pos;
    for (int i = 0; i < N; i++) {
        if (dyt_player_next(pl, back, W * H * 3, &last) != 0)
            break;
    }
    llcheck("the reader wraps back to frame 0 after the last frame", last, 0);

    dyt_player_close(pl);
    free(frame);
    free(back);

    printf("%s: %d failure%s\n", fails ? "FAIL" : "PASS", fails,
           fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
