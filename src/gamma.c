/* gamma.c - gamma ramp construction and application. */
#include "gamma.h"
#include <math.h>
#include <string.h>

/* Slider units -> real factors.
 *
 * brightness : 0..1000  -> offset  -0.25 .. +0.25
 * contrast   : 0..1000  -> scale    0.50 .. 1.50
 * gamma      : 0..1000  -> exponent 0.50 .. 1.50 (1.0 = neutral)
 */
static double u_gamma(int units)      { return 0.50 + (double)units / 1000.0; }
static double u_contrast(int units)  { return 0.50 + (double)units / 1000.0; }
static double u_brightness(int units) { return ((double)units / 1000.0 - 0.5) * 0.5; }

double gamma_factor(int units) { return u_gamma(units); }
double gamma_contrast_factor(int units) { return u_contrast(units); }

/* Channel gains for colour temperature. t is -0.5 (cool) .. +0.5 (warm).
 * Multiplicative so black stays black and only mid/high tones shift.
 * The clamp is a safety net: a negative gain would zero two channels and
 * saturate the third, which SetDeviceGammaRamp would happily accept as a
 * valid (if solid) ramp. */
static void temp_gains(int temperature, double *r, double *g, double *b)
{
    double t = (double)temperature / 1000.0;
    *r = 1.0 + t * 0.20;
    *g = 1.0 + t * 0.02;
    *b = 1.0 - t * 0.20;

    if (*r < 0.0) *r = 0.0;
    if (*g < 0.0) *g = 0.0;
    if (*b < 0.0) *b = 0.0;
    if (*r > 4.0) *r = 4.0;
    if (*g > 4.0) *g = 4.0;
    if (*b > 4.0) *b = 4.0;
}

void gamma_defaults(GammaParams *p)
{
    p->brightness  = 500;
    p->contrast    = 500;
    p->gamma       = 500;
    p->temperature = 0;
}

int gamma_is_default(const GammaParams *p)
{
    return p->brightness == 500 && p->contrast == 500 &&
           p->gamma == 500 && p->temperature == 0;
}

void gamma_build(const GammaParams *p, WORD out[3][GAMMA_STEPS])
{
    /* Sanitise defensively. Sliders and the INI loader already clamp, but a
     * ramp is the one thing here that can physically ruin someone's display,
     * so gamma_build refuses to trust its input. */
    GammaParams s;
    double gain_r, gain_g, gain_b;
    double contrast, brightness;
    int c, i;

    s.brightness  = (p->brightness  <    0) ?    0 : (p->brightness  > 1000) ? 1000 : p->brightness;
    s.contrast    = (p->contrast    <    0) ?    0 : (p->contrast    > 1000) ? 1000 : p->contrast;
    s.gamma       = (p->gamma       <    0) ?    0 : (p->gamma       > 1000) ? 1000 : p->gamma;
    s.temperature = (p->temperature < -500) ? -500 : (p->temperature > 500) ? 500 : p->temperature;

    p = &s;

    temp_gains(p->temperature, &gain_r, &gain_g, &gain_b);
    contrast = u_contrast(p->contrast);
    brightness = u_brightness(p->brightness);

    /* The gamma curve and the contrast/brightness shaping depend only on the
     * ramp index, not on the channel, so they are the same for all three
     * channels. Computing them once here instead of inside the channel loop
     * cuts 768 pow() calls per ramp down to 256. The arithmetic is identical
     * and in the same order, so the output is bit-for-bit what it was before.
     *
     * Worth doing because it costs nothing, but do not expect it to be felt:
     * measured on the target machine this is 0.0149 ms -> 0.0052 ms per ramp,
     * about 1 ms of CPU per second of dragging. The ramp maths is not the
     * bottleneck; SetDeviceGammaRamp is. */
    {
        double shaped[GAMMA_STEPS];

        for (i = 0; i < GAMMA_STEPS; ++i) {
            double v = (double)i / (double)(GAMMA_STEPS - 1);

            v = pow(v, u_gamma(p->gamma));
            v = (v - 0.5) * contrast + 0.5;
            shaped[i] = v + brightness;
        }

        for (c = 0; c < 3; ++c) {
            double gain = (c == 0) ? gain_r : (c == 1) ? gain_g : gain_b;
            WORD prev = 0;

            for (i = 0; i < GAMMA_STEPS; ++i) {
                double v = shaped[i] * gain;

                if (v < 0.0) v = 0.0;
                if (v > 1.0) v = 1.0;

                {
                    long w = (long)(v * 65535.0 + 0.5);
                    /* Windows rejects non-monotonic ramps; guarantee it. */
                    if (w < (long)prev) w = (long)prev;
                    out[c][i] = (WORD)w;
                    prev = out[c][i];
                }
            }
        }
    }
}

/* Tri-state, and the distinction matters. Returns 1 if the ramp on the device
 * matches these params, 0 if it was readable and genuinely differs (a mode
 * change, display sleep or a fullscreen game reset it), and -1 if
 * GetDeviceGammaRamp failed so the question could not be answered at all
 * (RDP, HDR, some docks).
 *
 * Collapsing the last two into a single 0 made the caller count a game that
 * briefly changed the ramp as a permanent read failure, and after four tries
 * it disabled recovery and reported "gamma ramp is not readable here" - which
 * was not true. */
int gamma_ramp_matches(HDC hdc, const GammaParams *p)
{
    WORD current[3][GAMMA_STEPS];
    WORD want[3][GAMMA_STEPS];
    int c, i;

    if (!GetDeviceGammaRamp(hdc, current))
        return -1;

    gamma_build(p, want);

    for (c = 0; c < 3; ++c) {
        for (i = 0; i < GAMMA_STEPS; ++i) {
            int diff = (int)current[c][i] - (int)want[c][i];
            if (diff < 0) diff = -diff;
            if (diff > 256) /* ~0.4% of full scale */
                return 0;
        }
    }
    return 1;
}

int gamma_apply(HDC hdc, const GammaParams *p)
{
    WORD ramp[3][GAMMA_STEPS];

    if (!hdc)
        return 0;

    gamma_build(p, ramp);
    return SetDeviceGammaRamp(hdc, ramp) ? 1 : 0;
}

int gamma_reset(HDC hdc)
{
    WORD ramp[3][GAMMA_STEPS];
    int c, i;

    if (!hdc)
        return 0;

    for (c = 0; c < 3; ++c)
        for (i = 0; i < GAMMA_STEPS; ++i)
            ramp[c][i] = (WORD)((long)i * 65535L / (GAMMA_STEPS - 1));

    return SetDeviceGammaRamp(hdc, ramp) ? 1 : 0;
}
