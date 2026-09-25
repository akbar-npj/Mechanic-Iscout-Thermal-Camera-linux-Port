/*
 * frame_ready_test.c — unit tests for the coalescing wakeup.
 *
 * The primitive exists to stop a per-frame producer from queueing one wakeup
 * per frame behind a slow consumer, so the contract worth pinning is exactly
 * that: N pings with nobody listening must arm *one* wakeup, not N.  That is
 * tested deterministically and without threads first, because it is the
 * property everything else depends on.
 *
 * The threaded cases then cover the two things a single-threaded test cannot:
 * that a blocking wait is actually woken by a producer, and that the arm /
 * consume bookkeeping stays exact when the producer outruns the consumer.
 *
 * Everything here is synthetic — no camera, no libuvc, no toolkit.
 *
 * build:  via the Makefile (make check)
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "frame_ready.h"

static int fails;

static void ok(const char *what) { printf("  ok   %s\n", what); }

static void fail(const char *what, const char *detail)
{
    printf("  FAIL %-52s %s\n", what, detail);
    fails++;
}

static void intcheck(const char *what, int got, int want)
{
    if (got == want) {
        ok(what);
    } else {
        char d[96];
        snprintf(d, sizeof d, "got %d want %d", got, want);
        fail(what, d);
    }
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

/* A condition, for the assertions that are not an equality. */
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

/* ------------------------------------------------ 1. deterministic coalescing
 *
 * No threads: this is the contract itself, so it must be exact rather than
 * statistical.  100 pings with nobody consuming can only arm once.
 */
static void test_coalescing(void)
{
    dyt_frame_ready_t *fr = dyt_frame_ready_create();
    int armed = 0, coalesced = 0, i;

    printf("-- deterministic coalescing --\n");

    if (!fr) {
        fail("create", "returned NULL");
        return;
    }

    for (i = 0; i < 100; i++) {
        if (dyt_frame_ready_ping(fr))
            armed++;
        else
            coalesced++;
    }
    intcheck("100 pings arm exactly one wakeup", armed, 1);
    intcheck("the other 99 coalesce",             coalesced, 99);

    intcheck("test() consumes it",       dyt_frame_ready_test(fr), 1);
    intcheck("test() on empty is 0",     dyt_frame_ready_test(fr), 0);
    intcheck("wait(0) on empty is 0",    dyt_frame_ready_wait(fr, 0), 0);

    /* And the bit re-arms after a consume, so a later frame is not lost. */
    intcheck("a ping re-arms after a consume", dyt_frame_ready_ping(fr), 1);
    intcheck("wait(0) consumes it",            dyt_frame_ready_wait(fr, 0), 1);
    intcheck("wait(0) on empty is 0",          dyt_frame_ready_wait(fr, 0), 0);

    dyt_frame_ready_free(fr);
}

/* ---------------------------------------------------------- 2. the timeout
 *
 * The lower bound is the deterministic half: the kernel will not expire a
 * timed wait before its deadline, and a spurious wake re-loops against the
 * same absolute deadline rather than extending it.  The upper bounds stay
 * deliberately generous — this gates make check, so it must not be flaky.
 */
static void test_timeout(void)
{
    dyt_frame_ready_t *fr = dyt_frame_ready_create();
    double t0, dt;
    int    r;

    printf("-- timeout --\n");

    if (!fr) {
        fail("create", "returned NULL");
        return;
    }

    t0 = now_ms();
    r  = dyt_frame_ready_wait(fr, 50);
    dt = now_ms() - t0;

    intcheck("wait(50) times out", r, 0);
    check("wait(50) did not expire early", dt >= 45.0, "returned before 45 ms");
    check("wait(50) returned promptly",    dt < 1000.0, "took >= 1 s");
    intcheck("the timeout consumed nothing", dyt_frame_ready_test(fr), 0);

    intcheck("ping arms", dyt_frame_ready_ping(fr), 1);
    t0 = now_ms();
    r  = dyt_frame_ready_wait(fr, 5000);
    dt = now_ms() - t0;

    intcheck("a pending ping returns immediately", r, 1);
    check("wait() did not sit out the 5 s timeout", dt < 1000.0,
          "took >= 1 s despite a pending ping");
    intcheck("wait() consumed it", dyt_frame_ready_test(fr), 0);

    dyt_frame_ready_free(fr);
}

/* ------------------------------------------- 3. wait(-1) is woken by a ping
 *
 * The outcome is deterministic whichever way the race falls: if the ping lands
 * first, wait() sees pending and returns; if wait() blocks first, the ping
 * signals it.  The sleep only biases toward exercising the blocked path, so
 * nothing here asserts a time.
 *
 * The hazard this case carries is a *hang*, which would wedge make check
 * forever.  A joinable watchdog turns that into a clean failure.
 */
#define WATCHDOG_MS 10000

static dyt_frame_ready_t *watch_fr;
static volatile int       watch_done;

