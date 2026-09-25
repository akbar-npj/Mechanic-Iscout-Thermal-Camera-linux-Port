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

#include "control.h"

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

/* Poll 0x0200 until not-busy && not-pending.  Returns 0 on ready, -2 on an
 * error status, -1 on a transfer failure or after the 1000-iteration cap.
 * *status_out receives the last byte read.  Shared by the with-result and
 * without-result transaction forms so the two can never drift apart. */
static int poll_ready(dyt_transfer_fn xfer, void *handle, uint8_t *status_out)
{
    uint8_t status = 0;
    int i;

    for (i = 0; i < 1000; i++) {
        if (dyt_diy_communicate(xfer, handle, 0xC1, 0x44, 0x0078, 0x0200,
                                &status, 1) != 1) {
            if (status_out) *status_out = status;
            return -1;
        }
        if (!(status & DYT_STATUS_BUSY) && !(status & DYT_STATUS_READY))
            break;
        if (status & DYT_STATUS_ERROR) {
            if (status_out) *status_out = status;
            return -2;
        }
    }
    if (status_out) *status_out = status;
    if (i >= 1000)
        return -1;   /* poll loop exhausted — device stuck busy */
    return 0;
}

int dyt_transaction_cmd(dyt_transfer_fn xfer, void *handle,
                        const uint8_t cmd[8], uint16_t cmd_wIndex,
                        int *status_out)
{
    uint8_t status = 0;
    int rc;

    /* (1) OUT the 8-byte command.  After the ret==8→0 normalisation,
     * success is 0. */
    if (dyt_diy_communicate(xfer, handle, 0x41, 0x45, 0x0078, cmd_wIndex,
                            (uint8_t *)cmd, 8) != 0) {
        if (status_out) *status_out = 0;
        return -1;
    }

    /* (2) Poll the status byte.  Collect through a local so the caller's
     * int never keeps stale high bytes from a previous value. */
    rc = poll_ready(xfer, handle, &status);
    if (status_out) *status_out = status;
    return rc;
}

