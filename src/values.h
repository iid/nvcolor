/* values.h - slider <-> display-state conversion, and value formatting.
 *
 * Extracted from nvcolor.c so that the host-side test harness can link it.
 *
 * Why this exists: slider_value(), slider_set(), typed_set() and the two
 * formatters used to be static inside nvcolor.c, so tests/run-tests.sh could
 * not compile them. The suite therefore re-implemented the arithmetic in
 * gamma_test.c and checked its own copies - which is how the contrast readout
 * shipped as "100%" at its default, contradicting the README, without a single
 * test failing. One of those copies was even tautological: a check of
 * `50 + ((mn - mn) * ...)`, where the parenthesised term is identically zero,
 * so it could never fail whatever the formula did.
 *
 * Nothing here touches Win32 beyond lstrcpynW, which the shim supplies, so the
 * whole module still builds and runs on a non-Windows host.
 */
#ifndef VALUES_H
#define VALUES_H

#include "state.h"

/* Order mirrors the "Adjust desktop color settings" page in the real NVIDIA
 * Control Panel - Brightness, Contrast, Gamma, then a break, then Digital
 * vibrance and Hue - so muscle memory carries over from the panel people
 * already know. Temperature is ours alone, since NVIDIA's page has no such
 * control, and sits with the other three GDI ramp sliders.
 *
 * The INI persists by key name rather than by index, so this ordering cannot
 * corrupt an existing settings.ini. */
typedef enum {
    SL_BRIGHTNESS = 0,
    SL_CONTRAST,
    SL_GAMMA,
    SL_TEMPERATURE,
    SL_VIBRANCE,
    SL_HUE,
    SL_COUNT
} SliderId;

typedef struct {
    const wchar_t *label;
    int step;    /* quantisation for drag / wheel */
    int bipolar; /* 1 = centre of track is the neutral point */
} SliderDef;

/* Must stay in the same order as SliderId: indexed by it. */
extern const SliderDef g_sliders[SL_COUNT];

/* Current slider position in 0..1000 internal units. Always returns a value in
 * range, even if dvc came back from the driver outside its own min..max. */
int  slider_value(int idx, const DisplayState *d);

/* The neutral position for a slider, in the same 0..1000 units. */
int  slider_default(int idx);

/* Set from 0..1000 internal units. */
void slider_set(int idx, DisplayState *d, int v);

/* Quantisation for drag and wheel, so a step always changes the value. */
int  slider_step(int idx, const DisplayState *d);

int  slider_enabled(int idx, const DisplayState *d);

/* Where the "unmodified" point sits on the track, in 0..1000 units. */
int  slider_neutral(int idx);

/* The readout shown on the row: "50%", "1.15", "-15", "neutral", "n/a". */
void format_value(int idx, const DisplayState *d, wchar_t *out, int outLen);

/* The editable form of the same number: no unit suffix, and temperature reads
 * "0" rather than "neutral", so what appears when the field opens is something
 * that can be typed straight back in. */
void format_editable(int idx, const DisplayState *d, wchar_t *out, int outLen);

/* Parse a user-typed value in the units format_editable() displays. Returns 1
 * on success, 0 on anything it will not accept - including a trailing garbage
 * character, so "12abc" and "1.2.3" are refused rather than quietly reading as
 * 12 and 1.2. NaN and infinity are refused explicitly. */
int  parse_number(const wchar_t *s, double *out);

/* Write a typed value straight into the display units it was typed in.
 * Deliberately bypasses slider_set(), whose lossy integer divisors made a typed
 * hue of 210 come back as 209. Out-of-range input is clamped, not refused. */
void typed_set(int idx, DisplayState *d, double v);

#endif /* VALUES_H */
