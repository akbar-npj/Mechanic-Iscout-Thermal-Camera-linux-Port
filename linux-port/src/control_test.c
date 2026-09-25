/*
 * control_test.c — mock-transfer unit tests for control.c.
 *
 * Tests the transaction state machine and the three device-identity readers
 * with a mock dyt_transfer_fn that serves canned bytes — no hardware, no
 * libusb.  The mock is stateful and records every transfer it sees, so the
 * tests can assert the *wire form* (which register, which payload, which
 * length) and not just the parsed result.
 *
 * That matters for the parameter read-back: reading the wrong register does
 * not fail on the real device, it returns whatever is left in the shared
 * buffer.  Asserting the exact OUT/IN sequence is the only way to catch it
 * without hardware.
 *
 * build:  via the Makefile (make control-test)
 */
#include <stdio.h>
#include <string.h>

#include "control.h"
#include "serial.h"   /* dyt_sn_str — the serial display helper */

#define MOCK_MAX_OUT 8

typedef struct {
    /* poll behaviour */
    int      poll_iter;
    int      poll_ready_at;    /* iteration at which to answer ready (0x00) */
    uint8_t  poll_busy;        /* status while busy (default 0x01) */
    uint8_t  poll_error;       /* if non-zero, answer this from poll_error_at */
    int      poll_error_at;    /* iteration from which poll_error applies */
    int      busy_forever;     /* if 1, always return busy */

    /* canned results */
    uint8_t  in_1d08[16];
    int      in_1d08_len;
    uint8_t  in_1d10[2];
    int      in_1d10_len;

    /* if fail_param_on, the OUT of parameter slot fail_param_slot fails */
    int      fail_param_on;
    int      fail_param_slot;

    /* observed traffic */
    int      out_count, poll_count, in_count;
    int      n_out;
    uint16_t out_reg[MOCK_MAX_OUT];
    uint8_t  out_data[MOCK_MAX_OUT][8];
} mock_state;

static int mock_xfer(void *handle, uint8_t bmRequestType, uint8_t bRequest,
    uint16_t wValue, uint16_t wIndex, uint8_t *data, uint16_t wLength,
    unsigned timeout)
{
    mock_state *st = (mock_state *)handle;
    (void)bRequest; (void)wValue; (void)timeout;

    if (bmRequestType == 0x41) {          /* host -> device */
        st->out_count++;
        if (st->n_out < MOCK_MAX_OUT) {
            st->out_reg[st->n_out] = wIndex;
            memcpy(st->out_data[st->n_out], data, wLength > 8 ? 8 : wLength);
            st->n_out++;
        }
        /* Simulate a device that rejects one parameter slot outright. */
        if (wIndex == 0x9d00 && wLength == 8 && data[0] == 0x14 &&
            st->fail_param_on && data[3] == (uint8_t)st->fail_param_slot)
            return -1;
        return wLength;   /* dyt_diy_communicate normalises 8 -> 0 */
    }

    if (bmRequestType == 0xc1) {          /* device -> host */
        if (wIndex == 0x0200) {           /* status poll */
            st->poll_count++;
            st->poll_iter++;
            if (st->busy_forever) {
                data[0] = 0x01;
            } else if (st->poll_error && st->poll_iter >= st->poll_error_at) {
                data[0] = st->poll_error;
            } else if (st->poll_iter < st->poll_ready_at) {
                data[0] = st->poll_busy;
            } else {
                data[0] = 0x00;
            }
            return 1;
        }
        if (wIndex == 0x1d08) {           /* primary result register */
            st->in_count++;
            memcpy(data, st->in_1d08, (size_t)st->in_1d08_len);
            return st->in_1d08_len;
        }
        if (wIndex == 0x1d10) {           /* parameter read-back register */
            st->in_count++;
            memcpy(data, st->in_1d10, (size_t)st->in_1d10_len);
            return st->in_1d10_len;
        }
    }

    return -1;   /* unexpected transfer */
}

