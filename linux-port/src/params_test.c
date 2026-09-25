/*
 * params_test.c — unit tests for the stored-parameter codec (params.c).
 *
 * No hardware and no libusb: every value below is either a hand-computed
 * encoding of a known float or a literal read off the reference unit /
 * MechaniscoutPcap/4.pcapng, so the test pins the *device's* numbers, not
 * this implementation's.
 *
 * build:  via the Makefile (make params-test)
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "params.h"

static int fails;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            printf("  FAIL %s:%d: ", __func__, __LINE__);       \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
            fails++;                                            \
        }                                                       \
    } while (0)

/* ------------------------------------------------------------- encoding */

static int test_encode_kelvin(void)
{
    /* MechaniscoutPcap/4.pcapng frame 5178: the app wrote 01 2c for the
     * reflected temperature, and the read-back (frame 4251) says 01 2c. */
    CHECK(dyt_param_encode_kelvin(26.85f) == 300, "26.85 C -> %u, want 300",
          dyt_param_encode_kelvin(26.85f));
    CHECK(dyt_param_encode_kelvin(25.0f) == 298, "25 C -> %u, want 298",
          dyt_param_encode_kelvin(25.0f));
    CHECK(dyt_param_encode_kelvin(0.0f) == 273, "0 C -> %u, want 273",
          dyt_param_encode_kelvin(0.0f));

    /* The cast truncates toward zero, not toward -inf: -0.5 °C is 272.65
     * and must encode to 272.  A floor() implementation would give 273. */
    CHECK(dyt_param_encode_kelvin(-0.5f) == 272, "-0.5 C -> %u, want 272",
          dyt_param_encode_kelvin(-0.5f));

    /* Negative celsius is reachable: -40 °C is 233 K. */
    CHECK(dyt_param_encode_kelvin(-40.0f) == 233, "-40 C -> %u, want 233",
          dyt_param_encode_kelvin(-40.0f));

    CHECK(dyt_param_decode_kelvin(300) > 26.84f &&
          dyt_param_decode_kelvin(300) < 26.86f,
          "300 K -> %.4f C", dyt_param_decode_kelvin(300));
    CHECK(dyt_param_decode_kelvin(273) > -0.16f &&
          dyt_param_decode_kelvin(273) < -0.14f,
          "273 K -> %.4f C", dyt_param_decode_kelvin(273));
    return 0;
}

static int test_encode_ratio(void)
{
    /* Frame 4502: emissivity 127.  127/128 = 0.9921875 exactly, so this is
     * the value the app must have held.  Note 0.992f is *not* it: 0.992f is
     * 126.976/128 and truncates to 126, which is why the check below pins
     * both numbers. */
    CHECK(dyt_param_encode_ratio(0.9921875f) == 127, "127/128 -> %u, want 127",
          dyt_param_encode_ratio(0.9921875f));
    CHECK(dyt_param_encode_ratio(0.992f) == 126, "0.992 -> %u, want 126",
          dyt_param_encode_ratio(0.992f));
    CHECK(dyt_param_encode_ratio(0.95f) == 121, "0.95 -> %u, want 121",
          dyt_param_encode_ratio(0.95f));
    CHECK(dyt_param_encode_ratio(1.0f) == 128, "1.0 -> %u, want 128",
          dyt_param_encode_ratio(1.0f));
    CHECK(dyt_param_encode_ratio(0.0f) == 0, "0.0 -> %u, want 0",
          dyt_param_encode_ratio(0.0f));

    /* Frame 6532: distance 127, i.e. 0.9921875 m. */
    CHECK(dyt_param_encode_ratio(0.99f) == 126, "0.99 -> %u, want 126",
          dyt_param_encode_ratio(0.99f));
    CHECK(dyt_param_encode_ratio(20.0f) == 2560, "20 m -> %u, want 2560",
          dyt_param_encode_ratio(20.0f));

    CHECK(fabsf(dyt_param_decode_ratio(127) - 0.9921875f) < 1e-7f,
          "127 -> %.6f", dyt_param_decode_ratio(127));
    CHECK(dyt_param_decode_ratio(128) == 1.0f, "128 -> %f",
          dyt_param_decode_ratio(128));
    CHECK(dyt_param_decode_ratio(0) == 0.0f, "0 -> %f",
          dyt_param_decode_ratio(0));
    return 0;
}

/* ---------------------------------------------------------------- names */

