#pragma once

#include <Windows.h>
#include <functional>

#include "../shared/WinRingsConfig.h"

namespace winrings
{
    class FullWorkAreaCtrl
    {
    public:
        FullWorkAreaCtrl() = default;
        ~FullWorkAreaCtrl();

        FullWorkAreaCtrl(const FullWorkAreaCtrl&) = delete;
        FullWorkAreaCtrl& operator=(const FullWorkAreaCtrl&) = delete;

        bool Initialize(HMODULE shellModule, const SwitcherConfig& cfg, std::function<void()> onVisibilityChanged);
        void UpdateConfig(const SwitcherConfig& cfg);
        void Shutdown();

        bool IsTaskbarHidden() const noexcept { return m_hidden; }
        RECT GetPrimaryTaskbarRect() const noexcept;

    private:
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept;
        LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

        static BOOL CALLBACK CheckMonitorProc(HMONITOR hMon, HDC hdc, LPRECT lprc, LPARAM lParam);
        static BOOL CALLBACK ApplyMonitorProc(HMONITOR hMon, HDC hdc, LPRECT lprc, LPARAM lParam);
        static BOOL CALLBACK MoveBridgeProc(HWND child, LPARAM lParam);
        static BOOL CALLBACK FindBridgeProc(HWND child, LPARAM lParam);
        static void CALLBACK TrayWinEventProc(
            HWINEVENTHOOK hHook, DWORD event, HWND hwnd,
            LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime);

        bool AnyMonitorNeedsFix() const;
        void ApplyFullWorkArea();
        void RestoreDefaultWorkArea();

        void SyncHotkeys(bool enable);
        void UnhookTray();
        void RefreshTrayHandle();
        void ApplyRegionForCurrentShift(int monLeft, int monTop, int monW, int trayTop, int trayH, HWND hBridge);
        void ApplyCenteredPill();
        void VerifyRegionOnly();
        void RestoreTaskbarLayout();
        void ScheduleTrayApply(UINT delayMs);
        void ScheduleWorkAreaApply(UINT delayMs);
        void ToggleTaskbarVisibility();
        void ToggleCenterMode();

        HMODULE m_module = nullptr;
        HWND m_hwnd = nullptr;
        bool m_classRegistered = false;
        UINT m_msgTaskbarCreated = 0;

        SwitcherConfig m_config;
        std::function<void()> m_onVisibilityChanged;

        bool m_enabled = false;
        bool m_centerMode = true;
        bool m_hidden = false;
        bool m_hotkeysActive = false;

        bool m_applyingWork = false;
        ULONGLONG m_workBurstStart = 0;
        DWORD m_workBurstCount = 0;

        HWND m_hTray = nullptr;
        HWINEVENTHOOK m_hTrayHook = nullptr;
        bool m_applyingTray = false;
        ULONGLONG m_trayBurstStart = 0;
        DWORD m_trayBurstCount = 0;
        int m_lastShift = -1;
        int m_lastMonW = 0;
        int m_lastTrayH = 42;

        static FullWorkAreaCtrl* s_instance;
    };
} // namespace winrings
