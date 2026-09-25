/*
 * units.h — temperature units for the DYT thermal camera Linux port.
 *
 * The vendor app exposes a °C / °F / K selector (RE Docs 06 §3.2:
 * `DYConstants.tempUnit = {"°C", "℉", "K"}`), stores the choice under the
 * `temp_unit` preference, and re-renders every displayed value through it.
 * The port needs the same conversion in two places — the viewer's readouts
 * and (later) the DYT image metadata — so it lives here rather than being
 * open-coded in each front-end.
 *
 * Everything in this module is pure: no camera, no GUI, no image library.
 * That is what makes it testable without hardware (units_test.c).
 *
 * **Suffix vs label.** dyt_unit_suffix() returns ASCII ("C"/"F"/"K") and is
 * what dyt_temp_format() emits, because the strings it produces end up in
 * terminals and in OpenCV's putText(), neither of which renders the vendor's
 * "°C"/"℉" reliably. dyt_unit_label() returns the vendor's display form for
 * front-ends that can draw UTF-8 (the Qt6 app will).
 *
 * build:  cc -O2 -g -Wall -Wextra -ffp-contract=off -I. -c units.c
 */
#ifndef DYT_UNITS_H
#define DYT_UNITS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DYT_UNIT_C = 0,   /* degrees Celsius  — the pipeline's native unit */
    DYT_UNIT_F,       /* degrees Fahrenheit */
    DYT_UNIT_K,       /* kelvin */
    DYT_UNIT_N        /* count / sentinel — not a valid unit */
} dyt_unit_t;

/* Convert a Celsius temperature (what the pipeline produces) into `u`.
 * Returns the input unchanged if `u` is out of range, so a bad enum can
 * never produce a garbage reading.  NaN propagates. */
float dyt_temp_convert(dyt_unit_t u, float celsius);

/* The inverse: convert a temperature expressed in `u` back to Celsius.
 * Used by the parameter-entry paths, which take user input in display
 * units but must send Kelvin-scaled values to the device. */
float dyt_temp_to_celsius(dyt_unit_t u, float value);

/* ASCII suffix: "C", "F", "K".  NULL if `u` is out of range. */
const char *dyt_unit_suffix(dyt_unit_t u);

/* Vendor display form: "°C", "℉", "K" (UTF-8).  NULL if out of range. */
const char *dyt_unit_label(dyt_unit_t u);

/* Full English name: "Celsius", "Fahrenheit", "Kelvin".  NULL if out of
 * range.  For settings dialogs and --help text. */
const char *dyt_unit_name(dyt_unit_t u);

/* Parse a unit selector.  Case-insensitive; accepts the ASCII forms
 * ("c", "C", "celsius", "fahrenheit", "kelvin") and a leading degree sign
 * ("°C" -> Celsius), which is skipped.  Returns the dyt_unit_t, or -1 if
 * the string is not a unit.  NULL and "" return -1. */
int dyt_unit_parse(const char *s);

/* The next unit in the app's cycle order (C -> F -> K -> C).  Out-of-range
 * input returns DYT_UNIT_C.  Backs the viewer's "u" key. */
dyt_unit_t dyt_unit_next(dyt_unit_t u);

/* Format a Celsius temperature in `u` into `buf`, e.g. "31.2 C".
 * Returns the number of characters written, excluding the NUL; -1 on a
 * bad argument, an out-of-range unit, or truncation (n too small).
 * NaN renders as "nan C" rather than being suppressed — the caller
 * decides whether a non-physical pixel should be shown at all. */
int dyt_temp_format(dyt_unit_t u, float celsius, char *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* DYT_UNITS_H */
