/*
 * frame_ready.c — the coalescing wakeup.  See frame_ready.h.
 *
 * One mutex, one condition variable, one pending bit.  The bit is read and
 * written only under the mutex, so it needs no atomics and must NOT be
 * volatile: the mutex supplies every ordering this needs, and marking it
 * volatile would suggest otherwise while buying nothing.
 *
 * ping() signals *after* unlocking.  That is safe here because the predicate
 * is mutex-guarded and every waiter re-checks it in a loop, so a signal that
 * arrives late cannot lose a wakeup — the waiter observes pending == 1 and
 * does not sleep.  Signalling outside the lock also means the woken consumer
 * does not immediately block on the mutex ping() still holds, which matters
 * because ping() runs on the frame callback thread.
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -Isrc -c frame_ready.c
 */
#define _POSIX_C_SOURCE 200809L   /* pthread_condattr_setclock, clock_gettime */

#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include "frame_ready.h"

struct dyt_frame_ready {
    pthread_mutex_t m;
    pthread_cond_t  cv;         /* CLOCK_MONOTONIC — see dyt_frame_ready_create */
    int             pending;    /* guarded by m */
};

dyt_frame_ready_t *dyt_frame_ready_create(void)
{
    dyt_frame_ready_t *fr;
    pthread_condattr_t attr;

    fr = calloc(1, sizeof *fr);
    if (!fr)
        return NULL;

    if (pthread_mutex_init(&fr->m, NULL) != 0) {
        free(fr);
        return NULL;
    }

    /* The timed wait must ride a clock that a system-clock step cannot move.
     * pthread_cond_timedwait interprets its deadline against the *condition
     * variable's* clock, which defaults to CLOCK_REALTIME — so an NTP step
     * would make the wait fire early, late, or hang.  The attribute has to be
     * set before cond_init; it cannot be changed afterwards. */
    if (pthread_condattr_init(&attr) != 0) {
        pthread_mutex_destroy(&fr->m);
        free(fr);
        return NULL;
    }
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0 ||
        pthread_cond_init(&fr->cv, &attr) != 0) {
        pthread_condattr_destroy(&attr);
        pthread_mutex_destroy(&fr->m);
        free(fr);
        return NULL;
    }
    pthread_condattr_destroy(&attr);

    return fr;
}

void dyt_frame_ready_free(dyt_frame_ready_t *fr)
{
    if (!fr)
        return;
    pthread_cond_destroy(&fr->cv);
    pthread_mutex_destroy(&fr->m);
    free(fr);
}

int dyt_frame_ready_ping(dyt_frame_ready_t *fr)
{
    int armed;

    if (!fr)
        return 0;

    pthread_mutex_lock(&fr->m);
    armed       = !fr->pending;
    fr->pending = 1;
    pthread_mutex_unlock(&fr->m);

    /* Only the ping that armed the wakeup signals.  A ping that coalesced has
     * nothing new to say — a wakeup is already pending. */
    if (armed)
        pthread_cond_signal(&fr->cv);

    return armed;
}

int dyt_frame_ready_test(dyt_frame_ready_t *fr)
{
    int got;

    if (!fr)
        return 0;

    pthread_mutex_lock(&fr->m);
    got         = fr->pending;
    fr->pending = 0;
    pthread_mutex_unlock(&fr->m);

    return got;
}

int dyt_frame_ready_wait(dyt_frame_ready_t *fr, int timeout_ms)
{
    struct timespec deadline = { 0, 0 };
    int             got      = 0;

    if (!fr)
        return 0;

    /* Non-blocking is exactly test(), so route it there rather than through
     * the clock and the condvar.  This is also what keeps the two entry points
     * from drifting apart. */
    if (timeout_ms == 0)
        return dyt_frame_ready_test(fr);

    pthread_mutex_lock(&fr->m);

    if (timeout_ms < 0) {
        while (!fr->pending)
            pthread_cond_wait(&fr->cv, &fr->m);
        fr->pending = 0;
        got         = 1;
    } else {
        /* One absolute deadline for the whole wait, computed once.  Deriving
         * it from "now" on every wakeup would let each spurious wake extend
         * the timeout, turning a bounded wait into an unbounded one. */
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec  += timeout_ms / 1000;
        deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }

        while (!fr->pending) {
            /* Break on any error, not just ETIMEDOUT: a persistent EINVAL
             * would otherwise spin. */
            if (pthread_cond_timedwait(&fr->cv, &fr->m, &deadline) != 0)
                break;
        }

        /* A ping can land exactly as the deadline expires, so look once more
         * before reporting a timeout. */
        if (fr->pending) {
            fr->pending = 0;
            got         = 1;
        }
    }

    pthread_mutex_unlock(&fr->m);
    return got;
}