static int test_names(void)
{
    int i;

    CHECK(strcmp(dyt_param_name(DYT_PARAM_REFLECTED), "reflected") == 0,
          "slot1 name = %s", dyt_param_name(DYT_PARAM_REFLECTED));
    CHECK(strcmp(dyt_param_name(DYT_PARAM_EMISSIVITY), "emissivity") == 0,
          "slot3 name = %s", dyt_param_name(DYT_PARAM_EMISSIVITY));
    CHECK(strcmp(dyt_param_name(9), "slot9") == 0,
          "slot9 name = %s", dyt_param_name(9));
    CHECK(strcmp(dyt_param_name(-1), "?") == 0,
          "slot -1 name = %s", dyt_param_name(-1));
    CHECK(strcmp(dyt_param_name(99), "?") == 0,
          "slot 99 name = %s", dyt_param_name(99));

    /* Every slot in range must have a distinct, non-empty name. */
    for (i = 0; i < DYT_PARAM_N; i++) {
        const char *a = dyt_param_name(i);
        int j;
        CHECK(a && a[0], "slot %d has no name", i);
        for (j = 0; j < i; j++)
            CHECK(strcmp(a, dyt_param_name(j)) != 0,
                  "slots %d and %d share the name %s", j, i, a);
    }

    CHECK(dyt_param_is_kelvin(DYT_PARAM_REFLECTED) == 1, "slot1 not kelvin");
    CHECK(dyt_param_is_kelvin(DYT_PARAM_AMBIENT) == 1, "slot2 not kelvin");
    CHECK(dyt_param_is_kelvin(DYT_PARAM_EMISSIVITY) == 0, "slot3 kelvin");
    CHECK(dyt_param_is_ratio(DYT_PARAM_EMISSIVITY) == 1, "slot3 not ratio");
    CHECK(dyt_param_is_ratio(DYT_PARAM_DISTANCE) == 1, "slot4 not ratio");
    CHECK(dyt_param_is_ratio(DYT_PARAM_REFLECTED) == 0, "slot1 ratio");
    CHECK(dyt_param_is_kelvin(9) == 0 && dyt_param_is_ratio(9) == 0,
          "slot9 claims a unit");

    CHECK(dyt_param_unit(DYT_PARAM_DISTANCE) &&
          strcmp(dyt_param_unit(DYT_PARAM_DISTANCE), "m") == 0,
          "slot4 unit = %s", dyt_param_unit(DYT_PARAM_DISTANCE) ?
                             dyt_param_unit(DYT_PARAM_DISTANCE) : "(null)");
    CHECK(dyt_param_unit(9) == NULL, "slot9 has a unit");
    return 0;
}

/* ------------------------------------------------------------- snapshot */

