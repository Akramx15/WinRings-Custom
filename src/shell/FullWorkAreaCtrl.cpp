#include "FullWorkAreaCtrl.h"

#include "ShellController.h"

namespace winrings
{
    namespace
    {
        constexpr wchar_t kClassName[] = L"WinRings.FullWorkArea.v1";
        constexpr wchar_t kBridgeClass[] = L"Windows.UI.Composition.DesktopWindowContentBridge";

        constexpr UINT_PTR kTimerWorkArea = 41;
        constexpr UINT_PTR kTimerTray = 42;
        constexpr UINT_PTR kTimerVerify = 43;

        constexpr int kHkToggleHide = 4101;
        constexpr int kHkToggleCenter = 4102;

        struct BridgeCtx
        {
            HWND parent = nullptr;
            int offsetX = 0;
            int width = 0;
            int height = 0;
            HWND firstBridge = nullptr;
        };
    } // namespace

    FullWorkAreaCtrl* FullWorkAreaCtrl::s_instance = nullptr;

    FullWorkAreaCtrl::~FullWorkAreaCtrl()
    {
        Shutdown();
    }

    bool FullWorkAreaCtrl::Initialize(
        HMODULE shellModule,
        const SwitcherConfig& cfg,
        std::function<void()> onVisibilityChanged,
        std::function<void()> onPowerOrDisplayResume)
    {
        Shutdown();
        m_module = shellModule;
        m_config = cfg;
        m_onVisibilityChanged = std::move(onVisibilityChanged);
        m_onPowerOrDisplayResume = std::move(onPowerOrDisplayResume);
        s_instance = this;

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = m_module;
        wc.lpszClassName = kClassName;
        if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
            m_classRegistered = true;

        // Top-level hidden WS_POPUP (NOT HWND_MESSAGE) so WM_SETTINGCHANGE / WM_DISPLAYCHANGE /
        // WM_POWERBROADCAST / TaskbarCreated broadcasts are received reliably.
        m_hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kClassName,
            L"",
            WS_POPUP,
            0, 0, 0, 0,
            nullptr, nullptr, m_module, this);
        if (!m_hwnd)
            return false;

        m_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

        m_enabled = cfg.fullWorkAreaEnabled;
        m_centerMode = cfg.fullWorkAreaPillClip;
        m_hidden = false;

