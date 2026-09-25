/*
 * frame_ready.h — a coalescing wakeup: one pending frame, at most one wakeup.
 *
 * A producer that signals once per frame can queue one event per frame behind
 * a slow consumer — which is the opposite of what a latest-frame display
 * wants, since every queued event but the last describes a frame that has
 * already been superseded.  This collapses that to a single pending bit: the
 * first ping arms a wakeup, and every ping until it is consumed folds into
 * it.  So the consumer is told "there is work", never "here are N frames".
 *
 * Note what this is *not*.  The frame itself already lives in dyt_session_t,
 * lock-protected, overwritten in place, and rendered under that same lock, so
 * a slow consumer already drops frames and cannot race the render.  This
 * primitive carries no pixels and no payload — it only says the frame moved.
 * That is why it is not part of the session: session.h constraint 2 keeps the
 * session device-free and poll-based, and nothing here changes that.
 *
 * Toolkit-free and device-free.  The front end owns the wiring.  A Qt6 app
 * wraps dyt_session_capture_on_frame() with its own callback that calls the
 * engine adapter and then pings, and services the ping from the GUI thread; a
 * recorder loop waits instead of sleeping.
 *
 * Single consumer.  A ping is consumed by exactly one test()/wait(); two
 * threads waiting on the same handle would steal each other's pings.
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -Isrc -c frame_ready.c
 */
#ifndef DYT_FRAME_READY_H
#define DYT_FRAME_READY_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dyt_frame_ready dyt_frame_ready_t;

/* Allocate a wakeup.  Returns NULL on failure. */
dyt_frame_ready_t *dyt_frame_ready_create(void);

/* Release it.  Safe on NULL.
 *
 * The caller must have stopped every thread that could still ping or wait on
 * it first: freeing one out from under a blocked waiter is undefined, and no
 * flag can be checked cheaply enough to make it safe on the frame path. */
void dyt_frame_ready_free(dyt_frame_ready_t *fr);

/* Producer side.  Callable from any thread, never blocks, cannot fail.
 *
 * Returns 1 if this ping armed the wakeup, or 0 if a previous ping was still
 * unconsumed (this one coalesced into it).  That return value is the whole
 * coalescing contract, and it is deliberately the only accounting offered: a
 * caller that wants a drop count writes
 *
 *     if (!dyt_frame_ready_ping(fr)) drops++;
 *
 * Keeping the count in the caller is what lets "coalesced" mean whatever that
 * caller needs it to mean — a coalesced ping is not necessarily a frame the
 * user lost.  Safe on NULL (returns 0). */
int dyt_frame_ready_ping(dyt_frame_ready_t *fr);

/* Consumer side, non-blocking.  Consumes a pending ping.
 * Returns 1 if there was one, else 0.  Safe on NULL (returns 0). */
int dyt_frame_ready_test(dyt_frame_ready_t *fr);

/* Consumer side, blocking.  Waits up to `timeout_ms` for a ping and consumes
 * it.  `timeout_ms < 0` waits indefinitely; 0 is non-blocking and identical to
 * test().  Returns 1 if a ping was consumed, else 0 (timeout, or NULL).
 *
 * The wait rides CLOCK_MONOTONIC, so a system-clock step cannot make it fire
 * early or hang. */
int dyt_frame_ready_wait(dyt_frame_ready_t *fr, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* DYT_FRAME_READY_H */
