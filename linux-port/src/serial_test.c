/*
 * serial_test.c — unit tests for the DecryptSNE port (serial.c).
 *
 * No hardware and no vendor library are needed: DecryptSNE is a pure
 * byte transform, so a hand-computed known-answer vector is enough to
 * pin it.  The vector below was derived by applying the steps of
 * RE Docs 04 §4.9 to in[i] = i by hand.
 *
 * build:  via the Makefile (make serial-test)
 */
#include <stdio.h>
#include <string.h>

#include "serial.h"

/* DecryptSNE applied to in[i] = i, worked through step by step:
 *   b[0] ^= 0x12            -> b[0] = 0x12
 *   b[i] ^= 0x1d (all i)    -> b = 0f 1c 1f 1e 19 18 1b 1a 15 14 17 16 11 10 13
 *   swap (0,7)(2,9)(4,11)   -> b = 1a 1c 14 1e 16 18 1b 0f 15 1f 17 19 11 10 13
 *   mix tail                -> b = 0b 0c 07 1e 06 09 04 18 0c 1f 17 19 11 10 13
 */
static const uint8_t expect[15] = {
    0x0b, 0x0c, 0x07, 0x1e, 0x06, 0x09, 0x04, 0x18,
    0x0c, 0x1f, 0x17, 0x19, 0x11, 0x10, 0x13
};

static int test_known_answer(void)
{
    uint8_t in[15], out[15];
    int i;

    for (i = 0; i < 15; i++) in[i] = (uint8_t)i;
    memset(out, 0, sizeof out);

    dyt_decrypt_sne(out, in);

    printf("test_known_answer: ");
    for (i = 0; i < 15; i++) printf("%02x", out[i]);
    printf("\n");

    if (memcmp(out, expect, 15) != 0) {
        printf("  FAIL: expected ");
        for (i = 0; i < 15; i++) printf("%02x", expect[i]);
        printf("\n");
        return 1;
    }
    printf("  PASS\n");
    return 0;
}

/* out and in may alias (the header documents this). */
static int test_in_place(void)
{
    uint8_t buf[15], ref[15], src[15];
    int i;

    for (i = 0; i < 15; i++) src[i] = (uint8_t)(0xa0 + i);
    memcpy(buf, src, 15);

    dyt_decrypt_sne(ref, src);        /* separate buffers */
    dyt_decrypt_sne(buf, buf);        /* aliased */

    printf("test_in_place: %s\n",
           memcmp(buf, ref, 15) == 0 ? "identical" : "MISMATCH");
    if (memcmp(buf, ref, 15) != 0) { printf("  FAIL\n"); return 1; }
    printf("  PASS\n");
    return 0;
}

static int test_variant(void)
{
    uint8_t sn_t[15] = { 'D', 'Y', 'T', 'E', 'P', 'K', '7', '8', 0 };
    uint8_t sn_c[15] = { 'D', 'Y', 'C', 'R', 'P', 'K', '7', '8', 0 };
    int fails = 0;

    if (dyt_serial_variant(sn_t) != 0) {
        printf("test_variant: 'DYT...' reported as variant\n");
        fails++;
    }
    if (dyt_serial_variant(sn_c) != 1) {
        printf("test_variant: 'DYC...' not reported as variant\n");
        fails++;
    }
    if (fails) { printf("  FAIL\n"); return 1; }
    printf("test_variant: 'T'->0, 'C'->1\n");
    printf("  PASS\n");
    return 0;
}

static int test_serial_str(void)
{
    uint8_t sn[15] = { 'D', 'Y', 'C', 'R', 'P', 'K', '7', '8', 0, 0, 0, 0, 0, 0, 0 };
    char s[16];
    int n = dyt_serial_str(s, sn);

    printf("test_serial_str: \"%s\" (len %d)\n", s, n);
    if (strcmp(s, "DYCRPK78") != 0 || n != 8) {
        printf("  FAIL\n");
        return 1;
    }
    printf("  PASS\n");
    return 0;
}

int main(void)
{
    int fails = 0;
    printf("=== serial_test ===\n");
    fails += test_known_answer();
    fails += test_in_place();
    fails += test_variant();
    fails += test_serial_str();
    printf("=== %s ===\n", fails ? "FAIL" : "ALL PASS");
    return fails ? 1 : 0;
}
