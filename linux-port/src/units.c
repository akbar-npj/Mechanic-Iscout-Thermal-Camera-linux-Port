/*
 * units.c — temperature unit conversion, parsing and formatting.
 *
 * See units.h for why this is a library module and why the ASCII suffix and
 * the UTF-8 label are separate functions.  Pure: no camera, no GUI.
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -I. -c units.c -o units.o
 */
#include <ctype.h>
#include <stdio.h>

#include "units.h"

/* --------------------------------------------------------------- tables */

static const char *const suffixes[DYT_UNIT_N] = { "C", "F", "K" };
static const char *const labels[DYT_UNIT_N]   = { "\xc2\xb0" "C",   /* °C */
                                                  "\xe2\x84\x89",   /* ℉ */
                                                  "K" };
static const char *const names[DYT_UNIT_N]    = { "Celsius",
                                                  "Fahrenheit",
                                                  "Kelvin" };

static int valid(dyt_unit_t u)
{
    /* Cast through int: comparing an enum against 0 directly can trip
     * -Wtype-limits, since all enumerators here are non-negative. */
    int i = (int)u;
    return i >= 0 && i < DYT_UNIT_N;
}

/* ------------------------------------------------------------ conversion */

float dyt_temp_convert(dyt_unit_t u, float celsius)
{
    switch (u) {
      case DYT_UNIT_C: return celsius;
      case DYT_UNIT_F: return celsius * 9.0f / 5.0f + 32.0f;
      case DYT_UNIT_K: return celsius + 273.15f;
      default:         return celsius;   /* out of range: pass through */
    }
}

float dyt_temp_to_celsius(dyt_unit_t u, float value)
{
    switch (u) {
      case DYT_UNIT_C: return value;
      case DYT_UNIT_F: return (value - 32.0f) * 5.0f / 9.0f;
      case DYT_UNIT_K: return value - 273.15f;
      default:         return value;
    }
}

/* --------------------------------------------------------------- naming */

const char *dyt_unit_suffix(dyt_unit_t u)
{
    return valid(u) ? suffixes[(int)u] : NULL;
}

const char *dyt_unit_label(dyt_unit_t u)
{
    return valid(u) ? labels[(int)u] : NULL;
}

const char *dyt_unit_name(dyt_unit_t u)
{
    return valid(u) ? names[(int)u] : NULL;
}

/* -------------------------------------------------------------- parsing */

static int eq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

int dyt_unit_parse(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    char c;

    if (!s)
        return -1;

    /* Skip a leading degree sign so "°C" parses.  This drops every leading
     * byte >= 0x80, which covers "°C"/"°F" but not the single-codepoint
     * forms "℃"/"℉" (those have no trailing ASCII letter) — acceptable,
     * since the CLI passes "C"/"F"/"K" and a GUI has its own selector. */
    while (*p >= 0x80)
        p++;

    c = (char)tolower(*p);
    if (c == '\0')
        return -1;

    if (c == 'c' && (p[1] == '\0' || eq_ci((const char *)p, "celsius")))
        return DYT_UNIT_C;
    if (c == 'f' && (p[1] == '\0' || eq_ci((const char *)p, "fahrenheit")))
        return DYT_UNIT_F;
    if (c == 'k' && (p[1] == '\0' || eq_ci((const char *)p, "kelvin")))
        return DYT_UNIT_K;
    return -1;
}

dyt_unit_t dyt_unit_next(dyt_unit_t u)
{
    if (!valid(u))
        return DYT_UNIT_C;
    return (dyt_unit_t)(((int)u + 1) % DYT_UNIT_N);
}

/* ------------------------------------------------------------ formatting */

int dyt_temp_format(dyt_unit_t u, float celsius, char *buf, size_t n)
{
    int w;

    if (!buf || n == 0 || !valid(u))
        return -1;

    w = snprintf(buf, n, "%.1f %s",
                 (double)dyt_temp_convert(u, celsius), dyt_unit_suffix(u));
    if (w < 0 || (size_t)w >= n)
        return -1;   /* truncated — better to report failure than lie */
    return w;
}
