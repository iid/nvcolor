/* gamma.h - 256-entry RGB gamma ramp generation.
 *
 * Brightness, contrast, gamma and colour temperature are NOT NVAPI controls.
 * The old Control Panel applied them through the GDI gamma ramp
 * (SetDeviceGammaRamp), and that is what we do here.
 *
 * All values are stored as 0..1000 internal units so the UI sliders and the
 * INI file share one integer representation.
 */
#ifndef GAMMA_H
#define GAMMA_H

#include <windows.h>

#define GAMMA_STEPS 256
#define GAMMA_UNITS 1000

typedef struct {
    int brightness;  /*  0..1000, 500 = neutral */
    int contrast;    /*  0..1000, 500 = neutral */
    int gamma;       /*  0..1000, 500 = neutral (1.0) */
    int temperature; /* -1000..1000, 0 = neutral, + = warm */
} GammaParams;

void gamma_defaults(GammaParams *p);
int  gamma_is_default(const GammaParams *p);

/* Slider units (0..1000) -> real factor, for display purposes.
 * gamma: 0.50 .. 1.50, returns 1.0 at 500. */
double gamma_factor(int units);

/* Slider units (0..1000) -> contrast multiplier, 0.50 .. 1.50. */
double gamma_contrast_factor(int units);

/* out must point to a WORD[3][256] block. */
void gamma_build(const GammaParams *p, WORD out[3][GAMMA_STEPS]);

/* TRUE (1) if the ramp currently on the device matches these params, 0 if it
 * was readable and genuinely differs, -1 if GetDeviceGammaRamp failed and the
 * question could not be answered. The three cases need different handling. */
int  gamma_ramp_matches(HDC hdc, const GammaParams *p);

/* Returns 1 on success. */
int  gamma_apply(HDC hdc, const GammaParams *p);

/* Restores a linear, unadjusted ramp. */
int  gamma_reset(HDC hdc);

#endif /* GAMMA_H */
