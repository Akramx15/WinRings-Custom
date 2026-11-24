#pragma once

#include <Windows.h>
#include <stdint.h>

#define WINRINGS_PRODUCT_NAME_W             L"WinRings"
#define WINRINGS_PRODUCT_NAME_A             "WinRings"
#define WINRINGS_HOST_FILENAME_W            L"WinRingsHost.exe"
#define WINRINGS_SHELL_FILENAME_W           L"WinRingsShell.dll"
#define WINRINGS_TOOLS_FILENAME_W           L"WinRingsTools.exe"
#define WINRINGS_SETUP_FILENAME_W           L"WinRingsSetup.exe"

#define WINRINGS_HOST_WINDOW_CLASS_W        L"WinRings.Host.Window.v1"
#define WINRINGS_HOST_WINDOW_TITLE_W        L"WinRingsHost"
#define WINRINGS_SHELL_CONTROL_CLASS_W      L"WinRings.Shell.Control.v1"
#define WINRINGS_OVERLAY_CLASS_W            L"WinRings.Overlay.v1"
#define WINRINGS_THUMBCARD_CLASS_W          L"WinRings.ThumbCard.v1"

#define WINRINGS_HOST_MUTEX_W               L"Local\\WinRings.Host.Instance.v1"
#define WINRINGS_SETUP_MUTEX_W              L"Local\\WinRings.Setup.Instance.v1"
#define WINRINGS_HOST_ATTEMPT_COOKIE_PROP_W L"WinRings.Host.AttemptCookie.v1"
#define WINRINGS_HOST_SAFETY_REGISTRY_W     L"Software\\WinRings\\HostSafety"
#define WINRINGS_HOST_SAFETY_MUTEX_PREFIX_W L"Global\\WinRings.HostSafety.v1."

#define WINRINGS_HOST_CONTROL_MESSAGE_W     L"WinRings.Host.Control.v1"
#define WINRINGS_SHELL_STATUS_MESSAGE_W     L"WinRings.Shell.Status.v1"
#define WINRINGS_SHELL_COMMAND_MESSAGE_W    L"WinRings.Shell.Command.v1"
#define WINRINGS_TASKBAR_CREATED_MESSAGE_W  L"TaskbarCreated"

#define WINRINGS_GET_API_EXPORT_A           "WinRings_GetApi"
#define WINRINGS_RUN_PREVIEW_EXPORT_A       "WinRings_RunPreview"

#define WINRINGS_PRODUCT_VERSION_MAJOR      3
#define WINRINGS_PRODUCT_VERSION_MINOR      0
#define WINRINGS_PRODUCT_VERSION_PATCH      4
#define WINRINGS_PRODUCT_VERSION_BUILD      0
#define WINRINGS_PRODUCT_VERSION_STR_A      "3.0.4.0"
#define WINRINGS_PRODUCT_VERSION_STR_W      L"3.0.4.0"

// Private, unregistered COM class for Win11 XAML diagnostics site.
inline constexpr CLSID WinRingsXamlDiagClass =
{ 0x9b4f3a12, 0x7e84, 0x4c61, { 0xa5, 0xd2, 0x38, 0xf0, 0xc7, 0x1e, 0x94, 0x2b } };
