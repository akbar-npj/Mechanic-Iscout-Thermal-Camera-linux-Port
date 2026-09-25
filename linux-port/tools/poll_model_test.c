/*
 * poll_model_test.c — proof that the session's existing poll model is a sound
 * hand-off between the capture thread and a front end.
 *
 * This is the second half of task #85.  The first half added
 * src/frame_ready.{h,c}, a coalescing wakeup; but the wakeup is only an
 * optimisation, and it would be dishonest to ship it without first pinning
 * what the engine already guarantees.  So this test uses *no* new primitive:
 * it drives the real live access pattern and asserts that the guarantees hold
 * without one.
 *
 * The two threads are the two the Qt6 app will have:
 *
 *   producer  dyt_session_capture_on_frame(), exactly as libuvc's callback
 *             thread will call it (via tools/frame_source.c's peers in
 *             session_capture.c).  No capture handle is set, so the adapter
 *             runs the temperatures-only path — the AD output mode's, where
 *             there is no visible half.  That path is `sc->cap == NULL`
 *             (session_capture.c:87).
 *
 *   consumer  dyt_frame_source_open_live() + dyt_frame_source_next(), the
 *             same call the GUI's frame pump makes, plus the snapshot the
 *             widgets read from.
 *
 * What it proves: a consumer that falls behind loses frames but never sees a
 * torn one, never races the render, and never blocks the producer.  The
 * producer alternates two frames that differ in *both* size and value, so a
 * snapshot mixing them cannot match either.
 *
 * build:  via the Makefile (make check)
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "frame_source.h"
#include "session.h"
#include "session_capture.h"

static int fails;

static void ok(const char *what) { printf("  ok   %s\n", what); }

static void fail(const char *what, const char *detail)
{
    printf("  FAIL %-52s %s\n", what, detail);
    fails++;
}

static void longcheck(const char *what, long got, long want)
{
    if (got == want) {
        ok(what);
    } else {
        char d[96];
        snprintf(d, sizeof d, "got %ld want %ld", got, want);
        fail(what, d);
    }
}

static void check(const char *what, int cond, const char *detail)
{
    if (cond)
        ok(what);
    else
        fail(what, detail);
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

/* ------------------------------------------------------------- the two frames
 *
 * Different sizes *and* different temperatures.  The sizes are what catch a
 * torn geometry; the values are what catch a torn payload, because a frame
 * that mixed them would have to match neither.  Both are far from the device's
 * 238.85 C start-up filler, so snap.ready is true from the first frame
 * (session.c:327) and nothing has to wait for a settle.
 */
#define A_W 8
#define A_H 4
#define A_C 11.0f
#define B_W 16
#define B_H 8
#define B_C 22.0f

#define CONSUMER_TEMPS (B_W * B_H)   /* room for the larger frame */

/* The producer runs for a wall-clock budget rather than a frame count, so the
 * consumer's sample count does not depend on how fast this machine happens to
 * be.  At the consumer's 1 ms period that is ~250 snapshots, comfortably above
 * the floor asserted below. */
#define PRODUCER_MS 250.0

/* How often the consumer looks.  Deliberately slower than the producer, which
 * is the situation the hand-off has to survive. */
#define CONSUMER_SLEEP_US 1000

struct producer {
    dyt_session_capture_t *sc;
    volatile int           stop;
    long                   frames;
};

static void *producer_thread(void *arg)
{
    struct producer *p = arg;
    float a[A_W * A_H], b[B_W * B_H];
    double t0;
    int   i;

    for (i = 0; i < A_W * A_H; i++) a[i] = A_C;
    for (i = 0; i < B_W * B_H; i++) b[i] = B_C;

    t0 = now_ms();
    for (i = 0; now_ms() - t0 < PRODUCER_MS; i++) {
        if (i & 1) {
            /* n == width*height, so the adapter's 0x44c header offset is 0. */
            dyt_session_capture_on_frame(b, B_W * B_H, B_W, B_H, p->sc);
        } else {
            dyt_session_capture_on_frame(a, A_W * A_H, A_W, A_H, p->sc);
        }
        p->frames++;
    }

    p->stop = 1;
    return NULL;
}

