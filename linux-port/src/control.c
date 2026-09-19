/*
 * control.c — USB control primitive, transaction state machine, and
 * opcode table for the DYT thermal camera Linux port.
 *
 * Everything here is transcribed from RE Docs 04 (usb-protocol), which
 * is verified against libuvc.so / libUVCCamera.so decompilation.  The
 * transaction state machine is callback-abstracted (dyt_transfer_fn)
 * so it can be unit-tested with a mock that serves canned bytes
 * (control_test.c) — no hardware or libusb needed.
 *
 * build:  cc -O2 -g -Wall -Wextra -Wno-unused-parameter -ffp-contract=off \
 *           -I. [-DDYT_HAVE_LIBUSB -lusb-1.0] -c control.c -o control.o
 */
#include <stddef.h>
#include <string.h>
#include "control.h"
#include "serial.h"

/* ----------------------------------------------------------------- mode dispatch */

dyt_mode_t dyt_mode_for_vidpid(uint16_t vid, uint16_t pid)
{
    /* RE Docs 04 §4.10 — UVCCamera::connect @ 0x15c28c.  First two
     * params are VID then PID (standard saki4510t/libuvc signature). */
    if (vid == 0x1514 && pid == 0x0001)
        return DYT_MODE_44C;
    if ((vid == 0x0bda && pid == 0x5840) ||
        (vid == 0x0bda && pid == 0x5830))
        return DYT_MODE_1000;
    if ((vid == 0x0bda && pid == 0x5846) ||
        (vid == 0x0bda && pid == 0x31da) ||
        (vid == 0x0581 && pid == 0x0b00))
        return DYT_MODE_3EB;
    return DYT_MODE_0;
}

/* ----------------------------------------------------------------- vendor primitive */

int dyt_diy_communicate(dyt_transfer_fn xfer, void *handle,
                        uint8_t  bmRequestType,
                        uint8_t  bRequest,
                        uint16_t wValue,
                        uint16_t wIndex,
                        uint8_t *data,
                        uint16_t wLength)
{
    /* RE Docs 04 §4.2: timeout is always 1000 ms; only ret==8 is
     * normalised to 0 (8-byte command-register writes).  1-, 2-, 15-
     * and 28-byte transfers return the raw libusb byte count. */
    int r = xfer(handle, bmRequestType, bRequest, wValue, wIndex,
                 data, wLength, 1000);
    if (r == 8) r = 0;
    return r;
}

/* ----------------------------------------------------------------- transaction */

int dyt_transaction_ex(dyt_transfer_fn xfer, void *handle,
                       const uint8_t cmd[8], uint16_t cmd_wIndex,
                       uint8_t *result, int result_len, int *status_out)
{
    uint8_t status = 0;
    int i, done = 0;

    /* (1) OUT the 8-byte command to the command register (0x1d00, or
     * 0x9d00 for stream start / parameter reads).  After the ret==8→0
     * normalisation, success is 0. */
    if (dyt_diy_communicate(xfer, handle, 0x41, 0x45, 0x0078, cmd_wIndex,
                            (uint8_t *)cmd, 8) != 0) {
        if (status_out) *status_out = status;
        return -1;
    }

    /* (2) Poll the status byte (0x0200) until not-busy && not-pending.
     * Loop exits when bit0 (BUSY) and bit1 (READY) are both clear.
     * Any error bit (bits 2..7) aborts with -2.  Max 1000 iterations. */
    for (i = 0; i < 1000; i++) {
        if (dyt_diy_communicate(xfer, handle, 0xC1, 0x44, 0x0078, 0x0200,
                                &status, 1) != 1) {
            if (status_out) *status_out = status;
            return -1;
        }
        if (!(status & DYT_STATUS_BUSY) && !(status & DYT_STATUS_READY)) {
            done = 1;
            break;
        }
        if (status & DYT_STATUS_ERROR) {
            if (status_out) *status_out = status;
            return -2;
        }
    }
    if (status_out) *status_out = status;
    if (!done)
        return -1;   /* poll loop exhausted — device stuck busy */

    /* (3) IN the result from the result register (0x1d08).  The vendor
     * returns the raw byte count, so the caller compares against the
     * expected length. */
    if (dyt_diy_communicate(xfer, handle, 0xC1, 0x44, 0x0078, 0x1d08,
                            result, (uint16_t)result_len) != result_len)
        return -1;
    return 0;
}

