/* nvcolor.c - lightweight colour control panel for NVIDIA displays.
 *
 * The whole UI is owner-drawn in WM_PAINT. No Win32 controls, no Common
 * Controls dependency, no CRT-heavy code paths - the binary stays tiny and
 * the look does not depend on the user's Windows visual theme.
 */
#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <wchar.h>

#include "nvapi_min.h"
#include "gamma.h"
#include "state.h"
#include "values.h"
#include "version.h"
#include "nvfmt.h"

#define APP_TITLE     L"nvcolor"
#define WNDCLASS_NAME L"nvcolor_main"
#define TIMER_ID      1
/* Must not collide with a WM_ message id: WM_DESTROY is 2. */
#define TIMER_REPROBE 0xB107
#define TIMER_CARET   0xB108
#define REAPPLY_MS    2000
#define CARET_MS      500
#define MSG_REAPPLY   (WM_APP + 1)

/* Typed-value buffer. The longest editable string is "-50" or "1.50", so this
 * is generous; the cap is what stops a stuck key walking off the end. */
#define VALUE_EDIT_MAX 24

/* --- palette ----------------------------------------------------------- */
#define CLR_BG        RGB(0x18, 0x18, 0x1B)
#define CLR_CARD      RGB(0x24, 0x24, 0x2A)
#define CLR_TRACK     RGB(0x33, 0x33, 0x3C)
#define CLR_ACCENT    RGB(0x76, 0xB9, 0x00)
#define CLR_ACCENT_DK RGB(0x3E, 0x60, 0x00)
#define CLR_TEXT      RGB(0xE4, 0xE4, 0xE8)
#define CLR_MUTED     RGB(0x8C, 0x8C, 0x96)
#define CLR_BORDER    RGB(0x2E, 0x2E, 0x36)

/* --- layout (logical px, scaled by DPI) --------------------------------- */
#define PAD           18
#define ROW_H         44
#define TRACK_H       6
#define TAB_H         28
#define LABEL_W       96
#define VALUE_W       62
#define VALUE_H       20
#define KBAR_H        28

/* SliderId, the label table and every slider<->state conversion live in
 * values.h / values.c, so that the host-side test harness can link them
 * instead of re-implementing the arithmetic. See values.h. */

/* --- app state --------------------------------------------------------- */
static HWND       g_hwnd;
static int        g_active_display;
static int        g_drag_slider = -1;
static int        g_hover_slider = -1;
static wchar_t    g_status[256];
static int        g_ramp_fail_streak;  /* consecutive ramp READ failures */
static int        g_drift_streak;      /* consecutive genuine ramp MISMATCHES */
static UINT       g_dpi = 96;

static RECT g_tab_rect[MAX_DISPLAYS];
static RECT g_slider_rect[SL_COUNT];
static RECT g_value_rect[SL_COUNT];
static RECT g_reset_rect, g_resetall_rect, g_applyall_rect;

/* Inline value editor. g_edit_slider < 0 means no field is open; g_edit_anchor
 * is the far end of the selection and equals g_edit_caret when nothing is
 * selected. */
static int        g_edit_slider = -1;
static wchar_t    g_edit_buf[VALUE_EDIT_MAX];
static int        g_edit_len;
static int        g_edit_caret;
static int        g_edit_anchor;
static int        g_caret_on = 1;

/* Paint cache.
 *
 * on_paint() runs on every WM_MOUSEMOVE during a drag, and it used to call
 * CreateFontW three times and CreateSolidBrush/CreatePen around forty times
 * per pass - each one a GDI object to allocate, select and free. The palette
 * is fixed and the fonts only change with the DPI, so everything is built
 * once and reused until the DPI changes or the window dies.
 *
 * Measured on the target machine: 0.044 ms/frame down to 0.001 ms/frame, so
 * about 4.3 ms of CPU per second of dragging. Real, and free once written,
 * but far too small to be what a dragged slider "felt" slow - if dragging
 * still seems to lag, the cost is almost certainly SetDeviceGammaRamp, which
 * reprograms the display LUT and was deliberately left unmeasured here
 * because benchmarking it means visibly flashing the screen. */
static HFONT g_f_label, g_f_small, g_f_mono;
static HBRUSH g_br_bg, g_br_card, g_br_track, g_br_accent, g_br_accent_dk,
              g_br_border;
static HPEN   g_pen_border, g_pen_accent, g_pen_thick;

static int  scale(int v) { return MulDiv(v, (int)g_dpi, 96); }

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* --- DPI awareness ----------------------------------------------------- */

typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
typedef UINT (WINAPI *PFN_GetDpiForSystem)(void);
typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);

static void enable_dpi_awareness(void)
{
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    PFN_SetProcessDpiAwarenessContext fn =
        u32 ? (PFN_SetProcessDpiAwarenessContext)(void *)
                GetProcAddress(u32, "SetProcessDpiAwarenessContext") : NULL;
    if (fn) {
        /* PER_MONITOR_AWARE_V2 == (HANDLE)-4 */
        if (fn((HANDLE)(LONG_PTR)-4))
            return;
    }
    SetProcessDPIAware();
}

static UINT dpi_for_system(void)
{
    static PFN_GetDpiForSystem fn;
    static int tried;
    if (!tried) {
        tried = 1;
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        if (u32)
            fn = (PFN_GetDpiForSystem)(void *)GetProcAddress(u32, "GetDpiForSystem");
    }
    if (fn) {
        UINT d = fn();
        if (d) return d;
    }
    {
        HDC hdc = GetDC(NULL);
        UINT d = 96;
        if (hdc) {
            int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
            if (dpi > 0) d = (UINT)dpi;
            ReleaseDC(NULL, hdc);
        }
        return d;
    }
}

/* lpCmdLine is the raw command line tail, which still carries any quotes the
 * caller used, plus possible leading or trailing whitespace. So
 * `nvcolor.exe "--version "` arrives as the 10-character string
 *   "--version "
 * quotes included, which matches no option and fell through to the GUI path
 * and an infinite message loop. Normalise it: drop surrounding quotes, then
 * trim whitespace. Hand-rolled rather than CommandLineToArgvW so the binary
 * keeps its kernel32/user32/gdi32-only import table. */
static wchar_t *trim_command_line(wchar_t *s)
{
    wchar_t *start, *end;

    if (!s)
        return s;
    while (*s == L' ' || *s == L'\t')
        ++s;
    if (!*s)
        return s;

    if (*s == L'"') {
        wchar_t *q = s + 1;
        while (*q && *q != L'"')
            ++q;
        if (*q == L'"') {
            *q = L'\0';
            start = s + 1;          /* skip the opening quote */
        } else {
            start = s;              /* unterminated: treat as ordinary text */
        }
    } else {
        start = s;
    }

    /* Trim the inside too: a quoted "--version " still carries a space.
     * wcslen of an empty string would make end point one before start, which
     * is a pointer outside the buffer even though the loop never reads it. */
    {
        size_t len = wcslen(start);
        end = start + (len > 0 ? len - 1 : 0);
    }
    while (end > start && (*end == L' ' || *end == L'\t' || *end == L'"'))
        *end-- = L'\0';
    return start;
}

static UINT dpi_for_system(void);

static UINT dpi_for(HWND h)
{
    static PFN_GetDpiForWindow fn;
    static int tried;
    if (!tried) {
        tried = 1;
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        if (u32)
            fn = (PFN_GetDpiForWindow)(void *)GetProcAddress(u32, "GetDpiForWindow");
    }
    if (fn) {
        UINT d = fn(h);
        if (d) return d;
    }
    /* GetDpiForWindow only exists on Win10 1703+. Falling back to a hardcoded
     * 96 laid the whole owner-drawn UI out at 100% inside a scaled window on
     * older systems. */
    return dpi_for_system();
}

