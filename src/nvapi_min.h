/* nvapi_min.h - minimal NVAPI bindings for nvcolor.
 *
 * Only what we need: display enumeration, Digital Vibrance, and HUE.
 * Loaded dynamically from nvapi64.dll via nvapi_QueryInterface, so this
 * header declares no import library requirements.
 *
 * Function IDs come from falahati/NvAPIWrapper's reverse-engineered table.
 * The DVC/HUE ones are private (not in NVIDIA's public nvapi.h) and may
 * disappear in a future driver branch - every lookup is checked for NULL.
 */
#ifndef NVAPI_MIN_H
#define NVAPI_MIN_H

#include <windows.h>

#include "state_limits.h"

typedef int         NvAPI_Status;
typedef unsigned int NvU32;
typedef int          NvS32;

#define NVAPI_OK                    0
#define NVAPI_SHORT_STRING_MAX      64

/* Public function IDs. */
#define FN_NvAPI_Initialize                    0x150E828
#define FN_NvAPI_Unload                        0xD22BDD7E
#define FN_NvAPI_EnumNvidiaDisplayHandle       0x9ABDD40D
#define FN_NvAPI_GetAssociatedNvidiaDisplayName 0x22A78B05
#define FN_NvAPI_GetErrorMessage               0x6C2D048C

/* Private function IDs (still exported by nvapi64.dll). */
#define FN_NvAPI_GetDVCInfo        0x4085DE45
#define FN_NvAPI_GetDVCInfoEx      0x0E45002D
#define FN_NvAPI_SetDVCLevel       0x172409B4
#define FN_NvAPI_SetDVCLevelEx     0x4A82C2B1
#define FN_NvAPI_GetHUEInfo        0x95B64341
#define FN_NvAPI_SetHUEAngle       0x0F5A0F22C

/* MAKE_NVAPI_VERSION is sizeof-based in the real header:
 *   (NvU32)(sizeof(typeName) | ((ver) << 16))
 * Not a hash of the type name. */
#define MAKE_NVAPI_VERSION(typeName, ver) (NvU32)(sizeof(typeName) | ((ver) << 16))

typedef struct { NvU32 version; NvS32 currentLevel; NvS32 minLevel; NvS32 maxLevel; }       NV_DVC_INFO;
typedef struct { NvU32 version; NvS32 currentLevel; NvS32 defaultLevel; NvS32 minLevel;
                 NvS32 maxLevel; }                                                         NV_DVC_INFO_EX;
typedef struct { NvU32 version; NvS32 currentAngle; NvS32 defaultAngle; }                    NV_HUE_INFO;

#define NV_DVC_INFO_VER     MAKE_NVAPI_VERSION(NV_DVC_INFO, 1)
#define NV_DVC_INFO_EX_VER  MAKE_NVAPI_VERSION(NV_DVC_INFO_EX, 1)
#define NV_HUE_INFO_VER     MAKE_NVAPI_VERSION(NV_HUE_INFO, 1)

typedef NvAPI_Status (WINAPI *PFN_Initialize)(void);
typedef NvAPI_Status (WINAPI *PFN_Unload)(void);
typedef NvAPI_Status (WINAPI *PFN_EnumDisplay)(NvU32 index, void **displayHandle);
typedef NvAPI_Status (WINAPI *PFN_GetDisplayName)(void *displayHandle, char *name);
typedef NvAPI_Status (WINAPI *PFN_GetErrorMessage)(NvAPI_Status status, char *buf);
typedef NvAPI_Status (WINAPI *PFN_GetDVCInfo)(void *displayHandle, NvU32 outputId, NV_DVC_INFO *info);
typedef NvAPI_Status (WINAPI *PFN_GetDVCInfoEx)(void *displayHandle, NvU32 outputId, NV_DVC_INFO_EX *info);
typedef NvAPI_Status (WINAPI *PFN_SetDVCLevel)(void *displayHandle, NvU32 outputId, NvS32 level);
typedef NvAPI_Status (WINAPI *PFN_SetDVCLevelEx)(void *displayHandle, NvU32 outputId, NV_DVC_INFO_EX *info);
typedef NvAPI_Status (WINAPI *PFN_GetHUEInfo)(void *displayHandle, NvU32 outputId, NV_HUE_INFO *info);
typedef NvAPI_Status (WINAPI *PFN_SetHUEAngle)(void *displayHandle, NvU32 outputId, NvS32 angle);

typedef void *(__stdcall *PFN_QueryInterface)(NvU32 functionId);

typedef struct {
    HMODULE             dll;
    PFN_QueryInterface  query;

    PFN_Initialize      initialize;
    PFN_Unload          unload;
    PFN_EnumDisplay     enumDisplay;
    PFN_GetDisplayName  getDisplayName;
    PFN_GetErrorMessage getErrorMessage;

    /* NULL if unavailable - the app degrades instead of crashing. */
    PFN_GetDVCInfo      getDvcInfo;
    PFN_GetDVCInfoEx    getDvcInfoEx;
    PFN_SetDVCLevel     setDvcLevel;
    PFN_SetDVCLevelEx   setDvcLevelEx;
    PFN_GetHUEInfo      getHueInfo;
    PFN_SetHUEAngle     setHueAngle;
} NvApi;

extern NvApi g_nvapi;

/* Returns 1 on success. On failure writes a human-readable reason to `err`. */
int  nvapi_open(char *err, int errLen);
void nvapi_close(void);
int  nvapi_dvc_available(void);
int  nvapi_hue_available(void);

/* Display enumeration. out must hold `max` entries of 64+ bytes. */
int  nvapi_display_count(void);

#endif /* NVAPI_MIN_H */