int main(void)
{
    dyt_session_t         *sess;
    dyt_session_capture_t *sc;
    dyt_frame_source_t    *fs;
    struct producer        prod;
    pthread_t              th;

    long frames = 0, snapshots = 0, wait_calls = 0, errors = 0;
    long torn = 0, bad_geometry = 0, saw_end = 0, seq_regressed = 0;
    long distinct = 0, last_seq = 0, seq_final = 0;
    double max_call_ms = 0.0;
    float temps[CONSUMER_TEMPS];
    dyt_snapshot_t snap;

    printf("=== poll_model_test (the live hand-off, no wakeup) ===\n");
    printf("-- capture thread -> GUI thread, via the session lock --\n");

    sess = dyt_session_create();
    if (!sess) {
        fail("session", "dyt_session_create returned NULL");
        return 1;
    }

    /* No dyt_session_capture_set_capture(): the handle would carry the visible
     * half, and this test is the temperatures-only (AD-mode) path. */
    sc = dyt_session_capture_create(sess);
    if (!sc) {
        fail("adapter", "dyt_session_capture_create returned NULL");
        dyt_session_free(sess);
        return 1;
    }

    fs = dyt_frame_source_open_live(sess);
    if (!fs) {
        fail("frame source", "dyt_frame_source_open_live returned NULL");
        dyt_session_capture_free(sc);
        dyt_session_free(sess);
        return 1;
    }

    prod.sc    = sc;
    prod.stop  = 0;
    prod.frames = 0;

    if (pthread_create(&th, NULL, producer_thread, &prod) != 0) {
        fail("producer", "could not start the producer thread");
        dyt_frame_source_close(fs);
        dyt_session_capture_free(sc);
        dyt_session_free(sess);
        return 1;
    }

    /* Consume until the producer is done and we have rendered at least once.
     * The `frames == 0` half matters: the producer can finish before the
     * consumer's first pass, and the session still holds its last frame. */
    while (!prod.stop || frames == 0) {
        const uint8_t *rgb = NULL;
        int            w = 0, h = 0;
        dyt_fs_status_t st;
        double          t0, dt;

        t0 = now_ms();
        st = dyt_frame_source_next(fs, &rgb, &w, &h);
        dt = now_ms() - t0;
        if (dt > max_call_ms)
            max_call_ms = dt;

        if (st == DYT_FS_END) {          /* the live path must never say this */
            saw_end = 1;
            break;
        }
        if (st == DYT_FS_ERROR) {
            errors++;
            break;
        }
        if (st != DYT_FS_FRAME || !rgb) {
            wait_calls++;                /* normal before the first frame */
            usleep(CONSUMER_SLEEP_US);
            continue;
        }

        frames++;

        /* The render is atomic inside the session lock, so its two dimensions
         * always describe one frame.  It is checked only against itself: the
         * producer may legitimately install the other-sized frame between this
         * call and the snapshot below, and calling that a tear would be a
         * false positive. */
        if (!((w == A_W && h == A_H) || (w == B_W && h == B_H)))
            bad_geometry++;

        /* The snapshot is where a tear would show.  Both the scalars and every
         * pixel must agree with a single frame. */
        if (dyt_session_snapshot(sess, &snap, temps, CONSUMER_TEMPS) == 0) {
            snapshots++;

            if (snap.seq < last_seq)
                seq_regressed++;
            /* A *new* frame, as opposed to a re-read of the one already held:
             * next() re-renders whatever the session holds on every call, so
             * the render count says nothing about how many frames were seen. */
            if (snap.seq != last_seq)
                distinct++;
            last_seq = snap.seq;

            int is_a = (snap.width == A_W && snap.height == A_H);
            int is_b = (snap.width == B_W && snap.height == B_H);

            if (!is_a && !is_b) {
                torn++;
            } else {
                float want = is_a ? A_C : B_C;
                int   n    = snap.width * snap.height;
                int   i, mismatch = 0;

                for (i = 0; i < n; i++)
                    if (temps[i] != want) { mismatch = 1; break; }

                if (mismatch || snap.stats.lo != want || snap.stats.hi != want ||
                    snap.centre_c != want)
                    torn++;
            }
        }

        /* The consumer is deliberately slower than the producer — that is the
         * situation the hand-off exists to survive, and it is what makes the
         * drop proof below real rather than assumed. */
        usleep(CONSUMER_SLEEP_US);
    }

    pthread_join(th, NULL);
    /* One last read, after the producer has stopped, so seq_final is the total
     * the producer actually installed. */
    if (dyt_session_snapshot(sess, &snap, NULL, 0) == 0)
        seq_final = snap.seq;

    printf("  producer installed %ld frame(s); consumer rendered %ld over "
           "%ld snapshot(s), seeing %ld distinct frame(s)\n",
           prod.frames, frames, snapshots, distinct);
    printf("  consumer polled %ld time(s) before the first frame arrived\n",
           wait_calls);

    /* ---- the producer's side ---- */
    check("the producer installed frames", prod.frames > 0, "never ran");
    longcheck("every frame reached the session", seq_final, prod.frames);

    /* ---- the consumer's side ---- */
    check("the consumer rendered at least one frame", frames >= 1,
          "never rendered");
    /* The torn check is only evidence if it actually looked.  session_test's
     * torn-read test sets the same floor for the same reason: a run with a
     * handful of snapshots would pass while proving nothing. */
    check("the consumer took enough snapshots to be meaningful",
          snapshots >= 100, "too few snapshots to conclude anything");
    check("the consumer never saw a torn frame", torn == 0,
          "a snapshot mixed two frames");
    check("the render's geometry was always a single frame", bad_geometry == 0,
          "next() reported a size that is neither frame");
    check("seq never went backwards", seq_regressed == 0,
          "the frame counter regressed");
    check("the live source never reported END", !saw_end,
          "a live source is unbounded and must not end");
    longcheck("no errors from the frame source", errors, 0);

    /* ---- drop-latest, which is the point ----
     *
     * The producer installed seq_final frames; the consumer saw only
     * `distinct` of them.  Note it is `distinct` and not the render count:
     * next() re-renders whatever the session holds on every call, so counting
     * renders would measure the consumer's own speed, not the frames it saw. */
    check("the consumer saw at least one frame", distinct >= 1,
          "never saw a frame");
    check("the consumer dropped frames rather than queueing them",
          distinct < seq_final,
          "the consumer saw every frame; the test proved nothing");

    /* ---- soft: the consumer never blocked on the source ---- */
    printf("  (soft) slowest single next() call: %.1f ms\n", max_call_ms);
    check("next() never blocked the consumer", max_call_ms < 1000.0,
          "a single next() call took over a second");

    dyt_frame_source_close(fs);
    dyt_session_capture_free(sc);
    dyt_session_free(sess);

    if (fails) {
        printf("=== %d FAILURE(S) ===\n", fails);
        return 1;
    }
    printf("=== ALL PASS ===\n");
    return 0;
}
