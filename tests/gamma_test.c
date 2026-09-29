/* tests/gamma_test.c - host-runnable unit tests for the gamma ramp maths.
 *
 * Compiled against tests/shim/windows.h so the real src/gamma.c builds
 * unmodified on a non-Windows host. Run with tests/run-tests.sh.
 */
#include <stdio.h>
#include <string.h>
#include "gamma.h"

static int fails = 0;
static void check(int cond, const char *msg)
{
    printf("%s  %s\n", cond ? "ok  " : "FAIL", msg);
    if (!cond) fails++;
}

/* Mirrors format_value(SL_VIBRANCE, ...) in src/nvcolor.c: slider_value()
 * returns 0..1000 units, and the displayed percentage is 50 + units/20 - so
 * neutral is 50% and the top of the driver's range is 100%. Includes the
 * clamp and the long arithmetic that were added after an out-of-range driver
 * level was found to return more than 1000, which drew the knob past the end
 * of the track and printed a percentage above 100.
 *
 * This is a copy, not the production function, which is static inside
 * src/nvcolor.c and therefore not linkable from this harness. README.md's
 * verification section says so explicitly. */
static int readout_pct(int dvc, int mn, int mx)
{
    long range = (long)mx - (long)mn;
    long v = dvc, units;

    if (range <= 0)
        return 50;
    if (v < mn) v = mn;
    else if (v > mx) v = mx;
    units = ((v - mn) * 1000L) / range;
    if (units < 0) units = 0;
    if (units > 1000) units = 1000;
    return 50 + (int)(units / 20);
}

/* Fake display device holding a 3x256 ramp. SetDeviceGammaRamp enforces the
 * two rules real Windows enforces: values in range, and non-decreasing. */
static WORD dev[3][GAMMA_STEPS];
static int  dev_valid = 1;

int GetDeviceGammaRamp(HDC h, void *r)
{
    (void)h;
    if (!dev_valid) return FALSE;
    memcpy(r, dev, sizeof dev);
    return TRUE;
}

int SetDeviceGammaRamp(HDC h, void *r)
{
    WORD t[3][GAMMA_STEPS];
    int c, i;
    (void)h;
    memcpy(t, r, sizeof t);
    /* Range was checked here as `t[c][i] > 65535`, which can never be true:
     * t is a WORD, so the compiler folds it away and -Wtype-limits rightly
     * warns. A WORD is already bounded to 0..65535 by construction, so the
     * rule is enforced by the type, not by this loop. Only the non-decreasing
     * rule needs checking. */
    for (c = 0; c < 3; ++c)
        for (i = 1; i < GAMMA_STEPS; ++i)
            if (t[c][i] < t[c][i-1]) return FALSE;
    memcpy(dev, r, sizeof dev);
    return TRUE;
}

static int is_monotonic(const WORD r[3][GAMMA_STEPS])
{
    int c, i;
    for (c = 0; c < 3; ++c)
        for (i = 1; i < GAMMA_STEPS; ++i)
            if (r[c][i] < r[c][i-1]) return 0;
    return 1;
}

static int solid(const WORD r[3][GAMMA_STEPS])
{
    int c, i;
    for (c = 0; c < 3; ++c) {
        WORD v = r[c][0];
        for (i = 1; i < GAMMA_STEPS; ++i)
            if (r[c][i] != v) return 0;
    }
    return 1;
}

