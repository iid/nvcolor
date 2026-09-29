/* state.c - enumerate displays, merge persisted settings, apply to hardware. */
#include "state.h"
#include "nvapi_min.h"
#include "nvfmt.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

AppState g_state;

static wchar_t g_ini_path[MAX_PATH];
static HDC     g_dcs[MAX_DISPLAYS];
static int     g_dc_failed[MAX_DISPLAYS];

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void state_defaults(void)
{
    int i;
    memset(&g_state, 0, sizeof(g_state));
    for (i = 0; i < MAX_DISPLAYS; ++i)
        gamma_defaults(&g_state.displays[i].gamma);
}

/* --- INI helpers (wide: display labels may not be ASCII) ---------------- */

static int read_int(const wchar_t *section, const wchar_t *key, int def)
{
    wchar_t buf[32];
    wchar_t *end = NULL;
    long v;
    DWORD n = GetPrivateProfileStringW(section, key, L"", buf,
                                       (DWORD)(sizeof(buf) / sizeof(buf[0])), g_ini_path);
    if (n == 0)
        return def;
    /* _wtoi returns 0 for unparseable text, which after clamping meant
     * "Brightness=oops" silently became -50% brightness. Require the whole
     * token to be numeric, otherwise fall back to the default. */
    v = wcstol(buf, &end, 10);
    if (end == buf)
        return def;
    while (*end == L' ' || *end == L'\t')
        ++end;
    if (*end != L'\0')
        return def;
    if (v < -2147483647L || v > 2147483647L)
        return def;
    return (int)v;
}

static void read_str(const wchar_t *section, const wchar_t *key, wchar_t *out, int outLen)
{
    DWORD n;
    if (!out || outLen <= 0)
        return;
    n = GetPrivateProfileStringW(section, key, L"", out, (DWORD)outLen, g_ini_path);
    if (n == 0)
        out[0] = L'\0';
    out[outLen - 1] = L'\0';
}

static void write_int(const wchar_t *section, const wchar_t *key, int value)
{
    wchar_t buf[32];
    NV_SNWPRINTF(buf, (int)(sizeof(buf) / sizeof(buf[0])), L"%d", value);
    WritePrivateProfileStringW(section, key, buf, g_ini_path);
}

static void write_str(const wchar_t *section, const wchar_t *key, const wchar_t *value)
{
    WritePrivateProfileStringW(section, key, value, g_ini_path);
}

/* --- enumeration ------------------------------------------------------- */

/* Turns "\\.\DISPLAY1" into "Display 1". */
static void prettify(const wchar_t *devname, wchar_t *out, int outLen)
{
    const wchar_t *digits;
    const wchar_t *p;

    if (!out || outLen <= 0)
        return;
    digits = devname;

    if (!devname || !*devname) {
        lstrcpynW(out, L"Display", outLen);
        out[outLen - 1] = L'\0';
        return;
    }

    for (p = devname; *p; ++p) {
        if (*p >= L'0' && *p <= L'9') { digits = p; break; }
    }

    if (*digits)
        NV_SNWPRINTF(out, outLen, L"Display %s", digits);
    else
        lstrcpynW(out, devname, outLen);

    out[outLen - 1] = L'\0';
}

