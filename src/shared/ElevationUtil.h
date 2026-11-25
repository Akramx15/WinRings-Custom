#pragma once

#include <Windows.h>

namespace winrings
{
    inline bool IsCurrentProcessElevated() noexcept
    {
        HANDLE hToken = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken) || !hToken)
            return false;

        TOKEN_ELEVATION elev{};
        DWORD retLen = 0;
        const BOOL ok = GetTokenInformation(hToken, TokenElevation, &elev, sizeof(elev), &retLen);
        CloseHandle(hToken);
        return ok != FALSE && elev.TokenIsElevated != 0;
    }

    inline bool IsProcessElevated(DWORD pid) noexcept
    {
        if (pid == 0)
            return false;

        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProc)
            return GetLastError() == ERROR_ACCESS_DENIED;

        bool elevated = false;
        HANDLE hToken = nullptr;
        if (OpenProcessToken(hProc, TOKEN_QUERY, &hToken) && hToken)
        {
            TOKEN_ELEVATION elev{};
            DWORD retLen = 0;
            if (GetTokenInformation(hToken, TokenElevation, &elev, sizeof(elev), &retLen))
                elevated = (elev.TokenIsElevated != 0);
            CloseHandle(hToken);
        }
        else if (GetLastError() == ERROR_ACCESS_DENIED)
        {
            elevated = true;
        }
        CloseHandle(hProc);
        return elevated;
    }

    inline bool IsWindowElevated(HWND hwnd) noexcept
    {
        if (!hwnd || !IsWindow(hwnd))
            return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        return IsProcessElevated(pid);
    }

    inline bool IsInputDesktopAccessible() noexcept
    {
        HDESK hInput = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
        if (!hInput)
            return false;
        CloseDesktop(hInput);
        return true;
    }

    inline bool IsElevatedOrSecureDesktopBlockingMediumHooks(POINT cursorPt) noexcept
    {
        if (!IsInputDesktopAccessible())
            return true;
        if (IsWindowElevated(GetForegroundWindow()))
            return true;
        if (IsWindowElevated(WindowFromPoint(cursorPt)))
            return true;
        return false;
    }

    inline bool ShouldAdminBridgeInterceptInput(
        bool hostElevated,
        bool shellReady,
        bool interactiveInput,
        bool overlayVisible,
        bool foregroundElevated,
        bool cursorWindowElevated,
        bool isKeyboardOrRawInput) noexcept
    {
        if (!hostElevated || !shellReady || !interactiveInput || overlayVisible)
            return false;
        if (foregroundElevated)
            return true;
        if (!isKeyboardOrRawInput && cursorWindowElevated)
            return true;
        return false;
    }
} // namespace winrings
