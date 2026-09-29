/* values.c - slider <-> display-state conversion, and value formatting.
 *
 * See values.h for why this lives outside nvcolor.c: so the host-side tests
 * link the real functions instead of re-implementing the arithmetic.
 */
#include "values.h"
#include "gamma.h"
#include "nvfmt.h"
#include <stdlib.h>
#include <math.h>
#include <wchar.h>

const SliderDef g_sliders[SL_COUNT] = {
    { L"Brightness",  5, 1 },
    { L"Contrast",    5, 1 },
    { L"Gamma",       5, 1 },
    { L"Temperature", 5, 1 },
    { L"Vibrance",    5, 0 },
    { L"Hue",         5, 0 },
};

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

int slider_value(int idx, const DisplayState *d)
{
    switch (idx) {
    case SL_VIBRANCE: {
        /* Clamped and widened deliberately. An out-of-range dvc (a driver
         * reporting currentLevel outside its own min..max) produced a value
         * above 1000, which drew the knob past the end of the track and
         * printed a percentage over 100. The multiply is done in long because
         * the range comes from the driver unvalidated and (dvc - min) * 1000
         * overflows int for any range wider than about 2M. */
        int lo = d->dvc_min, hi = d->dvc_max, v = d->dvc, units;
        long range = (long)hi - (long)lo;

        if (range <= 0)
            return 0;
        if (v < lo) v = lo;
        else if (v > hi) v = hi;
        units = (int)(((long)(v - lo) * 1000L) / range);
        if (units < 0) units = 0;
        if (units > 1000) units = 1000;
        return units;
    }
    case SL_HUE:
        return d->hue * 1000 / 359;
    case SL_BRIGHTNESS:  return d->gamma.brightness;
    case SL_CONTRAST:    return d->gamma.contrast;
    case SL_GAMMA:       return d->gamma.gamma;
    case SL_TEMPERATURE: return d->gamma.temperature + 500;
    default: return 0;
    }
}

int slider_default(int idx)
{
    switch (idx) {
    case SL_VIBRANCE:    return 0;   /* neutral == dvc_min */
    case SL_HUE:         return 0;   /* neutral == 0 degrees */
    case SL_BRIGHTNESS:  return 500;
    case SL_CONTRAST:    return 500;
    case SL_GAMMA:       return 500;
    case SL_TEMPERATURE: return 500;
    default: return 500;
    }
}

void slider_set(int idx, DisplayState *d, int v)
{
    v = clampi(v, 0, 1000);
    switch (idx) {
    case SL_VIBRANCE:
        if (d->dvc_max > d->dvc_min)
            d->dvc = d->dvc_min + (int)((long)v * (d->dvc_max - d->dvc_min) / 1000);
        break;
    case SL_HUE:
        d->hue = v * 359 / 1000;
        if (d->hue < 0) d->hue = 0;
        if (d->hue > 359) d->hue = 359;
        break;
    case SL_BRIGHTNESS:  d->gamma.brightness  = v; break;
    case SL_CONTRAST:    d->gamma.contrast    = v; break;
    case SL_GAMMA:       d->gamma.gamma       = v; break;
    case SL_TEMPERATURE: d->gamma.temperature = v - 500; break;
    default: break;
    }
}

/* A fixed step of 5 out of 1000 quantises to far more positions than a
 * display with a 0..10 DVC range can represent, so every other drag step
 * became a no-op that still pushed a full state_apply. */
int slider_step(int idx, const DisplayState *d)
{
    if (idx == SL_VIBRANCE) {
        int range = d->dvc_max - d->dvc_min + 1;
        int step;
        if (range <= 1)
            return 1000;
        step = 1000 / range;
        if (step < 5) step = 5;
        return step;
    }
    return g_sliders[idx].step;
}

int slider_enabled(int idx, const DisplayState *d)
{
    if (idx == SL_VIBRANCE) return d->dvc_supported;
    if (idx == SL_HUE)      return d->hue_supported;
    return 1;
}

int slider_neutral(int idx)
{
    if (!g_sliders[idx].bipolar)
        return slider_default(idx);
    return 500;
}

void format_value(int idx, const DisplayState *d, wchar_t *out, int outLen)
{
    wchar_t buf[32];
    if (!out || outLen <= 0)
        return;
    buf[0] = L'\0';

    switch (idx) {
    case SL_VIBRANCE:
        if (d->dvc_max > d->dvc_min)
            NV_SNWPRINTF(buf, 32, L"%d%%", 50 + slider_value(SL_VIBRANCE, d) / 20);
        else
            lstrcpynW(buf, L"n/a", 32);
        break;
    case SL_HUE:
        NV_SNWPRINTF(buf, 32, L"%d\u00b0", d->hue);
        break;
    case SL_BRIGHTNESS:
        /* Percent, as the NVIDIA Control Panel shows it, with 50% neutral
         * rather than a signed -50..+50 offset. */
        NV_SNWPRINTF(buf, 32, L"%d%%", 50 + (d->gamma.brightness - 500) / 10);
        break;
    case SL_CONTRAST:
        /* Percent, on the same 0..100 scale as Brightness with 50% neutral.
         * The internal multiplier is 0.50 + units/1000, so units/10 is
         * exactly 0%, 50%, 100% at the flattest, normal and strongest
         * settings. Showing neutral as "100%" was actively misleading - it
         * reads like maximum contrast, as in a high-contrast accessibility
         * mode, when it actually means "no change at all". */
        NV_SNWPRINTF(buf, 32, L"%d%%", d->gamma.contrast / 10);
        break;
    case SL_GAMMA:
        NV_SNWPRINTF(buf, 32, L"%.2f", gamma_factor(d->gamma.gamma));
        break;
    case SL_TEMPERATURE:
        if (d->gamma.temperature == 0) lstrcpynW(buf, L"neutral", 32);
        else NV_SNWPRINTF(buf, 32, L"%+d", d->gamma.temperature / 10);
        break;
    default:
        lstrcpynW(buf, L"-", 32);
        break;
    }

    buf[31] = L'\0';
    lstrcpynW(out, buf, outLen);
    out[outLen - 1] = L'\0';
}