static void enumerate(void)
{
    int n = nvapi_display_count();
    int i;

    if (n > MAX_DISPLAYS)
        n = MAX_DISPLAYS;

    g_state.count = n;

    for (i = 0; i < n; ++i) {
        DisplayState *d = &g_state.displays[i];
        void *handle = NULL;
        char name[NVAPI_SHORT_STRING_MAX];

        gamma_defaults(&d->gamma);
        d->dvc_min = 0;
        d->dvc_max = 0;
        d->dvc = 0;
        d->hue = 0;
        d->dvc_supported = nvapi_dvc_available();
        d->hue_supported = nvapi_hue_available();
        d->devname[0] = '\0';
        d->wdevname[0] = L'\0';
        d->label[0] = L'\0';

        if (g_nvapi.enumDisplay((NvU32)i, &handle) != NVAPI_OK || !handle) {
            /* Keep a usable section key so two failing displays cannot share
             * section "" and cross-contaminate each other's settings. */
            NV_SNWPRINTF(d->wdevname, DEVNAME_MAX, L"Display%d", i + 1);
            d->wdevname[DEVNAME_MAX - 1] = L'\0';
            lstrcpynW(d->label, d->wdevname, LABEL_MAX);
            d->dvc_supported = 0;
            d->hue_supported = 0;
            continue;
        }

        name[0] = '\0';
        if (g_nvapi.getDisplayName)
            g_nvapi.getDisplayName(handle, name);

        /* Only trust the name if it actually looks like a GDI display device
         * ("\\.\DISPLAYn"). Anything else cannot be handed to CreateDCA,
         * and prettify() would turn e.g. "NVIDIA GeForce RTX 4090" into the
         * tab label "Display 4090" while orphaning the saved INI section. */
        if (name[0] && name[0] == '\\' && name[1] == '.' && name[2] == '\\') {
            lstrcpynA(d->devname, name, DEVNAME_MAX);
            d->devname[DEVNAME_MAX - 1] = '\0';
        } else {
            NV_SNPRINTF(d->devname, DEVNAME_MAX, "\\\\.\\DISPLAY%d", i + 1);
        }
        /* cch must be the full DEVNAME_MAX, not DEVNAME_MAX - 1: a length of
         * -1 includes the terminating NUL, so a full-length name needs room
         * for it or the conversion fails outright. */
        if (MultiByteToWideChar(CP_ACP, 0, d->devname, -1, d->wdevname, DEVNAME_MAX) == 0)
            d->wdevname[0] = L'\0';
        if (!d->wdevname[0])
            NV_SNWPRINTF(d->wdevname, DEVNAME_MAX, L"Display%d", i + 1);
        d->wdevname[DEVNAME_MAX - 1] = L'\0';

        prettify(d->wdevname, d->label, LABEL_MAX);

        if (d->dvc_supported) {
            NV_DVC_INFO info;
            ZeroMemory(&info, sizeof(info));
            info.version = NV_DVC_INFO_VER;
            if (g_nvapi.getDvcInfo(handle, 0, &info) == NVAPI_OK) {
                d->dvc_min = info.minLevel;
                d->dvc_max = info.maxLevel;
                d->dvc = info.currentLevel;

                /* A degenerate or inverted range means the driver answered but
                 * has no vibrance to control. Leaving this "supported" gave an
                 * enabled-looking slider that could not move and silently did
                 * nothing - "n/a" in the readout, and slider_step() returning
                 * 1000. Treat it as unsupported, which is what it is. */
                if (d->dvc_max <= d->dvc_min) {
                    d->dvc_supported = 0;
                    d->dvc_min = 0;
                    d->dvc_max = 0;
                    d->dvc = 0;
                } else if (d->dvc < d->dvc_min) {
                    /* Never trust the driver's current level to be inside its
                     * own range: an out-of-range dvc made slider_value() return
                     * more than 1000, which drew the knob off the end of the
                     * track and printed a percentage above 100. */
                    d->dvc = d->dvc_min;
                } else if (d->dvc > d->dvc_max) {
                    d->dvc = d->dvc_max;
                }
            } else {
                d->dvc_supported = 0;
            }
        }

        if (d->hue_supported) {
            NV_HUE_INFO info;
            ZeroMemory(&info, sizeof(info));
            info.version = NV_HUE_INFO_VER;
            if (g_nvapi.getHueInfo(handle, 0, &info) == NVAPI_OK)
                d->hue = info.currentAngle;
            else
                d->hue_supported = 0;
        }
    }
}

/* --- load / save ------------------------------------------------------- */

/* Fold the values stored in the INI over the freshly enumerated hardware
 * state. Split out so it can also run after a display topology change. */
static void merge_saved(void)
{
    int i;

    for (i = 0; i < g_state.count; ++i) {
        DisplayState *d = &g_state.displays[i];
        const wchar_t *sec = d->wdevname;
        wchar_t stored[LABEL_MAX];

        read_str(sec, L"Label", stored, LABEL_MAX);
        if (stored[0])
            lstrcpynW(d->label, stored, LABEL_MAX);
        d->label[LABEL_MAX - 1] = L'\0';

        if (d->dvc_supported) {
            int v = read_int(sec, L"DVC", d->dvc);
            if (v < d->dvc_min) v = d->dvc_min;
            if (v > d->dvc_max) v = d->dvc_max;
            d->dvc = v;
        }

        if (d->hue_supported) {
            int v = read_int(sec, L"Hue", d->hue) % 360;
            if (v < 0) v += 360;
            d->hue = v;
        }

        /* Clamped: an unclamped value here produced a negative channel gain
         * and a solid-colour display, with no way back out short of a reset. */
        d->gamma.brightness  = clampi(read_int(sec, L"Brightness",  500),    0, 1000);
        d->gamma.contrast    = clampi(read_int(sec, L"Contrast",    500),    0, 1000);
        d->gamma.gamma       = clampi(read_int(sec, L"Gamma",       500),    0, 1000);
        d->gamma.temperature = clampi(read_int(sec, L"Temperature",   0), -500,  500);
    }

    g_state.apply_to_all = read_int(L"Global", L"ApplyToAll", 0) ? 1 : 0;
}

