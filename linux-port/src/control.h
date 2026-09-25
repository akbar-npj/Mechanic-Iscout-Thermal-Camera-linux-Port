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
#include "frame.h"    /* for dyt_mode_t */
#include "params.h"   /* for dyt_params_t */

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

/* The same transaction *without* the result read, for the commands that
 * answer with nothing (tinyStartStream, setTinyCOutputADValue, the
 * do_tinyC_order cases).  Steps (1) and (2) only.  Returns 0 on success,
 * -1 on transfer failure / poll exhaustion, -2 on a status error. */
int dyt_transaction_cmd(dyt_transfer_fn xfer, void *handle,
                        const uint8_t cmd[8], uint16_t cmd_wIndex,
                        int *status_out);

/* Status-byte bits (RE Docs 04 §4.2). */
#define DYT_STATUS_BUSY    0x01   /* command still executing */
#define DYT_STATUS_READY   0x02   /* pending — loop exits when clear */
#define DYT_STATUS_ERROR   0xfc   /* bits 2..7 — any set aborts */

/* Static opcode table (RE Docs 04 §4.2, the .rodata constants).  These
 * are the 8-byte command payloads copied into a stack buffer before each
 * transfer.  wIndex is the register the command is written to (0x1d00 or
 * 0x9d00); note that dyt_transaction always uses 0x1d00, so 0x9d00
 * opcodes must be sent via dyt_diy_communicate directly.
 *
 * result_wIndex / result_len describe where the command's *answer* lives
 * when it has one, so a caller does not have to hard-code per-opcode
 * geometry.  They are 0 for commands that answer with nothing.  This is
 * not cosmetic: reading the wrong register does not fail, it returns
 * whatever the device left in the shared buffer, so three different
 * commands "succeed" with identical bytes (measured 2026-09-25). */
typedef struct {
    const char *name;
    uint8_t  cmd[8];
    uint16_t wIndex;         /* command register */
    uint16_t result_wIndex;  /* result register, 0 = no read-back */
    uint16_t result_len;     /* result length in bytes, 0 = no read-back */
    const char *note;        /* "" safe | "WRITE" writes device state.  The
                              * substring "WRITE" flags a write for probe's
                              * read-only guard; a "RUNTIME WRITE" is a
                              * volatile parameter, a bare "WRITE" is
                              * persistent calibration (RE Docs 04 §4.8). */
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

/* --------------------------------------------------------- device identity
 *
 * All three readers below are read-only and were verified against the
 * reference 0bda:5840 unit on 2026-09-25, byte-for-byte identical to what
 * the Android app does in MechaniscoutPcap/4.pcapng (frames 4270/4284,
 * 4286/4300 and 4216-4269 respectively).
 */

/* The module serial: a 16-byte ASCII record, NUL-padded ("202605575259"
 * on the reference unit).  Order id 0x15 — command
 * 05 84 07 00 00 00 00 10 -> 0x1d00, 16-byte result from 0x1d08.
 *
 * The APK's build of this order differs in two payload bytes
 * (05 84 07 00 00 10 00 10) and returns the same 16 bytes, so those bytes
 * are not significant on this firmware.
 *
 * Returns 0 on success, -1 if the device did not answer. */
int dyt_read_module_sn(dyt_transfer_fn xfer, void *handle, uint8_t out[16]);

/* The raw 15-byte user serial.  Order id 0x14 — command
 * 01 82 00 7f f0 00 00 0f -> 0x1d00, 15-byte result from 0x1d08.
 *
 * This is the *input* to DecryptSNE (serial.h), which the vendor runs
 * with a key derived from the module serial.  On the reference unit the
 * decode is "DYCSTI09GG01292" (RE Docs 04 §4.9).  (Earlier revisions
 * called this the "obfuscated identity record" and treated the raw bytes
 * as the deliverable — that was a consequence of a wrong DecryptSNE
 * transcription.)
 *
 * Returns 0 on success, -1 if the device did not answer. */
int dyt_read_user_sn(dyt_transfer_fn xfer, void *handle, uint8_t out[15]);

/* Convenience wrapper: read the module serial into sn_out (16 bytes).
 *
 * allowlist_path is accepted for API completeness and is unused — the
 * vendor's allow-list comparison (configs_maintenanceguy.txt) is an
 * app-side licence check a client we write has no reason to enforce
 * (RE Docs 08 §8.11).  Returns 0 on success, -1 if the read failed. */
int dyt_verify_serial(dyt_transfer_fn xfer, void *handle,
                      const char *allowlist_path, uint8_t sn_out[16]);

/* ------------------------------------------------------- stored parameters
 *
 * The getTinyCParams read-back.  Its wire form is *not* the canonical
 * 3-step transaction — it carries an extra pre-fill write and reads from
 * the alternate result register 0x1d10:
 *
 *   OUT  14 85 00 <index> 00 00 00 00   -> 0x9d00
 *   OUT  00 00 00 00 00 00 00 02        -> 0x1d08  (result-length pre-fill;
 *                                                  0 for the write orders)
 *   poll 0x0200 until not-busy && not-pending
 *   IN   2 bytes                        <- 0x1d10
 *
 * The payload is big-endian.  Index meanings and units live in params.h.
 */
int dyt_read_param(dyt_transfer_fn xfer, void *handle, unsigned index,
                   uint16_t *out);

/* Read every slot 0..DYT_PARAM_N-1 into *out, which is initialised first.
 * Returns the number of slots read successfully, so a caller can tell
 * "device refused everything" from "every slot really is zero". */
int dyt_read_all_params(dyt_transfer_fn xfer, void *handle, dyt_params_t *out);

/* Return values shared by the readers above. */
#define DYT_READ_OK       0
#define DYT_READ_IO      -1   /* transfer failed or poll loop exhausted */
#define DYT_READ_STATUS  -2   /* device reported an error status */
#define DYT_READ_RANGE   -3   /* argument out of range */

/* --------------------------------------------------------- runtime write
 *
 * `dyt_write_param` sends the vendor's `sendOrder(type, value)` — a runtime
 * parameter write (emissivity / ambient / reflected / distance), NOT
 * calibration.  See params.h for the encoding and the safety note.
 *
 * Wire form (RE Docs 04 §4.2, verified against MechaniscoutPcap/4.pcapng):
 * two OUT transfers and **no status poll** —
 *
 *   OUT 0x9d00 <- 14 c5 00 <type> 00 00 <hi> <lo>
 *   OUT 0x1d08 <- 00 00 00 00 00 00 00 02
 *
 * This is the only function in the port that writes to the device.  It is
 * never called from a frame callback; call it from the owning thread.
 *
 * Returns DYT_READ_OK, or DYT_READ_IO / DYT_READ_RANGE. */
int dyt_write_param(dyt_transfer_fn xfer, void *handle, int type, float value);


#ifdef __cplusplus
}
#endif

#endif /* DYT_CONTROL_H */
