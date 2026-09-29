/* verify_structs.c - compile-time assertions pinning every NVAPI constant
 * this project depends on. Building this file is the test.
 *
 * NVAPI's MAKE_NVAPI_VERSION is sizeof-based, not a name token, so the
 * version word handed to the driver is a function of struct size.
 */
#include "nvapi_min.h"
#include "gamma.h"
#include "state.h"

#define STATIC_ASSERT(name, cond) typedef char static_assert_##name[(cond) ? 1 : -1]

STATIC_ASSERT(dvc_info_size,    sizeof(NV_DVC_INFO)    == 16);
STATIC_ASSERT(dvc_info_ex_size, sizeof(NV_DVC_INFO_EX) == 20);
STATIC_ASSERT(hue_info_size,    sizeof(NV_HUE_INFO)    == 12);
STATIC_ASSERT(dvc_version,      NV_DVC_INFO_VER        == 0x00010010u);
STATIC_ASSERT(dvc_ex_version,   NV_DVC_INFO_EX_VER     == 0x00010014u);
STATIC_ASSERT(hue_version,      NV_HUE_INFO_VER        == 0x0001000Cu);

STATIC_ASSERT(fn_initialize,     FN_NvAPI_Initialize    == 0x150E828u);
STATIC_ASSERT(fn_unload,         FN_NvAPI_Unload        == 0xD22BDD7Eu);
/* Was the one id in this header that nothing pinned, so the README's "every
 * NVAPI function id" was true of ten of eleven. Optional to use - see the
 * g_nvapi.getErrorMessage lookup - but it is still a magic constant. */
STATIC_ASSERT(fn_get_error_msg,  FN_NvAPI_GetErrorMessage == 0x6C2D048Cu);
STATIC_ASSERT(fn_enum_display,   FN_NvAPI_EnumNvidiaDisplayHandle        == 0x9ABDD40Du);
STATIC_ASSERT(fn_display_name,   FN_NvAPI_GetAssociatedNvidiaDisplayName == 0x22A78B05u);
STATIC_ASSERT(fn_get_dvcinfo,    FN_NvAPI_GetDVCInfo    == 0x4085DE45u);
STATIC_ASSERT(fn_get_dvcinfo_ex, FN_NvAPI_GetDVCInfoEx  == 0x0E45002Du);
STATIC_ASSERT(fn_set_dvc_level,  FN_NvAPI_SetDVCLevel   == 0x172409B4u);
STATIC_ASSERT(fn_set_dvc_lvl_ex, FN_NvAPI_SetDVCLevelEx == 0x4A82C2B1u);
STATIC_ASSERT(fn_get_hueinfo,    FN_NvAPI_GetHUEInfo    == 0x95B64341u);
STATIC_ASSERT(fn_set_hueangle,   FN_NvAPI_SetHUEAngle   == 0x0F5A0F22Cu);

STATIC_ASSERT(gamma_params_size, sizeof(GammaParams) == 16);
STATIC_ASSERT(gamma_steps,       GAMMA_STEPS == 256);
STATIC_ASSERT(short_string_max,  NVAPI_SHORT_STRING_MAX == 64);
STATIC_ASSERT(max_displays,      MAX_DISPLAYS == 16);