        if (m_enabled)
        {
            SyncHotkeys(cfg.fullWorkAreaHotkeys);
            ApplyFullWorkArea();
            if (m_centerMode)
                ApplyCenteredPill();
        }
        ShellDiagLog(L"FullWorkArea", L"Init enabled=%d pill=%d hotkeys=%d",
            m_enabled ? 1 : 0, m_centerMode ? 1 : 0, cfg.fullWorkAreaHotkeys ? 1 : 0);
        return true;
    }

    void FullWorkAreaCtrl::UpdateConfig(const SwitcherConfig& cfg)
    {
        const bool prevEnabled = m_enabled;
        const bool prevCenter = m_centerMode;
        m_config = cfg;
        m_enabled = cfg.fullWorkAreaEnabled;
        m_centerMode = cfg.fullWorkAreaPillClip;

        if (!m_hwnd)
            return;

        if (!m_enabled)
        {
            if (prevEnabled)
            {
                SyncHotkeys(false);
                KillTimer(m_hwnd, kTimerWorkArea);
                KillTimer(m_hwnd, kTimerTray);
                KillTimer(m_hwnd, kTimerVerify);
                RestoreTaskbarLayout();
                RestoreDefaultWorkArea();
                if (m_onVisibilityChanged)
                    m_onVisibilityChanged();
            }
            return;
        }

        SyncHotkeys(cfg.fullWorkAreaHotkeys);
        ApplyFullWorkArea();

        if (!m_hidden)
        {
            if (m_centerMode)
            {
                m_trayBurstCount = 0;
                ApplyCenteredPill();
            }
            else if (prevCenter)
            {
                RestoreTaskbarLayout();
            }
        }
    }

    void FullWorkAreaCtrl::Shutdown()
    {
        SyncHotkeys(false);
        if (m_hwnd)
        {
            KillTimer(m_hwnd, kTimerWorkArea);
            KillTimer(m_hwnd, kTimerTray);
            KillTimer(m_hwnd, kTimerVerify);
        }

        if (m_enabled)
        {
            RestoreTaskbarLayout();
            RestoreDefaultWorkArea();
        }
        else
        {
            UnhookTray();
        }

        if (m_hwnd && IsWindow(m_hwnd))
        {
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
        }
        if (m_classRegistered && m_module)
        {
            UnregisterClassW(kClassName, m_module);
            m_classRegistered = false;
        }
        if (s_instance == this)
            s_instance = nullptr;
        m_enabled = false;
        m_hidden = false;
        m_hTray = nullptr;
        m_lastShift = -1;
        m_lastIsTop = -1;
        m_onPowerOrDisplayResume = nullptr;
    }

    void FullWorkAreaCtrl::OnWakeOrUnlockImmediate()
    {
        m_lastShift = -1;
        m_lastIsTop = -1;
        m_trayBurstCount = 0;
        m_workBurstCount = 0;

        if (!m_enabled)
            return;

        ApplyFullWorkArea();
        if (m_centerMode && !m_hidden)
            ApplyCenteredPill();

        if (m_hwnd)
        {
            ScheduleWorkAreaApply(80);
            if (m_centerMode && !m_hidden)
                ScheduleTrayApply(60);
        }
    }

    RECT FullWorkAreaCtrl::GetPrimaryTaskbarRect() const noexcept
    {
        RECT rc{};
        HWND hTray = m_hTray;
        if (!hTray || !IsWindow(hTray))
            hTray = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (hTray && IsWindow(hTray))
            GetWindowRect(hTray, &rc);
        return rc;
    }

    void FullWorkAreaCtrl::SyncHotkeys(bool enable)
    {
        if (!m_hwnd)
            return;
        if (enable && !m_hotkeysActive)
        {
            if (!RegisterHotKey(m_hwnd, kHkToggleHide, MOD_WIN | MOD_NOREPEAT, VK_OEM_3))
                RegisterHotKey(m_hwnd, kHkToggleHide, MOD_ALT | MOD_NOREPEAT, VK_OEM_3);
            if (!RegisterHotKey(m_hwnd, kHkToggleCenter, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, VK_OEM_3))
                RegisterHotKey(m_hwnd, kHkToggleCenter, MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_OEM_3);
            m_hotkeysActive = true;
        }
        else if (!enable && m_hotkeysActive)
        {
            UnregisterHotKey(m_hwnd, kHkToggleHide);
            UnregisterHotKey(m_hwnd, kHkToggleCenter);
            m_hotkeysActive = false;
        }
    }

    BOOL CALLBACK FullWorkAreaCtrl::CheckMonitorProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam)
    {
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(hMon, &mi))
        {
            if (mi.rcWork.left != mi.rcMonitor.left ||
                mi.rcWork.top != mi.rcMonitor.top ||
                mi.rcWork.right != mi.rcMonitor.right ||
                mi.rcWork.bottom != mi.rcMonitor.bottom)
            {
                *reinterpret_cast<BOOL*>(lParam) = TRUE;
                return FALSE;
            }
        }
        return TRUE;
    }

    BOOL CALLBACK FullWorkAreaCtrl::ApplyMonitorProc(HMONITOR hMon, HDC, LPRECT, LPARAM)
    {
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(hMon, &mi))
        {
            if (mi.rcWork.left != mi.rcMonitor.left ||
                mi.rcWork.top != mi.rcMonitor.top ||
                mi.rcWork.right != mi.rcMonitor.right ||
                mi.rcWork.bottom != mi.rcMonitor.bottom)
            {
                RECT rc = mi.rcMonitor;
                SystemParametersInfoW(SPI_SETWORKAREA, 0, &rc, 0);
            }
        }
        return TRUE;
    }

    bool FullWorkAreaCtrl::AnyMonitorNeedsFix() const
    {
        BOOL needs = FALSE;
        EnumDisplayMonitors(nullptr, nullptr, CheckMonitorProc, reinterpret_cast<LPARAM>(&needs));
        return needs != FALSE;
    }

    void FullWorkAreaCtrl::ApplyFullWorkArea()
    {
        if (m_applyingWork)
            return;
        m_applyingWork = true;
        EnumDisplayMonitors(nullptr, nullptr, ApplyMonitorProc, 0);
        m_applyingWork = false;
    }

    void FullWorkAreaCtrl::RestoreDefaultWorkArea()
    {
        RECT rc{};
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc, 0))
        {
            if (rc.top == 0)
                rc.top = 1;
            SystemParametersInfoW(SPI_SETWORKAREA, 0, &rc, SPIF_SENDCHANGE);
        }
    }

    BOOL CALLBACK FullWorkAreaCtrl::MoveBridgeProc(HWND child, LPARAM lParam)
    {
        auto* ctx = reinterpret_cast<BridgeCtx*>(lParam);
        if (GetParent(child) != ctx->parent)
            return TRUE;

        wchar_t cls[96]{};
        if (GetClassNameW(child, cls, _countof(cls)) > 0 && wcscmp(cls, kBridgeClass) == 0)
        {
            if (!ctx->firstBridge)
                ctx->firstBridge = child;
            SetWindowPos(
                child, nullptr, ctx->offsetX, 0, ctx->width, ctx->height,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
        }
        return TRUE;
    }

    BOOL CALLBACK FullWorkAreaCtrl::FindBridgeProc(HWND child, LPARAM lParam)
    {
        auto* ctx = reinterpret_cast<BridgeCtx*>(lParam);
        if (GetParent(child) != ctx->parent)
            return TRUE;

        wchar_t cls[96]{};
        if (GetClassNameW(child, cls, _countof(cls)) > 0 && wcscmp(cls, kBridgeClass) == 0)
        {
            ctx->firstBridge = child;
            return FALSE;
        }
        return TRUE;
    }

    void FullWorkAreaCtrl::UnhookTray()
    {
        if (m_hTrayHook)
        {
            UnhookWinEvent(m_hTrayHook);
            m_hTrayHook = nullptr;
        }
    }

    void CALLBACK FullWorkAreaCtrl::TrayWinEventProc(
        HWINEVENTHOOK, DWORD, HWND hwnd,
        LONG idObject, LONG idChild, DWORD, DWORD)
    {
        FullWorkAreaCtrl* self = s_instance;
        if (!self || !self->m_enabled || !self->m_centerMode || self->m_hidden || self->m_applyingTray)
            return;
        if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
            return;
        if (hwnd == self->m_hTray || GetParent(hwnd) == self->m_hTray)
            self->ScheduleTrayApply(16);
    }

    void FullWorkAreaCtrl::RefreshTrayHandle()
    {
        HWND h = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (h != m_hTray)
        {
            UnhookTray();
            m_hTray = h;
            m_lastShift = -1;
            m_lastIsTop = -1;
        }
        if (m_hTray && m_centerMode && !m_hTrayHook)
        {
            DWORD pid = 0;
            const DWORD tid = GetWindowThreadProcessId(m_hTray, &pid);
            if (pid && tid)
            {
                m_hTrayHook = SetWinEventHook(
                    EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
                    nullptr, TrayWinEventProc, pid, tid, WINEVENT_OUTOFCONTEXT);
            }
        }
    }

    void FullWorkAreaCtrl::ApplyRegionForCurrentShift(int monLeft, int monTop, int monW, int trayTop, int trayH, HWND hBridge)
    {
        int actualShift = 0;
        if (hBridge)
        {
            RECT rcB{};
            if (GetWindowRect(hBridge, &rcB))
            {
                const int s = monLeft - rcB.left;
                if (s > 10 && s < (monW / 3))
                    actualShift = s;
            }
        }

        const bool isTop = (m_config.topBarEnabled && m_config.topBarDockTop)
            ? true
            : (trayTop <= monTop + 4);
        const int isTopInt = isTop ? 1 : 0;

        if (actualShift == m_lastShift && monW == m_lastMonW &&
            trayH == m_lastTrayH && isTopInt == m_lastIsTop)
        {
            return;
        }

        m_lastShift = actualShift;
        m_lastMonW = monW;
        m_lastTrayH = trayH;
        m_lastIsTop = isTopInt;

        int rgnLeft = (monW * 27) / 100;
        int rgnRight = monW - actualShift;
        if (rgnRight <= rgnLeft + 300)
        {
            rgnLeft = 0;
            rgnRight = monW;
        }

        const int y1 = isTop ? -18 : 0;
        const int y2 = isTop ? (trayH + 1) : (trayH + 19);

        HRGN hRgn = CreateRoundRectRgn(rgnLeft, y1, rgnRight + 1, y2, 20, 20);
        if (hRgn)
        {
            if (!SetWindowRgn(m_hTray, hRgn, TRUE))
                DeleteObject(hRgn);
        }
    }

    void FullWorkAreaCtrl::ScheduleTrayApply(UINT delayMs)
    {
        if (!m_hwnd)
            return;
        const ULONGLONG now = GetTickCount64();
        if ((now - m_trayBurstStart) > 2500)
        {
            m_trayBurstStart = now;
            m_trayBurstCount = 0;
        }
        ++m_trayBurstCount;
        if (m_trayBurstCount > 6)
        {
            SetTimer(m_hwnd, kTimerVerify, 200, nullptr);
            return;
        }
        SetTimer(m_hwnd, kTimerTray, delayMs, nullptr);
    }

    void FullWorkAreaCtrl::ScheduleWorkAreaApply(UINT delayMs)
    {
        if (!m_hwnd)
            return;
        const ULONGLONG now = GetTickCount64();
        if ((now - m_workBurstStart) > 3000)
        {
            m_workBurstStart = now;
            m_workBurstCount = 0;
        }
        ++m_workBurstCount;
        if (m_workBurstCount > 5)
            delayMs = 2000;
        SetTimer(m_hwnd, kTimerWorkArea, delayMs, nullptr);
    }

    void FullWorkAreaCtrl::ApplyCenteredPill()
    {
        if (!m_enabled || !m_centerMode)
            return;

        RefreshTrayHandle();
        if (!m_hTray || m_hidden)
            return;

        HMONITOR hMon = MonitorFromWindow(m_hTray, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{ sizeof(mi) };
        if (!GetMonitorInfoW(hMon, &mi))
            return;

        RECT rcTray{};
        if (!GetWindowRect(m_hTray, &rcTray))
            return;

        const int monW = mi.rcMonitor.right - mi.rcMonitor.left;
        int trayH = rcTray.bottom - rcTray.top;
        if (trayH <= 0)
            trayH = 42;

        const int shiftPx = (monW * 11) / 100;

        m_applyingTray = true;
        BridgeCtx ctx{};
        ctx.parent = m_hTray;
        ctx.offsetX = -shiftPx;
        ctx.width = monW;
        ctx.height = trayH;
        EnumChildWindows(m_hTray, MoveBridgeProc, reinterpret_cast<LPARAM>(&ctx));

        ApplyRegionForCurrentShift(mi.rcMonitor.left, mi.rcMonitor.top, monW, rcTray.top, trayH, ctx.firstBridge);
        m_applyingTray = false;

        if (m_hwnd)
            SetTimer(m_hwnd, kTimerVerify, 250, nullptr);
    }

    void FullWorkAreaCtrl::VerifyRegionOnly()
    {
        if (!m_enabled || !m_hTray || !m_centerMode || m_hidden)
            return;

        HMONITOR hMon = MonitorFromWindow(m_hTray, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{ sizeof(mi) };
        if (!GetMonitorInfoW(hMon, &mi))
            return;

        RECT rcTray{};
        if (!GetWindowRect(m_hTray, &rcTray))
            return;

        const int monW = mi.rcMonitor.right - mi.rcMonitor.left;
        int trayH = rcTray.bottom - rcTray.top;
        if (trayH <= 0)
            trayH = 42;

        BridgeCtx ctx{};
        ctx.parent = m_hTray;
        ctx.offsetX = 0;
        ctx.width = monW;
        ctx.height = trayH;
        EnumChildWindows(m_hTray, FindBridgeProc, reinterpret_cast<LPARAM>(&ctx));

        ApplyRegionForCurrentShift(mi.rcMonitor.left, mi.rcMonitor.top, monW, rcTray.top, trayH, ctx.firstBridge);
    }

    void FullWorkAreaCtrl::RestoreTaskbarLayout()
    {
        UnhookTray();
        HWND hTray = (m_hTray && IsWindow(m_hTray)) ? m_hTray : FindWindowW(L"Shell_TrayWnd", nullptr);
        if (!hTray)
            return;

        if (m_hidden)
        {
            ShowWindowAsync(hTray, SW_SHOW);
            m_hidden = false;
        }

        HMONITOR hMon = MonitorFromWindow(hTray, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(hMon, &mi))
        {
            RECT rcTray{};
            int trayH = (m_lastTrayH > 0) ? m_lastTrayH : 42;
            if (GetWindowRect(hTray, &rcTray) && (rcTray.bottom - rcTray.top) > 0)
                trayH = rcTray.bottom - rcTray.top;
            const int monW = mi.rcMonitor.right - mi.rcMonitor.left;

            BridgeCtx ctx{};
            ctx.parent = hTray;
            ctx.offsetX = 0;
            ctx.width = monW;
            ctx.height = trayH;
            EnumChildWindows(hTray, MoveBridgeProc, reinterpret_cast<LPARAM>(&ctx));

            SetWindowRgn(hTray, nullptr, TRUE);
        }
        m_lastShift = -1;
        m_lastIsTop = -1;
    }

    void FullWorkAreaCtrl::ToggleTaskbarVisibility()
    {
        RefreshTrayHandle();
        if (!m_hTray)
            return;

        m_hidden = !m_hidden;
        ShowWindowAsync(m_hTray, m_hidden ? SW_HIDE : SW_SHOW);
        if (!m_hidden && m_centerMode)
        {
            m_trayBurstCount = 0;
            ScheduleTrayApply(80);
        }
        if (m_onVisibilityChanged)
            m_onVisibilityChanged();
    }

    void FullWorkAreaCtrl::ToggleCenterMode()
    {
        m_centerMode = !m_centerMode;
        m_trayBurstCount = 0;
        if (m_centerMode)
            ApplyCenteredPill();
        else
            RestoreTaskbarLayout();
        ApplyFullWorkArea();
    }

    LRESULT CALLBACK FullWorkAreaCtrl::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept
    {
        auto* self = reinterpret_cast<FullWorkAreaCtrl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE)
        {
            self = static_cast<FullWorkAreaCtrl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self)
                self->m_hwnd = hwnd;
        }
        if (self)
        {
            try { return self->HandleMessage(hwnd, msg, wp, lp); }
            catch (...) {}
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT FullWorkAreaCtrl::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_HOTKEY && m_enabled)
        {
            if (wp == kHkToggleHide)
                ToggleTaskbarVisibility();
            else if (wp == kHkToggleCenter)
                ToggleCenterMode();
            return 0;
        }

        if (msg == WM_TIMER && m_enabled)
        {
            if (wp == kTimerWorkArea)
            {
                KillTimer(hwnd, kTimerWorkArea);
                if (AnyMonitorNeedsFix())
                    ApplyFullWorkArea();
                return 0;
            }
            if (wp == kTimerTray)
            {
                KillTimer(hwnd, kTimerTray);
                if (m_centerMode && !m_hidden)
                    ApplyCenteredPill();
                return 0;
            }
            if (wp == kTimerVerify)
            {
                KillTimer(hwnd, kTimerVerify);
                VerifyRegionOnly();
                return 0;
            }
        }

        if (msg == WM_SETTINGCHANGE && m_enabled)
        {
            if (!m_applyingWork && wp == SPI_SETWORKAREA && AnyMonitorNeedsFix())
            {
                if (m_workBurstCount == 0)
                    ApplyFullWorkArea();
                ScheduleWorkAreaApply(60);
            }
            return 0;
        }

        if (msg == WM_DISPLAYCHANGE)
        {
            OnWakeOrUnlockImmediate();
            if (m_onPowerOrDisplayResume)
                m_onPowerOrDisplayResume();
            return 0;
        }

        if (msg == WM_POWERBROADCAST)
        {
            if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND || wp == PBT_APMRESUMECRITICAL)
            {
                OnWakeOrUnlockImmediate();
                if (m_onPowerOrDisplayResume)
                    m_onPowerOrDisplayResume();
            }
            return TRUE;
        }

        if (m_msgTaskbarCreated && msg == m_msgTaskbarCreated)
        {
            m_hTray = nullptr;
            m_lastShift = -1;
            m_lastIsTop = -1;
            m_trayBurstCount = 0;
            if (m_enabled)
            {
                ScheduleWorkAreaApply(80);
                if (m_centerMode)
                    ScheduleTrayApply(120);
            }
            if (m_onPowerOrDisplayResume)
                m_onPowerOrDisplayResume();
            return 0;
        }

        return DefWindowProcW(hwnd, msg, wp, lp);
    }
} // namespace winrings
