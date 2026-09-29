/* tests/values_test.c - unit tests for the real slider conversion and
 * formatting code in src/values.c.
 *
 * These functions used to be static inside src/nvcolor.c, which this harness
 * cannot compile. The suite therefore re-implemented the arithmetic here and
 * checked its own copies, which is how the contrast readout shipped as "100%"
 * at its default while the README claimed something else and every test still
 * passed. One of those copies was tautological to begin with. Now the real
 * functions are linked and a regression in them fails the suite.
 */
#include <stdio.h>
#include <string.h>
#include "values.h"

static int fails = 0;
static void check(int cond, const char *msg)
{
    printf("%s  %s\n", cond ? "ok  " : "FAIL", msg);
    if (!cond) fails++;
}

/* src/gamma.c is linked in for gamma_factor()/gamma_contrast_factor(), which
 * are pure maths, but the file as a whole also references the two GDI ramp
 * entry points. Nothing here calls them - these exist only to satisfy the
 * linker. gamma_test.c has its own, stateful, fake device for the ramp tests. */
int GetDeviceGammaRamp(HDC h, void *r) { (void)h; (void)r; return TRUE; }
int SetDeviceGammaRamp(HDC h, void *r) { (void)h; (void)r; return TRUE; }

/* The shim declares this because src/values.c formats wide readouts into
 * caller-supplied buffers; here is the host implementation. Bounded copy,
 * always NUL-terminates, matching the documented Win32 behaviour. */
int lstrcpynW(wchar_t *dst, const wchar_t *src, int cch)
{
    int i = 0;
    if (!dst || cch <= 0)
        return 0;
    if (src)
        for (; i < cch - 1 && src[i]; ++i)
            dst[i] = src[i];
    dst[i] = L'\0';
    return i;
}

static void init(DisplayState *d, int dvc_min, int dvc_max)
{
    memset(d, 0, sizeof *d);
    d->dvc_min = dvc_min;
    d->dvc_max = dvc_max;
    d->dvc = dvc_min;
    d->dvc_supported = 1;
    d->hue_supported = 1;
    d->hue = 0;
    gamma_defaults(&d->gamma);
}

