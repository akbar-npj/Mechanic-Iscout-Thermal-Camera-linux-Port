/*
 * units_test.c — unit tests for units.c.
 *
 * The conversion is trivial; what this guards is the *edges*: that a bad
 * enum cannot silently produce a plausible-looking reading, that the
 * Fahrenheit and Kelvin round-trips do not drift, and that dyt_temp_format
 * reports truncation instead of returning a clipped string.  Those are the
 * failure modes that would show up as a wrong number on screen rather than
 * as a crash.
 *
 * build:  via the Makefile (make check)
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "units.h"

static int fails;

static void ok(const char *what)
{
    printf("  ok   %s\n", what);
}

static void fail(const char *what, const char *detail)
{
    printf("  FAIL %-46s %s\n", what, detail);
    fails++;
}

static int near(float a, float b, float eps)
{
    return fabsf(a - b) <= eps;
}

/* --- conversion --------------------------------------------------------- */

static void conv(const char *what, dyt_unit_t u, float c,
                 float want, float eps)
{
    float got = dyt_temp_convert(u, c);

    if (near(got, want, eps)) {
        ok(what);
    } else {
        char d[96];
        snprintf(d, sizeof d, "got %.4f want %.4f", (double)got, (double)want);
        fail(what, d);
    }
}

/* --- formatting --------------------------------------------------------- */

static void fmt(const char *what, dyt_unit_t u, float c,
                const char *want)
{
    char buf[64];
    int  n = dyt_temp_format(u, c, buf, sizeof buf);

    if (n < 0) {
        fail(what, "dyt_temp_format returned -1");
        return;
    }
    if (strcmp(buf, want) == 0) {
        char d[96];
        snprintf(d, sizeof d, "got \"%s\"", buf);
        (void)d;
        ok(what);
    } else {
        char d[96];
        snprintf(d, sizeof d, "got \"%s\" want \"%s\"", buf, want);
        fail(what, d);
    }
}

/* --- parsing ------------------------------------------------------------ */

static void parse(const char *what, const char *s, int want)
{
    int got = dyt_unit_parse(s);

    if (got == want) {
        ok(what);
    } else {
        char d[96];
        snprintf(d, sizeof d, "got %d want %d", got, want);
        fail(what, d);
    }
}

/* --- naming ------------------------------------------------------------- */

static void suffix(const char *what, dyt_unit_t u, const char *want)
{
    const char *got = dyt_unit_suffix(u);

    if (want == NULL) {
        if (got == NULL) { ok(what); return; }
        fail(what, "expected NULL");
        return;
    }
    if (got && strcmp(got, want) == 0) {
        ok(what);
    } else {
        char d[96];
        snprintf(d, sizeof d, "got \"%s\" want \"%s\"",
                 got ? got : "(null)", want);
        fail(what, d);
    }
}

