/*
 * serial.h — serial-number decoding for the DYT thermal camera
 * Linux port.
 *
 * UVCPreviewIR::DecryptSNE (libUVCCamera.so @ 0x1698bc) turns the
 * 15-byte *raw user serial* the device returns from getTinyCUserSn
 * (order id 0x14, command 01 82 00 7f f0 00 00 0f) into the printable
 * user serial.  The key is derived from the module serial returned by
 * getTinyCRobotSn (order id 0x15): the vendor passes
 * `module_sn_buffer + 6` as the key string and hashes `key + 2`, i.e.
 * the module serial's last four digits, modulo 127.
 *
 *   reference unit: module serial "202605575259"
 *                   key      "575259"
 *                   key + 2  "5259"   -> mod = 5259 % 127 = 52
 *                   raw user serial 06 7d 5a 48 2c 66 6a 79 79 58 2d 44 2f 24 2f
 *                   decoded         "DYCSTI09GG01292"
 *
 * The decoded serial's 3rd byte is a hardware-variant flag: the vendor
 * tests SN[2] == 'C' in setMachineSetting (RE Docs 04 §4.2/§4.9) and
 * in do_preview.  The observed families are e.g. "DYCSTI09…" ('C') and
 * "DYTEPK78" ('T').
 *
 * Note: this is a *decode*, not a licence check.  The vendor's
 * allow-list comparison is app-side and a client we write has no
 * reason to enforce it (RE Docs 08 §8.11).
 *
 * build:  cc -O2 -g -Wall -Wextra -Wno-unused-parameter -ffp-contract=off \
 *           -I. -c serial.c -o serial.o
 */
#ifndef DYT_SERIAL_H
#define DYT_SERIAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The key byte: atoi((char *)module_sn + 8) % 127, truncated to
 * uint8_t — the module serial's last four digits, signed-remainder
 * semantics and all (RE Docs 04 §4.9.1).  module_sn is the 16-byte
 * module serial; `len` is how many bytes are valid.  Returns 0 if the
 * serial is too short or not numeric. */
uint8_t dyt_sn_key(const uint8_t *module_sn, int len);

/* DecryptSNE — in[15] is the raw user serial, out[15] the decoded
 * serial.  `key` is the byte from dyt_sn_key().  out and in may alias.
 * The transform is not an involution (it is not its own inverse). */
void dyt_decrypt_sne(uint8_t out[15], const uint8_t in[15], uint8_t key);

/* Hardware-variant flag: 1 if the decoded serial's 3rd byte is 'C',
 * 0 otherwise.  Pass the *decoded* serial. */
int dyt_serial_variant(const uint8_t sn[15]);

/* Copy a decoded serial into a NUL-terminated string, stopping at the
 * first non-printable byte.  dst must hold at least 16 bytes.  Returns
 * the number of characters written (excluding the NUL). */
int dyt_serial_str(char *dst, const uint8_t sn[15]);

/* The same copy over an arbitrary-length blob, so it also serves the
 * 16-byte plaintext module serial (control.h).  dst must hold at least
 * n+1 bytes.  Returns the number of characters written. */
int dyt_sn_str(char *dst, const uint8_t *sn, int n);

#ifdef __cplusplus
}
#endif

#endif /* DYT_SERIAL_H */