int dyt_transaction(dyt_transfer_fn xfer, void *handle,
                    const uint8_t cmd[8],
                    uint8_t *result15, int *status_out)
{
    return dyt_transaction_ex(xfer, handle, cmd, 0x1d00,
                              result15, 15, status_out);
}

/* ----------------------------------------------------------------- opcode table */

/* RE Docs 04 §4.2 — the static .rodata constants from libUVCCamera.so.
 * These are the 8-byte command payloads memcpy'd into a stack buffer
 * before each transfer.  wIndex is the register written (0x1d00 or
 * 0x9d00); note dyt_transaction always writes 0x1d00, so 0x9d00
 * opcodes must be sent via dyt_diy_communicate directly. */
const dyt_opcode_t dyt_opcodes[] = {
    { "tinyStartStream",
      { 0x0f, 0xc1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09 }, 0x9d00, "" },
    { "setTinyCOutputADValue",
      { 0x0a, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 0x1d00, "" },
    { "getTinyCUserData",
      { 0x0d, 0xc1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 0x1d00, "" },
    { "getTinyCParams",
      { 0x14, 0x85, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00 }, 0x9d00, "" },
    { "sendTinyCParamsModification",
      { 0x14, 0xc5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 0x9d00,
      "WRITE — writes parameter block (persistent)" },
    { "setMachineSetting_write",
      { 0x14, 0xc5, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00 }, 0x9d00,
      "WRITE — writes calibration coefficient (irreversible)" },
    { "setMachineSetting_read",
      { 0x14, 0x85, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00 }, 0x9d00, "" },
    { "do_tinyC_order_case5",
      { 0x0d, 0x8b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 }, 0x1d00, "" },
    { "do_tinyC_order_case7a",
      { 0x14, 0x83, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00 }, 0x9d00, "" },
    { "do_tinyC_order_case7b",
      { 0x14, 0xc3, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00 }, 0x9d00, "" },
};
const int dyt_opcodes_n =
    (int)(sizeof(dyt_opcodes) / sizeof(dyt_opcodes[0]));

/* tinyStartStream2 — 9-byte payload written to 0x1d08 (RE Docs 04 §4.2).
 * Separate from the 8-byte table because its length differs. */
const uint8_t dyt_tinyStartStream2_cmd[9] = {
    0x00, 0x00, 0x01, 0x00, 0x01, 0x80, 0x19, 0x00, 0x02
};

/* ----------------------------------------------------------------- libusb backend */

#ifdef DYT_HAVE_LIBUSB
#include <libusb.h>

static int dyt_libusb_xfer(void *handle, uint8_t bmRequestType,
    uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
    uint8_t *data, uint16_t wLength, unsigned timeout)
{
    /* Straight pass-through to libusb; dyt_diy_communicate applies the
     * vendor's ret==8→0 normalisation. */
    return libusb_control_transfer((libusb_device_handle *)handle,
        bmRequestType, bRequest, wValue, wIndex,
        data, wLength, timeout);
}

dyt_transfer_fn dyt_libusb_transfer(void *unused)
{
    (void)unused;
    return dyt_libusb_xfer;
}

#else  /* !DYT_HAVE_LIBUSB */

dyt_transfer_fn dyt_libusb_transfer(void *unused)
{
    (void)unused;
    return NULL;   /* not compiled with libusb — build with -DDYT_HAVE_LIBUSB */
}
#endif

/* ----------------------------------------------------------------- serial read */

int dyt_verify_serial(dyt_transfer_fn xfer, void *handle,
                      const char *allowlist_path, uint8_t sn_out[15])
{
    /* Read-only: getTinyCUserData (RE Docs 04 §4.8) — the same bytes as
     * the "getTinyCUserData" entry in dyt_opcodes[], written to the
     * command register 0x1d00, 15-byte result. */
    static const uint8_t cmd[8] = { 0x0d, 0xc1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t raw[15];
    uint8_t sn[15];

    (void)allowlist_path;   /* licence gate deliberately not enforced (§8.11) */

    if (dyt_transaction_ex(xfer, handle, cmd, 0x1d00, raw, 15, NULL) != 0)
        return -1;

    dyt_decrypt_sne(sn, raw);
    if (sn_out)
        memcpy(sn_out, sn, 15);
    return 0;
}