static int test_snapshot(void)
{
    dyt_params_t p;
    int i;

    dyt_params_init(&p);
    CHECK(dyt_params_count(&p) == 0, "fresh snapshot has %d slots",
          dyt_params_count(&p));
    CHECK(dyt_params_ok(&p, 1) == 0, "fresh snapshot reports slot1 ok");
    CHECK(dyt_params_value(&p, 1) != dyt_params_value(&p, 1),
          "unread slot1 did not decode to NaN");

    /* Out-of-range writes must be rejected and must not corrupt anything. */
    CHECK(dyt_params_set(&p, -1, 0xffff) == -1, "set(-1) accepted");
    CHECK(dyt_params_set(&p, DYT_PARAM_N, 0xffff) == -1, "set(16) accepted");
    CHECK(dyt_params_fail(&p, -1) == -1, "fail(-1) accepted");
    CHECK(dyt_params_set(NULL, 0, 1) == -1, "set(NULL) accepted");
    CHECK(dyt_params_count(&p) == 0, "a rejected set changed the snapshot");

    CHECK(dyt_params_set(&p, DYT_PARAM_REFLECTED, 300) == 0, "set slot1");
    CHECK(dyt_params_set(&p, DYT_PARAM_EMISSIVITY, 127) == 0, "set slot3");
    CHECK(dyt_params_count(&p) == 2, "count = %d, want 2", dyt_params_count(&p));
    CHECK(dyt_params_ok(&p, DYT_PARAM_REFLECTED) == 1, "slot1 not ok");
    CHECK(dyt_params_ok(&p, DYT_PARAM_AMBIENT) == 0, "slot2 ok but unread");

    CHECK(fabsf(dyt_params_value(&p, DYT_PARAM_REFLECTED) - 26.85f) < 1e-4f,
          "slot1 = %.4f", dyt_params_value(&p, DYT_PARAM_REFLECTED));
    CHECK(fabsf(dyt_params_value(&p, DYT_PARAM_EMISSIVITY) - 0.9921875f) < 1e-7f,
          "slot3 = %.6f", dyt_params_value(&p, DYT_PARAM_EMISSIVITY));

    /* A failed read clears the bit *and* the stale value. */
    CHECK(dyt_params_fail(&p, DYT_PARAM_REFLECTED) == 0, "fail slot1");
    CHECK(dyt_params_ok(&p, DYT_PARAM_REFLECTED) == 0, "slot1 still ok");
    CHECK(p.raw[DYT_PARAM_REFLECTED] == 0, "failed slot kept value %u",
          p.raw[DYT_PARAM_REFLECTED]);
    CHECK(dyt_params_count(&p) == 1, "count after fail = %d", dyt_params_count(&p));

    /* Slot0 and the machine coefficient have no unit but do report a value. */
    dyt_params_set(&p, DYT_PARAM_SLOT0, 32);
    CHECK(dyt_params_value(&p, DYT_PARAM_SLOT0) == 32.0f, "slot0 = %f",
          dyt_params_value(&p, DYT_PARAM_SLOT0));
    dyt_params_set(&p, DYT_PARAM_MACHINE, 1);
    CHECK(dyt_params_value(&p, DYT_PARAM_MACHINE) == 1.0f, "slot5 = %f",
          dyt_params_value(&p, DYT_PARAM_MACHINE));

    /* An uninterpreted slot has no unit, so it must be NaN even when read. */
    dyt_params_set(&p, 7, 15878);
    CHECK(dyt_params_value(&p, 7) != dyt_params_value(&p, 7),
          "slot7 decoded to %f but has no known unit", dyt_params_value(&p, 7));

    /* All 16 slots can be held at once. */
    dyt_params_init(&p);
    for (i = 0; i < DYT_PARAM_N; i++)
        CHECK(dyt_params_set(&p, i, (uint16_t)(0x100 + i)) == 0,
              "set slot %d", i);
    CHECK(dyt_params_count(&p) == DYT_PARAM_N, "count = %d, want 16",
          dyt_params_count(&p));
    CHECK(p.valid == 0xffffu, "valid mask = 0x%04x, want 0xffff", p.valid);
    for (i = 0; i < DYT_PARAM_N; i++)
        CHECK(p.raw[i] == (uint16_t)(0x100 + i), "slot %d = 0x%04x", i, p.raw[i]);
    return 0;
}

/* ----------------------------------------------------------- radiometry */