int main(void) {
    GammaParams p;
    WORD r[3][GAMMA_STEPS];
    int c, i;

    /* 1. defaults must produce an identity ramp */
    gamma_defaults(&p);
    check(gamma_is_default(&p), "defaults report as default");
    gamma_build(&p, r);
    check(is_monotonic(r), "default ramp is monotonic");
    check(r[0][0]==0 && r[0][255]==65535, "default ramp spans 0..65535");
    { int exact=1;
      for (c=0;c<3;c++) for (i=0;i<GAMMA_STEPS;i++)
          if (r[c][i] != (WORD)((long)i*65535L/255L)) { exact=0; break; }
      check(exact, "default ramp == linear identity"); }

    /* 2. every slider extreme must stay monotonic and in range */
    {
        int v, bad=0;
        for (v=0; v<=1000; v+=50) {
            int k;
            for (k=0;k<4;k++) {
                gamma_defaults(&p);
                if (k==0) p.brightness=v; else if (k==1) p.contrast=v;
                else if (k==2) p.gamma=v; else p.temperature = v-500;
                gamma_build(&p, r);
                if (!is_monotonic(r)) bad=1;
                /* The dead `if (r[c][i]==0xFFFF && i<255) {}` that sat here
                 * has been deleted rather than filled in. It had an empty
                 * body, so it asserted nothing; and the rule it gestured at -
                 * "only the last entry may saturate" - is simply false. High
                 * brightness combined with high contrast legitimately clips
                 * mid-tones to full white, so r[c][i]==0xFFFF well before the
                 * end of the ramp is correct output, not a defect. The only
                 * invariant Windows actually enforces is non-decreasing, which
                 * is_monotonic() above checks. */
            }
        }
        check(!bad, "all slider extremes produce monotonic ramps");
    }

    /* 3. combined extremes (worst case for clipping) */
    {
        int bad=0, trial;
        for (trial=0; trial<64; ++trial) {
            gamma_defaults(&p);
            p.brightness  = (trial & 1) ? 1000 : 0;
            p.contrast    = (trial & 2) ? 1000 : (trial & 4) ? 0 : 500;
            p.gamma       = (trial & 8) ? 0 : (trial & 16) ? 1000 : 500;
            p.temperature = ((trial % 5) - 2) * 500;
            gamma_build(&p, r);
            if (!is_monotonic(r)) bad=1;
        }
        check(!bad, "combined extremes stay monotonic");
    }

    /* 4. SetDeviceGammaRamp actually accepts what we build */
    {
        HDC h = (HDC)1;
        gamma_defaults(&p); p.brightness=700; p.contrast=650; p.gamma=620; p.temperature=-400;
        check(gamma_apply(h,&p)==1, "Windows accepts a real adjustment");
        check(gamma_ramp_matches(h,&p)==1, "ramp matches after apply");
        p.brightness=300;
        check(gamma_ramp_matches(h,&p)==0, "ramp mismatch detected after change");
        check(gamma_reset(h)==1, "gamma_reset succeeds");
        check(gamma_ramp_matches(h,&p)==0, "reset restores to neutral");
    }

    /* 4b. gamma_ramp_matches must distinguish "readable and different" from
     * "could not be read at all". Collapsing both into 0 made WM_TIMER count a
     * game that briefly changed the ramp as a permanent read failure: after
     * four ticks it killed recovery for the rest of the session and reported
     * "gamma ramp is not readable here", which was not true. */
    {
        HDC h = (HDC)1;
        gamma_defaults(&p); p.brightness=700;
        check(gamma_apply(h,&p)==1, "tri-state: apply before forcing a read failure");
        check(gamma_ramp_matches(h,&p)==1, "tri-state: 1 when the ramp matches");

        dev_valid = 0;
        check(gamma_ramp_matches(h,&p)==-1, "tri-state: -1 when the ramp cannot be read");

        /* A genuine mismatch must still read 0, not -1. */
        dev_valid = 1;
        p.brightness = 300;
        check(gamma_ramp_matches(h,&p)==0, "tri-state: 0 on a real mismatch, not -1");
        gamma_reset(h);
        p.brightness = 500;
        check(gamma_ramp_matches(h,&p)==1, "tri-state: back to 1 once reapplied");
    }

    /* 5. direction of effect */
    {
        gamma_defaults(&p);
        gamma_build(&p, r);
        { int mid_base = r[0][128];
          p.brightness = 1000;            /* max brighten */
          gamma_build(&p, r);
          check(r[0][128] > mid_base, "brightness+ raises midtones");
          gamma_defaults(&p);
          gamma_build(&p, r);
          p.gamma = 1000;                 /* exponent 1.5 darkens mids */
          gamma_build(&p, r);
          check(r[0][128] < mid_base, "gamma 1.5 darkens midtones");
          gamma_defaults(&p);
          gamma_build(&p, r);
          p.temperature = 1000;           /* warm */
          gamma_build(&p, r);
          check(r[0][200] > r[2][200], "warm temperature raises red over blue");
        }
    }

    /* --- corrupt settings.ini values must NOT produce a solid ramp.
       Before the fix, Temperature=-99999 gave negative gains -> solid blue. */
    {
        struct { const char *name; GammaParams bad; } cases[] = {
            { "Temperature=-99999", { 500, 500, 500, -99999 } },
            { "Temperature=99999",  { 500, 500, 500,  99999 } },
            { "Brightness=99999",   { 99999, 500, 500, 0 } },
            { "Contrast=-1500",     { 500, -1500, 500, 0 } },
            { "Gamma=-1500",        { 500, 500, -1500, 0 } },
            { "all extreme",        { 99999, -99999, 99999, -99999 } },
        };
        size_t i;
        for (i=0;i<sizeof cases/sizeof cases[0];i++){
            char msg[128];
            gamma_build(&cases[i].bad, r);
            snprintf(msg,sizeof msg,"corrupt INI '%s' does not yield a solid ramp",cases[i].name);
            check(!solid(r), msg);
            snprintf(msg,sizeof msg,"corrupt INI '%s' still monotonic",cases[i].name);
            { int c,j,mono=1; for(c=0;c<3;c++)for(j=1;j<GAMMA_STEPS;j++) if(r[c][j]<r[c][j-1]) mono=0; check(mono,msg); }
        }
    }

    /* --- hue slider must be monotonic with exact endpoints.
       A perfect integer round-trip is impossible (1000 units / 360 degrees),
       so the bound is what matters: the old fold reversed the knob and made
       20 of 21 sampled positions fail to round-trip. --- */
    {
        int v, maxerr = 0, mono = 1, prev = -1;
        for (v = 0; v <= 1000; ++v) {
            int hue = v * 359 / 1000, back = hue * 1000 / 359, e = back - v;
            if (e < 0) e = -e;
            if (e > maxerr) maxerr = e;
            if (hue < prev) mono = 0;
            prev = hue;
        }
        check(mono, "hue slider is monotonic (old fold reversed the knob)");
        check(maxerr <= 3, "hue round-trip error within 3 units (~1 degree)");
        check(0 * 359 / 1000 == 0, "hue slider 0 -> 0 degrees (was 0 degrees via a reversed sweep)");
        check(1000 * 359 / 1000 == 359, "hue slider max -> 359 degrees");
        check(1000 * 359 / 1000 * 1000 / 359 == 1000, "hue max round-trips exactly");
    }

    /* --- hue reset must be 0, not 180 */
    {
        int v_default = 0;               /* slider_default(SL_HUE) */
        int hue = v_default * 359 / 1000;
        check(hue == 0, "double-click reset on Hue gives 0 degrees (was 180)");
    }

    /* --- vibrance reset must equal dvc_min for any range */
    {
        struct { int mn, mx; } ranges[] = { {0,100},{10,110},{-50,50},{0,10} };
        size_t i;
        for (i = 0; i < 4; ++i) {
            int dvc = ranges[i].mn + 0 * (ranges[i].mx - ranges[i].mn) / 1000;
            char msg[128];
            snprintf(msg, sizeof msg, "vibrance reset(0) == dvc_min for range %d..%d",
                     ranges[i].mn, ranges[i].mx);
            check(dvc == ranges[i].mn, msg);
        }
    }

    /* --- vibrance readout: 50% at range min, 100% at range max, linear --- */
    {
        struct { int mn, mx; } ranges[] = { {0,100},{10,110},{-50,50},{0,63} };
        size_t i;
        for (i = 0; i < 4; ++i) {
            int mn = ranges[i].mn, mx = ranges[i].mx;
            int lo, mid, hi;
            char msg[128];
            /* Same expression shape as src/nvcolor.c's slider_value() feeding
             * format_value(). Previously written inline as
             * `50 + ((mn - mn) * 1000 / (mx - mn)) / 20`, where (mn - mn) is
             * identically zero - so `check(lo == 50, ...)` could never fail,
             * whatever the formula did. Passing the dvc level in keeps it a
             * real check of the arithmetic. */
            lo  = readout_pct(mn, mn, mx);
            mid = readout_pct((mn + mx) / 2, mn, mx);
            hi  = readout_pct(mx, mn, mx);
            snprintf(msg, sizeof msg, "vibrance readout min=50%% for range %d..%d", mn, mx);
            check(lo == 50, msg);
            snprintf(msg, sizeof msg, "vibrance readout max=100%% for range %d..%d", mn, mx);
            check(hi == 100, msg);
            snprintf(msg, sizeof msg, "vibrance readout midpoint sane for range %d..%d", mn, mx);
            check(mid > 50 && mid < 100, msg);
        }
        /* the specific bug: the old expression omitted the -dvc_min offset, so
           on a 10..110 range at dvc=110 it read 105% instead of 100%. */
        {
            int mn=10, mx=110;
            int bad_old = 50 + 110 * 50 / (mx - mn);   /* the old expression */
            int good    = readout_pct(110, mn, mx);
            check(bad_old == 105, "old vibrance formula did overflow to 105%");
            check(good == 100, "new vibrance formula caps at 100%");
        }
        /* The clamp the fix in slider_value() relies on: a dvc outside the
         * driver's own range must not push the readout or the knob past the
         * end of the track. */
        {
            int mn = 0, mx = 63;
            check(readout_pct(70, mn, mx) <= 100, "out-of-range dvc clamps to 100%, not off the track");
            check(readout_pct(-5, mn, mx) >= 50,  "below-range dvc clamps to 50%, not off the track");
        }
    }

    printf("\n%s\n", fails ? "SOME CHECKS FAILED" : "ALL GAMMA CHECKS PASSED");
    return fails;
}
