/*
 * alarm_test.c — unit tests for alarm.c.
 *
 * Hysteresis is the whole point of the module, so the interesting cases are
 * the two edges: a reading inside the band must NOT clear a standing alarm,
 * and a reading exactly on the threshold must NOT trip one.  Both are the
 * kind of off-by-one that turns a useful alarm into a flickering one.
 *
 * build:  via the Makefile (make check)
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "alarm.h"

static int fails;

static void ok(const char *what) { printf("  ok   %s\n", what); }

static void fail(const char *what, const char *detail)
{
    printf("  FAIL %-46s %s\n", what, detail);
    fails++;
}

static void intcheck(const char *what, int got, int want)
{
    if (got == want) {
        ok(what);
    } else {
        char d[96];
        snprintf(d, sizeof d, "got %d want %d", got, want);
        fail(what, d);
    }
}

static void statecheck(const char *what, dyt_alarm_t *a, float hot, float cold,
                       dyt_alarm_state_t want)
{
    dyt_alarm_state_t got = dyt_alarm_update(a, hot, cold);
    if (got == want) {
        ok(what);
    } else {
        char d[128];
        snprintf(d, sizeof d, "got %s want %s",
                 dyt_alarm_name(got), dyt_alarm_name(want));
        fail(what, d);
    }
}

/* --- lifecycle ---------------------------------------------------------- */

static void test_lifecycle(void)
{
    dyt_alarm_t a;

    dyt_alarm_init(&a);
    intcheck("init: disabled", a.enabled, 0);
    intcheck("init: state is none", (int)dyt_alarm_state(&a), DYT_ALARM_NONE);
    intcheck("init: not tripped", dyt_alarm_tripped(&a), 0);
    intcheck("init: default hysteresis",
             (int)(a.hyst == DYT_ALARM_HYST_DEFAULT), 1);
    intcheck("init: hot is NaN before any reading", isnan(a.last_hot), 1);

    /* A disabled alarm never trips, however extreme the reading. */
    statecheck("disabled: extreme hot still none", &a, 999.0f, -999.0f,
               DYT_ALARM_NONE);

    dyt_alarm_set(&a, 20.0f, 30.0f, 2.0f);
    intcheck("set: enabled", a.enabled, 1);
    intcheck("set: sane", dyt_alarm_is_sane(&a), 1);

    dyt_alarm_disable(&a);
    intcheck("disable: disabled", a.enabled, 0);
    statecheck("disable: extreme hot still none", &a, 999.0f, 999.0f,
               DYT_ALARM_NONE);

    /* NULL safety. */
    dyt_alarm_init(NULL);
    dyt_alarm_set(NULL, 0, 1, 1);
    dyt_alarm_disable(NULL);
    dyt_alarm_reset(NULL);
    intcheck("NULL: is_sane", dyt_alarm_is_sane(NULL), 0);
    intcheck("NULL: state", (int)dyt_alarm_state(NULL), DYT_ALARM_NONE);
    intcheck("NULL: tripped", dyt_alarm_tripped(NULL), 0);
    intcheck("NULL: update", (int)dyt_alarm_update(NULL, 1, 1), DYT_ALARM_NONE);
}

/* --- the high side ------------------------------------------------------ */

static void test_high(void)
{
    dyt_alarm_t a;

    dyt_alarm_set(&a, 20.0f, 30.0f, 2.0f);

    statecheck("high: inside band -> none", &a, 25.0f, 25.0f, DYT_ALARM_NONE);
    statecheck("high: exactly at threshold -> none", &a, 30.0f, 25.0f,
               DYT_ALARM_NONE);
    statecheck("high: above threshold -> high", &a, 30.1f, 25.0f,
               DYT_ALARM_HIGH);

    /* Standing alarm must survive readings inside the hysteresis band. */
    statecheck("high: 29 still high (in band)", &a, 29.0f, 25.0f,
               DYT_ALARM_HIGH);
    statecheck("high: 28.5 still high (in band)", &a, 28.5f, 25.0f,
               DYT_ALARM_HIGH);
    statecheck("high: exactly hi-hyst still high", &a, 28.0f, 25.0f,
               DYT_ALARM_HIGH);

    /* Only past the band does it clear. */
    statecheck("high: 27.9 clears", &a, 27.9f, 25.0f, DYT_ALARM_NONE);
    statecheck("high: stays cleared", &a, 25.0f, 25.0f, DYT_ALARM_NONE);
    statecheck("high: re-trips above threshold", &a, 31.0f, 25.0f,
               DYT_ALARM_HIGH);
}

/* --- the low side ------------------------------------------------------- */

static void test_low(void)
{
    dyt_alarm_t a;

    dyt_alarm_set(&a, 20.0f, 30.0f, 2.0f);

    statecheck("low: inside band -> none", &a, 25.0f, 25.0f, DYT_ALARM_NONE);
    statecheck("low: exactly at threshold -> none", &a, 25.0f, 20.0f,
               DYT_ALARM_NONE);
    statecheck("low: below threshold -> low", &a, 25.0f, 19.9f, DYT_ALARM_LOW);

    statecheck("low: 21 still low (in band)", &a, 25.0f, 21.0f, DYT_ALARM_LOW);
    statecheck("low: exactly lo+hyst still low", &a, 25.0f, 22.0f,
               DYT_ALARM_LOW);

    statecheck("low: 22.1 clears", &a, 25.0f, 22.1f, DYT_ALARM_NONE);
    statecheck("low: stays cleared", &a, 25.0f, 25.0f, DYT_ALARM_NONE);
    statecheck("low: re-trips below threshold", &a, 25.0f, 18.0f,
               DYT_ALARM_LOW);
}