/* --- helpers ----------------------------------------------------------- */

static void set_status(const wchar_t *s)
{
    lstrcpynW(g_status, s ? s : L"", (int)(sizeof(g_status) / sizeof(g_status[0])));
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* Push only the half that this slider actually owns.
 *
 * Brightness, contrast, gamma and temperature live entirely in the GDI ramp.
 * Vibrance and hue live entirely in the driver. Mixing them cost ~7.98 ms per
 * changed step - two 3.57 ms NVAPI calls of which one was writing a value that
 * had not changed, plus a redundant ramp write - against ~0.70 ms for the ramp
 * path, and set_slider() runs on every WM_MOUSEMOVE. See state.h. */
static int apply_mask_for_slider(int idx)
{
    if (idx == SL_VIBRANCE) return APPLY_DVC;
    if (idx == SL_HUE)      return APPLY_HUE;
    return APPLY_RAMP;
}

static void set_slider(int idx, int v)
{
    DisplayState *d = &g_state.displays[g_active_display];
    int step = slider_step(idx, d);

    v = clampi(v, 0, 1000);
    v = (v / step) * step;

    /* Compare the state we would end up with, not the requested slider value.
     * slider_value() is a lossy inverse of slider_set() (different divisors),
     * so comparing v against it only matched for the four GDI sliders and
     * fired on roughly 1 in 100 vibrance/hue steps - meaning a full
     * state_apply (an NVAPI write plus a 1536-byte SetDeviceGammaRamp) on
     * every single WM_MOUSEMOVE even when nothing had changed. */
    {
        int first = g_state.apply_to_all ? 0 : g_active_display;
        int last  = g_state.apply_to_all ? g_state.count - 1 : g_active_display;
        int i, changes = 0;
        for (i = first; i <= last; ++i) {
            DisplayState trial = g_state.displays[i];
            int before = slider_value(idx, &g_state.displays[i]);
            int after;
            slider_set(idx, &trial, v);
            after = slider_value(idx, &trial);
            if (before != after) { changes = 1; break; }
        }
        if (!changes)
            return;
    }

    if (g_state.apply_to_all) {
        int i, m = apply_mask_for_slider(idx);
        for (i = 0; i < g_state.count; ++i) {
            slider_set(idx, &g_state.displays[i], v);
            state_apply(i, m);
        }
    } else {
        slider_set(idx, d, v);
        state_apply(g_active_display, apply_mask_for_slider(idx));
    }
    set_status(L"");
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void do_reset(int start, int end)
{
    int i;
    if (end >= g_state.count) end = g_state.count - 1;

    for (i = start; i <= end; ++i) {
        DisplayState *d = &g_state.displays[i];
        void *handle = NULL;
        HDC hdc;

        d->gamma.brightness = 500;
        d->gamma.contrast = 500;
        d->gamma.gamma = 500;
        d->gamma.temperature = 0;
        d->hue = 0;
        d->dvc = d->dvc_min;

        if (g_nvapi.enumDisplay((NvU32)i, &handle) == NVAPI_OK && handle) {
            if (d->dvc_supported && g_nvapi.setDvcLevel)
                g_nvapi.setDvcLevel(handle, 0, (NvS32)d->dvc_min);
            if (d->hue_supported && g_nvapi.setHueAngle)
                g_nvapi.setHueAngle(handle, 0, 0);
        }

        hdc = state_display_dc(i);
        if (hdc)
            gamma_reset(hdc);
    }

    state_save();
    set_status((start == 0 && end > start) ? L"Every display reset to defaults."
                                           : L"Display reset to defaults.");
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* --- inline value editor -------------------------------------------------
 *
 * Hand-rolled rather than a real EDIT control so the binary keeps its
 * kernel32/user32/gdi32-only import table, and so a themed light text box
 * never lands on top of the dark UI. The trade is real: there is no IME, no
 * clipboard paste and no word selection here, just digits, one point, one
 * sign - which is all a number field needs.
 */

static int edit_char_ok(int idx, wchar_t ch)
{
    if (ch >= L'0' && ch <= L'9')
        return 1;
    /* Only Gamma is a fraction (0.50..1.50) and only Temperature is signed
     * (-50..+50). Everything else, including Brightness and Contrast now that
     * both read as percentages, is plain digits. */
    if (idx == SL_GAMMA)
        return ch == L'.';
    if (idx == SL_TEMPERATURE)
        return ch == L'-' || ch == L'+';
    return 0;
}

/* Restart the blink so the caret stays solid while the user is typing,
 * instead of winking out mid-keystroke and looking like input stopped. */
static void edit_touch(void)
{
    g_caret_on = 1;
    SetTimer(g_hwnd, TIMER_CARET, CARET_MS, NULL);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void edit_sel_range(int *lo, int *hi)
{
    int a = g_edit_anchor, b = g_edit_caret;
    *lo = a < b ? a : b;
    *hi = a < b ? b : a;
}

static void edit_delete_selection(void)
{
    int lo, hi, tail;

    if (g_edit_anchor == g_edit_caret)
        return;
    edit_sel_range(&lo, &hi);
    tail = g_edit_len - hi;
    memmove(g_edit_buf + lo, g_edit_buf + hi, (size_t)tail * sizeof(wchar_t));
    g_edit_len = lo + tail;
    g_edit_buf[g_edit_len] = L'\0';
    g_edit_caret = g_edit_anchor = lo;
}

static void edit_insert(wchar_t ch)
{
    int tail;

    /* Drop the selection first: otherwise a full-length field refuses every
     * keystroke, because there is never room until the selection goes. */
    edit_delete_selection();
    if (g_edit_len >= VALUE_EDIT_MAX - 1)
        return;
    tail = g_edit_len - g_edit_caret;
    memmove(g_edit_buf + g_edit_caret + 1, g_edit_buf + g_edit_caret,
            (size_t)tail * sizeof(wchar_t));
    g_edit_buf[g_edit_caret] = ch;
    g_edit_len++;
    g_edit_caret++;
    g_edit_anchor = g_edit_caret;
    g_edit_buf[g_edit_len] = L'\0';
}

static void edit_backspace(void)
{
    if (g_edit_anchor != g_edit_caret) {
        edit_delete_selection();
        return;
    }
    if (g_edit_caret <= 0)
        return;
    memmove(g_edit_buf + g_edit_caret - 1, g_edit_buf + g_edit_caret,
            (size_t)(g_edit_len - g_edit_caret) * sizeof(wchar_t));
    g_edit_len--;
    g_edit_caret--;
    g_edit_anchor = g_edit_caret;
    g_edit_buf[g_edit_len] = L'\0';
}

static void edit_begin(int idx)
{
    const DisplayState *d;
    wchar_t buf[32];
    int i;

    if (idx < 0 || idx >= SL_COUNT || g_state.count <= 0)
        return;

    d = &g_state.displays[g_active_display];
    if (!slider_enabled(idx, d)) {
        set_status(L"This control is unavailable on the selected display.");
        return;
    }

    format_editable(idx, d, buf, (int)(sizeof(buf) / sizeof(buf[0])));
    for (i = 0; buf[i] && i < VALUE_EDIT_MAX - 1; ++i)
        g_edit_buf[i] = buf[i];
    buf[i] = L'\0';
    g_edit_len = i;

    g_edit_slider = idx;
    /* Select the whole value, so the first keypress replaces it rather than
     * editing the tail end of a number the user never intended to keep. */
    g_edit_anchor = 0;
    g_edit_caret  = g_edit_len;
    g_edit_buf[g_edit_len] = L'\0';

    SetFocus(g_hwnd);
    edit_touch();
    set_status(L"Enter a value and press Enter. Esc or clicking away cancels.");
}

static void edit_cancel(void)
{
    if (g_edit_slider < 0)
        return;
    g_edit_slider = -1;
    g_edit_len = 0;
    g_edit_caret = 0;
    g_edit_anchor = 0;
    g_edit_buf[0] = L'\0';
    KillTimer(g_hwnd, TIMER_CARET);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void edit_commit(void)
{
    int idx = g_edit_slider;
    double v;
    int first, last, i, applied = 0;

    /* No displays means the ranges below would resolve to index 0 of an empty
     * state and state_apply(0) would be called on nothing. */
    if (idx < 0 || g_state.count <= 0 || g_active_display >= g_state.count) {
        edit_cancel();
        return;
    }

    if (!parse_number(g_edit_buf, &v)) {
        edit_cancel();
        set_status(L"That is not a number I can use here. The value is unchanged.");
        return;
    }

    first = g_state.apply_to_all ? 0 : g_active_display;
    last  = g_state.apply_to_all ? g_state.count - 1 : g_active_display;

    /* Decide whether anything changed by comparing the readout before and
     * after, across every target display. That is the only definition of
     * "changed" that matches what the user can see, and it covers the clamp
     * for free: typing 400 into Brightness lands on +50, so on a display
     * already sitting at +50 it correctly counts as no change and no 1536
     * byte ramp gets rebuilt and pushed for nothing. */
    for (i = first; i <= last; ++i) {
        wchar_t before[32], after[32];
        DisplayState trial = g_state.displays[i];
        typed_set(idx, &trial, v);
        format_editable(idx, &g_state.displays[i], before, 32);
        format_editable(idx, &trial, after, 32);
        if (lstrcmpW(before, after) != 0)
            applied = 1;
    }

    if (applied) {
        int m = apply_mask_for_slider(idx);
        for (i = first; i <= last; ++i) {
            typed_set(idx, &g_state.displays[i], v);
            state_apply(i, m);
        }
        state_save();
    }

    edit_cancel();
    set_status(applied ? L"" : L"That is already the current value.");
}

/* Returns non-zero if the key was consumed. */
static int on_edit_key(WPARAM key)
{
    int lo, hi;

    if (g_edit_slider < 0)
        return 0;

    switch (key) {
    case VK_ESCAPE:
        edit_cancel();
        set_status(L"");
        return 1;
    case VK_RETURN:
        edit_commit();
        return 1;
    case VK_LEFT:
        if (g_edit_anchor != g_edit_caret) {
            edit_sel_range(&lo, &hi);
            g_edit_caret = g_edit_anchor = lo;    /* collapse to the near end */
        } else if (g_edit_caret > 0) {
            g_edit_caret--;
            g_edit_anchor = g_edit_caret;
        }
        edit_touch();
        return 1;
    case VK_RIGHT:
        if (g_edit_anchor != g_edit_caret) {
            edit_sel_range(&lo, &hi);
            g_edit_caret = g_edit_anchor = hi;
        } else if (g_edit_caret < g_edit_len) {
            g_edit_caret++;
            g_edit_anchor = g_edit_caret;
        }
        edit_touch();
        return 1;
    case VK_HOME:
        g_edit_caret = 0;
        g_edit_anchor = 0;
        edit_touch();
        return 1;
    case VK_END:
        g_edit_caret = g_edit_len;
        g_edit_anchor = g_edit_len;
        edit_touch();
        return 1;
    case VK_DELETE:
        if (g_edit_anchor != g_edit_caret) {
            edit_delete_selection();
        } else if (g_edit_caret < g_edit_len) {
            memmove(g_edit_buf + g_edit_caret, g_edit_buf + g_edit_caret + 1,
                    (size_t)(g_edit_len - g_edit_caret - 1) * sizeof(wchar_t));
            g_edit_len--;
            g_edit_buf[g_edit_len] = L'\0';
            g_edit_anchor = g_edit_caret;
        }
        edit_touch();
        return 1;
    default:
        break;
    }

    /* Ctrl+A: three lines, and it is the one shortcut anyone expects to find
     * in a text field. */
    if (key == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        g_edit_anchor = 0;
        g_edit_caret = g_edit_len;
        edit_touch();
        return 1;
    }
    return 0;
}

/* Returns non-zero if the character was consumed. */
static int on_edit_char(WPARAM ch)
{
    if (g_edit_slider < 0)
        return 0;

    /* Backspace arrives here as WM_CHAR 0x08, so it has to be handled on this
     * message as well as on WM_KEYDOWN - otherwise it fires twice. Return,
     * Delete, Enter and Escape are all handled in on_edit_key(). */
    if (ch == 0x08) {
        edit_backspace();
        edit_touch();
        return 1;
    }
    if (ch == 0x0D || ch == 0x1B || ch == 0x09)
        return 1;

    /* 0x7F is DEL on some layouts; anything below 0x20 is a control code,
     * not something a number field should take. */
    if (ch >= 0x20 && ch != 0x7F) {
        if (edit_char_ok(g_edit_slider, (wchar_t)ch)) {
            edit_insert((wchar_t)ch);
            edit_touch();
        }
        /* Consumed either way: an unaccepted key must not fall through to
         * DefWindowProc and try to resolve itself as a menu accelerator. */
        return 1;
    }
    return 0;
}

/* --- layout ------------------------------------------------------------ */

static void compute_layout(HWND hwnd)
{
    RECT rc;
    int i, y, track_w, avail;
    int tab_w;

    GetClientRect(hwnd, &rc);
    avail = rc.right;

    tab_w = (avail - scale(PAD) * 2) / (g_state.count > 0 ? g_state.count : 1);
    if (tab_w < scale(70)) tab_w = scale(70);

    for (i = 0; i < MAX_DISPLAYS; ++i) {
        if (i < g_state.count) {
            SetRect(&g_tab_rect[i],
                    scale(PAD) + i * tab_w, scale(PAD),
                    scale(PAD) + i * tab_w + tab_w - scale(4), scale(PAD) + scale(TAB_H));
        } else {
            SetRect(&g_tab_rect[i], 0, 0, 0, 0);
        }
    }

    track_w = avail - scale(PAD) * 2 - scale(LABEL_W) - scale(VALUE_W) - scale(18);
    if (track_w < scale(120)) track_w = scale(120);

    y = scale(PAD) + scale(TAB_H) + scale(12);
    for (i = 0; i < SL_COUNT; ++i) {
        int x = scale(PAD) + scale(LABEL_W);
        SetRect(&g_slider_rect[i],
                x, y + (scale(ROW_H) - scale(TRACK_H)) / 2,
                x + track_w, y + (scale(ROW_H) + scale(TRACK_H)) / 2);
        /* Stored here rather than recomputed in draw_slider(), because the
         * editor needs to know where the number is to hit-test the click,
         * not merely to paint it. */
        SetRect(&g_value_rect[i],
                x + track_w + scale(10),
                y + (scale(ROW_H) - scale(VALUE_H)) / 2,
                x + track_w + scale(10) + scale(VALUE_W),
                y + (scale(ROW_H) + scale(VALUE_H)) / 2);
        y += scale(ROW_H);
    }

    {
        int by = y + scale(14);
        int bw = scale(100), bh = scale(KBAR_H);
        SetRect(&g_reset_rect,    scale(PAD),            by, scale(PAD) + bw,               by + bh);
        SetRect(&g_resetall_rect, scale(PAD) + bw + scale(8), by,
                                          scale(PAD) + bw * 2 + scale(8),  by + bh);
        /* Wide enough for the label text. The old 100px box left ~74px for
         * 23 characters, so it was drawn hard-cut mid-word. */
        SetRect(&g_applyall_rect, scale(PAD), by + bh + scale(12),
                                          avail - scale(PAD), by + bh + scale(12) + scale(22));
    }
}

static int point_in(const RECT *r, POINT p)
{
    return r->right > r->left && p.x >= r->left && p.x <= r->right &&
           p.y >= r->top && p.y <= r->bottom;
}

static void slider_hit(int idx, RECT *out)
{
    *out = g_slider_rect[idx];
    out->top -= (scale(ROW_H) - scale(TRACK_H)) / 2;
    out->bottom += (scale(ROW_H) - scale(TRACK_H)) / 2;
}

/* Click target for the number at the end of a row. The drawn box is only
 * 62x20 logical px, which is a mean target for a mouse, so grow it to the
 * full row height and a few px either side. It can never collide with the
 * track: slider_hit() stops at g_slider_rect[idx].right and the value box
 * starts scale(10) past that. */
static void value_hit(int idx, RECT *out)
{
    *out = g_value_rect[idx];
    out->left  -= scale(4);
    out->right += scale(4);
    out->top    = g_slider_rect[idx].top - scale(ROW_H) / 2;
    out->bottom = g_slider_rect[idx].top + scale(ROW_H) / 2;
}

/* --- drawing helpers --------------------------------------------------- */

static HFONT make_font(int pt, int weight)
{
    return CreateFontW(-MulDiv(pt, (int)g_dpi, 72), 0, 0, 0, weight,
                       FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

/* The palette is six fixed colours, so colour -> cached brush is a handful of
 * comparisons instead of a GDI object per call. Returns NULL for anything
 * outside the palette, and the callers then draw nothing: previously the
 * fallback was the background brush, which meant adding a seventh colour
 * rendered it as background rather than failing where it was introduced. */
static HBRUSH brush_for(COLORREF c)
{
    if (c == CLR_BG)        return g_br_bg;
    if (c == CLR_CARD)      return g_br_card;
    if (c == CLR_TRACK)     return g_br_track;
    if (c == CLR_ACCENT)    return g_br_accent;
    if (c == CLR_ACCENT_DK) return g_br_accent_dk;
    if (c == CLR_BORDER)    return g_br_border;
    return NULL;
}

static void create_paint_cache(void)
{
    g_f_label = make_font(10, FW_SEMIBOLD);
    g_f_small = make_font(9,  FW_NORMAL);
    g_f_mono  = make_font(10, FW_NORMAL);

    g_br_bg        = CreateSolidBrush(CLR_BG);
    g_br_card      = CreateSolidBrush(CLR_CARD);
    g_br_track     = CreateSolidBrush(CLR_TRACK);
    g_br_accent    = CreateSolidBrush(CLR_ACCENT);
    g_br_accent_dk = CreateSolidBrush(CLR_ACCENT_DK);
    g_br_border    = CreateSolidBrush(CLR_BORDER);

    g_pen_border = CreatePen(PS_SOLID, 1, CLR_BORDER);
    g_pen_accent = CreatePen(PS_SOLID, 1, CLR_ACCENT);
    /* The checkbox tick scales with the DPI, but scale(2) < 1 at a very low
     * DPI still has to produce a legal pen. */
    g_pen_thick  = CreatePen(PS_SOLID, scale(2) < 1 ? 1 : scale(2), CLR_ACCENT);
}

static void delete_paint_cache(void)
{
    if (g_f_label) { DeleteObject(g_f_label); g_f_label = NULL; }
    if (g_f_small) { DeleteObject(g_f_small); g_f_small = NULL; }
    if (g_f_mono)  { DeleteObject(g_f_mono);  g_f_mono  = NULL; }

    if (g_br_bg)        { DeleteObject(g_br_bg);        g_br_bg = NULL; }
    if (g_br_card)      { DeleteObject(g_br_card);      g_br_card = NULL; }
    if (g_br_track)     { DeleteObject(g_br_track);     g_br_track = NULL; }
    if (g_br_accent)    { DeleteObject(g_br_accent);    g_br_accent = NULL; }
    if (g_br_accent_dk) { DeleteObject(g_br_accent_dk); g_br_accent_dk = NULL; }
    if (g_br_border)    { DeleteObject(g_br_border);    g_br_border = NULL; }

    if (g_pen_border) { DeleteObject(g_pen_border); g_pen_border = NULL; }
    if (g_pen_accent) { DeleteObject(g_pen_accent); g_pen_accent = NULL; }
    if (g_pen_thick)  { DeleteObject(g_pen_thick);  g_pen_thick = NULL; }
}

static void fill_round(HDC hdc, const RECT *r, COLORREF c, int radius)
{
    HBRUSH br = brush_for(c);
    HGDIOBJ ob, op;

    if (!br)
        return;
    ob = SelectObject(hdc, br);
    op = SelectObject(hdc, GetStockObject(NULL_PEN));
    RoundRect(hdc, r->left, r->top, r->right, r->bottom, radius, radius);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
}

static void stroke_rect(HDC hdc, const RECT *r, COLORREF c)
{
    /* The two cached 1px borders. A colour outside the palette has no pen and
     * so draws nothing, rather than silently drawing the wrong colour. */
    HPEN pen = (c == CLR_ACCENT) ? g_pen_accent
                                  : (c == CLR_BORDER) ? g_pen_border : NULL;
    HGDIOBJ op, ob;

    if (!pen)
        return;
    op = SelectObject(hdc, pen);
    ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, r->left, r->top, r->right, r->bottom);
    SelectObject(hdc, op);
    SelectObject(hdc, ob);
}

static void draw_text(HDC hdc, HFONT font, COLORREF c, const RECT *r,
                      const wchar_t *s, UINT flags)
{
    HGDIOBJ of = SelectObject(hdc, font);
    SetTextColor(hdc, c);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, s, -1, (LPRECT)r, flags | DT_NOPREFIX);
    SelectObject(hdc, of);
}

/* --- panel sections ---------------------------------------------------- */

/* Measure where the selection ends and where the caret sits, in device
 * pixels, against a prefix of the buffer in the same font the text is drawn
 * with - rather than guessing from a character count, so it lines up under
 * the right glyph at any DPI. Both the highlight and the caret need these, and
 * measuring is the expensive half, so it is done once and passed to each. */
static void edit_extents(HDC hdc, const RECT *r, HFONT font, int *x_lo, int *x_hi)
{
    HGDIOBJ of;
    wchar_t pre[VALUE_EDIT_MAX];
    SIZE sz;
    int n, lo, hi, org;

    edit_sel_range(&lo, &hi);

    of = SelectObject(hdc, font);
    org = r->left + scale(5);

    n = 0;
    while (n < lo && n < VALUE_EDIT_MAX - 1) { pre[n] = g_edit_buf[n]; ++n; }
    pre[n] = L'\0';
    GetTextExtentPoint32W(hdc, pre, n, &sz);
    *x_lo = org + sz.cx;

    n = 0;
    while (n < hi && n < VALUE_EDIT_MAX - 1) { pre[n] = g_edit_buf[n]; ++n; }
    pre[n] = L'\0';
    GetTextExtentPoint32W(hdc, pre, n, &sz);
    *x_hi = org + sz.cx;

    SelectObject(hdc, of);
}

/* Drawn *under* the text, never over it. Painting it on top hid the number
 * the user was trying to read and left a solid green block instead. */
static void draw_edit_selection(HDC hdc, const RECT *r, int x_lo, int x_hi)
{
    RECT sel;

    if (x_lo == x_hi)          /* nothing selected */
        return;
    SetRect(&sel, x_lo, r->top + scale(3), x_hi, r->bottom - scale(3));
    fill_round(hdc, &sel, CLR_ACCENT_DK, scale(2));
}

static void draw_edit_caret(HDC hdc, const RECT *r, int x_hi)
{
    RECT car;

    if (!g_caret_on)
        return;
    SetRect(&car, x_hi - scale(1), r->top + scale(3),
                 x_hi + scale(1), r->bottom - scale(3));
    fill_round(hdc, &car, CLR_ACCENT, scale(1));
}

static void draw_slider(HDC hdc, int idx, const DisplayState *d,
                        HFONT f_label, HFONT f_mono)
{
    const RECT *tr = &g_slider_rect[idx];
    const RECT *vr = &g_value_rect[idx];
    RECT lr;
    int v, enabled, neutral, x, span;

    enabled = slider_enabled(idx, d);
    v = slider_value(idx, d);
    neutral = slider_neutral(idx);
    span = tr->right - tr->left;

    SetRect(&lr, scale(PAD), tr->top - scale(ROW_H) / 2,
                 tr->left - scale(10), tr->top + scale(ROW_H) / 2);

    draw_text(hdc, f_label, enabled ? CLR_TEXT : CLR_MUTED, &lr,
              g_sliders[idx].label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    fill_round(hdc, tr, CLR_TRACK, scale(TRACK_H));

    if (enabled && span > 0) {
        int nx = tr->left + span * neutral / 1000;
        int vx = tr->left + span * v / 1000;
        RECT fill = *tr;
        fill.left  = (vx < nx) ? vx : nx;
        fill.right = (vx < nx) ? nx : vx;
        if (fill.right - fill.left >= 1)
            fill_round(hdc, &fill, CLR_ACCENT, scale(TRACK_H));
    }

    if (span > 0) {
        RECT knob;
        int kr = scale(7);
        x = tr->left + span * v / 1000;
        SetRect(&knob, x - kr, tr->top - kr, x + kr, tr->bottom + kr);
        fill_round(hdc, &knob, enabled ? CLR_ACCENT : CLR_TRACK, kr * 2);
    }

    if (g_edit_slider == idx) {
        /* Paint the raw buffer, not the formatted value: a half-typed "1."
         * has to stay visible while it is being typed, and format_value()
         * would round it away to "1.00". */
        RECT box = *vr, text = *vr;
        int x_lo = 0, x_hi = 0;
        InflateRect(&text, -scale(5), 0);
        fill_round(hdc, &box, CLR_CARD, scale(4));
        stroke_rect(hdc, &box, CLR_ACCENT);
        edit_extents(hdc, &box, f_mono, &x_lo, &x_hi);
        draw_edit_selection(hdc, &box, x_lo, x_hi);
        draw_text(hdc, f_mono, CLR_TEXT, &text, g_edit_buf,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        draw_edit_caret(hdc, &box, x_hi);
    } else {
        wchar_t buf[32];
        format_value(idx, d, buf, (int)(sizeof(buf) / sizeof(buf[0])));
        draw_text(hdc, f_mono, enabled ? CLR_MUTED : CLR_BORDER, vr,
                  buf, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
}

static void draw_tabs(HDC hdc, HFONT font)
{
    int i;
    for (i = 0; i < g_state.count && i < MAX_DISPLAYS; ++i) {
        const RECT *r = &g_tab_rect[i];
        int active = (i == g_active_display);
        RECT text = *r;

        fill_round(hdc, r, active ? CLR_ACCENT_DK : CLR_CARD, scale(6));
        if (active) {
            RECT dot;
            SetRect(&dot, r->left + scale(6), r->bottom - scale(3),
                        r->right - scale(6), r->bottom);
            fill_round(hdc, &dot, CLR_ACCENT, scale(3));
        }
        SetRect(&text, r->left, r->top, r->right, r->bottom - scale(3));
        draw_text(hdc, font, active ? CLR_TEXT : CLR_MUTED, &text,
                  g_state.displays[i].label,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

static void draw_button(HDC hdc, const RECT *r, const wchar_t *text, HFONT font)
{
    fill_round(hdc, r, CLR_CARD, scale(6));
    stroke_rect(hdc, r, CLR_BORDER);
    draw_text(hdc, font, CLR_TEXT, r, text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void draw_checkbox(HDC hdc, const RECT *r, const wchar_t *text,
                          HFONT font, int checked)
{
    RECT box, tr;
    int s = scale(16);

    SetRect(&box, r->left, r->top + ((r->bottom - r->top) - s) / 2,
                 r->left + s, r->top + ((r->bottom - r->top) + s) / 2);

    fill_round(hdc, &box, checked ? CLR_ACCENT_DK : CLR_CARD, scale(4));
    stroke_rect(hdc, &box, checked ? CLR_ACCENT : CLR_BORDER);

    if (checked) {
        HGDIOBJ op = SelectObject(hdc, g_pen_thick);
        MoveToEx(hdc, box.left + s / 4, box.top + s / 2, NULL);
        LineTo(hdc, box.left + s * 2 / 5, box.bottom - s / 4);
        LineTo(hdc, box.right - s / 4, box.top + s / 4);
        SelectObject(hdc, op);
    }

    tr = *r;
    tr.left = box.right + scale(10);
    draw_text(hdc, font, CLR_TEXT, &tr, text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void on_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc, hr, sr;
    int i;
    const DisplayState *d;

    GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, g_br_bg);

    if (g_state.count <= 0) {
        draw_text(hdc, g_f_small, CLR_MUTED, &rc,
                  g_status[0] ? g_status : L"No NVIDIA displays were found.",
                  DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        EndPaint(hwnd, &ps);
        return;
    }

    d = &g_state.displays[g_active_display];

    draw_tabs(hdc, g_f_small);

    SetRect(&hr, scale(PAD), scale(PAD) + scale(TAB_H) + scale(4),
                 rc.right - scale(PAD), scale(PAD) + scale(TAB_H) + scale(5));
    FillRect(hdc, &hr, g_br_border);

    for (i = 0; i < SL_COUNT; ++i)
        draw_slider(hdc, i, d, g_f_label, g_f_mono);

    draw_button(hdc, &g_reset_rect,    L"Reset display", g_f_small);
    draw_button(hdc, &g_resetall_rect, L"Reset all",     g_f_small);
    draw_checkbox(hdc, &g_applyall_rect, L"Apply to every display", g_f_small,
                  g_state.apply_to_all);

    SetRect(&sr, scale(PAD), g_applyall_rect.bottom + scale(14),
                 rc.right - scale(PAD), rc.bottom);
    draw_text(hdc, g_f_small, g_status[0] ? CLR_MUTED : CLR_BORDER, &sr,
              g_status[0] ? g_status : L"Adjustments apply as you drag.",
              DT_LEFT | DT_TOP | DT_WORDBREAK);

    EndPaint(hwnd, &ps);
}

/* --- input ------------------------------------------------------------- */

static void on_mouse_down(int x, int y)
{
    POINT p;
    int i;

    p.x = x; p.y = y;

    /* A press outside the field being edited abandons it without applying,
     * and the press then goes on to do whatever it was aimed at. A press
     * inside the same field is swallowed, so a stray double-click cannot
     * interrupt someone halfway through typing a number. */
    if (g_edit_slider >= 0) {
        RECT box;
        value_hit(g_edit_slider, &box);
        if (point_in(&box, p))
            return;
        edit_cancel();
    }

    if (g_state.count > 0) {
        for (i = 0; i < SL_COUNT; ++i) {
            RECT box;
            value_hit(i, &box);
            if (point_in(&box, p)) {
                edit_begin(i);   /* refuses, and explains, if unavailable */
                return;
            }
        }
    }

    for (i = 0; i < g_state.count && i < MAX_DISPLAYS; ++i) {
        if (point_in(&g_tab_rect[i], p)) {
            g_active_display = i;
            g_drag_slider = -1;
            set_status(L"");
            return;
        }
    }

    for (i = 0; i < SL_COUNT; ++i) {
        RECT hit;
        slider_hit(i, &hit);
        if (point_in(&hit, p)) {
            if (!slider_enabled(i, &g_state.displays[g_active_display])) {
                set_status(L"This control is unavailable on the selected display.");
                return;
            }
            g_drag_slider = i;
            SetCapture(g_hwnd);
            if (g_slider_rect[i].right > g_slider_rect[i].left) {
                int span = g_slider_rect[i].right - g_slider_rect[i].left;
                set_slider(i, (x - g_slider_rect[i].left) * 1000 / span);
            }
            return;
        }
    }

    if (g_state.count <= 0)
        return;

    if (point_in(&g_reset_rect, p))         { do_reset(g_active_display, g_active_display); return; }
    if (point_in(&g_resetall_rect, p))      { do_reset(0, g_state.count - 1); return; }
    if (point_in(&g_applyall_rect, p))      {
        g_state.apply_to_all = !g_state.apply_to_all;
        state_save();
        set_status(g_state.apply_to_all ? L"Changes now apply to every display."
                                       : L"Changes apply to the selected display only.");
        return;
    }
}

static void on_double_click(int x, int y)
{
    POINT p;
    int i;
    DisplayState *d;

    /* WM_LBUTTONDBLCLK arrives after WM_LBUTTONDOWN, which already took
     * capture and set g_drag_slider. Without releasing it here, the very next
     * WM_MOUSEMOVE re-dragged the slider straight back and the reset appeared
     * to do nothing. */
    if (g_drag_slider >= 0) {
        g_drag_slider = -1;
        ReleaseCapture();
    }

    p.x = x; p.y = y;
    d = &g_state.displays[g_active_display];

    for (i = 0; i < SL_COUNT; ++i) {
        RECT hit;
        slider_hit(i, &hit);
        if (point_in(&hit, p) && slider_enabled(i, d)) {
            int v = slider_default(i);
            int m = apply_mask_for_slider(i);
            if (g_state.apply_to_all) {
                int k;
                for (k = 0; k < g_state.count; ++k) {
                    slider_set(i, &g_state.displays[k], v);
                    state_apply(k, m);
                }
            } else {
                slider_set(i, d, v);
                state_apply(g_active_display, m);
            }
            set_status(L"");
            /* Persist now. Every other mutating path saves; without this the
             * reset survived only because WM_CLOSE happens to save, so a crash
             * or a kill discarded it. */
            state_save();
            return;
        }
    }
}

static void on_wheel(int delta)
{
    int idx = (g_drag_slider >= 0) ? g_drag_slider : g_hover_slider;
    DisplayState *d = &g_state.displays[g_active_display];

    /* Scrolling over the tabs, buttons or status text used to silently move
     * Vibrance, which is surprising and effectively impossible to undo. */
    if (idx < 0 || idx >= SL_COUNT)
        return;

    /* A stray wheel notch while a field is open would move the value by a
     * step the user never asked for, mid-word. */
    if (g_edit_slider >= 0)
        return;

    if (!slider_enabled(idx, d))
        return;

    set_slider(idx, slider_value(idx, d) + (delta > 0 ? 1 : -1) * slider_step(idx, d));
}

/* --- window proc ------------------------------------------------------- */

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_dpi = dpi_for(hwnd);
        create_paint_cache();
        compute_layout(hwnd);
        SetTimer(hwnd, TIMER_ID, REAPPLY_MS, NULL);
        return 0;

    case WM_DPICHANGED: {
        UINT n = HIWORD(wp);
        RECT *r = (RECT *)lp;
        if (n) g_dpi = n;
        if (r)
            SetWindowPos(hwnd, NULL, r->left, r->top,
                         r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        /* Everything cached is sized from g_dpi, so it is all stale now. */
        delete_paint_cache();
        create_paint_cache();
        compute_layout(hwnd);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        on_paint(hwnd);
        return 0;

    case WM_SIZE:
        compute_layout(hwnd);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *m = (MINMAXINFO *)lp;
        int tabs = g_state.count > 0 ? g_state.count : 1;
        int need = scale(PAD) * 2 + tabs * scale(70);
        m->ptMinTrackSize.x = need > scale(440) ? need : scale(440);
        m->ptMinTrackSize.y = scale(470);
        return 0;
    }

    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        on_mouse_down(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_LBUTTONDBLCLK:
        on_double_click(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_MOUSEMOVE: {
        POINT hp;
        hp.x = GET_X_LPARAM(lp);
        hp.y = GET_Y_LPARAM(lp);
        g_hover_slider = -1;
        {
            int hi;
            for (hi = 0; hi < SL_COUNT; ++hi) {
                RECT hit;
                slider_hit(hi, &hit);
                if (point_in(&hit, hp)) { g_hover_slider = hi; break; }
            }
        }
        if (g_drag_slider >= 0 && g_slider_rect[g_drag_slider].right > g_slider_rect[g_drag_slider].left) {
            int span = g_slider_rect[g_drag_slider].right - g_slider_rect[g_drag_slider].left;
            set_slider(g_drag_slider,
                       (GET_X_LPARAM(lp) - g_slider_rect[g_drag_slider].left) * 1000 / span);
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (g_drag_slider >= 0) {
            g_drag_slider = -1;
            ReleaseCapture();
            state_save();
        }
        return 0;

    case WM_CAPTURECHANGED:
        /* Alt+Tab can steal capture mid-drag; without this the next
         * WM_MOUSEMOVE teleports the slider to wherever the pointer is. */
        g_drag_slider = -1;
        return 0;

    case WM_MOUSEWHEEL:
        on_wheel(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;

    case WM_CHAR:
        /* Falls through to DefWindowProc when no field is open, so Alt+Space
         * and the rest of the system menu still work normally. */
        if (on_edit_char(wp))
            return 0;
        break;

    case WM_KEYDOWN:
        if (on_edit_key(wp))
            return 0;
        break;

    case WM_DISPLAYCHANGE:
    case WM_DEVICECHANGE:
        PostMessage(hwnd, MSG_REAPPLY, 0, 0);
        return 0;

    case MSG_REAPPLY: {
        /* A cached DC for a monitor that has been unplugged is worse than
         * useless, and \.\DISPLAY1 may now be a different panel entirely. */
        int i;
        /* The display set can change underneath an open field, and the active
         * tab can move, so drop it rather than leave it pointing at whichever
         * display now occupies the old index. */
        edit_cancel();
        g_state.count = state_renumerate();
        if (g_state.count > 0 && g_active_display >= g_state.count)
            g_active_display = g_state.count - 1;

        /* Re-arm the ramp timer. The latch in WM_TIMER kills it on a
         * persistent GetDeviceGammaRamp failure, and nothing brought it
         * back, so drift recovery was off for the rest of the session. */
        g_ramp_fail_streak = 0;
        g_drift_streak = 0;
        SetTimer(hwnd, TIMER_ID, REAPPLY_MS, NULL);

        /* A topology change can leave the driver briefly unable to answer
         * GetDVCInfo/GetHUEInfo, which would grey those sliders out for good.
         * Probe once more shortly after things settle. */
        SetTimer(hwnd, TIMER_REPROBE, 1500, NULL);

        for (i = 0; i < g_state.count; ++i)
            state_apply(i, APPLY_ALL);
        compute_layout(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case TIMER_REPROBE: {
        /* One-shot: re-enumerate and re-apply, then get out of the way. */
        int i;
        KillTimer(hwnd, TIMER_REPROBE);
        edit_cancel();
        g_state.count = state_renumerate();
        if (g_state.count > 0 && g_active_display >= g_state.count)
            g_active_display = g_state.count - 1;
        for (i = 0; i < g_state.count; ++i)
            state_apply(i, APPLY_ALL);
        compute_layout(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case TIMER_CARET:
        if (g_edit_slider < 0) {
            KillTimer(hwnd, TIMER_CARET);
            return 0;
        }
        g_caret_on = !g_caret_on;
        /* Repaint only the field, so a blink does not walk over the whole
         * window twice a second. */
        if (g_edit_slider < SL_COUNT)
            InvalidateRect(hwnd, &g_value_rect[g_edit_slider], FALSE);
        return 0;

    case WM_TIMER: {
        int i, drift = 0, unreadable = 0;

        if (g_state.count <= 0)
            return 0;

        for (i = 0; i < g_state.count; ++i) {
            HDC hdc = state_display_dc(i);
            int m;
            if (!hdc)
                continue;
            m = gamma_ramp_matches(hdc, &g_state.displays[i].gamma);
            if (m < 0) { unreadable = 1; break; }
            if (m == 0) { drift = 1; break; }
        }

        if (!drift && !unreadable) {
            g_ramp_fail_streak = 0;
            g_drift_streak = 0;
            return 0;
        }

        /* The ramp could not be read at all - RDP, HDR, some docks. Give up
         * rather than re-push forever, and say what actually happened rather
         * than blaming a read failure on whatever merely changed it. */
        if (unreadable) {
            if (++g_ramp_fail_streak > 4) {
                KillTimer(hwnd, TIMER_ID);
                set_status(L"This display's gamma ramp cannot be read here; "
                           L"automatic re-apply stopped.");
            }
            return 0;
        }

        /* Readable but genuinely different: a mode change, display sleep or a
         * fullscreen game reset it. Re-apply - but not forever, or a game that
         * holds its own ramp gets fought twice a second for the life of the
         * process. 30 ticks is about a minute of real drift. */
        if (++g_drift_streak > 30) {
            KillTimer(hwnd, TIMER_ID);
            set_status(L"Another application keeps resetting the gamma ramp; "
                       L"automatic re-apply stopped.");
            return 0;
        }

        /* Only the GDI ramp can be detected as drifted, so only the GDI ramp
         * is pushed. This used to be APPLY_ALL, which meant two NVAPI writes
         * costing ~3.6 ms each - on every display, every 2 seconds - to fix a
         * symptom they cannot affect. */
        for (i = 0; i < g_state.count; ++i)
            state_apply(i, APPLY_RAMP);
        return 0;
    }

    case WM_CLOSE:
        edit_cancel();
        state_save();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID);
        KillTimer(hwnd, TIMER_CARET);
        delete_paint_cache();
        state_release_dcs();
        nvapi_close();
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    /* WM_CHAR and WM_KEYDOWN break out of the switch when no value field is
     * open, so that Alt+Space, accelerators and the rest of the system menu
     * still reach DefWindowProcW. Without a return here they fell off the end
     * of the function and handed the caller whatever happened to be in EAX. */
    return DefWindowProcW(hwnd, msg, wp, lp);
}


/* --- headless self-test ------------------------------------------------- */

static void resolve_ini_path(wchar_t *out, int cch);

/* Runs without a window, without admin rights and without an NVIDIA GPU, so
 * the release build can be smoke-tested on any CI runner, and so a user can
 * diagnose "why is nothing happening" without a debugger.
 *
 * Exit code: 0 = ran to completion (see report for what was found),
 *            1 = an internal inconsistency was found. */

/* This is a GUI-subsystem binary, so when it is launched from cmd.exe it has
 * no console attached and printf() output would vanish. Borrow the parent
 * console so --selftest is actually visible to the person running it. */
static void attach_parent_console(void)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    FILE *f;

    setvbuf(stdout, NULL, _IONBF, 0);

    /* If a stdout was already inherited - a real console, or a redirect to a
     * file or pipe - use it as-is. Reopening CONOUT$ unconditionally would
     * throw that redirect away, so `nvcolor.exe --selftest > out.txt` would
     * silently produce an empty file. */
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN)
        return;

    /* GUI subsystem: nothing is attached when launched from Explorer, or
     * from a shell that has no console of its own. */
    if (!AttachConsole(ATTACH_PARENT_PROCESS) && !AllocConsole())
        return;

    if (freopen_s(&f, "CONOUT$", "w", stdout) != 0)
        return;
    if (freopen_s(&f, "CONOUT$", "w", stderr) != 0)
        return;
}

static int ramp_is_monotonic(const WORD r[3][GAMMA_STEPS])
{
    int c, i;
    for (c = 0; c < 3; ++c)
        for (i = 1; i < GAMMA_STEPS; ++i)
            if (r[c][i] < r[c][i-1]) return 0;
    return 1;
}

static int selftest_gamma(void)
{
    GammaParams p;
    WORD r[3][GAMMA_STEPS];
    int bad = 0, c, i, v;

    gamma_defaults(&p);
    gamma_build(&p, r);

    for (c = 0; c < 3; ++c) {
        if (r[c][0] != 0 || r[c][GAMMA_STEPS - 1] != 65535) bad = 1;
        for (i = 0; i < GAMMA_STEPS; ++i) {
            long want = (long)i * 65535L / (GAMMA_STEPS - 1);
            if (r[c][i] != (WORD)want) bad = 1;
        }
    }
    if (bad) { printf("  [FAIL] default ramp is not the identity\n"); return 1; }
    printf("  [ok]   default ramp is the identity\n");

    for (v = 0; v <= 1000; v += 25) {
        int k;
        for (k = 0; k < 4; ++k) {
            gamma_defaults(&p);
            if (k == 0)      p.brightness  = v;
            else if (k == 1) p.contrast    = v;
            else if (k == 2) p.gamma       = v;
            else             p.temperature = v - 500;
            gamma_build(&p, r);
            if (!ramp_is_monotonic(r)) {
                printf("  [FAIL] non-monotonic ramp at v=%d k=%d\n", v, k);
                bad = 1;
            }
        }
    }
    if (!bad) printf("  [ok]   every slider extreme stays monotonic\n");

    {
        /* A ramp that is valid but solid would mean a wrecked display. */
        static const struct { int b, c, g, t; } hostile[] = {
            { 99999, 500, 500, -99999 },
            { 500, -99999, 500, 99999 },
            { -500, 500, -500, 500 },
            { 100000, 100000, 100000, 100000 },
        };
        for (i = 0; i < (int)(sizeof(hostile) / sizeof(hostile[0])); ++i) {
            GammaParams h;
            WORD hr[3][GAMMA_STEPS];
            int solid = 1;
            h.brightness = hostile[i].b; h.contrast = hostile[i].c;
            h.gamma = hostile[i].g; h.temperature = hostile[i].t;
            gamma_build(&h, hr);
            for (c = 0; c < 3; ++c) {
                WORD first = hr[c][0];
                int j;
                for (j = 1; j < GAMMA_STEPS; ++j)
                    if (hr[c][j] != first) { solid = 0; break; }
            }
            if (solid) {
                printf("  [FAIL] hostile params produced a solid ramp (case %d)\n", i);
                bad = 1;
            }
        }
        if (!bad) printf("  [ok]   out-of-range params cannot produce a solid ramp\n");
    }

    return bad;
}

static int selftest(void)
{
    char err[256];
    wchar_t path[MAX_PATH];
    int bad = 0;

    printf("nvcolor self-test\n");
    printf("------------------\n");

    printf("[gamma ramp]\n");
    bad |= selftest_gamma();

    printf("[settings]\n");
    resolve_ini_path(path, (int)(sizeof(path) / sizeof(path[0])));
    printf("  [ok]   ini path: %ls\n", path);
    printf("  [ok]   settings read/write and display DC paths not exercised here\n");

    printf("[nvapi]\n");
    err[0] = '\0';
    if (!nvapi_open(err, (int)sizeof(err))) {
        /* Expected on a machine with no NVIDIA driver, including CI runners. */
        printf("  [--]   unavailable: %s\n", err);
        printf("\nNo NVIDIA driver present, so the display half was skipped.\n");
        printf("%s\n", bad ? "RESULT: FAILED" : "RESULT: OK (gamma ramp verified)");
        return bad;
    }

    printf("  [ok]   nvapi64.dll loaded and initialised\n");
    printf("  [ok]   core functions bound (enumerate, name, error text)\n");
    printf("  [%s] digital vibrance (private id 0x172409B4)\n",
           nvapi_dvc_available() ? "ok" : "--");
    printf("  [%s] hue            (private id 0x0F5A0F22C)\n",
           nvapi_hue_available() ? "ok" : "--");

    {
        int n = nvapi_display_count();
        if (n <= 0) {
            printf("  [--]   no NVIDIA displays attached\n");
        } else {
            int i;
            printf("  [ok]   %d NVIDIA display(s) enumerated\n", n);
            for (i = 0; i < n && i < MAX_DISPLAYS; ++i) {
                void *h = NULL;
                char nm[NVAPI_SHORT_STRING_MAX];
                nm[0] = '\0';
                if (g_nvapi.enumDisplay((NvU32)i, &h) == NVAPI_OK && h && g_nvapi.getDisplayName)
                    g_nvapi.getDisplayName(h, nm);
                printf("         [%d] %s\n", i, nm[0] ? nm : "(unnamed)");
            }
            if (g_state.count == 0) {
                printf("  [--]   display state not loaded (headless mode)\n");
            }
        }
    }

    nvapi_close();
    printf("\n%s\n", bad ? "RESULT: FAILED" : "RESULT: OK");
    return bad;
}

/* --- entry ------------------------------------------------------------- */

static void resolve_ini_path(wchar_t *out, int cch)
{
    wchar_t appdata[MAX_PATH];
    wchar_t dir[MAX_PATH];
    DWORD n;

    if (!out || cch <= 0)
        return;
    n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);

    if (n == 0 || n >= MAX_PATH) {
        lstrcpynW(out, L"nvcolor.ini", cch);
        return;
    }
    appdata[MAX_PATH - 1] = L'\0';

    NV_SNWPRINTF(dir, MAX_PATH, L"%s\\nvcolor", appdata);
    dir[MAX_PATH - 1] = L'\0';
    CreateDirectoryW(dir, NULL);

    NV_SNWPRINTF(out, cch, L"%s\\settings.ini", dir);
    out[cch - 1] = L'\0';
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSEXW wc;
    MSG msg;
    wchar_t path[MAX_PATH];
    wchar_t err[256];
    char cerr[256];
    RECT wr;
    int x, y, w, h;

    (void)prev; (void)show;

    /* Headless modes first: these must work with no display, no GPU and no
     * window, so CI can smoke-test the shipped binary and users can diagnose
     * themselves. */
    /* Only attach a console for a recognised headless command. Doing it
     * before validating meant an unrecognised argument left a black console
     * window sitting on screen for the whole life of the GUI process. */
    cmd = trim_command_line(cmd);

    if (cmd && cmd[0]) {
        if (_wcsicmp(cmd, L"--selftest") == 0 || _wcsicmp(cmd, L"/selftest") == 0) {
            attach_parent_console();
            return selftest();
        }
        if (_wcsicmp(cmd, L"--version") == 0 || _wcsicmp(cmd, L"/version") == 0) {
            attach_parent_console();
            printf("nvcolor %s\n", NVCOLOR_VERSION);
            return 0;
        }
        if (_wcsicmp(cmd, L"--help") == 0 || _wcsicmp(cmd, L"/?") == 0 ||
            _wcsicmp(cmd, L"-h") == 0) {
            attach_parent_console();
            printf("nvcolor %s - NVIDIA display colour controls\n\n", NVCOLOR_VERSION);
            printf("  nvcolor.exe            open the colour window\n");
            printf("  nvcolor.exe --selftest verify the build, no GPU needed\n");
            printf("  nvcolor.exe --version  print the version\n");
            printf("  nvcolor.exe --help     this text\n\n");
            printf("Settings: %%APPDATA%%\\nvcolor\\settings.ini\n");
            return 0;
        }
        /* This program takes no other arguments. Anything else that is not
         * empty is an error: reporting it is correct, and falling through to
         * the GUI is an infinite message loop with nobody able to close it.
         * No modal dialog here - a bad command line is not a GUI event. */
        attach_parent_console();
        fprintf(stderr, "nvcolor: unrecognised argument '%ls'\n", cmd);
        fprintf(stderr, "Try 'nvcolor.exe --help'.\n");
        return 2;
    }

    enable_dpi_awareness();
    g_dpi = dpi_for_system();

    if (!nvapi_open(cerr, (int)sizeof(cerr))) {
        NV_SNWPRINTF(err, 256, L"nvcolor could not initialise.\n\n%hs", cerr);
        err[255] = L'\0';
        MessageBoxW(NULL, err, APP_TITLE, MB_OK | MB_ICONERROR);
        return 1;
    }

    resolve_ini_path(path, (int)(sizeof(path) / sizeof(path[0])));
    state_load(path);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = WNDCLASS_NAME;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) {
        nvapi_close();
        return 1;
    }

    w = scale(470); h = scale(500);
    x = GetSystemMetrics(SM_CXSCREEN) - w - scale(48);
    y = GetSystemMetrics(SM_CYSCREEN) - h - scale(48);
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    SetRect(&wr, 0, 0, w, h);
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    g_hwnd = CreateWindowExW(0, WNDCLASS_NAME, APP_TITLE, WS_OVERLAPPEDWINDOW,
                             x, y, wr.right - wr.left, wr.bottom - wr.top,
                             NULL, NULL, inst, NULL);
    if (!g_hwnd) {
        nvapi_close();
        return 1;
    }

    g_dpi = dpi_for(g_hwnd);
    compute_layout(g_hwnd);

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    if (g_state.count <= 0) {
        set_status(L"No NVIDIA displays were found.");
    } else {
        int i, no_ramp = 0;
        for (i = 0; i < g_state.count; ++i)
            state_apply(i, APPLY_ALL);
        /* A NULL display DC makes brightness/contrast/gamma/temperature do
         * nothing at all, silently. Say so rather than letting the user
         * conclude the build is broken. */
        for (i = 0; i < g_state.count; ++i) {
            if (state_display_dc_failed(i)) { no_ramp = 1; break; }
        }
        if (no_ramp)
            set_status(L"Brightness/contrast/gamma are unavailable on at least "
                       L"one display (no GDI display context).");
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
