#pragma once

#include <Windows.h>
#include <stdint.h>

#include "WinRingsIdentity.h"

#define WINRINGS_ABI_VERSION      UINT32_C(0x00010000)
#define WINRINGS_PRODUCT_VERSION  UINT32_C(0x00010000)

enum WinRingsDllStatus : UINT32
{
    WINRINGS_DLL_STATUS_READY    = 1,
    WINRINGS_DLL_STATUS_FAILED   = 2,
    WINRINGS_DLL_STATUS_DISARMED = 3,
};

enum WinRingsCommand : UINT32
{
    WINRINGS_CMD_TOGGLE_RING         = 1,
    WINRINGS_CMD_EXIT                = 2,
    WINRINGS_CMD_SUSPEND_INPUT       = 3,
    WINRINGS_CMD_REFRESH_INPUT       = 4,
    WINRINGS_CMD_RELOAD_CONFIG       = 5,
    WINRINGS_CMD_TOGGLE_TOPBAR       = 6,
    WINRINGS_CMD_QUERY_SHELL_STATE   = 7,
    WINRINGS_CMD_TOGGLE_MEDIA_ISLAND = 8,
    WINRINGS_CMD_OPEN_RING_AT        = 9,
    WINRINGS_CMD_DIRECT_SWITCH_STEP  = 10,
    WINRINGS_CMD_DIRECT_SWITCH_COMMIT = 11,
    WINRINGS_CMD_DOUBLE_TAP_LAST_APP = 12,
    WINRINGS_CMD_MEDIA_WHEEL         = 13,
    WINRINGS_CMD_ADMIN_TOGGLE_RING   = 14,
    WINRINGS_CMD_REATTACH            = 0xFFFF,
};

// Bitmask returned by WINRINGS_CMD_QUERY_SHELL_STATE
enum WinRingsShellStateBits : UINT32
{
    WINRINGS_STATE_RING_READY           = 1u << 0,
    WINRINGS_STATE_XAML_ATTACHED        = 1u << 1,
    WINRINGS_STATE_TOPBAR_HOOKED        = 1u << 2,
    WINRINGS_STATE_BACKDROP_ACTIVE      = 1u << 3,
    WINRINGS_STATE_CLOCK_ACTIVE         = 1u << 4,
    WINRINGS_STATE_APPS_HIDDEN          = 1u << 5,
    WINRINGS_STATE_MEDIA_ISLAND_ACTIVE  = 1u << 6,
    WINRINGS_STATE_ADMIN_BRIDGE_ACTIVE  = 1u << 7,
};

#define WINRINGS_COPYDATA_WINDOW_ACTION ((ULONG_PTR)0x57524143u)

enum WinRingsAdminWindowActionKind : UINT32
{
    WINRINGS_ADMIN_ACT_ACTIVATE       = 1,
    WINRINGS_ADMIN_ACT_MINIMIZE       = 2,
    WINRINGS_ADMIN_ACT_CLOSE          = 3,
    WINRINGS_ADMIN_ACT_TERMINATE_PID  = 4,
    WINRINGS_ADMIN_ACT_SET_WINDOW_POS = 5,
    WINRINGS_ADMIN_ACT_MAXIMIZE       = 6,
    WINRINGS_ADMIN_ACT_RESTORE        = 7,
};

struct WinRingsAdminWindowActionPayload
{
    UINT32 cbSize;
    UINT32 action;
    UINT64 attemptCookie;
    UINT64 targetHwnd;
    UINT32 targetPid;
    UINT32 swpFlags;
    INT32 x;
    INT32 y;
    INT32 cx;
    INT32 cy;
};

enum WinRingsHostControl : UINT32
{
    WINRINGS_HOST_CTRL_TOGGLE             = 1,
    WINRINGS_HOST_CTRL_EXIT               = 2,
    WINRINGS_HOST_CTRL_STATUS             = 3,
    WINRINGS_HOST_CTRL_RELOAD             = 4,
    WINRINGS_HOST_CTRL_RESET_SAFETY       = 5,
    WINRINGS_HOST_CTRL_TOGGLE_TOPBAR      = 6,
    WINRINGS_HOST_CTRL_QUERY_DETAILS      = 7,
    WINRINGS_HOST_CTRL_TOGGLE_MEDIA_ISLAND = 8,
};

enum WinRingsHostStatus : UINT32
{
    WINRINGS_HOST_STATUS_STARTING        = 1,
    WINRINGS_HOST_STATUS_INJECTING       = 2,
    WINRINGS_HOST_STATUS_READY           = 3,
    WINRINGS_HOST_STATUS_FAILED          = 4,
    WINRINGS_HOST_STATUS_STOPPING        = 5,
    WINRINGS_HOST_STATUS_SAFETY_DISABLED = 6,
};

typedef BOOL (WINAPI *WinRingsRequestShutdownFn)(HWND controlWindow);

struct WinRingsApi
{
    UINT32 cbSize;
    UINT32 abiVersion;
    UINT32 productVersion;
    UINT32 reserved;
    WinRingsRequestShutdownFn RequestShutdown;
};

typedef const WinRingsApi *(WINAPI *WinRingsGetApiFn)(UINT32 requestedAbi);
typedef int (WINAPI *WinRingsRunPreviewFn)(void);

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef WINRINGS_SHELL_EXPORTS
#define WINRINGS_SHELL_API __declspec(dllexport)
#else
#define WINRINGS_SHELL_API __declspec(dllimport)
#endif

WINRINGS_SHELL_API const WinRingsApi *WINAPI WinRings_GetApi(UINT32 requestedAbi);
WINRINGS_SHELL_API int WINAPI WinRings_RunPreview(void);

#undef WINRINGS_SHELL_API

#ifdef __cplusplus
}
#endif
