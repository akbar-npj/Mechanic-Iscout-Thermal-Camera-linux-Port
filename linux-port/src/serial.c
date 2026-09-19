/*
 * serial.c — serial-number deobfuscation (UVCPreviewIR::DecryptSNE).
 *
 * Transcribed byte-for-byte from the decompilation in RE Docs 04 §4.9.
 * The vendor reuses the constant 0x1d throughout (it is also the
 * command-register index 0x1d00 / result 0x1d08) — not a coincidence
 * worth reading into, but it is why the same byte shows up twice here.
 *
 * build:  cc -O2 -g -Wall -Wextra -Wno-unused-parameter -ffp-contract=off \
 *           -I. -c serial.c -o serial.o
 */
#include <string.h>

#include "serial.h"

void dyt_decrypt_sne(uint8_t out[15], const uint8_t in[15])
{
    uint8_t b[15];
    int i;

    memcpy(b, in, 15);

    b[0] ^= 0x12;
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
            b[4] ^= b[13]; b[5] ^= b[12];
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

int dyt_serial_str(char *dst, const uint8_t sn[15])
{
    int n = 0;

    while (n < 15 && sn[n] >= 0x20 && sn[n] < 0x7f) {
        dst[n] = (char)sn[n];
        n++;
    }
    dst[n] = '\0';
    return n;
}
