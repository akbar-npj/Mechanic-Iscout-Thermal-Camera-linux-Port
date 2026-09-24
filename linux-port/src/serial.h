/*
 * serial.h — serial-number deobfuscation for the DYT thermal camera
 * Linux port.
 *
 * UVCPreviewIR::DecryptSNE (libUVCCamera.so @ 0x1698bc) is a short,
 * fully-recovered byte transform (RE Docs 04 §4.9).  It takes the
 * 15-byte blob the device returns from getTinyCRobotSn / getTinyCUserSn
 * and turns it into the ASCII serial (e.g. "DYTEPK78").
 *
 * The decoded serial's 3rd byte is a hardware-variant flag: the vendor
 * tests SN[2] == 'C' in do_preview and in setMachineSetting
 * (RE Docs 04 §4.2, 09 §7.5).  The two observed families are
 * "DYTEPK78" (SN[2]='T') and "DYCRPK78" (SN[2]='C').
 *
 * Note: this is a *decode*, not a licence check.  The vendor's
 * allow-list comparison (configs_maintenanceguy.txt, AES-encrypted) is
 * app-side and a client we write has no reason to enforce it
 * (RE Docs 08 §8.11).
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

/* DecryptSNE — in[15] is the raw device blob, out[15] the decoded
 * serial.  out and in may alias.  The transform is applied in place
 * and is not an involution (it is not its own inverse). */
void dyt_decrypt_sne(uint8_t out[15], const uint8_t in[15]);

/* Hardware-variant flag: 1 if the decoded serial's 3rd byte is 'C',
 * 0 otherwise (RE Docs 09 §7.5).  Pass the *decoded* serial. */
int dyt_serial_variant(const uint8_t sn[15]);

/* Copy a decoded serial into a NUL-terminated string, stopping at the
 * first non-printable byte.  dst must hold at least 16 bytes.  Returns
 * the number of characters written (excluding the NUL). */
int dyt_serial_str(char *dst, const uint8_t sn[15]);

#ifdef __cplusplus
}
#endif

#endif /* DYT_SERIAL_H */
