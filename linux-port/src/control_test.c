/*
 * control_test.c — mock-transfer unit test for dyt_transaction.
 *
 * Tests the canonical transaction state machine (control.c) with a mock
 * dyt_transfer_fn that serves canned bytes — no hardware, no libusb.
 * The mock is stateful: it distinguishes OUT (wIndex=0x1d00), poll
 * (wIndex=0x0200) and IN (wIndex=0x1d08) by the wIndex field, exactly
 * as dyt_transaction issues them.
 *
 * Three cases:
 *   1. success: poll busy ×2 then ready at iter 3, IN returns 15 bytes
 *      → dyt_transaction returns 0, result matches the canned blob.
 *   2. error: poll returns 0x05 (error bits) → returns -2.
 *   3. busy forever: poll always returns 0x01 → returns -1 after the
 *      1000-iteration cap (microseconds with a mock — no I/O).
 *
 * build:  via the Makefile (make control-test)
 */
#include <stdio.h>
#include <string.h>

#include "control.h"

typedef struct {
    int      poll_iter;
    int      poll_ready_at;     /* iteration at which to return ready (0x00) */
    uint8_t  poll_busy;        /* status while busy (default 0x01) */
    uint8_t  poll_error;       /* if non-zero, always return this status */
    int      busy_forever;     /* if 1, always return busy */
    uint8_t  in_result[15];    /* canned 15-byte IN blob */
    int      out_count;
    int      poll_count;
    int      in_count;
} mock_state;

static int mock_xfer(void *handle, uint8_t bmRequestType, uint8_t bRequest,
    uint16_t wValue, uint16_t wIndex, uint8_t *data, uint16_t wLength,
    unsigned timeout)
{
    mock_state *st = (mock_state *)handle;
    (void)bRequest; (void)wValue; (void)timeout;

    /* OUT command register (0x1d00) — return wLength (8) on success. */
    if (wIndex == 0x1d00) {
        st->out_count++;
        return wLength;   /* dyt_diy_communicate normalises 8→0 */
    }

    /* Status poll (0x0200) — 1-byte status. */
    if (wIndex == 0x0200) {
        st->poll_count++;
        st->poll_iter++;
        if (st->busy_forever) {
            data[0] = 0x01;                /* BUSY */
        } else if (st->poll_error) {
            data[0] = st->poll_error;     /* error bits */
        } else if (st->poll_iter < st->poll_ready_at) {
            data[0] = st->poll_busy;      /* busy */
        } else {
            data[0] = 0x00;                /* ready: !(BUSY) && !(READY) → break */
        }
        return 1;
    }

    /* IN result (0x1d08) — 15 bytes. */
    if (wIndex == 0x1d08) {
        st->in_count++;
        if (wLength >= 15)
            memcpy(data, st->in_result, 15);
        return 15;
    }

    return -1;   /* unexpected transfer */
}

static int test_success(void)
{
    mock_state st = {0};
    st.poll_ready_at = 3;       /* ready on iteration 3 (busy on 1-2) */
    st.poll_busy = 0x01;        /* BUSY while waiting */
    for (int i = 0; i < 15; i++) st.in_result[i] = (uint8_t)(0x40 + i);

    uint8_t cmd[8] = { 0x0d, 0xc1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t result[15] = {0};
    int status = -999;

    int rc = dyt_transaction(mock_xfer, &st, cmd, result, &status);

    printf("test_success: rc=%d status=0x%02x "
           "out=%d poll=%d in=%d poll_iter=%d\n",
           rc, status, st.out_count, st.poll_count, st.in_count,
           st.poll_iter);

    if (rc != 0) { printf("  FAIL: expected 0, got %d\n", rc); return 1; }
    if (status != 0x00) { printf("  FAIL: status 0x%02x, expected 0x00\n",
                                 (unsigned)status); return 1; }
    if (st.out_count != 1) { printf("  FAIL: %d OUT calls\n", st.out_count); return 1; }
    if (st.poll_count != 3) { printf("  FAIL: %d poll calls (expected 3)\n",
                                     st.poll_count); return 1; }
    if (st.in_count != 1) { printf("  FAIL: %d IN calls\n", st.in_count); return 1; }
    if (memcmp(result, st.in_result, 15) != 0) {
        printf("  FAIL: result mismatch\n"); return 1;
    }
    printf("  PASS\n");
    return 0;
}

static int test_error(void)
{
    mock_state st = {0};
    st.poll_error = 0x05;       /* bit0 (BUSY) + bit2 (ERROR) */

    uint8_t cmd[8] = { 0x0d, 0xc1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t result[15] = {0};
    int status = -999;

    int rc = dyt_transaction(mock_xfer, &st, cmd, result, &status);

    printf("test_error: rc=%d status=0x%02x poll=%d\n",
           rc, (unsigned)status, st.poll_count);

    if (rc != -2) { printf("  FAIL: expected -2, got %d\n", rc); return 1; }
    if (status != 0x05) { printf("  FAIL: status 0x%02x, expected 0x05\n",
                                 (unsigned)status); return 1; }
    if (st.in_count != 0) { printf("  FAIL: IN should not be called\n"); return 1; }
    printf("  PASS\n");
    return 0;
}

static int test_busy_forever(void)
{
    mock_state st = {0};
    st.busy_forever = 1;

    uint8_t cmd[8] = { 0x0d, 0xc1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t result[15] = {0};
    int status = -999;

    int rc = dyt_transaction(mock_xfer, &st, cmd, result, &status);

    printf("test_busy_forever: rc=%d status=0x%02x poll=%d\n",
           rc, (unsigned)status, st.poll_count);

    if (rc != -1) { printf("  FAIL: expected -1, got %d\n", rc); return 1; }
    if (st.poll_count != 1000) {
        printf("  FAIL: expected 1000 poll calls, got %d\n", st.poll_count);
        return 1;
    }
    if (st.in_count != 0) { printf("  FAIL: IN should not be called\n"); return 1; }
    printf("  PASS\n");
    return 0;
}

/* Also sanity-check the VID/PID → mode dispatch. */
static int test_mode_dispatch(void)
{
    int fails = 0;
    struct { uint16_t vid, pid; dyt_mode_t want; } cases[] = {
        { 0x1514, 0x0001, DYT_MODE_44C  },
        { 0x0bda, 0x5840, DYT_MODE_1000 },
        { 0x0bda, 0x5830, DYT_MODE_1000 },
        { 0x0bda, 0x5846, DYT_MODE_3EB  },
        { 0x0bda, 0x31da, DYT_MODE_3EB  },
        { 0x0581, 0x0b00, DYT_MODE_3EB  },
        { 0x1234, 0x5678, DYT_MODE_0    },
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        dyt_mode_t got = dyt_mode_for_vidpid(cases[i].vid, cases[i].pid);
        if (got != cases[i].want) {
            printf("test_mode_dispatch: VID %04x PID %04x → %d, want %d\n",
                   cases[i].vid, cases[i].pid, (int)got, (int)cases[i].want);
            fails++;
        }
    }
    if (fails) { printf("  FAIL: %d mismatches\n", fails); return 1; }
    printf("test_mode_dispatch: %d cases OK\n",
           (int)(sizeof(cases)/sizeof(cases[0])));
    printf("  PASS\n");
    return 0;
}

int main(void)
{
    int fails = 0;
    printf("=== control_test ===\n");
    fails += test_success();
    fails += test_error();
    fails += test_busy_forever();
    fails += test_mode_dispatch();
    printf("=== %s ===\n", fails ? "FAIL" : "ALL PASS");
    return fails ? 1 : 0;
}