/* --- both sides at once ------------------------------------------------- */

static void test_both(void)
{
    dyt_alarm_t a;

    dyt_alarm_set(&a, 20.0f, 30.0f, 2.0f);
    statecheck("both: wide scene trips both", &a, 35.0f, 15.0f,
               DYT_ALARM_BOTH);
    intcheck("both: tripped() true", dyt_alarm_tripped(&a), 1);
    intcheck("both: name", strcmp(dyt_alarm_name(DYT_ALARM_BOTH), "high+low"),
             0);

    /* Clearing one side leaves the other standing. */
    statecheck("both: hot clears, low remains", &a, 25.0f, 15.0f,
               DYT_ALARM_LOW);
    statecheck("both: low clears too", &a, 25.0f, 25.0f, DYT_ALARM_NONE);
}

/* --- reset, NaN, and configuration edges -------------------------------- */

static void test_edges(void)
{
    dyt_alarm_t a;

    /* reset() clears the latches but stays armed. */
    dyt_alarm_set(&a, 20.0f, 30.0f, 2.0f);
    statecheck("edge: tripped", &a, 35.0f, 15.0f, DYT_ALARM_BOTH);
    dyt_alarm_reset(&a);
    intcheck("edge: reset clears", (int)dyt_alarm_state(&a), DYT_ALARM_NONE);
    intcheck("edge: reset stays armed", a.enabled, 1);
    statecheck("edge: re-arms on next reading", &a, 35.0f, 15.0f,
               DYT_ALARM_BOTH);

    /* A dropped frame must not clear a standing alarm. */
    dyt_alarm_set(&a, 20.0f, 30.0f, 2.0f);
    statecheck("edge: tripped", &a, 35.0f, 15.0f, DYT_ALARM_BOTH);
    statecheck("edge: NaN leaves both latched", &a, NAN, NAN, DYT_ALARM_BOTH);
    intcheck("edge: NaN did not overwrite last_hot",
             (int)(a.last_hot == 35.0f), 1);

    /* ...and a NaN must not invent one either. */
    dyt_alarm_set(&a, 20.0f, 30.0f, 2.0f);
    statecheck("edge: NaN from clear stays clear", &a, NAN, NAN,
               DYT_ALARM_NONE);

    /* Non-positive hysteresis falls back to the default. */
    dyt_alarm_set(&a, 20.0f, 30.0f, 0.0f);
    intcheck("edge: zero hysteresis -> default",
             (int)(a.hyst == DYT_ALARM_HYST_DEFAULT), 1);
    dyt_alarm_set(&a, 20.0f, 30.0f, -5.0f);
    intcheck("edge: negative hysteresis -> default",
             (int)(a.hyst == DYT_ALARM_HYST_DEFAULT), 1);

    /* Inverted thresholds are reported, not silently repaired.  The two
     * sides stay independent, so an inverted pair trips on nearly anything:
     * a reading above `hi` trips high, one below `lo` trips low. */
    dyt_alarm_set(&a, 40.0f, 10.0f, 2.0f);
    intcheck("edge: inverted thresholds flagged insane",
             dyt_alarm_is_sane(&a), 0);
    statecheck("edge: inverted trips high above hi", &a, 50.0f, 45.0f,
               DYT_ALARM_HIGH);
    statecheck("edge: inverted trips low below lo", &a, 5.0f, 0.0f,
               DYT_ALARM_LOW);

    /* Changing the thresholds drops any standing latch. */
    dyt_alarm_set(&a, 20.0f, 30.0f, 2.0f);
    statecheck("edge: tripped", &a, 35.0f, 25.0f, DYT_ALARM_HIGH);
    dyt_alarm_set(&a, 20.0f, 40.0f, 2.0f);
    intcheck("edge: reconfiguring clears the latch",
             (int)dyt_alarm_state(&a), DYT_ALARM_NONE);
}

static void test_names(void)
{
    intcheck("name: none", strcmp(dyt_alarm_name(DYT_ALARM_NONE), "none"), 0);
    intcheck("name: high", strcmp(dyt_alarm_name(DYT_ALARM_HIGH), "high"), 0);
    intcheck("name: low",  strcmp(dyt_alarm_name(DYT_ALARM_LOW),  "low"),  0);
    intcheck("name: both", strcmp(dyt_alarm_name(DYT_ALARM_BOTH), "high+low"),
             0);
}

int main(void)
{
    printf("=== alarm_test (thresholds + hysteresis) ===\n");

    test_lifecycle();
    test_high();
    test_low();
    test_both();
    test_edges();
    test_names();

    if (fails) {
        printf("=== %d FAILURE(S) ===\n", fails);
        return 1;
    }
    printf("=== ALL PASS ===\n");
    return 0;
}
