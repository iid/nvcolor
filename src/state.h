/* state.h - per-display settings and persistence. */
#ifndef STATE_H
#define STATE_H

#include <windows.h>
#include "gamma.h"
#include "state_limits.h"

#define DEVNAME_MAX   64
#define LABEL_MAX     128

typedef struct {
    char     devname[DEVNAME_MAX];  /* "\\.\DISPLAY1" - needed for CreateDCA */
    wchar_t  wdevname[DEVNAME_MAX]; /* same value, wide, used as the INI section */
    wchar_t  label[LABEL_MAX];      /* friendly name, shown on the tab */
    int      dvc;                   /* raw driver level, dvc_min..dvc_max */
    int      dvc_min, dvc_max;
    int      hue;                   /* 0..359 */
    GammaParams gamma;
    int      dvc_supported;
    int      hue_supported;
} DisplayState;

typedef struct {
    DisplayState displays[MAX_DISPLAYS];
    int          count;
    int          apply_to_all;
} AppState;

extern AppState g_state;

void state_defaults(void);

/* Enumerate NVIDIA displays and merge stored settings from the INI file. */
int  state_load(const wchar_t *iniPath);
int  state_save(void);

/* Re-enumerate displays after a topology change, dropping cached DCs and
 * re-merging the stored settings. Returns the new display count. */
int  state_renumerate(void);

HDC  state_display_dc(int index);

/* Non-zero if CreateDCA has failed for this display, which means the four
 * GDI gamma sliders silently do nothing on it. */
int  state_display_dc_failed(int index);

void state_release_dcs(void);

/* What a given apply should push. The three are independent: vibrance and hue
 * are driver controls, the other four live only in the GDI ramp. Pushing a
 * component that did not change is not free.
 *
 * Measured with the value already in effect, so these are lower bounds:
 *
 *     SetDVCLevel         ~3.57 ms   <-- dominates everything
 *     SetHUEAngle         ~3.57 ms
 *     SetDeviceGammaRamp  ~0.70 ms
 *     EnumDisplayHandles  ~0.13 ms
 *
 * An apply that pushed all of it cost ~7.98 ms, and set_slider() runs on every
 * WM_MOUSEMOVE, so dragging vibrance blocked the message loop for several
 * milliseconds per step - which is what made it feel sticky - while the four
 * ramp-only sliders cost ~0.70 ms. Push only what changed. */
#define APPLY_DVC   0x01
#define APPLY_HUE   0x02
#define APPLY_RAMP  0x04
#define APPLY_ALL   (APPLY_DVC | APPLY_HUE | APPLY_RAMP)

/* Apply the selected components of one display's settings. `what` is a mask of
 * APPLY_*; APPLY_ALL is for startup, re-apply and drift recovery, where the
 * driver's state may be stale regardless of what the struct says. */
int  state_apply(int index, int what);

#endif /* STATE_H */