/* Did OUT number `i` carry exactly these bytes to this register? */
static int out_is(const mock_state *st, int i, uint16_t reg,
                  const uint8_t *want, int n)
{
    if (i < 0 || i >= st->n_out)
        return 0;
    if (st->out_reg[i] != reg)
        return 0;
    return memcmp(st->out_data[i], want, (size_t)n) == 0;
}

/* ------------------------------------------------------ transaction core */

static int test_success(void)
{
    mock_state st = {0};
    uint8_t cmd[8] = { 0x0d, 0xc1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t result[15] = {0};
    int status = -999, i, rc;

    st.poll_ready_at = 3;       /* ready on iteration 3 (busy on 1-2) */
    st.poll_busy = 0x01;
    st.in_1d08_len = 15;
    for (i = 0; i < 15; i++) st.in_1d08[i] = (uint8_t)(0x40 + i);

    rc = dyt_transaction(mock_xfer, &st, cmd, result, &status);

    printf("test_success: rc=%d status=0x%02x out=%d poll=%d in=%d poll_iter=%d\n",
           rc, status, st.out_count, st.poll_count, st.in_count, st.poll_iter);

    if (rc != 0) { printf("  FAIL: expected 0, got %d\n", rc); return 1; }
    if (status != 0x00) { printf("  FAIL: status 0x%02x\n", (unsigned)status); return 1; }
    if (st.out_count != 1) { printf("  FAIL: %d OUT calls\n", st.out_count); return 1; }
    if (st.poll_count != 3) { printf("  FAIL: %d poll calls (expected 3)\n", st.poll_count); return 1; }
    if (st.in_count != 1) { printf("  FAIL: %d IN calls\n", st.in_count); return 1; }
    if (!out_is(&st, 0, 0x1d00, cmd, 8)) {
        printf("  FAIL: OUT payload/register wrong\n"); return 1;
    }
    if (memcmp(result, st.in_1d08, 15) != 0) {
        printf("  FAIL: result mismatch\n"); return 1;
    }
    printf("  PASS\n");
    return 0;
}

static int test_error(void)
{
    mock_state st = {0};
    uint8_t cmd[8] = { 0x0d, 0xc1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t result[15] = {0};
    int status = -999, rc;

    st.poll_error = 0x05;       /* bit0 (BUSY) + bit2 (ERROR) */
    st.poll_error_at = 1;
    st.in_1d08_len = 15;

    rc = dyt_transaction(mock_xfer, &st, cmd, result, &status);

    printf("test_error: rc=%d status=0x%02x poll=%d\n",
           rc, (unsigned)status, st.poll_count);

    if (rc != -2) { printf("  FAIL: expected -2, got %d\n", rc); return 1; }
    if (status != 0x05) { printf("  FAIL: status 0x%02x\n", (unsigned)status); return 1; }
    if (st.in_count != 0) { printf("  FAIL: IN should not be called\n"); return 1; }
    printf("  PASS\n");
    return 0;
}

static int test_busy_forever(void)
{
    mock_state st = {0};
    uint8_t cmd[8] = { 0x0d, 0xc1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t result[15] = {0};
    int status = -999, rc;

    st.busy_forever = 1;
    st.in_1d08_len = 15;

    rc = dyt_transaction(mock_xfer, &st, cmd, result, &status);

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

/* ------------------------------------------------------- device identity */

/* The module serial, exactly as read off the reference unit 2026-09-25 and
 * as MechaniscoutPcap/4.pcapng frame 4285 carries it. */
static const uint8_t kModuleSn[16] = {
    '2','0','2','6','0','5','5','7','5','2','5','9', 0, 0, 0, 0
};

/* The raw user serial — frame 4301.  DecryptSNE (serial.h) turns this into
 * "DYCSTI09GG01292" using the key from kModuleSn (RE Docs 04 §4.9). */
static const uint8_t kUserSnRaw[15] = {
    0x06,0x7d,0x5a,0x48,0x2c,0x66,0x6a,0x79,0x79,0x58,0x2d,0x44,0x2f,0x24,0x2f
};

static int test_read_module_sn(void)
{
    static const uint8_t want[8] =
        { 0x05, 0x84, 0x07, 0x00, 0x00, 0x00, 0x00, 0x10 };
    mock_state st = {0};
    uint8_t sn[16];
    char s[17];
    int rc, len;

    memset(sn, 0xaa, sizeof sn);
    st.in_1d08_len = 16;
    memcpy(st.in_1d08, kModuleSn, 16);

    rc = dyt_read_module_sn(mock_xfer, &st, sn);

    printf("test_read_module_sn: rc=%d out=%d poll=%d in=%d\n",
           rc, st.out_count, st.poll_count, st.in_count);

    if (rc != DYT_READ_OK) { printf("  FAIL: rc=%d\n", rc); return 1; }
    if (!out_is(&st, 0, 0x1d00, want, 8)) {
        printf("  FAIL: command payload/register wrong\n"); return 1; }
    if (st.in_count != 1) { printf("  FAIL: %d IN calls\n", st.in_count); return 1; }
    if (memcmp(sn, kModuleSn, 16) != 0) {
        printf("  FAIL: serial mismatch\n"); return 1; }

    len = dyt_sn_str(s, sn, 16);
    printf("  module serial \"%s\" (len %d)\n", s, len);
    if (strcmp(s, "202605575259") != 0 || len != 12) {
        printf("  FAIL: string \"%s\" len %d\n", s, len); return 1; }
    printf("  PASS\n");
    return 0;
}

static int test_read_user_sn(void)
{
    static const uint8_t want[8] =
        { 0x01, 0x82, 0x00, 0x7f, 0xf0, 0x00, 0x00, 0x0f };
    mock_state st = {0};
    uint8_t sn[15], dec[15];
    char s[16];
    int rc;

    memset(sn, 0xaa, sizeof sn);
    st.in_1d08_len = 15;
    memcpy(st.in_1d08, kUserSnRaw, 15);

    rc = dyt_read_user_sn(mock_xfer, &st, sn);

    printf("test_read_user_sn: rc=%d out=%d poll=%d in=%d\n",
           rc, st.out_count, st.poll_count, st.in_count);

    if (rc != DYT_READ_OK) { printf("  FAIL: rc=%d\n", rc); return 1; }
    if (!out_is(&st, 0, 0x1d00, want, 8)) {
        printf("  FAIL: command payload/register wrong\n"); return 1; }
    if (memcmp(sn, kUserSnRaw, 15) != 0) {
        printf("  FAIL: raw user serial mismatch\n"); return 1; }

    /* The read is only half the job: decode it with the module-serial key. */
    dyt_decrypt_sne(dec, sn, dyt_sn_key(kModuleSn, 16));
    dyt_serial_str(s, dec);
    printf("  decoded user serial \"%s\"\n", s);
    if (strcmp(s, "DYCSTI09GG01292") != 0) {
        printf("  FAIL: decode \"%s\"\n", s); return 1; }
    printf("  PASS\n");
    return 0;
}

/* A read that fails must not leave the caller's buffer looking valid. */
static int test_read_failure(void)
{
    mock_state st = {0};
    uint8_t sn[16];
    int rc;

    memset(sn, 0xaa, sizeof sn);
    st.in_1d08_len = 15;        /* device answers short */
    st.busy_forever = 1;

    rc = dyt_read_module_sn(mock_xfer, &st, sn);
    printf("test_read_failure: rc=%d\n", rc);
    if (rc == DYT_READ_OK) { printf("  FAIL: reported success\n"); return 1; }
    if (sn[0] != 0xaa) { printf("  FAIL: wrote to the buffer anyway\n"); return 1; }

    /* A NULL out pointer is a programming error, not a device error. */
    rc = dyt_read_module_sn(mock_xfer, &st, NULL);
    if (rc != DYT_READ_RANGE) { printf("  FAIL: NULL -> rc=%d\n", rc); return 1; }
    printf("  PASS\n");
    return 0;
}

/* ---------------------------------------------------- stored parameters */

static int test_read_param(void)
{
    static const uint8_t want_cmd[8] =
        { 0x14, 0x85, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t want_sel[8] = { 0,0,0,0,0,0,0,0x02 };
    mock_state st = {0};
    uint16_t v = 0;
    int rc;

    st.in_1d10_len = 2;
    st.in_1d10[0] = 0x00;       /* big-endian 127, as frame 4233 returns */
    st.in_1d10[1] = 0x7f;

    rc = dyt_read_param(mock_xfer, &st, DYT_PARAM_EMISSIVITY, &v);

    printf("test_read_param: rc=%d value=%u out=%d poll=%d in=%d\n",
           rc, v, st.out_count, st.poll_count, st.in_count);

    if (rc != DYT_READ_OK) { printf("  FAIL: rc=%d\n", rc); return 1; }
    if (v != 127) { printf("  FAIL: value %u, want 127\n", v); return 1; }
    if (st.out_count != 2) { printf("  FAIL: %d OUT calls, want 2\n", st.out_count); return 1; }
    if (!out_is(&st, 0, 0x9d00, want_cmd, 8)) {
        printf("  FAIL: command OUT wrong\n"); return 1; }
    if (!out_is(&st, 1, 0x1d08, want_sel, 8)) {
        printf("  FAIL: result-length pre-fill OUT wrong\n"); return 1; }
    if (st.in_count != 1) { printf("  FAIL: %d IN calls\n", st.in_count); return 1; }
    printf("  PASS\n");
    return 0;
}

/* The index byte is the payload, so it must survive into the command. */
static int test_read_param_index(void)
{
    mock_state st = {0};
    uint16_t v = 0;
    int rc, i;

    st.in_1d10_len = 2;
    st.in_1d10[0] = 0x01;
    st.in_1d10[1] = 0x2c;       /* 300, as frame 4251 returns */

    for (i = 0; i <= 255; i += 85) {
        st.n_out = 0;
        rc = dyt_read_param(mock_xfer, &st, (unsigned)i, &v);
        if (rc != DYT_READ_OK) { printf("  FAIL: index %d rc=%d\n", i, rc); return 1; }
        if (v != 300) { printf("  FAIL: index %d value %u\n", i, v); return 1; }
        if (st.out_data[0][3] != (uint8_t)i) {
            printf("  FAIL: index %d encoded as %u\n", i, st.out_data[0][3]);
            return 1;
        }
    }

    /* Out of range is a caller error and must not touch the bus. */
    st.n_out = 0; st.out_count = 0;
    rc = dyt_read_param(mock_xfer, &st, 256u, &v);
    if (rc != DYT_READ_RANGE) { printf("  FAIL: 256 -> rc=%d\n", rc); return 1; }
    if (st.out_count != 0) { printf("  FAIL: 256 issued a transfer\n"); return 1; }
    printf("test_read_param_index: 0/85/170/255 encoded, 256 rejected\n");
    printf("  PASS\n");
    return 0;
}

static int test_read_param_status_error(void)
{
    mock_state st = {0};
    uint16_t v = 0;
    int rc;

    st.poll_error = 0x0e;       /* what the capture's device answers */
    st.poll_error_at = 1;
    st.in_1d10_len = 2;

    rc = dyt_read_param(mock_xfer, &st, 1, &v);
    printf("test_read_param_status_error: rc=%d in=%d\n", rc, st.in_count);
    if (rc != DYT_READ_STATUS) { printf("  FAIL: rc=%d, want %d\n", rc, DYT_READ_STATUS); return 1; }
    if (st.in_count != 0) { printf("  FAIL: read after an error status\n"); return 1; }
    printf("  PASS\n");
    return 0;
}

static int test_read_all_params(void)
{
    mock_state st = {0};
    dyt_params_t p;
    int n;

    /* One slot refuses outright; the other fifteen answer 300. */
    st.in_1d10_len = 2;
    st.in_1d10[0] = 0x01;
    st.in_1d10[1] = 0x2c;
    st.fail_param_on = 1;
    st.fail_param_slot = 7;

    n = dyt_read_all_params(mock_xfer, &st, &p);

    printf("test_read_all_params: %d/%d slots, slot7 ok=%d, slot1 ok=%d\n",
           n, DYT_PARAM_N, dyt_params_ok(&p, 7), dyt_params_ok(&p, 1));

    if (n != DYT_PARAM_N - 1) {
        printf("  FAIL: read %d slots, want %d\n", n, DYT_PARAM_N - 1);
        return 1;
    }
    if (dyt_params_ok(&p, 7)) { printf("  FAIL: failed slot marked ok\n"); return 1; }
    if (p.raw[7] != 0) { printf("  FAIL: failed slot kept value %u\n", p.raw[7]); return 1; }
    if (!dyt_params_ok(&p, 1)) { printf("  FAIL: good slot not ok\n"); return 1; }
    if (p.raw[1] != 300) { printf("  FAIL: slot1 = %u\n", p.raw[1]); return 1; }
    if (dyt_params_count(&p) != DYT_PARAM_N - 1) {
        printf("  FAIL: count %d\n", dyt_params_count(&p)); return 1; }
    /* Two OUTs per slot, except the rejected one which never gets its
     * result-length pre-fill. */
    if (st.out_count != 2 * DYT_PARAM_N - 1) {
        printf("  FAIL: %d OUT calls, want %d\n", st.out_count, 2 * DYT_PARAM_N - 1);
        return 1;
    }
    printf("  PASS\n");
    return 0;
}

/* The one write.  It must be exactly two OUT transfers and *no* status
 * poll — the vendor's sendOrder does not poll (RE Docs 04 §4.2), and a
 * poll here would be a different, unverified transaction shape. */
static int test_write_param(void)
{
    static const uint8_t want_cmd[8] =
        { 0x14, 0xc5, 0x00, 0x03, 0x00, 0x00, 0x00, 0x7f };
    static const uint8_t want_sel[8] = { 0,0,0,0,0,0,0,0x02 };
    mock_state st = {0};
    int rc;

    rc = dyt_write_param(mock_xfer, &st, DYT_ORDER_EMISSIVITY, 0.9921875f);

    printf("test_write_param: rc=%d out=%d poll=%d in=%d\n",
           rc, st.out_count, st.poll_count, st.in_count);

    if (rc != DYT_READ_OK) { printf("  FAIL: rc=%d\n", rc); return 1; }
    if (st.out_count != 2) {
        printf("  FAIL: %d OUT calls, want 2\n", st.out_count); return 1; }
    if (!out_is(&st, 0, 0x9d00, want_cmd, 8)) {
        printf("  FAIL: command OUT wrong\n"); return 1; }
    if (!out_is(&st, 1, 0x1d08, want_sel, 8)) {
        printf("  FAIL: result-length pre-fill OUT wrong\n"); return 1; }
    if (st.poll_count != 0) {
        printf("  FAIL: %d status polls, want 0\n", st.poll_count); return 1; }
    if (st.in_count != 0) {
        printf("  FAIL: %d IN calls, want 0\n", st.in_count); return 1; }

    /* A rejected value must not reach the device at all. */
    st.out_count = 0;
    rc = dyt_write_param(mock_xfer, &st, DYT_ORDER_EMISSIVITY, -1.0f);
    if (rc != DYT_READ_RANGE || st.out_count != 0) {
        printf("  FAIL: bad value rc=%d out=%d\n", rc, st.out_count); return 1; }
    rc = dyt_write_param(mock_xfer, &st, 9, 1.0f);
    if (rc != DYT_READ_RANGE || st.out_count != 0) {
        printf("  FAIL: bad type rc=%d out=%d\n", rc, st.out_count); return 1; }

    printf("  PASS\n");
    return 0;
}

/* ---------------------------------------------------------- opcode table */

static const dyt_opcode_t *find_op(const char *name)
{
    int i;
    for (i = 0; i < dyt_opcodes_n; i++)
        if (strcmp(dyt_opcodes[i].name, name) == 0)
            return &dyt_opcodes[i];
    return NULL;
}

/* The table's result geometry must be self-consistent and must match the
 * three commands whose answers are verified against hardware. */
static int test_opcode_table(void)
{
    static const struct {
        const char *name;
        uint16_t cmd_reg, res_reg, res_len;
    } want[] = {
        { "getTinyCModuleSn",      0x1d00, 0x1d08, 16 },
        { "getTinyCUserSn",        0x1d00, 0x1d08, 15 },
        { "getTinyCParams",        0x9d00, 0x1d10,  2 },
        { "setMachineSetting_read",0x9d00, 0x1d10,  2 },
        { "getTinyCUserData",      0x1d00, 0x1d08,  1 },
    };
    int i, fails = 0;

    for (i = 0; i < dyt_opcodes_n; i++) {
        const dyt_opcode_t *op = &dyt_opcodes[i];
        int has_res = op->result_len != 0 || op->result_wIndex != 0;
        /* Either both result fields are set, or neither. */
        if (has_res && (op->result_len == 0 || op->result_wIndex == 0)) {
            printf("  FAIL: '%s' has a half-specified result\n", op->name);
            fails++;
        }
    }

    for (unsigned k = 0; k < sizeof(want)/sizeof(want[0]); k++) {
        const dyt_opcode_t *op = find_op(want[k].name);
        if (!op) {
            printf("  FAIL: '%s' missing from the table\n", want[k].name);
            fails++; continue;
        }
        if (op->wIndex != want[k].cmd_reg || op->result_wIndex != want[k].res_reg ||
            op->result_len != want[k].res_len) {
            printf("  FAIL: '%s' cmd=0x%04x res=0x%04x/%u, want 0x%04x/0x%04x/%u\n",
                   op->name, op->wIndex, op->result_wIndex, op->result_len,
                   want[k].cmd_reg, want[k].res_reg, want[k].res_len);
            fails++;
        }
    }

    /* Both write entries must stay WRITE-flagged, or probe's guard is moot:
     * sendTinyCParamsModification (runtime/volatile) and setMachineSetting_write
     * (persistent calibration).  The flag is what keeps probe read-only. */
    {
        const char *writes[] = { "sendTinyCParamsModification",
                                 "setMachineSetting_write" };
        for (unsigned k = 0; k < 2; k++) {
            const dyt_opcode_t *op = find_op(writes[k]);
            if (!op || !op->note || !strstr(op->note, "WRITE")) {
                printf("  FAIL: '%s' is not WRITE-flagged\n", writes[k]);
                fails++;
            }
        }
    }

    /* The runtime write is two OUT transfers with no read-back (verified
     * 2026-09-25); its table entry must not claim a result.  A stale 0x1d10/2
     * here once described the *read* transaction, not the write. */
    {
        const dyt_opcode_t *op = find_op("sendTinyCParamsModification");
        if (op && (op->result_wIndex != 0 || op->result_len != 0)) {
            printf("  FAIL: runtime write claims a result 0x%04x/%u\n",
                   op->result_wIndex, op->result_len);
            fails++;
        }
    }

    printf("test_opcode_table: %d entries, %d failure(s)\n", dyt_opcodes_n, fails);
    if (fails) { printf("  FAIL\n"); return 1; }
    printf("  PASS\n");
    return 0;
}

/* dyt_verify_serial is a thin wrapper over the module-SN read now; it used
 * to issue a 15-byte read after `0d c1`, which the device does not answer. */
static int test_verify_serial(void)
{
    mock_state st = {0};
    uint8_t sn[16];
    int rc;

    st.in_1d08_len = 16;
    memcpy(st.in_1d08, kModuleSn, 16);

    rc = dyt_verify_serial(mock_xfer, &st, "configs_maintenanceguy.txt", sn);
    printf("test_verify_serial: rc=%d\n", rc);
    if (rc != DYT_READ_OK) { printf("  FAIL: rc=%d\n", rc); return 1; }
    if (memcmp(sn, kModuleSn, 16) != 0) { printf("  FAIL: mismatch\n"); return 1; }
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
    fails += test_read_module_sn();
    fails += test_read_user_sn();
    fails += test_read_failure();
    fails += test_read_param();
    fails += test_read_param_index();
    fails += test_read_param_status_error();
    fails += test_read_all_params();
    fails += test_write_param();
    fails += test_opcode_table();
    fails += test_verify_serial();
    printf("=== %s ===\n", fails ? "FAIL" : "ALL PASS");
    return fails ? 1 : 0;
}