static int test_radiometry(void)
{
    dyt_radiometry_t r;
    dyt_params_t p;

    /* The reference unit, as read live 2026-09-25 (and as the Android app
     * reads it back in MechaniscoutPcap/4.pcapng frames 4233/4251/4269). */
    dyt_params_init(&p);
    dyt_params_set(&p, DYT_PARAM_REFLECTED, 300);
    dyt_params_set(&p, DYT_PARAM_AMBIENT, 300);
    dyt_params_set(&p, DYT_PARAM_EMISSIVITY, 127);
    dyt_params_set(&p, DYT_PARAM_DISTANCE, 127);

    CHECK(dyt_params_radiometry(&p, &r) == DYT_RADIO_ALL,
          "ok mask = 0x%x, want 0xf", r.ok);
    CHECK(r.reflected_k == 300 && r.ambient_k == 300, "temps %u/%u",
          r.reflected_k, r.ambient_k);
    CHECK(r.emissivity == 127 && r.distance == 127, "ratios %u/%u",
          r.emissivity, r.distance);

    CHECK(fabsf(dyt_radiometry_reflected_c(&r) - 26.85f) < 1e-4f,
          "reflected = %.4f C", dyt_radiometry_reflected_c(&r));
    CHECK(fabsf(dyt_radiometry_ambient_c(&r) - 26.85f) < 1e-4f,
          "ambient = %.4f C", dyt_radiometry_ambient_c(&r));
    CHECK(fabsf(dyt_radiometry_emissivity(&r) - 0.9921875f) < 1e-7f,
          "emissivity = %.6f", dyt_radiometry_emissivity(&r));
    CHECK(fabsf(dyt_radiometry_distance_m(&r) - 0.9921875f) < 1e-7f,
          "distance = %.6f m", dyt_radiometry_distance_m(&r));

    /* A partial snapshot must leave the unread slots NaN, not defaulted. */
    dyt_params_init(&p);
    dyt_params_set(&p, DYT_PARAM_EMISSIVITY, 100);
    CHECK(dyt_params_radiometry(&p, &r) == DYT_RADIO_EMISSIVITY,
          "partial ok mask = 0x%x, want 0x4", r.ok);
    CHECK(dyt_radiometry_reflected_c(&r) != dyt_radiometry_reflected_c(&r),
          "unread reflected decoded to %f", dyt_radiometry_reflected_c(&r));
    CHECK(dyt_radiometry_ambient_c(&r) != dyt_radiometry_ambient_c(&r),
          "unread ambient decoded to %f", dyt_radiometry_ambient_c(&r));
    CHECK(dyt_radiometry_distance_m(&r) != dyt_radiometry_distance_m(&r),
          "unread distance decoded to %f", dyt_radiometry_distance_m(&r));
    CHECK(fabsf(dyt_radiometry_emissivity(&r) - 100.0f / 128.0f) < 1e-7f,
          "emissivity = %.6f", dyt_radiometry_emissivity(&r));

    CHECK(dyt_params_radiometry(&p, NULL) == 0, "NULL out accepted");
    CHECK(dyt_params_radiometry(NULL, &r) == 0, "NULL snapshot accepted");
    CHECK(r.ok == 0, "NULL snapshot left ok = 0x%x", r.ok);

    /* The shipped defaults are exactly what the unit reports. */
    dyt_radiometry_default(&r);
    CHECK(r.ok == DYT_RADIO_ALL, "default ok = 0x%x", r.ok);
    CHECK(r.reflected_k == 300 && r.ambient_k == 300 &&
          r.emissivity == 127 && r.distance == 127,
          "defaults %u/%u/%u/%u", r.reflected_k, r.ambient_k,
          r.emissivity, r.distance);

    CHECK(dyt_radiometry_reflected_c(NULL) != dyt_radiometry_reflected_c(NULL),
          "NULL radiometry did not decode to NaN");
    return 0;
}

/* Round-trip: the encoding the port writes is the encoding the port reads. */
static int test_roundtrip(void)
{
    static const float c[] = { -40.0f, -10.0f, 0.0f, 20.0f, 26.85f, 100.0f, 400.0f };
    static const float v[] = { 0.0f, 0.05f, 0.5f, 0.99f, 1.0f, 4.0f, 20.0f };
    unsigned i;

    for (i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
        uint16_t raw = dyt_param_encode_kelvin(c[i]);
        float back = dyt_param_decode_kelvin(raw);
        /* Truncation means the round trip lands within 1 K below the input. */
        CHECK(back <= c[i] + 1e-4f && back > c[i] - 1.001f,
              "kelvin round trip %.2f -> %u -> %.3f", c[i], raw, back);
    }
    for (i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        uint16_t raw = dyt_param_encode_ratio(v[i]);
        float back = dyt_param_decode_ratio(raw);
        CHECK(back <= v[i] + 1e-6f && back > v[i] - 1.0f / 128.0f - 1e-6f,
              "ratio round trip %.3f -> %u -> %.5f", v[i], raw, back);
    }
    return 0;
}

/* --------------------------------------------------------- write order */

static int cmd_is(const uint8_t cmd[8], const uint8_t want[8])
{
    return memcmp(cmd, want, 8) == 0;
}