int main(void)
{
    DisplayState d;
    wchar_t buf[32];
    double v;
    int i;

    printf("slider value conversion\n");
    printf("-----------------------\n");

    /* --- neutral position, and what the readout says for it --------------- */
    {
        init(&d, 0, 63);
        check(slider_default(SL_BRIGHTNESS) == 500, "brightness neutral is 500");
        check(slider_default(SL_CONTRAST) == 500,   "contrast neutral is 500");
        check(slider_default(SL_GAMMA) == 500,      "gamma neutral is 500");
        check(slider_default(SL_TEMPERATURE) == 500,"temperature neutral is 500");
        check(slider_default(SL_VIBRANCE) == 0,     "vibrance neutral is 0 (== dvc_min)");
        check(slider_default(SL_HUE) == 0,          "hue neutral is 0 degrees");
    }

    /* --- the bug this suite could not previously catch -------------------- */
    {
        init(&d, 0, 63);

        /* Contrast at its default must not read as "100%", which reads like
         * maximum contrast rather than "no change at all". */
        format_value(SL_CONTRAST, &d, buf, 32);
        check(wcscmp(buf, L"50%") == 0, "contrast reads 50% at default, not 100%");
        format_value(SL_BRIGHTNESS, &d, buf, 32);
        check(wcscmp(buf, L"50%") == 0, "brightness reads 50% at default");

        d.gamma.contrast = 0;
        format_value(SL_CONTRAST, &d, buf, 32);
        check(wcscmp(buf, L"0%") == 0, "contrast reads 0% at its flattest");
        d.gamma.contrast = 1000;
        format_value(SL_CONTRAST, &d, buf, 32);
        check(wcscmp(buf, L"100%") == 0, "contrast reads 100% at its strongest");

        /* Temperature is the one signed slider, and has no NVIDIA equivalent. */
        format_value(SL_TEMPERATURE, &d, buf, 32);
        check(wcscmp(buf, L"neutral") == 0, "temperature reads 'neutral' at default");
        d.gamma.temperature = -150;
        format_value(SL_TEMPERATURE, &d, buf, 32);
        check(wcscmp(buf, L"-15") == 0, "temperature reads -15 when cool");
        format_value(SL_HUE, &d, buf, 32);
        check(wcscmp(buf, L"0\u00b0") == 0, "hue reads 0 degrees at default");
    }

    /* --- vibrance readout across driver ranges --------------------------- */
    {
        struct { int mn, mx; } ranges[] = { {0,100},{10,110},{-50,50},{0,63} };
        for (i = 0; i < 4; ++i) {
            int mn = ranges[i].mn, mx = ranges[i].mx;
            char msg[128];

            init(&d, mn, mx);
            format_value(SL_VIBRANCE, &d, buf, 32);
            snprintf(msg, sizeof msg, "vibrance reads 50%% at range min %d..%d", mn, mx);
            check(wcscmp(buf, L"50%") == 0, msg);

            d.dvc = mx;
            format_value(SL_VIBRANCE, &d, buf, 32);
            snprintf(msg, sizeof msg, "vibrance reads 100%% at range max %d..%d", mn, mx);
            check(wcscmp(buf, L"100%") == 0, msg);
        }
    }

    /* --- an out-of-range driver level must not escape 0..1000 ------------- */
    {
        init(&d, 0, 63);
        d.dvc = 70;                        /* above the driver's own maxLevel */
        check(slider_value(SL_VIBRANCE, &d) <= 1000,
              "out-of-range dvc clamps inside 0..1000");
        format_value(SL_VIBRANCE, &d, buf, 32);
        check(wcscmp(buf, L"100%") == 0, "out-of-range dvc still reads 100%, not 106%");

        d.dvc = -5;                        /* below its own minLevel */
        check(slider_value(SL_VIBRANCE, &d) >= 0, "below-range dvc clamps inside 0..1000");
        format_value(SL_VIBRANCE, &d, buf, 32);
        check(wcscmp(buf, L"50%") == 0, "below-range dvc reads 50%, not off the track");
    }

    /* --- a degenerate range is not silently "enabled" --------------------- */
    {
        init(&d, 10, 10);                   /* max == min: nothing to control */
        check(slider_value(SL_VIBRANCE, &d) == 0, "degenerate range yields 0, not a divide by zero");
        format_value(SL_VIBRANCE, &d, buf, 32);
        check(wcscmp(buf, L"n/a") == 0, "degenerate range reads n/a");
        check(slider_step(SL_VIBRANCE, &d) == 1000, "degenerate range has no usable step");

        init(&d, 20, 10);                   /* inverted */
        check(slider_value(SL_VIBRANCE, &d) == 0, "inverted range yields 0, not garbage");
    }

    /* --- hue is monotonic and its endpoints exact ------------------------- */
    {
        int prev = -1, mono = 1;
        init(&d, 0, 100);
        for (i = 0; i <= 1000; i += 5) {
            slider_set(SL_HUE, &d, i);
            {
                int u = slider_value(SL_HUE, &d);
                if (u < prev) mono = 0;
                prev = u;
            }
        }
        check(mono, "hue is monotonic across the whole track");

        init(&d, 0, 100);
        slider_set(SL_HUE, &d, 0);
        check(d.hue == 0, "hue at slider 0 is 0 degrees");
        slider_set(SL_HUE, &d, 1000);
        check(d.hue == 359, "hue at slider 1000 is 359 degrees");
        check(slider_value(SL_HUE, &d) == 1000, "hue max round-trips exactly");
    }

    /* --- parse_number: refuse rather than guess --------------------------- */
    {
        check(parse_number(L"50", &v) == 1 && v == 50.0,  "parses a plain integer");
        check(parse_number(L"-15", &v) == 1 && v == -15.0,"parses a negative");
        check(parse_number(L"+7", &v) == 1 && v == 7.0,   "parses an explicit plus");
        check(parse_number(L"1.15", &v) == 1 && v == 1.15, "parses a decimal");
        check(parse_number(L"1.", &v) == 1 && v == 1.0,   "parses a half-typed decimal");
        check(parse_number(L"", &v) == 0,    "refuses an empty string");
        check(parse_number(L"12abc", &v) == 0,"refuses trailing garbage");
        check(parse_number(L"1.2.3", &v) == 0,"refuses a second decimal point");
        check(parse_number(L"-", &v) == 0,   "refuses a lone sign");
        check(parse_number(L"nan", &v) == 0,  "refuses NaN");
        check(parse_number(L"inf", &v) == 0,  "refuses infinity");
    }

    /* --- typed_set: out of range clamps, in range is exact ---------------- */
    {
        init(&d, 0, 63);

        typed_set(SL_HUE, &d, 210.0);
        format_value(SL_HUE, &d, buf, 32);
        check(wcscmp(buf, L"210\u00b0") == 0, "typed hue 210 reads back 210, not 209");

        typed_set(SL_HUE, &d, 400.0);
        check(d.hue == 359, "typed hue 400 clamps to 359");
        typed_set(SL_HUE, &d, -20.0);
        check(d.hue == 0,   "typed hue -20 clamps to 0");

        typed_set(SL_BRIGHTNESS, &d, 70.0);
        format_value(SL_BRIGHTNESS, &d, buf, 32);
        check(wcscmp(buf, L"70%") == 0, "typed brightness 70 reads back 70%");
        typed_set(SL_BRIGHTNESS, &d, 400.0);
        check(d.gamma.brightness == 1000, "typed brightness 400 clamps to the top");

        typed_set(SL_CONTRAST, &d, 100.0);
        format_value(SL_CONTRAST, &d, buf, 32);
        check(wcscmp(buf, L"100%") == 0, "typed contrast 100 reads back 100%");
        typed_set(SL_CONTRAST, &d, -50.0);
        check(d.gamma.contrast == 0, "typed contrast -50 clamps to the bottom");

        typed_set(SL_GAMMA, &d, 1.20);
        format_value(SL_GAMMA, &d, buf, 32);
        check(wcscmp(buf, L"1.20") == 0, "typed gamma 1.20 reads back 1.20");

        typed_set(SL_TEMPERATURE, &d, -15.0);
        check(d.gamma.temperature == -150, "typed temperature -15 is -150 internal units");
        typed_set(SL_TEMPERATURE, &d, 90.0);
        check(d.gamma.temperature == 500, "typed temperature 90 clamps to +50");
    }

    /* --- what the field opens with must be typeable back in --------------- */
    {
        init(&d, 0, 63);
        for (i = 0; i < SL_COUNT; ++i) {
            char msg[512];
            wchar_t shown[32], retyped[32];
            DisplayState t;

            format_editable(i, &d, shown, 32);
            if (!parse_number(shown, &v)) {
                snprintf(msg, sizeof msg, "slider %d opens with typeable text '%ls'", i, shown);
                check(0, msg);
                continue;
            }
            t = d;
            typed_set(i, &t, v);
            format_editable(i, &t, retyped, 32);
            snprintf(msg, sizeof msg, "slider %d: '%ls' survives a type-and-recommit round trip", i, shown);
            check(wcscmp(shown, retyped) == 0, msg);
        }
    }

    printf("\n%s\n", fails ? "SOME CHECKS FAILED" : "ALL VALUE CHECKS PASSED");
    return fails;
}
