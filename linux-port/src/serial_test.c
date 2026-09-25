/*
 * serial_test.c — unit tests for the DecryptSNE port (serial.c).
 *
 * No hardware and no vendor library are needed: DecryptSNE is a pure
 * byte transform, so known-answer vectors are enough to pin it.  The
 * reference vector is the live-measured one from RE Docs 04 §4.9.3.
 *
 * build:  via the Makefile (make check)
 */
#include <stdio.h>
#include <string.h>

#include "serial.h"

/* Live reference unit: module serial "202605575259" -> key 52, and the
 * raw user serial returned by getTinyCUserSn (order id 0x14). */
static const uint8_t ref_module_sn[16] = {
    '2','0','2','6','0','5','5','7','5','2','5','9', 0, 0, 0, 0
};
static const uint8_t ref_raw[15] = {
    0x06, 0x7d, 0x5a, 0x48, 0x2c, 0x66, 0x6a, 0x79, 0x79,
    0x58, 0x2d, 0x44, 0x2f, 0x24, 0x2f
};
static const uint8_t ref_expect[15] = {
    0x44, 0x59, 0x43, 0x53, 0x54, 0x49, 0x30, 0x39, 0x47,
    0x47, 0x30, 0x31, 0x32, 0x39, 0x32          /* "DYCSTI09GG01292" */
};

/* DecryptSNE applied to in[i] = i with key 52, worked through by hand:
 *   pre-XORs           -> 12 01 02 37 04 05 06 15 1a 3d 0a 3f 0c 0d 0e
 *   b[i] ^= 0x1d (all) -> 0f 1c 1f 2a 19 18 1b 08 07 20 17 22 11 10 13
 *   swap (0,7)(2,9)(4,11) -> 08 1c 20 2a 22 18 1b 0f 07 1f 17 19 11 10 13
 *   mix tail           -> 19 0c 33 39 32 09 04 18 1e 1f 17 19 11 10 13
 */
static const uint8_t seq_expect[15] = {
    0x19, 0x0c, 0x33, 0x39, 0x32, 0x09, 0x04, 0x18,
    0x1e, 0x1f, 0x17, 0x19, 0x11, 0x10, 0x13
};

static int test_key(void)
{
    int fails = 0;

    /* The reference unit: atoi("5259") % 127. */
    if (dyt_sn_key(ref_module_sn, 16) != 52) {
        printf("test_key: reference serial -> %u, expected 52\n",
               dyt_sn_key(ref_module_sn, 16));
        fails++;
    }
    /* No digit suffix / too short -> 0. */
    {
        static const uint8_t nonnum[16] = { 'X','X','X','X','X','X','X','X',
                                            'X','X','X','X', 0,0,0,0 };
        if (dyt_sn_key(nonnum, 16) != 0) {
            printf("test_key: non-numeric serial -> %u, expected 0\n",
                   dyt_sn_key(nonnum, 16));
            fails++;
        }
        if (dyt_sn_key(ref_module_sn, 8) != 0) {
            printf("test_key: short serial -> %u, expected 0\n",
                   dyt_sn_key(ref_module_sn, 8));
            fails++;
        }
    }
    /* Boundary: 127 wraps to 0, 128 wraps to 1. */
    {
        static const uint8_t s127[16] = { '0','0','0','0','0','0','0','0',
                                          '0','1','2','7', 0,0,0,0 };
        static const uint8_t s128[16] = { '0','0','0','0','0','0','0','0',
                                          '0','1','2','8', 0,0,0,0 };
        if (dyt_sn_key(s127, 16) != 0 || dyt_sn_key(s128, 16) != 1) {
            printf("test_key: %%127 wrap wrong (%u, %u)\n",
                   dyt_sn_key(s127, 16), dyt_sn_key(s128, 16));
            fails++;
        }
    }

    if (fails) { printf("  FAIL\n"); return 1; }
    printf("test_key: 5259->52, non-numeric->0, 127->0, 128->1\n");
    printf("  PASS\n");
    return 0;
}

static int test_reference_vector(void)
{
    uint8_t out[15];
    char s[16];
    int i;

    dyt_decrypt_sne(out, ref_raw, dyt_sn_key(ref_module_sn, 16));
    dyt_serial_str(s, out);

    printf("test_reference_vector: \"%s\"\n", s);
    if (memcmp(out, ref_expect, 15) != 0 || strcmp(s, "DYCSTI09GG01292") != 0) {
        printf("  FAIL: expected ");
        for (i = 0; i < 15; i++) printf("%02x", ref_expect[i]);
        printf("\n");
        return 1;
    }
    /* The cross-check the docs rely on: setMachineSetting tests SN[2]=='C'. */
    if (!dyt_serial_variant(out)) {
        printf("  FAIL: decoded serial not flagged variant C\n");
        return 1;
    }
    printf("  PASS\n");
    return 0;
}

static int test_known_answer(void)
{
    uint8_t in[15], out[15];
    int i;

    for (i = 0; i < 15; i++) in[i] = (uint8_t)i;
    memset(out, 0, sizeof out);

    dyt_decrypt_sne(out, in, 52);

    printf("test_known_answer: ");
    for (i = 0; i < 15; i++) printf("%02x", out[i]);
    printf("\n");

    if (memcmp(out, seq_expect, 15) != 0) {
        printf("  FAIL: expected ");
        for (i = 0; i < 15; i++) printf("%02x", seq_expect[i]);
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

    dyt_decrypt_sne(ref, src, 52);        /* separate buffers */
    dyt_decrypt_sne(buf, buf, 52);        /* aliased */

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
    fails += test_key();
    fails += test_reference_vector();
    fails += test_known_answer();
    fails += test_in_place();
    fails += test_variant();
    fails += test_serial_str();
    printf("=== %s ===\n", fails ? "FAIL" : "ALL PASS");
    return fails ? 1 : 0;
}