void format_editable(int idx, const DisplayState *d, wchar_t *out, int outLen)
{
    wchar_t buf[32];
    if (!out || outLen <= 0)
        return;
    buf[0] = L'\0';

    switch (idx) {
    case SL_VIBRANCE:
        if (d->dvc_max > d->dvc_min)
            NV_SNWPRINTF(buf, 32, L"%d", 50 + slider_value(SL_VIBRANCE, d) / 20);
        else
            lstrcpynW(buf, L"0", 32);
        break;
    case SL_HUE:
        NV_SNWPRINTF(buf, 32, L"%d", d->hue);
        break;
    case SL_BRIGHTNESS:
        NV_SNWPRINTF(buf, 32, L"%d", 50 + (d->gamma.brightness - 500) / 10);
        break;
    case SL_CONTRAST:
        NV_SNWPRINTF(buf, 32, L"%d", d->gamma.contrast / 10);
        break;
    case SL_GAMMA:
        NV_SNWPRINTF(buf, 32, L"%.2f", gamma_factor(d->gamma.gamma));
        break;
    case SL_TEMPERATURE:
        NV_SNWPRINTF(buf, 32, L"%+d", d->gamma.temperature / 10);
        break;
    default:
        lstrcpynW(buf, L"0", 32);
        break;
    }

    buf[31] = L'\0';
    lstrcpynW(out, buf, outLen);
    out[outLen - 1] = L'\0';
}

/* Accepts what the editor's character filter lets through and nothing else: an
 * optional sign, digits, at most one decimal point. The whole string has to be
 * consumed, so "12abc" and "1.2.3" are refused rather than quietly reading as
 * 12 and 1.2. strtod() is locale-sensitive, but this program never calls
 * setlocale(), so the decimal point is always '.' and a half-typed "1." is
 * still valid.
 *
 * NaN and infinity are refused explicitly: strtod("nan") returns a double
 * with no error, and NaN would sail through every clamp comparison below
 * because it is neither less than nor greater than anything. */
int parse_number(const wchar_t *s, double *out)
{
    char buf[32];
    char *end;
    double v;
    int i, n = 0;

    if (!s || !out)
        return 0;

    for (i = 0; s[i] && n < (int)sizeof(buf) - 1; ++i) {
        if (s[i] > 0x7F)
            return 0;
        buf[n++] = (char)s[i];
    }
    if (s[i] != L'\0')        /* longer than the buffer: refuse, do not truncate */
        return 0;
    buf[n] = '\0';

    v = strtod(buf, &end);
    if (end == buf)
        return 0;
    while (*end == ' ' || *end == '\t')
        ++end;
    if (*end != '\0')
        return 0;

    if (v != v)                                    /* NaN */
        return 0;
    if (v < -1000000.0 || v > 1000000.0)          /* also catches +/-inf */
        return 0;

    *out = v;
    return 1;
}

void typed_set(int idx, DisplayState *d, double v)
{
    switch (idx) {
    case SL_VIBRANCE: {
        int lo = d->dvc_min, hi = d->dvc_max, want, best, bestErr = 0, i;

        if (hi <= lo)
            break;

        /* An absurd range means a corrupt INI rather than a driver report.
         * Cap what gets scanned: the loop below is bounded by this, and it
         * also keeps (i - lo) * 1000 inside an int. */
        if (hi - lo > 4096)
            hi = lo + 4096;

        /* The readout is 50 + slider_value/20, and slider_value() inverted
         * through slider_set() can land a whole percent out when the driver
         * range is not a round number: on a 0..7 range, percent 66 comes back
         * as 64. Scanning the driver range for the level whose readout is
         * closest to what was typed makes the round trip exact whenever the
         * typed value is one this display can actually show, and this runs
         * only on commit. */
        want = (int)lround(v);
        best = lo;
        for (i = lo; i <= hi; ++i) {
            int pct = 50 + ((i - lo) * 1000 / (hi - lo)) / 20;
            int err = pct > want ? pct - want : want - pct;
            if (i == lo || err < bestErr) {
                best = i;
                bestErr = err;
            }
        }
        d->dvc = best;
        break;
    }
    case SL_HUE:
        d->hue = clampi((int)lround(v), 0, 359);
        break;
    case SL_BRIGHTNESS:
        /* Typed as a percent, so invert the readout: percent 50 is the neutral
         * offset, and each percent point is 10 internal units. */
        d->gamma.brightness = clampi((int)lround((v - 50.0) * 10.0) + 500, 0, 1000);
        break;
    case SL_CONTRAST:
        /* Typed as a percent on the same 0..100 scale as the readout, 50%
         * neutral, so each percent point is 10 internal units. */
        d->gamma.contrast = clampi((int)lround(v * 10.0), 0, 1000);
        break;
    case SL_GAMMA:
        d->gamma.gamma = clampi((int)lround((v - 0.5) * 1000.0), 0, 1000);
        break;
    case SL_TEMPERATURE:
        d->gamma.temperature = clampi((int)lround(v * 10.0), -500, 500);
        break;
    default:
        break;
    }
}
