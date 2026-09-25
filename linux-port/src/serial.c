/*
 * serial.c — serial-number decoding (UVCPreviewIR::DecryptSNE).
 *
 * Transcribed from the disassembly in RE Docs 04 §4.9.  The published
 * transcription was missing six scattered pre-XORs and one term of the
 * i == 3 branch, which is why an earlier revision of this file (and of
 * the docs) produced non-printable garbage; see §4.9 for the corrected
 * routine and the live-verified reference vector.
 *
 * The vendor reuses the constant 0x1d throughout (it is also the
 * command-register index 0x1d00 / result 0x1d08) — not a coincidence
 * worth reading into, but it is why the same byte shows up twice here.
 *
 * build:  cc -O2 -g -Wall -Wextra -Wno-unused-parameter -ffp-contract=off \
 *           -I. -c serial.c -o serial.o
 */
#include <string.h>

#include "serial.h"

uint8_t dyt_sn_key(const uint8_t *module_sn, int len)
{
    /*
     * The vendor passes (module-SN buffer + 6) as DecryptSNE's key and
     * hashes key + 2, i.e. buffer + 8.  For the 12-digit module serial
     * that is the last four digits.  Reproduce atoi()'s int result and
     * C's truncated `% 127`, then keep the low byte.
     */
    long v = 0;
    int i, seen = 0, neg = 0;

    if (!module_sn || len <= 8)
        return 0;

    i = 8;
    if (i < len && (module_sn[i] == '-' || module_sn[i] == '+')) {
        neg = module_sn[i] == '-';
        i++;
    }
    for (; i < len; i++) {
        uint8_t c = module_sn[i];
        if (c < '0' || c > '9')
            break;
        v = v * 10 + (c - '0');
        seen = 1;
    }
    if (!seen)
        return 0;
    if (neg)
        v = -v;

    return (uint8_t)(int)((int)v % 127);
}

void dyt_decrypt_sne(uint8_t out[15], const uint8_t in[15], uint8_t key)
{
    uint8_t b[15];
    int i;

    memcpy(b, in, 15);

    b[0]  ^= 0x12;
    b[8]  ^= 0x12;
    b[11] ^= key;
    b[3]  ^= key;
    b[7]  ^= 0x12;
    b[9]  ^= key;

    for (i = 0; i < 15; i++)
        b[i] ^= 0x1d;

    /* Swap pairs (0,7), (2,9), (4,11). */
    for (i = 0; i < 5; i += 2) {
        uint8_t t = b[i];
        b[i]     = b[i + 7];
        b[i + 7] = t;
    }

    /* Mix the tail into the head, then diffuse within the block. */
    for (i = 0; i < 7; i += 3) {
        if (i == 0) {
            b[0] ^= b[12]; b[1] ^= b[13]; b[2] ^= b[14];
        } else if (i == 3) {
            b[3] ^= b[14]; b[4] ^= b[13]; b[5] ^= b[12];
        } else {   /* i == 6 */
            b[6] ^= b[9];  b[7] ^= b[10]; b[8] ^= b[11];
        }
    }

    memcpy(out, b, 15);
}

int dyt_serial_variant(const uint8_t sn[15])
{
    return sn[2] == 'C' ? 1 : 0;
}

int dyt_sn_str(char *dst, const uint8_t *sn, int n)
{
    int i = 0;

    if (!dst || !sn || n < 0)
        return 0;
    while (i < n && sn[i] >= 0x20 && sn[i] < 0x7f) {
        dst[i] = (char)sn[i];
        i++;
    }
    dst[i] = '\0';
    return i;
}

int dyt_serial_str(char *dst, const uint8_t sn[15])
{
    return dyt_sn_str(dst, sn, 15);
}
