/*
 * alarm.c — high/low temperature alarms with hysteresis (see alarm.h).
 *
 * Each side is a two-state Schmitt trigger; the only subtlety is which way
 * the comparison goes in each state, so both transitions are spelled out
 * rather than folded into one expression.
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -I. -c alarm.c -o alarm.o
 */
#include <math.h>

#include "alarm.h"

void dyt_alarm_init(dyt_alarm_t *a)
{
    if (!a)
        return;

    a->enabled   = 0;
    a->hi        = 0.0f;
    a->lo        = 0.0f;
    a->hyst      = DYT_ALARM_HYST_DEFAULT;
    a->hi_active = 0;
    a->lo_active = 0;
    a->last_hot  = NAN;
    a->last_cold = NAN;
}

void dyt_alarm_set(dyt_alarm_t *a, float lo, float hi, float hyst)
{
    if (!a)
        return;

    a->lo     = lo;
    a->hi     = hi;
    a->hyst   = (hyst > 0.0f && isfinite(hyst)) ? hyst : DYT_ALARM_HYST_DEFAULT;
    a->enabled = 1;

    /* Thresholds changed, so any standing latch is meaningless. */
    a->hi_active = 0;
    a->lo_active = 0;
}

void dyt_alarm_disable(dyt_alarm_t *a)
{
    if (!a)
        return;

    a->enabled   = 0;
    a->hi_active = 0;
    a->lo_active = 0;
}

void dyt_alarm_reset(dyt_alarm_t *a)
{
    if (!a)
        return;

    a->hi_active = 0;
    a->lo_active = 0;
}

int dyt_alarm_is_sane(const dyt_alarm_t *a)
{
    if (!a)
        return 0;
    return a->lo <= a->hi;
}

dyt_alarm_state_t dyt_alarm_update(dyt_alarm_t *a, float hot, float cold)
{
    if (!a || !a->enabled)
        return DYT_ALARM_NONE;

    if (isfinite(hot)) {
        a->last_hot = hot;
        if (!a->hi_active) {
            if (hot > a->hi)                 /* trip */
                a->hi_active = 1;
        } else {
            if (hot < a->hi - a->hyst)       /* clear, only past the band */
                a->hi_active = 0;
        }
    }

    if (isfinite(cold)) {
        a->last_cold = cold;
        if (!a->lo_active) {
            if (cold < a->lo)                /* trip */
                a->lo_active = 1;
        } else {
            if (cold > a->lo + a->hyst)      /* clear, only past the band */
                a->lo_active = 0;
        }
    }

    return dyt_alarm_state(a);
}

dyt_alarm_state_t dyt_alarm_state(const dyt_alarm_t *a)
{
    int bits;

    if (!a)
        return DYT_ALARM_NONE;

    bits = (a->hi_active ? DYT_ALARM_HIGH : 0) |
           (a->lo_active ? DYT_ALARM_LOW  : 0);
    return (dyt_alarm_state_t)bits;
}

int dyt_alarm_tripped(const dyt_alarm_t *a)
{
    return dyt_alarm_state(a) != DYT_ALARM_NONE;
}

const char *dyt_alarm_name(dyt_alarm_state_t s)
{
    switch (s) {
    case DYT_ALARM_NONE: return "none";
    case DYT_ALARM_HIGH: return "high";
    case DYT_ALARM_LOW:  return "low";
    case DYT_ALARM_BOTH: return "high+low";
    }
    return "?";
}
