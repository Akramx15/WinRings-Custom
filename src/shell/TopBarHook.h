#pragma once

#include <Windows.h>
#include <functional>

namespace winrings
{
    class TopBarHook
    {
    public:
        static bool Install();
        static bool IsHooked() noexcept;
        static bool IsSupportedBuild() noexcept;
        static bool IsTopDockActive() noexcept;

        // Dispatches a callable synchronously onto Shell_TrayWnd's UI thread via a scoped hook.
        static bool RunOnPrimaryTaskbarThread(HMODULE shellModule, std::function<void()> fn, DWORD timeoutMs = 1500);

        // Must be called on Explorer's primary taskbar UI thread when possible.
        // Returns true once applied to a captured ITrayUI instance, false if skipped/retry needed.
        static bool ApplyOnTaskbarUiThread(bool enabled, bool dockTop, int heightDip, bool allMonitors);

        // Cleanly restores original taskbar edge/height on the UI thread, then removes all patches.
        static void RestoreAndUnhook(const std::function<bool(std::function<void()>)>& runOnUiThread);

        static void RefreshSecondaryTaskbars() noexcept;
        static bool InstallSystemTrayHook() noexcept;
        static bool RepositionTrayPopupIfNeeded(HWND hwndPopup, int preferredAnchorX = (-2147483647 - 1)) noexcept;

        // Synchronous Win32 subclass on Shell_TrayWnd (independent of Taskbar.dll PE hooks)
        // to eliminate bottom-edge flashes on Sleep/Wake or DisplayChange when Top Bar is enabled.
        static bool SyncTrayTopLock(
            HMODULE shellModule,
            bool lockTop,
            int heightDip,
            std::function<void()> onTrayWakeOrDisplay = nullptr);
        static void RemoveTrayTopLock(HMODULE shellModule);
    };
} // namespace winrings
