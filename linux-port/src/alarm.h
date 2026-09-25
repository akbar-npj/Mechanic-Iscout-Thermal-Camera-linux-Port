/*
 * alarm.h — high/low temperature alarms with hysteresis.
 *
 * The vendor app exposes a high and a low threshold (`handleSetHighThrow` /
 * `handleSetLowThrow`, RE Docs 03 §3.5 rows 18/19) that fire when the scene
 * crosses them.  A naive `t > hi` test chatters: on a real sensor a scene
 * sitting exactly on the threshold toggles the alarm every frame, which is
 * both useless to look at and (in the vendor's app) an audible nuisance.
 *
 * So each side is a Schmitt trigger.  The high alarm *trips* when the reading
 * rises above `hi` and does not *clear* until it falls below `hi - hyst`.
 * The low alarm trips below `lo` and clears above `lo + hyst`.  The gap is
 * the hysteresis band, in the same units as the thresholds (Celsius).
 *
 * Pure: no camera, no GUI, no time.  The caller feeds readings; nothing here
 * decides *which* reading matters (the vendor evaluates the scene's extremes,
 * but a point probe is just as valid), so the caller passes the hot and cold
 * values it cares about.
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -I. -c alarm.c
 */
#ifndef DYT_ALARM_H
#define DYT_ALARM_H

#ifdef __cplusplus
extern "C" {
#endif

/* The default hysteresis, in Celsius.  Wide enough to stop per-frame chatter
 * from sensor noise, narrow enough not to hide a real crossing. */
#define DYT_ALARM_HYST_DEFAULT 1.0f

typedef enum {
    DYT_ALARM_NONE = 0,   /* nothing tripped */
    DYT_ALARM_HIGH = 1,   /* the hot side is tripped */
    DYT_ALARM_LOW  = 2,   /* the cold side is tripped */
    DYT_ALARM_BOTH = 3    /* both at once (a scene wider than the band) */
} dyt_alarm_state_t;

typedef struct {
    int   enabled;
    float hi;            /* high threshold; trips above this */
    float lo;            /* low threshold;  trips below this */
    float hyst;          /* hysteresis band; <= 0 falls back to the default */
    int   hi_active;     /* latched state, do not set directly */
    int   lo_active;
    float last_hot;      /* last readings fed in; NaN before the first */
    float last_cold;
} dyt_alarm_t;

/* Disabled, no thresholds, default hysteresis. */
void dyt_alarm_init(dyt_alarm_t *a);

/* Arm the alarm.  A non-positive `hyst` uses DYT_ALARM_HYST_DEFAULT.
 *
 * The thresholds are taken as given and the two sides stay independent, so
 * an inverted pair (hi < lo) is *not* repaired here.  Note what it means:
 * since the sides do not interact, an inverted pair trips on nearly every
 * reading — anything above `hi` trips high and anything below `lo` trips
 * low.  That is a configuration error for the caller to catch with
 * dyt_alarm_is_sane() before arming, not something this module papers over. */
void dyt_alarm_set(dyt_alarm_t *a, float lo, float hi, float hyst);

/* Disarm and clear both latches. */
void dyt_alarm_disable(dyt_alarm_t *a);

/* Clear the latches but stay armed (the vendor's "reset alarm"). */
void dyt_alarm_reset(dyt_alarm_t *a);

/* 1 when lo <= hi, i.e. the thresholds can actually be crossed. */
int dyt_alarm_is_sane(const dyt_alarm_t *a);

/* Feed one frame's readings.  `hot` drives the high side and `cold` the low
 * side; pass the same value for both for a single-point alarm.
 *
 * A NaN reading leaves that side's latch exactly as it was: a dropped frame
 * must not clear a standing alarm, and must not invent a new one.
 *
 * Returns the resulting state.  Safe on a disabled alarm (always NONE). */
dyt_alarm_state_t dyt_alarm_update(dyt_alarm_t *a, float hot, float cold);

/* The state as of the last update. */
dyt_alarm_state_t dyt_alarm_state(const dyt_alarm_t *a);

/* 1 when either side is tripped. */
int dyt_alarm_tripped(const dyt_alarm_t *a);

/* A stable, short label for a status line: "none", "high", "low", "high+low". */
const char *dyt_alarm_name(dyt_alarm_state_t s);

#ifdef __cplusplus
}
#endif

#endif /* DYT_ALARM_H */