int main(void)
{
    int i;

    printf("=== units_test ===\n");

    /* --- known conversions ------------------------------------------- */
    printf("-- conversion --\n");
    conv("0 C   -> C  = 0",        DYT_UNIT_C,    0.0f,    0.0f, 1e-6f);
    conv("0 C   -> F  = 32",       DYT_UNIT_F,    0.0f,   32.0f, 1e-4f);
    conv("0 C   -> K  = 273.15",   DYT_UNIT_K,    0.0f,  273.15f, 1e-3f);
    conv("100 C -> F  = 212",      DYT_UNIT_F,  100.0f,  212.0f, 1e-3f);
    conv("100 C -> K  = 373.15",   DYT_UNIT_K,  100.0f,  373.15f, 1e-3f);
    conv("-40 C -> F  = -40",      DYT_UNIT_F,  -40.0f,  -40.0f, 1e-3f);
    conv("25 C  -> F  = 77",       DYT_UNIT_F,   25.0f,   77.0f, 1e-3f);
    conv("37 C  -> F  = 98.6",     DYT_UNIT_F,   37.0f,   98.6f, 1e-3f);
    conv("31 C  -> K  = 304.15",   DYT_UNIT_K,   31.0f,  304.15f, 1e-3f);

    /* An out-of-range enum must pass the value through untouched rather
     * than index off the end of the tables. */
    conv("bad unit passes through", DYT_UNIT_N,  12.5f,   12.5f, 1e-6f);

    /* --- inverse + round trip ---------------------------------------- */
    printf("-- inverse --\n");
    {
        const float cs[] = { -40.0f, -10.0f, 0.0f, 21.5f, 25.0f, 37.0f,
                             100.0f, 300.0f };
        int bad_f = 0, bad_k = 0;

        for (i = 0; i < (int)(sizeof cs / sizeof cs[0]); i++) {
            float f = dyt_temp_convert(DYT_UNIT_F, cs[i]);
            float k = dyt_temp_convert(DYT_UNIT_K, cs[i]);
            if (!near(dyt_temp_to_celsius(DYT_UNIT_F, f), cs[i], 1e-3f))
                bad_f = 1;
            if (!near(dyt_temp_to_celsius(DYT_UNIT_K, k), cs[i], 1e-3f))
                bad_k = 1;
        }
        if (bad_f) fail("F round trip", "drifted");
        else       ok("F round trip (8 values)");
        if (bad_k) fail("K round trip", "drifted");
        else       ok("K round trip (8 values)");

        /* Celsius is the identity in both directions. */
        if (near(dyt_temp_to_celsius(DYT_UNIT_C, 42.0f), 42.0f, 1e-6f))
            ok("C identity");
        else
            fail("C identity", "not identity");
    }

    /* --- naming ------------------------------------------------------- */
    printf("-- naming --\n");
    suffix("suffix C", DYT_UNIT_C, "C");
    suffix("suffix F", DYT_UNIT_F, "F");
    suffix("suffix K", DYT_UNIT_K, "K");
    suffix("suffix out of range", DYT_UNIT_N, NULL);

    if (strcmp(dyt_unit_name(DYT_UNIT_C), "Celsius") == 0 &&
        strcmp(dyt_unit_name(DYT_UNIT_F), "Fahrenheit") == 0 &&
        strcmp(dyt_unit_name(DYT_UNIT_K), "Kelvin") == 0)
        ok("names");
    else
        fail("names", "unexpected");

    if (dyt_unit_name(DYT_UNIT_N) == NULL && dyt_unit_label(DYT_UNIT_N) == NULL)
        ok("name/label out of range are NULL");
    else
        fail("name/label out of range", "expected NULL");

    /* The label carries the vendor's degree sign (UTF-8). */
    if (dyt_unit_label(DYT_UNIT_C) &&
        strcmp(dyt_unit_label(DYT_UNIT_C), "\xc2\xb0" "C") == 0)
        ok("label C is UTF-8 degree-C");
    else
        fail("label C", "not \"\\xc2\\xb0C\"");

    /* --- parsing ------------------------------------------------------ */
    printf("-- parsing --\n");
    parse("parse \"C\"",            "C",            DYT_UNIT_C);
    parse("parse \"c\"",            "c",            DYT_UNIT_C);
    parse("parse \"celsius\"",      "celsius",      DYT_UNIT_C);
    parse("parse \"CELSIUS\"",      "CELSIUS",      DYT_UNIT_C);
    parse("parse \"F\"",            "F",            DYT_UNIT_F);
    parse("parse \"fahrenheit\"",   "fahrenheit",   DYT_UNIT_F);
    parse("parse \"K\"",            "K",            DYT_UNIT_K);
    parse("parse \"kelvin\"",       "kelvin",       DYT_UNIT_K);
    parse("parse \"\\xc2\\xb0C\"",  "\xc2\xb0" "C", DYT_UNIT_C);
    parse("parse \"\\xc2\\xb0F\"",  "\xc2\xb0" "F", DYT_UNIT_F);

    parse("reject NULL",            NULL,           -1);
    parse("reject \"\"",            "",             -1);
    parse("reject \"x\"",           "x",            -1);
    parse("reject \"cat\"",         "cat",          -1);
    parse("reject \"degrees\"",     "degrees",      -1);
    /* Documented limitation: the single-codepoint forms have no trailing
     * ASCII letter, so the leading-UTF-8 skip leaves nothing to match. */
    parse("reject \"\\xe2\\x84\\x89\" (U+2109)", "\xe2\x84\x89", -1);

    /* --- cycling ------------------------------------------------------ */
    printf("-- cycling --\n");
    if (dyt_unit_next(DYT_UNIT_C) == DYT_UNIT_F &&
        dyt_unit_next(DYT_UNIT_F) == DYT_UNIT_K &&
        dyt_unit_next(DYT_UNIT_K) == DYT_UNIT_C &&
        dyt_unit_next(DYT_UNIT_N) == DYT_UNIT_C)
        ok("C -> F -> K -> C, out-of-range -> C");
    else
        fail("cycling", "wrong order");

    /* --- formatting --------------------------------------------------- */
    printf("-- formatting --\n");
    fmt("format 25 C",          DYT_UNIT_C,  25.0f,  "25.0 C");
    fmt("format 25 C as F",     DYT_UNIT_F,  25.0f,  "77.0 F");
    fmt("format 0 C",           DYT_UNIT_C,   0.0f,  "0.0 C");
    fmt("format 100 C",         DYT_UNIT_C, 100.0f,  "100.0 C");
    fmt("format -40 C as F",    DYT_UNIT_F, -40.0f,  "-40.0 F");
    fmt("format 31.15 C",       DYT_UNIT_C,  31.15f, "31.1 C");

    /* Truncation must be reported, not silently clipped. */
    {
        char small[4];
        int  n = dyt_temp_format(DYT_UNIT_C, 25.0f, small, sizeof small);
        if (n < 0) ok("truncation returns -1");
        else       fail("truncation", "returned success");
    }
    {
        char buf[32];
        if (dyt_temp_format(DYT_UNIT_C, 25.0f, NULL, 10) < 0 &&
            dyt_temp_format(DYT_UNIT_C, 25.0f, buf, 0) < 0 &&
            dyt_temp_format(DYT_UNIT_N, 25.0f, buf, sizeof buf) < 0)
            ok("bad arguments return -1");
        else
            fail("bad arguments", "returned success");
    }

    /* NaN is surfaced as "nan", not suppressed — the caller decides. */
    {
        char buf[32];
        int  n = dyt_temp_format(DYT_UNIT_C, NAN, buf, sizeof buf);
        if (n > 0 && strstr(buf, "nan") != NULL)
            ok("NaN renders as nan");
        else
            fail("NaN", "not rendered as nan");
    }

    printf("=== %s ===\n", fails ? "FAIL" : "ALL PASS");
    return fails ? 1 : 0;
}
