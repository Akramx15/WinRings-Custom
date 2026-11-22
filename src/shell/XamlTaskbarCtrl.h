#pragma once

#include <Windows.h>
#include <cstdint>
#include <functional>

#include "../shared/WinRingsConfig.h"

namespace winrings
{
    class XamlTaskbarCtrl
    {
    public:
        static HRESULT GetClassObject(REFCLSID clsid, REFIID iid, void** result);
        static bool StartAttachment(HMODULE shellModule, const SwitcherConfig& initialConfig,
            std::function<void()> onFirstAttached = nullptr);
        static void EnsureAttachRetry(HMODULE shellModule, const SwitcherConfig& config);
        static void ApplyConfig(const SwitcherConfig& config);
        static void RestoreAll();

        static bool IsAttached() noexcept;
        static UINT32 GetStateBits() noexcept;
        static int GetSystemTrayWidthDip() noexcept;
        static void SetTrayMetricsCallback(std::function<void()> onTrayMetricsChanged);

        // Dispatches a callable synchronously onto the taskbar XAML UI thread.
        static bool RunOnUiThread(std::function<void()> fn, DWORD timeoutMs = 1500);
    };
} // namespace winrings