int dyt_transaction_ex(dyt_transfer_fn xfer, void *handle,
                       const uint8_t cmd[8], uint16_t cmd_wIndex,
                       uint8_t *result, int result_len, int *status_out)
{
    int rc;

    /* (1) OUT the 8-byte command to the command register (0x1d00, or
     * 0x9d00 for stream start / parameter reads), then (2) poll. */
    if ((rc = dyt_transaction_cmd(xfer, handle, cmd, cmd_wIndex,
                                  status_out)) != 0)
        return rc;

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
 * opcodes must be sent via dyt_diy_communicate directly.
 *
 * result_wIndex / result_len come from the issuing function, not from the
 * constant.  Where they are non-zero they are verified against the capture
 * (MechaniscoutPcap/4.pcapng) and against the reference unit:
 *
 *   getTinyCUserData   0d c1 -> 0x1d00 answers with a *single* status byte,
 *                      not 15.  Reading 15 bytes here "succeeds" and
 *                      returns the shared buffer's leftovers.
 *   getTinyCParams / setMachineSetting_read
 *                      14 85 -> the 2-byte answer lives in 0x1d10, and the
 *                      order needs the extra 0x1d08 pre-fill that
 *                      dyt_read_param() issues (see control.h). */
const dyt_opcode_t dyt_opcodes[] = {
    { "tinyStartStream",
      { 0x0f, 0xc1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09 }, 0x9d00,
      0, 0, "" },
    { "setTinyCOutputADValue",
      { 0x0a, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 0x1d00,
      0, 0, "" },
    { "getTinyCUserData",
      { 0x0d, 0xc1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 0x1d00,
      0x1d08, 1, "" },
    { "getTinyCParams",
      { 0x14, 0x85, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00 }, 0x9d00,
      0x1d10, 2, "" },
    { "sendTinyCParamsModification",
      { 0x14, 0xc5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 0x9d00,
      0, 0,
      "RUNTIME WRITE — volatile parameter (reversible, not calibration)" },
    { "setMachineSetting_write",
      { 0x14, 0xc5, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00 }, 0x9d00,
      0x1d10, 2,
      "WRITE — writes calibration coefficient (irreversible)" },
    { "setMachineSetting_read",
      { 0x14, 0x85, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00 }, 0x9d00,
      0x1d10, 2, "" },
    { "do_tinyC_order_case5",
      { 0x0d, 0x8b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 }, 0x1d00,
      0, 0, "" },
    { "do_tinyC_order_case7a",
      { 0x14, 0x83, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00 }, 0x9d00,
      0, 0, "" },
    { "do_tinyC_order_case7b",
      { 0x14, 0xc3, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00 }, 0x9d00,
      0, 0, "" },
    { "getTinyCModuleSn",
      { 0x05, 0x84, 0x07, 0x00, 0x00, 0x00, 0x00, 0x10 }, 0x1d00,
      0x1d08, 16, "" },
    { "getTinyCUserSn",
      { 0x01, 0x82, 0x00, 0x7f, 0xf0, 0x00, 0x00, 0x0f }, 0x1d00,
      0x1d08, 15, "" },
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

/* ------------------------------------------------------- device identity */

int dyt_read_module_sn(dyt_transfer_fn xfer, void *handle, uint8_t out[16])
{
    /* Order id 0x15.  The APK builds this payload as
     * 05 84 07 00 00 10 00 10; the capture and the reference unit both use
     * 05 84 07 00 00 00 00 10, and the unit answers 16 bytes either way
     * (measured 2026-09-25), so the two middle bytes are inert. */
    static const uint8_t cmd[8] =
        { 0x05, 0x84, 0x07, 0x00, 0x00, 0x00, 0x00, 0x10 };

    if (!out)
        return DYT_READ_RANGE;
    return dyt_transaction_ex(xfer, handle, cmd, 0x1d00, out, 16, NULL)
               == 0 ? DYT_READ_OK : DYT_READ_IO;
}

int dyt_read_user_sn(dyt_transfer_fn xfer, void *handle, uint8_t out[15])
{
    /* Order id 0x14 — the constant is byte-identical to the one the vendor
     * builds at runtime in getTinyCUserSn (RE Docs 04 §4.2/§4.8). */
    static const uint8_t cmd[8] =
        { 0x01, 0x82, 0x00, 0x7f, 0xf0, 0x00, 0x00, 0x0f };

    if (!out)
        return DYT_READ_RANGE;
    return dyt_transaction_ex(xfer, handle, cmd, 0x1d00, out, 15, NULL)
               == 0 ? DYT_READ_OK : DYT_READ_IO;
}

int dyt_verify_serial(dyt_transfer_fn xfer, void *handle,
                      const char *allowlist_path, uint8_t sn_out[16])
{
    (void)allowlist_path;   /* licence gate deliberately not enforced (§8.11) */
    return dyt_read_module_sn(xfer, handle, sn_out);
}

/* ---------------------------------------------------- stored parameters */

int dyt_read_param(dyt_transfer_fn xfer, void *handle, unsigned index,
                   uint16_t *out)
{
    /* Verified live 2026-09-25 and against capture frames 4216-4269. */
    static const uint8_t sel[8] = { 0, 0, 0, 0, 0, 0, 0, 0x02 };
    uint8_t cmd[8] = { 0x14, 0x85, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t buf[2] = { 0, 0 };
    uint8_t status = 0;
    int i;

    if (index > 0xffu)
        return DYT_READ_RANGE;
    cmd[3] = (uint8_t)index;

    if (dyt_diy_communicate(xfer, handle, 0x41, 0x45, 0x0078, 0x9d00,
                            cmd, 8) != 0)
        return DYT_READ_IO;

    /* The order is not complete until the result length is pre-filled.  The
     * write orders use 0 here; the read path uses the result length. */
    if (dyt_diy_communicate(xfer, handle, 0x41, 0x45, 0x0078, 0x1d08,
                            (uint8_t *)sel, 8) != 0)
        return DYT_READ_IO;

    for (i = 0; i < 1000; i++) {
        if (dyt_diy_communicate(xfer, handle, 0xC1, 0x44, 0x0078, 0x0200,
                                &status, 1) != 1)
            return DYT_READ_IO;
        if (!(status & DYT_STATUS_BUSY) && !(status & DYT_STATUS_READY))
            break;
        if (status & DYT_STATUS_ERROR)
            return DYT_READ_STATUS;
    }
    if (i >= 1000)
        return DYT_READ_IO;

    if (dyt_diy_communicate(xfer, handle, 0xC1, 0x44, 0x0078, 0x1d10,
                            buf, 2) != 2)
        return DYT_READ_IO;

    if (out)
        *out = (uint16_t)((buf[0] << 8) | buf[1]);   /* big-endian */
    return DYT_READ_OK;
}

int dyt_read_all_params(dyt_transfer_fn xfer, void *handle, dyt_params_t *out)
{
    int i, n = 0;

    if (!out)
        return 0;
    dyt_params_init(out);

    for (i = 0; i < DYT_PARAM_N; i++) {
        uint16_t v = 0;
        if (dyt_read_param(xfer, handle, (unsigned)i, &v) == DYT_READ_OK) {
            dyt_params_set(out, i, v);
            n++;
        } else {
            /* Leave the slot explicitly invalid rather than zero-valued, so
             * "the device refused" and "the device said 0" stay distinct. */
            dyt_params_fail(out, i);
        }
    }
    return n;
}

/* ----------------------------------------------------------- runtime write */

int dyt_write_param(dyt_transfer_fn xfer, void *handle, int type, float value)
{
    uint8_t cmd[8];

    if (dyt_params_build_cmd(cmd, type, value) != 0)
        return DYT_READ_RANGE;

    if (dyt_diy_communicate(xfer, handle, 0x41, 0x45, 0x0078, 0x9d00,
                            cmd, 8) != 0)
        return DYT_READ_IO;

    /* No status poll: the vendor's sendOrder is two OUT transfers, and the
     * capture shows nothing polled between them.  The order is not
     * "complete" until the result length is pre-filled (same as the read
     * path, control.c:dyt_read_param). */
    if (dyt_diy_communicate(xfer, handle, 0x41, 0x45, 0x0078, 0x1d08,
                            (uint8_t *)dyt_order_prefill, 8) != 0)
        return DYT_READ_IO;

    return DYT_READ_OK;
}