static int test_build_cmd(void)
{
    uint8_t cmd[8];

    /* The four orders the app actually sent, straight out of
     * MechaniscoutPcap/4.pcapng.  These pin the exact wire bytes. */
    {
        static const uint8_t emis[8] = { 0x14,0xc5,0x00,0x03,0x00,0x00,0x00,0x7f };
        static const uint8_t refl[8] = { 0x14,0xc5,0x00,0x01,0x00,0x00,0x01,0x2c };
        static const uint8_t amb[8]  = { 0x14,0xc5,0x00,0x02,0x00,0x00,0x01,0x2c };
        static const uint8_t dist[8] = { 0x14,0xc5,0x00,0x04,0x00,0x00,0x00,0x7f };

        CHECK(dyt_params_build_cmd(cmd, DYT_ORDER_EMISSIVITY, 0.9921875f) == 0 &&
              cmd_is(cmd, emis), "emissivity 0.9921875 -> %02x%02x%02x%02x%02x%02x%02x%02x",
              cmd[0],cmd[1],cmd[2],cmd[3],cmd[4],cmd[5],cmd[6],cmd[7]);
        CHECK(dyt_params_build_cmd(cmd, DYT_ORDER_REFLECTED, 26.85f) == 0 &&
              cmd_is(cmd, refl), "reflected 26.85 C -> %02x%02x%02x%02x%02x%02x%02x%02x",
              cmd[0],cmd[1],cmd[2],cmd[3],cmd[4],cmd[5],cmd[6],cmd[7]);
        CHECK(dyt_params_build_cmd(cmd, DYT_ORDER_AMBIENT, 26.85f) == 0 &&
              cmd_is(cmd, amb), "ambient 26.85 C -> %02x%02x%02x%02x%02x%02x%02x%02x",
              cmd[0],cmd[1],cmd[2],cmd[3],cmd[4],cmd[5],cmd[6],cmd[7]);
        CHECK(dyt_params_build_cmd(cmd, DYT_ORDER_DISTANCE, 0.9921875f) == 0 &&
              cmd_is(cmd, dist), "distance 0.9921875 m -> %02x%02x%02x%02x%02x%02x%02x%02x",
              cmd[0],cmd[1],cmd[2],cmd[3],cmd[4],cmd[5],cmd[6],cmd[7]);
    }

    /* The pre-fill is a fixed constant, not derived from the value. */
    {
        static const uint8_t want[8] = { 0,0,0,0,0,0,0,0x02 };
        CHECK(cmd_is(dyt_order_prefill, want), "prefill is not 00..00 02");
    }

    /* The type byte is the *only* thing that varies between orders of the
     * same value: 0.9921875 -> 127 for emissivity and distance alike. */
    {
        uint8_t a[8], b[8];
        dyt_params_build_cmd(a, DYT_ORDER_EMISSIVITY, 0.9921875f);
        dyt_params_build_cmd(b, DYT_ORDER_DISTANCE, 0.9921875f);
        CHECK(a[7] == 127 && b[7] == 127 && a[3] == 3 && b[3] == 4,
              "type/value bytes wrong");
    }

    /* Type range and value range are both rejected, and a rejected build
     * must not be sent (the caller checks the return). */
    CHECK(dyt_params_build_cmd(cmd, 0, 1.0f) == -1, "type 0 accepted");
    CHECK(dyt_params_build_cmd(cmd, 5, 1.0f) == -1, "type 5 accepted");
    CHECK(dyt_params_build_cmd(cmd, 0x14, 1.0f) == -1, "type 0x14 accepted");
    CHECK(dyt_params_build_cmd(cmd, DYT_ORDER_EMISSIVITY, -0.1f) == -1,
          "negative emissivity accepted");
    CHECK(dyt_params_build_cmd(cmd, DYT_ORDER_REFLECTED, -400.0f) == -1,
          "negative kelvin accepted");
    CHECK(dyt_params_build_cmd(cmd, DYT_ORDER_DISTANCE, 600.0f) == -1,
          "distance 600 m accepted (raw 76800 > 65535)");
    CHECK(dyt_params_build_cmd(NULL, DYT_ORDER_EMISSIVITY, 0.99f) == -1,
          "NULL cmd accepted");

    CHECK(dyt_order_type_ok(1) && dyt_order_type_ok(4), "type 1/4 rejected");
    CHECK(!dyt_order_type_ok(0) && !dyt_order_type_ok(5), "type 0/5 accepted");

    /* Truncation, not rounding — the same rule as the read-back codec. */
    {
        uint8_t c[8];
        dyt_params_build_cmd(c, DYT_ORDER_EMISSIVITY, 0.999f);
        CHECK(c[7] == 127, "0.999 * 128 = 127.87 should truncate to 127, got %u", c[7]);
    }
    return 0;
}

int main(void)
{
    printf("=== params_test ===\n");
    test_encode_kelvin();
    test_encode_ratio();
    test_build_cmd();
    test_names();
    test_snapshot();
    test_radiometry();
    test_roundtrip();
    printf("=== %s (%d failure%s) ===\n", fails ? "FAIL" : "ALL PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
