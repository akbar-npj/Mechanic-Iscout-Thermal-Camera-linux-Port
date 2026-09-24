/*
 * control.h — USB control primitive and transaction state machine for
 * the DYT thermal camera Linux port.
 *
 * The vendor's control channel is a thin wrapper around
 * libusb_control_transfer (RE Docs 04 §4.2): uvc_diy_communicate passes
 * the arguments through and normalises the return value (8 → 0).  Every
 * command is a canonical 3-step transaction: 8-byte OUT → 1-byte status
 * poll → 15-byte IN (RE Docs 04 §4.2).
 *
 * This header abstracts the transfer behind a callback (dyt_transfer_fn)
 * so the transaction state machine can be unit-tested with a mock that
 * serves canned bytes — no hardware, no libusb needed (control_test.c).
 * The real backend (dyt_libusb_transfer) compiles when libusb-1.0 is
 * present and runs only with a device attached.
 *
 * VID/PID → mode dispatch (RE Docs 04 §4.10) is also here, because the
 * mode determines the whole thermometry path and must be derived before
 * the first frame is expected.
 */
#ifndef DYT_CONTROL_H
#define DYT_CONTROL_H

#include <stdint.h>
#include "frame.h"   /* for dyt_mode_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Transfer callback.  Mirrors libusb_control_transfer's signature with a
 * void* handle so the state machine is testable without libusb.  Returns
 * the number of bytes transferred (>=0) or a negative libusb error. */
typedef int (*dyt_transfer_fn)(void *handle,
                               uint8_t  bmRequestType,
                               uint8_t  bRequest,
                               uint16_t wValue,
                               uint16_t wIndex,
                               uint8_t *data,
                               uint16_t wLength,
                               unsigned timeout);

/* Derive the device mode from VID/PID (RE Docs 04 §4.10).
 *   1514:0001                    → DYT_MODE_44C  (full radiometric)
 *   0BDA:5840 / 0BDA:5830        → DYT_MODE_1000 (direct AD)
 *   0BDA:5846 / 0BDA:31DA / 0581:0B00 → DYT_MODE_3EB
 *   anything else                 → DYT_MODE_0   (unsupported) */
dyt_mode_t dyt_mode_for_vidpid(uint16_t vid, uint16_t pid);

/* The vendor control primitive (RE Docs 04 §4.2).  Calls xfer(handle,...)
 * with a 1000 ms timeout (always 1000 ms at every observed call site),
 * then normalises ret == 8 → 0.  For 1-, 2-, 15- and 28-byte transfers
 * the raw libusb byte count is returned. */
int dyt_diy_communicate(dyt_transfer_fn xfer, void *handle,
                        uint8_t  bmRequestType,
                        uint8_t  bRequest,
                        uint16_t wValue,
                        uint16_t wIndex,
                        uint8_t *data,
                        uint16_t wLength);

/* One canonical command transaction (RE Docs 04 §4.2):
 *   (1) OUT  0x41/0x45/0x0078/<cmd_wIndex> — 8-byte command
 *   (2) poll 0xC1/0x44/0x0078/0x0200       — 1-byte status, up to 1000 iters
 *        break when !(s & BUSY) && !(s & PENDING)
 *        abort with -2 if (s & 0xfc) [ERROR bits]
 *   (3) IN   0xC1/0x44/0x0078/0x1d08       — result_len-byte result
 * Returns: 0 on success, -1 on transfer failure / poll exhaustion, -2 on
 * status error.  *status_out (if non-NULL) receives the last status byte.
 *
 * Most opcodes are written to the command register 0x1d00 and return 15
 * bytes, but stream start / parameter reads use the alternate register
 * 0x9d00 and the extended reads return 28 or 2 bytes (RE Docs 04 §4.2),
 * so the register and result length are parameters. */
int dyt_transaction_ex(dyt_transfer_fn xfer, void *handle,
                       const uint8_t cmd[8], uint16_t cmd_wIndex,
                       uint8_t *result, int result_len, int *status_out);

/* Convenience wrapper for the common case: command register 0x1d00,
 * 15-byte result.  Equivalent to
 *   dyt_transaction_ex(xfer, handle, cmd, 0x1d00, result15, 15, status_out). */
int dyt_transaction(dyt_transfer_fn xfer, void *handle,
                    const uint8_t cmd[8],
                    uint8_t *result15, int *status_out);

/* Status-byte bits (RE Docs 04 §4.2). */
#define DYT_STATUS_BUSY    0x01   /* command still executing */
#define DYT_STATUS_READY   0x02   /* pending — loop exits when clear */
#define DYT_STATUS_ERROR   0xfc   /* bits 2..7 — any set aborts */

/* Static opcode table (RE Docs 04 §4.2, the .rodata constants).  These
 * are the 8-byte command payloads copied into a stack buffer before each
 * transfer.  wIndex is the register the command is written to (0x1d00 or
 * 0x9d00); note that dyt_transaction always uses 0x1d00, so 0x9d00
 * opcodes must be sent via dyt_diy_communicate directly. */
typedef struct {
    const char *name;
    uint8_t  cmd[8];
    uint16_t wIndex;
    const char *note;     /* "" safe | "WRITE" writes persistent state */
} dyt_opcode_t;
extern const dyt_opcode_t dyt_opcodes[];
extern const int dyt_opcodes_n;

/* tinyStartStream2 is a 9-byte payload (not 8), written to 0x1d08, so it
 * is kept separate from the 8-byte opcode table. */
extern const uint8_t dyt_tinyStartStream2_cmd[9];

/* Real libusb-1.0 backend.  Returns a dyt_transfer_fn that wraps
 * libusb_control_transfer, or NULL if libusb was not compiled in.
 * Pass the libusb_device_handle* as `handle` to dyt_transaction. */
dyt_transfer_fn dyt_libusb_transfer(void *unused);

/* Serial-number read + decode (RE Docs 04 §4.8/§4.9, §4.10).
 *
 * Performs the read-only getTinyCUserData transaction, decodes the
 * 15-byte result with DecryptSNE (serial.h), and copies the decoded
 * serial to sn_out when non-NULL.
 *
 * Returns 0 (allow) unconditionally on a successful read.  The
 * allow-list comparison in the vendor binary (configs_maintenanceguy.txt)
 * is an app-side licence check; a client we write has no reason to
 * enforce it (RE Docs 08 §8.11).  allowlist_path is accepted for API
 * completeness and currently unused.  Returns -1 if the device did not
 * answer the read. */
int dyt_verify_serial(dyt_transfer_fn xfer, void *handle,
                      const char *allowlist_path, uint8_t sn_out[15]);

#ifdef __cplusplus
}
#endif

#endif /* DYT_CONTROL_H */
