#pragma once

#include <Windows.h>
#include <cstdint>

#include "../shared/WinRingsApi.h"

namespace winrings
{
    void ShellDiagLog(const wchar_t* tag, const wchar_t* fmt, ...) noexcept;

    class ShellController
    {
    public:
        static void BootstrapInExplorer(HMODULE shellModule);
        static int RunStandalonePreview(HMODULE shellModule);
        static bool RequestShutdownFromApi(HWND controlWindow) noexcept;
        static bool RequestHostWindowAction(
            WinRingsAdminWindowActionKind action,
            HWND targetHwnd,
            DWORD targetPid = 0,
            int x = 0,
            int y = 0,
            int cx = 0,
            int cy = 0,
            UINT swpFlags = 0) noexcept;
    };
} // namespace winrings