int state_load(const wchar_t *iniPath)
{
    lstrcpynW(g_ini_path, iniPath, MAX_PATH);
    g_ini_path[MAX_PATH - 1] = L'\0';

    state_defaults();
    enumerate();
    merge_saved();

    return g_state.count;
}

int state_renumerate(void)
{
    int i;

    state_release_dcs();
    for (i = 0; i < g_state.count; ++i)
        memset(&g_state.displays[i], 0, sizeof(DisplayState));

    enumerate();
    merge_saved();
    return g_state.count;
}

int state_save(void)
{
    int i;

    for (i = 0; i < g_state.count; ++i) {
        DisplayState *d = &g_state.displays[i];
        const wchar_t *sec = d->wdevname;

        write_str(sec, L"Label", d->label);
        /* Only write what we can read back, otherwise a driver release that
         * drops the private ids would overwrite the user's values with 0. */
        if (d->dvc_supported) write_int(sec, L"DVC", d->dvc);
        if (d->hue_supported) write_int(sec, L"Hue", d->hue);
        write_int(sec, L"Brightness",  d->gamma.brightness);
        write_int(sec, L"Contrast",    d->gamma.contrast);
        write_int(sec, L"Gamma",       d->gamma.gamma);
        write_int(sec, L"Temperature", d->gamma.temperature);
    }
    write_int(L"Global", L"ApplyToAll", g_state.apply_to_all);
    return 1;
}

/* --- apply ------------------------------------------------------------- */

HDC state_display_dc(int index)
{
    DisplayState *d;

    if (index < 0 || index >= g_state.count)
        return NULL;

    if (g_dcs[index])
        return g_dcs[index];

    d = &g_state.displays[index];
    if (d->devname[0]) {
        g_dcs[index] = CreateDCA("DISPLAY", d->devname, NULL, NULL);
        if (!g_dcs[index])
            g_dc_failed[index] = 1;   /* sticky, so we stop hammering CreateDCA */
    }

    return g_dcs[index];
}

int state_display_dc_failed(int index)
{
    if (index < 0 || index >= MAX_DISPLAYS)
        return 0;
    return g_dc_failed[index];
}

void state_release_dcs(void)
{
    int i;
    for (i = 0; i < MAX_DISPLAYS; ++i) {
        if (g_dcs[i]) {
            DeleteDC(g_dcs[i]);
            g_dcs[i] = NULL;
        }
        g_dc_failed[i] = 0;
    }
}

int state_apply(int index, int what)
{
    DisplayState *d;
    void *handle = NULL;
    HDC hdc;
    int ok = 1;

    if (index < 0 || index >= g_state.count)
        return 0;
    if (!what)
        return 1;

    d = &g_state.displays[index];

    if (what & (APPLY_DVC | APPLY_HUE)) {
        if (g_nvapi.enumDisplay((NvU32)index, &handle) != NVAPI_OK || !handle)
            ok = 0;

        if (ok && (what & APPLY_DVC) && d->dvc_supported && g_nvapi.setDvcLevel) {
            if (g_nvapi.setDvcLevel(handle, 0, (NvS32)d->dvc) != NVAPI_OK)
                ok = 0;
        }

        if (ok && (what & APPLY_HUE) && d->hue_supported && g_nvapi.setHueAngle) {
            if (g_nvapi.setHueAngle(handle, 0, (NvS32)d->hue) != NVAPI_OK)
                ok = 0;
        }
    }

    if (what & APPLY_RAMP) {
        hdc = state_display_dc(index);
        if (hdc) {
            if (!gamma_apply(hdc, &d->gamma))
                ok = 0;
        }
    }

    return ok;
}
