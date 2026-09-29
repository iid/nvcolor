/* nvapi_min.c - dynamic loading of nvapi64.dll and the nvapi_min.h bindings. */
#include "nvapi_min.h"
#include <stdio.h>
#include <string.h>

NvApi g_nvapi;

typedef struct { NvU32 id; void **slot; } FnBind;

static int bind_all(char *err, int errLen)
{
    static const FnBind binds[] = {
        { FN_NvAPI_Initialize,                     (void **)&g_nvapi.initialize     },
        { FN_NvAPI_Unload,                         (void **)&g_nvapi.unload         },
        { FN_NvAPI_EnumNvidiaDisplayHandle,        (void **)&g_nvapi.enumDisplay    },
        { FN_NvAPI_GetAssociatedNvidiaDisplayName, (void **)&g_nvapi.getDisplayName },
    };
    size_t i;

    for (i = 0; i < sizeof(binds) / sizeof(binds[0]); ++i) {
        void *p = g_nvapi.query(binds[i].id);
        if (!p) {
            if (err)
                snprintf(err, errLen,
                         "nvapi_QueryInterface failed for a core function "
                         "(id 0x%08X). Driver may be missing or unsupported.", binds[i].id);
            return 0;
        }
        *binds[i].slot = p;
    }

    /* Optional / unused. */
    g_nvapi.getErrorMessage = (PFN_GetErrorMessage)g_nvapi.query(FN_NvAPI_GetErrorMessage);

    /* Private functions: optional. */
    g_nvapi.getDvcInfo    = (PFN_GetDVCInfo)   g_nvapi.query(FN_NvAPI_GetDVCInfo);
    g_nvapi.getDvcInfoEx  = (PFN_GetDVCInfoEx) g_nvapi.query(FN_NvAPI_GetDVCInfoEx);
    g_nvapi.setDvcLevel   = (PFN_SetDVCLevel)  g_nvapi.query(FN_NvAPI_SetDVCLevel);
    g_nvapi.setDvcLevelEx = (PFN_SetDVCLevelEx)g_nvapi.query(FN_NvAPI_SetDVCLevelEx);
    g_nvapi.getHueInfo    = (PFN_GetHUEInfo)   g_nvapi.query(FN_NvAPI_GetHUEInfo);
    g_nvapi.setHueAngle   = (PFN_SetHUEAngle)  g_nvapi.query(FN_NvAPI_SetHUEAngle);

    return 1;
}

int nvapi_open(char *err, int errLen)
{
    PFN_QueryInterface query;

    if (g_nvapi.dll)
        return 1;

    /* LOAD_LIBRARY_SEARCH_SYSTEM32, not a bare LoadLibraryA. A bare name
     * searches the application directory first, and this ships as an unsigned
     * single EXE that users unzip somewhere writable - so a planted
     * nvapi64.dll sitting next to it would be loaded. Low severity here (the
     * manifest is asInvoker, so there is no token to steal, and anyone who
     * can write there can just replace the EXE) but it costs nothing to be
     * correct.
     *
     * The flag needs KB2533623, which predates Windows 7; systems without it
     * reject the call, so fall back to the plain load rather than fail. */
    g_nvapi.dll = LoadLibraryExW(L"nvapi64.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_nvapi.dll)
        g_nvapi.dll = LoadLibraryExW(L"nvapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_nvapi.dll) {
        g_nvapi.dll = LoadLibraryW(L"nvapi64.dll");
        if (!g_nvapi.dll)
            g_nvapi.dll = LoadLibraryW(L"nvapi.dll");
    }

    if (!g_nvapi.dll) {
        if (err)
            snprintf(err, errLen,
                     "Could not load nvapi64.dll. Is an NVIDIA driver installed?");
        return 0;
    }

    query = (PFN_QueryInterface)(void *)GetProcAddress(g_nvapi.dll, "nvapi_QueryInterface");
    if (!query) {
        if (err)
            snprintf(err, errLen, "nvapi_QueryInterface export not found.");
        FreeLibrary(g_nvapi.dll);
        g_nvapi.dll = NULL;
        return 0;
    }
    g_nvapi.query = query;

    /* nvapi_QueryInterface is usable straight from the DLL export, but every
     * other NvAPI call requires NvAPI_Initialize first - so bind the
     * pointers, then initialise, then hand out the handles. */
    if (!bind_all(err, errLen)) {
        nvapi_close();
        return 0;
    }

    if (g_nvapi.initialize() != NVAPI_OK) {
        if (err)
            snprintf(err, errLen, "NvAPI_Initialize failed.");
        nvapi_close();
        return 0;
    }
    return 1;
}

void nvapi_close(void)
{
    if (g_nvapi.unload)
        g_nvapi.unload();

    if (g_nvapi.dll) {
        FreeLibrary(g_nvapi.dll);
        g_nvapi.dll = NULL;
    }
    /* Do not leave pointers into the unloaded module behind. */
    memset(&g_nvapi, 0, sizeof(g_nvapi));
}

int nvapi_dvc_available(void)
{
    return g_nvapi.getDvcInfo && g_nvapi.setDvcLevel;
}

int nvapi_hue_available(void)
{
    return g_nvapi.getHueInfo && g_nvapi.setHueAngle;
}

int nvapi_display_count(void)
{
    void *handle = NULL;
    int n = 0;

    if (!g_nvapi.enumDisplay)
        return 0;

    while (g_nvapi.enumDisplay((NvU32)n, &handle) == NVAPI_OK && handle) {
        ++n;
        if (n >= MAX_DISPLAYS) /* defensive: driver should never report more */
            break;
    }
    return n;
}