static void *watchdog_thread(void *arg)
{
    double t0 = now_ms();
    (void)arg;
    while (!watch_done) {
        if (now_ms() - t0 > WATCHDOG_MS) {
            printf("  FAIL %-52s %s\n", "wait(-1) was never woken",
                   "no ping reached the blocked waiter");
            fflush(stdout);
            _exit(2);
        }
        usleep(10 * 1000);
    }
    return NULL;
}

static volatile int ping_go;

static void *ping_after_delay(void *arg)
{
    (void)arg;
    while (!ping_go)
        usleep(1000);                    /* let wait() block first */
    usleep(20 * 1000);
    dyt_frame_ready_ping(watch_fr);
    return NULL;
}

static void test_blocking_wait(void)
{
    pthread_t th, wd;
    int       r;

    printf("-- blocking wait --\n");

    watch_fr = dyt_frame_ready_create();
    if (!watch_fr) {
        fail("create", "returned NULL");
        return;
    }
    watch_done = 0;

    if (pthread_create(&wd, NULL, watchdog_thread, NULL) != 0) {
        fail("watchdog", "could not start the watchdog thread");
        dyt_frame_ready_free(watch_fr);
        return;
    }
    if (pthread_create(&th, NULL, ping_after_delay, NULL) != 0) {
        fail("producer", "could not start the producer thread");
        watch_done = 1;
        pthread_join(wd, NULL);
        dyt_frame_ready_free(watch_fr);
        return;
    }

    ping_go = 1;
    r       = dyt_frame_ready_wait(watch_fr, -1);

    watch_done = 1;
    pthread_join(th, NULL);
    pthread_join(wd, NULL);

    intcheck("wait(-1) returned on the ping", r, 1);
    intcheck("wait(-1) consumed it", dyt_frame_ready_test(watch_fr), 0);

    dyt_frame_ready_free(watch_fr);
    watch_fr = NULL;
}

/* ------------------------------------------- 4. the threaded arm/consume law
 *
 * A producer that outruns the consumer.  The invariant is exact and
 * interleaving-independent: every 0->1 transition of the pending bit is
 * consumed exactly once, and at most one arm is ever outstanding, so after a
 * final drain the consumer must have seen precisely as many pings as were
 * armed.
 *
 * Note what is deliberately NOT asserted: that armed < pings.  Whether
 * coalescing *happened* depends on timing; case 1 proves it deterministically.
 */
#define BURST_PINGS 20000

struct burst {
    dyt_frame_ready_t *fr;
    volatile int       stop;
    long               pings;
    long               armed;
};

static void *fast_ping(void *arg)
{
    struct burst *b = arg;

    /* do/while, not while: the producer must ping at least once. */
    do {
        if (dyt_frame_ready_ping(b->fr))
            b->armed++;
        b->pings++;
    } while (b->pings < BURST_PINGS);

    b->stop = 1;
    return NULL;
}

static void test_threaded(void)
{
    struct burst      b;
    dyt_frame_ready_t *fr = dyt_frame_ready_create();
    pthread_t          th;
    long               consumed = 0;

    printf("-- threaded coalescing --\n");

    if (!fr) {
        fail("create", "returned NULL");
        return;
    }

    b.fr    = fr;
    b.stop  = 0;
    b.pings = 0;
    b.armed = 0;

    if (pthread_create(&th, NULL, fast_ping, &b) != 0) {
        fail("producer", "could not start the producer thread");
        dyt_frame_ready_free(fr);
        return;
    }

    /* A slow consumer: the idle sleep is what makes coalescing likely, but it
     * is not load-bearing — the invariant below holds at any rate. */
    while (!b.stop) {
        if (dyt_frame_ready_test(fr))
            consumed++;
        else
            usleep(1000);
    }

    pthread_join(th, NULL);

    if (dyt_frame_ready_test(fr))       /* drain whatever is left */
        consumed++;

    longcheck("the producer ran to completion", b.pings, BURST_PINGS);
    check("at least one ping armed", b.armed >= 1, "nothing was ever armed");
    check("arms never exceed pings", b.armed <= b.pings,
          "more arms than pings");
    check("the consumer made progress", consumed >= 1, "never consumed a ping");
    longcheck("every arm was consumed exactly once", consumed, b.armed);

    dyt_frame_ready_free(fr);
}

/* ------------------------------------------------------------ 5. NULL safety */
static void test_null(void)
{
    printf("-- NULL safety --\n");

    dyt_frame_ready_free(NULL);
    ok("free(NULL) is a no-op");

    intcheck("ping(NULL) is 0",      dyt_frame_ready_ping(NULL), 0);
    intcheck("test(NULL) is 0",      dyt_frame_ready_test(NULL), 0);
    intcheck("wait(NULL, 50) is 0",  dyt_frame_ready_wait(NULL, 50), 0);
    intcheck("wait(NULL, -1) is 0",  dyt_frame_ready_wait(NULL, -1), 0);
}

int main(void)
{
    printf("=== frame_ready_test (coalescing wakeup) ===\n");

    test_coalescing();
    test_timeout();
    test_blocking_wait();
    test_threaded();
    test_null();

    if (fails) {
        printf("=== %d FAILURE(S) ===\n", fails);
        return 1;
    }
    printf("=== ALL PASS ===\n");
    return 0;
}
