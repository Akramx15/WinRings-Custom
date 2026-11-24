#include "WindowSnapCtrl.h"

#include <ShellScalingApi.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "ShellController.h"
#include "SlotModel.h"

namespace winrings
{
    namespace
    {
        constexpr wchar_t kSnapPreviewClassW[] = L"WinRings.SnapPreview.v1";
        constexpr ULONG_PTR kWinRingsInjectedTag = 0x57524E47ull;

        constexpr UINT_PTR kTimerLiveProbe = 1;
        constexpr UINT_PTR kTimerPendingExpire = 2;
        constexpr UINT_PTR kTimerSettleSnap = 3;

        using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);

        double QueryMonitorDpiScale(HMONITOR mon) noexcept
        {
            if (mon)
            {
                if (HMODULE shcore = GetModuleHandleW(L"shcore.dll"))
                {
                    if (auto fn = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor")))
                    {
                        UINT dx = 96, dy = 96;
                        if (SUCCEEDED(fn(mon, 0, &dx, &dy)) && dx >= 72)
                            return static_cast<double>(dx) / 96.0;
                    }
                }
            }
            return 1.0;
        }

        bool EndsWithExeCaseInsensitive(const std::wstring& s) noexcept
        {
            if (s.size() < 4)
                return false;
            return _wcsicmp(s.c_str() + s.size() - 4, L".exe") == 0;
        }

        const wchar_t* ZoneActionText(SnapZone zone, bool launchMode) noexcept
        {
            switch (zone)
            {
            case SnapZone::Left:
                return launchMode ? L"Launch & Snap Left + Snap Assist" : L"Snap Left + Snap Assist";
            case SnapZone::Right:
                return launchMode ? L"Launch & Snap Right + Snap Assist" : L"Snap Right + Snap Assist";
            case SnapZone::TopMaximize:
                return launchMode ? L"Launch & Maximize" : L"Maximize";
            default:
                return L"";
            }
        }

        BOOL CALLBACK EnumVisibleSnapshotProc(HWND hwnd, LPARAM lp)
        {
            auto* out = reinterpret_cast<std::unordered_set<HWND>*>(lp);
            if (out && IsWindowVisible(hwnd))
                out->insert(hwnd);
            return TRUE;
        }
    } // namespace

    WindowSnapCtrl::~WindowSnapCtrl()
    {
        Shutdown();
    }

    bool WindowSnapCtrl::Initialize(HMODULE shellModule)
    {
        Shutdown();
        m_module = shellModule;

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = PreviewWndProc;
        wc.hInstance = shellModule;
        wc.lpszClassName = kSnapPreviewClassW;
        if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
            m_classRegistered = true;

        m_previewHwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            kSnapPreviewClassW,
            L"",
            WS_POPUP,
            0, 0, 1, 1,
            nullptr, nullptr, shellModule, this);

        m_initialized = (m_previewHwnd != nullptr);
        return m_initialized;
    }

    void WindowSnapCtrl::Shutdown()
    {
        CancelActiveDrag();
        ClearPendingLaunch();
        if (m_previewHwnd)
        {
            KillTimer(m_previewHwnd, kTimerSettleSnap);
            DestroyWindow(m_previewHwnd);
            m_previewHwnd = nullptr;
        }
        ReleaseRenderFactories();
        if (m_classRegistered && m_module)
        {
            UnregisterClassW(kSnapPreviewClassW, m_module);
            m_classRegistered = false;
        }
        m_settleHwnd = nullptr;
        m_settleZone = SnapZone::None;
        m_settleMonitor = nullptr;
        m_initialized = false;
        m_module = nullptr;
    }

    bool WindowSnapCtrl::EnsureRenderFactories()
    {
        if (m_d2dFactory && m_dcTarget && m_dwriteFactory && m_badgeFmt)
            return true;

        if (!m_d2dFactory &&
            (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.ReleaseAndGetAddressOf())) || !m_d2dFactory))
        {
            return false;
        }

        if (!m_dcTarget)
        {
            D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
            if (FAILED(m_d2dFactory->CreateDCRenderTarget(&props, m_dcTarget.ReleaseAndGetAddressOf())) || !m_dcTarget)
                return false;
        }

        if (!m_dwriteFactory &&
            (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(m_dwriteFactory.ReleaseAndGetAddressOf()))) || !m_dwriteFactory))
        {
            return false;
        }

        if (!m_badgeFmt)
        {
            if (SUCCEEDED(m_dwriteFactory->CreateTextFormat(
                L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us",
                m_badgeFmt.ReleaseAndGetAddressOf())) && m_badgeFmt)
            {
                m_badgeFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_badgeFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                m_badgeFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
        }
        return true;
    }

    void WindowSnapCtrl::ReleaseRenderFactories()
    {
        m_badgeFmt.Reset();
        m_dwriteFactory.Reset();
        m_dcTarget.Reset();
        m_d2dFactory.Reset();
    }

    void WindowSnapCtrl::BringWindowForeground(HWND target) noexcept
    {
        if (!target || !IsWindow(target))
            return;
        if (!SetForegroundWindow(target))
        {
            HWND fg = GetForegroundWindow();
            const DWORD curTid = GetCurrentThreadId();
            const DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
            const DWORD tgtTid = GetWindowThreadProcessId(target, nullptr);
            const bool attFg = fgTid && fgTid != curTid && AttachThreadInput(curTid, fgTid, TRUE) != FALSE;
            const bool attTgt = tgtTid && tgtTid != curTid && tgtTid != fgTid && AttachThreadInput(curTid, tgtTid, TRUE) != FALSE;
            BringWindowToTop(target);
            SetForegroundWindow(target);
            if (attTgt) AttachThreadInput(curTid, tgtTid, FALSE);
            if (attFg) AttachThreadInput(curTid, fgTid, FALSE);
        }
    }

    void WindowSnapCtrl::BeginOpenWindowDrag(HWND targetHwnd, POINT cursorPt)
    {
        if (!m_initialized || !targetHwnd || !IsWindow(targetHwnd))
            return;

        CancelActiveDrag();
        ClearPendingLaunch();

        if (IsIconic(targetHwnd) || IsZoomed(targetHwnd))
            ShowWindow(targetHwnd, SW_RESTORE);

        HMONITOR hMon = MonitorFromPoint(cursorPt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(hMon, &mi))
        {
            RECT rc{};
            if (GetWindowRect(targetHwnd, &rc))
            {
                const int workW = mi.rcWork.right - mi.rcWork.left;
                const int workH = mi.rcWork.bottom - mi.rcWork.top;
                const int winW = (std::min)(static_cast<int>(rc.right - rc.left), (std::max)(320, workW - 24));
                const int winH = (std::min)(static_cast<int>(rc.bottom - rc.top), (std::max)(240, workH - 24));
                int newX = cursorPt.x - winW / 2;
                int newY = cursorPt.y - 18;
                newX = (std::clamp)(newX, static_cast<int>(mi.rcWork.left) - winW / 3,
                    static_cast<int>(mi.rcWork.right) - (std::max)(120, winW / 3));
                newY = (std::clamp)(newY, static_cast<int>(mi.rcWork.top),
                    (std::max)(static_cast<int>(mi.rcWork.top), static_cast<int>(mi.rcWork.bottom) - 48));
                SetWindowPos(targetHwnd, HWND_TOP, newX, newY, winW, winH, SWP_SHOWWINDOW);
            }
        }

        BringWindowForeground(targetHwnd);

        // If user already released LMB before async dispatch ran, apply snap immediately if over an edge.
        if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0)
        {
            HMONITOR snapMon = nullptr;
            double dpi = 1.0;
            const SnapZone zone = HitTestSnapZone(cursorPt, snapMon, dpi);
            if (zone != SnapZone::None)
                ApplyNativeSnap(targetHwnd, zone, snapMon);
            return;
        }

        PostMessageW(targetHwnd, WM_SYSCOMMAND, SC_MOVE | 0x0002, MAKELPARAM(cursorPt.x, cursorPt.y));

        m_liveProbeActive = true;
        m_liveProbeHwnd = targetHwnd;
        if (m_previewHwnd)
            SetTimer(m_previewHwnd, kTimerLiveProbe, 65, nullptr);
    }

    void WindowSnapCtrl::BeginClosedAppDrag(SlotKind kind, std::wstring label, std::wstring pathOrUrl, POINT cursorPt)
    {
        if (!m_initialized)
            return;

        CancelActiveDrag();
        ClearPendingLaunch();

        if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0)
        {
            m_fallbackActive = true;
            m_fallbackIsClosedApp = true;
            m_fallbackKind = kind;
            m_fallbackLabel = std::move(label);
            m_fallbackPathOrUrl = std::move(pathOrUrl);
            OnFallbackDragRelease(cursorPt);
            return;
        }

        m_fallbackActive = true;
        m_fallbackIsClosedApp = true;
        m_fallbackTargetHwnd = nullptr;
        m_fallbackKind = kind;
        m_fallbackLabel = std::move(label);
        m_fallbackPathOrUrl = std::move(pathOrUrl);
        UpdatePreviewForPoint(cursorPt);
    }

    SnapZone WindowSnapCtrl::HitTestSnapZone(POINT screenPt, HMONITOR& outMon, double& outDpiScale) const noexcept
    {
        outMon = MonitorFromPoint(screenPt, MONITOR_DEFAULTTONEAREST);
        outDpiScale = QueryMonitorDpiScale(outMon);
        MONITORINFO mi{ sizeof(mi) };
        if (!GetMonitorInfoW(outMon, &mi))
            return SnapZone::None;

        const int sidePx = static_cast<int>(std::round(56.0 * outDpiScale));
        const int topPx = static_cast<int>(std::round(44.0 * outDpiScale));
        return ClassifySnapEdge(
            screenPt.x, screenPt.y,
            mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom,
            sidePx, topPx);
    }

    void WindowSnapCtrl::UpdatePreviewForPoint(POINT screenPt)
    {
        if (!m_fallbackActive)
            return;

        HMONITOR mon = nullptr;
        double dpi = 1.0;
        const SnapZone zone = HitTestSnapZone(screenPt, mon, dpi);
        if (zone == SnapZone::None)
        {
            HidePreviewWindow();
            return;
        }

        if (zone == m_previewZone && mon == m_previewMon && m_previewHwnd && IsWindowVisible(m_previewHwnd))
            return;

        m_previewZone = zone;
        m_previewMon = mon;

        std::wstring badge = ZoneActionText(zone, m_fallbackIsClosedApp);
        if (m_fallbackIsClosedApp && !m_fallbackLabel.empty())
            badge = m_fallbackLabel + L"  •  " + badge;

        RenderZonePreview(mon, dpi, zone, badge);
    }

    void WindowSnapCtrl::RenderZonePreview(HMONITOR mon, double dpiScale, SnapZone zone, const std::wstring& badgeText)
    {
        if (!m_previewHwnd || !mon || zone == SnapZone::None || !EnsureRenderFactories())
            return;

        MONITORINFO mi{ sizeof(mi) };
        if (!GetMonitorInfoW(mon, &mi))
            return;

        const int pad = static_cast<int>(std::round(10.0 * dpiScale));
        const int workW = mi.rcWork.right - mi.rcWork.left;

        RECT targetRc = mi.rcWork;
        if (zone == SnapZone::Left)
        {
            targetRc.left = mi.rcWork.left + pad;
            targetRc.top = mi.rcWork.top + pad;
            targetRc.right = mi.rcWork.left + (workW / 2) - pad / 2;
            targetRc.bottom = mi.rcWork.bottom - pad;
        }
        else if (zone == SnapZone::Right)
        {
            targetRc.left = mi.rcWork.left + (workW / 2) + pad / 2;
            targetRc.top = mi.rcWork.top + pad;
            targetRc.right = mi.rcWork.right - pad;
            targetRc.bottom = mi.rcWork.bottom - pad;
        }
        else
        {
            targetRc.left = mi.rcWork.left + pad;
            targetRc.top = mi.rcWork.top + pad;
            targetRc.right = mi.rcWork.right - pad;
            targetRc.bottom = mi.rcWork.bottom - pad;
        }

        const int wPx = (std::max)(80, static_cast<int>(targetRc.right - targetRc.left));
        const int hPx = (std::max)(80, static_cast<int>(targetRc.bottom - targetRc.top));

        HDC screenDc = GetDC(nullptr);
        if (!screenDc)
            return;
        HDC memDc = CreateCompatibleDC(screenDc);
        if (!memDc)
        {
            ReleaseDC(nullptr, screenDc);
            return;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = wPx;
        bmi.bmiHeader.biHeight = -hPx;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib || !bits)
        {
            if (dib) DeleteObject(dib);
            DeleteDC(memDc);
            ReleaseDC(nullptr, screenDc);
            return;
        }
        std::memset(bits, 0, static_cast<size_t>(wPx) * hPx * 4u);
        HGDIOBJ oldBmp = SelectObject(memDc, dib);

        const RECT bindRc{ 0, 0, wPx, hPx };
        if (SUCCEEDED(m_dcTarget->BindDC(memDc, &bindRc)))
        {
            const float w = static_cast<float>(wPx / dpiScale);
            const float h = static_cast<float>(hPx / dpiScale);

            m_dcTarget->BeginDraw();
            m_dcTarget->Clear(D2D1::ColorF(0, 0, 0, 0));
            m_dcTarget->SetTransform(D2D1::Matrix3x2F::Scale(
                static_cast<float>(dpiScale), static_cast<float>(dpiScale)));

            auto mkBrush = [&](D2D1_COLOR_F col) {
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> br;
                m_dcTarget->CreateSolidColorBrush(col, br.GetAddressOf());
                return br;
            };

            const D2D1_ROUNDED_RECT zoneBox = D2D1::RoundedRect(
                D2D1::RectF(1.5f, 1.5f, w - 1.5f, h - 1.5f), 12.0f, 12.0f);
            if (auto fillBr = mkBrush(D2D1::ColorF(0.0f, 120.0f / 255.0f, 212.0f / 255.0f, 0.22f)))
                m_dcTarget->FillRoundedRectangle(zoneBox, fillBr.Get());
            if (auto rimBr = mkBrush(D2D1::ColorF(66.0f / 255.0f, 165.0f / 255.0f, 245.0f / 255.0f, 0.88f)))
                m_dcTarget->DrawRoundedRectangle(zoneBox, rimBr.Get(), 2.0f);

            if (!badgeText.empty())
            {
                const float pillW = (std::min)(w - 28.0f, 300.0f);
                const float pillH = 34.0f;
                const float px0 = (w - pillW) * 0.5f;
                const float py0 = (h - pillH) * 0.5f;
                const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(
                    D2D1::RectF(px0, py0, px0 + pillW, py0 + pillH), 17.0f, 17.0f);
                if (auto bg = mkBrush(D2D1::ColorF(20.0f / 255.0f, 22.0f / 255.0f, 28.0f / 255.0f, 0.90f)))
                    m_dcTarget->FillRoundedRectangle(pill, bg.Get());
                if (auto pr = mkBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f)))
                    m_dcTarget->DrawRoundedRectangle(pill, pr.Get(), 1.0f);
                if (m_badgeFmt)
                {
                    if (auto tb = mkBrush(D2D1::ColorF(245.0f / 255.0f, 247.0f / 255.0f, 250.0f / 255.0f, 0.98f)))
                        m_dcTarget->DrawTextW(badgeText.c_str(), static_cast<UINT32>(badgeText.size()),
                            m_badgeFmt.Get(), pill.rect, tb.Get());
                }
            }

            m_dcTarget->EndDraw();

            POINT ptDst{ targetRc.left, targetRc.top };
            SIZE sz{ wPx, hPx };
            POINT ptSrc{ 0, 0 };
            BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            UpdateLayeredWindow(m_previewHwnd, screenDc, &ptDst, &sz, memDc, &ptSrc, 0, &bf, ULW_ALPHA);
        }

        SelectObject(memDc, oldBmp);
        DeleteObject(dib);
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);

        SetWindowPos(m_previewHwnd, HWND_TOPMOST, targetRc.left, targetRc.top, wPx, hPx,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void WindowSnapCtrl::HidePreviewWindow() noexcept
    {
        m_previewZone = SnapZone::None;
        m_previewMon = nullptr;
        if (m_previewHwnd && IsWindow(m_previewHwnd) && IsWindowVisible(m_previewHwnd))
            ShowWindow(m_previewHwnd, SW_HIDE);
    }

    void WindowSnapCtrl::OnFallbackDragMove(POINT screenPt)
    {
        if (!m_fallbackActive)
            return;
        UpdatePreviewForPoint(screenPt);
    }

    bool WindowSnapCtrl::OnFallbackDragRelease(POINT screenPt)
    {
        if (!m_fallbackActive)
            return false;

        const bool closedMode = m_fallbackIsClosedApp;
        HWND targetHwnd = m_fallbackTargetHwnd;
        const SlotKind kind = m_fallbackKind;
        const std::wstring label = m_fallbackLabel;
        const std::wstring pathOrUrl = m_fallbackPathOrUrl;

        HMONITOR mon = nullptr;
        double dpi = 1.0;
        const SnapZone zone = HitTestSnapZone(screenPt, mon, dpi);

        CancelActiveDrag();

        if (zone == SnapZone::None)
            return true;

        if (!closedMode)
        {
            if (targetHwnd && IsWindow(targetHwnd))
                ApplyNativeSnap(targetHwnd, zone, mon);
            return true;
        }

        // Launch closed slot and arm event-driven snap on first new window
        ClearPendingLaunch();
        m_pendingLaunch.active = true;
        m_pendingLaunch.kind = kind;
        m_pendingLaunch.expectedAppKey = WindowTracker::NormalizeAppKey(pathOrUrl, label);
        m_pendingLaunch.matchExactExeKey = (kind == SlotKind::App) && EndsWithExeCaseInsensitive(pathOrUrl);
        m_pendingLaunch.zone = zone;
        m_pendingLaunch.targetMon = mon;
        m_pendingLaunch.startTick = GetTickCount64();
        m_pendingLaunch.deadlineTick = m_pendingLaunch.startTick + 6000;
        EnumWindows(EnumVisibleSnapshotProc, reinterpret_cast<LPARAM>(&m_pendingLaunch.preExistingHwnds));

        const std::wstring launchTarget = (kind == SlotKind::RecycleBin) ? L"shell:RecycleBinFolder" : pathOrUrl;
        if (!launchTarget.empty())
            SlotModel::LaunchPathOrUrl(launchTarget);

        if (m_previewHwnd)
            SetTimer(m_previewHwnd, kTimerPendingExpire, 6000, nullptr);

        ShellDiagLog(L"Snap", L"Pending launch snap: kind=%d label=%s zone=%d",
            static_cast<int>(kind), label.c_str(), static_cast<int>(zone));
        return true;
    }

    void WindowSnapCtrl::CancelActiveDrag()
    {
        if (m_previewHwnd && IsWindow(m_previewHwnd))
            KillTimer(m_previewHwnd, kTimerLiveProbe);

        m_liveProbeActive = false;
        m_liveProbeHwnd = nullptr;
        m_liveProbeLabel.clear();

        m_fallbackActive = false;
        m_fallbackIsClosedApp = false;
        m_fallbackTargetHwnd = nullptr;
        m_fallbackLabel.clear();
        m_fallbackPathOrUrl.clear();

        HidePreviewWindow();
    }

    void WindowSnapCtrl::ClearPendingLaunch() noexcept
    {
        if (m_previewHwnd && IsWindow(m_previewHwnd))
            KillTimer(m_previewHwnd, kTimerPendingExpire);
        m_pendingLaunch.active = false;
        m_pendingLaunch.expectedAppKey.clear();
        m_pendingLaunch.matchExactExeKey = false;
        m_pendingLaunch.zone = SnapZone::None;
        m_pendingLaunch.targetMon = nullptr;
        m_pendingLaunch.preExistingHwnds.clear();
    }

    void WindowSnapCtrl::OnWindowCreatedOrActivated(HWND hwnd)
    {
        if (!m_pendingLaunch.active || !hwnd || !IsWindow(hwnd))
            return;

        if (GetTickCount64() > m_pendingLaunch.deadlineTick)
        {
            ClearPendingLaunch();
            return;
        }

        if (m_pendingLaunch.preExistingHwnds.find(hwnd) != m_pendingLaunch.preExistingHwnds.end())
            return;

        std::wstring title;
        DWORD pid = 0;
        std::wstring exePath;
        if (!WindowTracker::IsEligibleWindow(hwnd, GetShellWindow(), m_previewHwnd,
            GetCurrentProcessId(), title, pid, exePath))
        {
            return;
        }

        if (m_pendingLaunch.matchExactExeKey && !m_pendingLaunch.expectedAppKey.empty())
        {
            const std::wstring key = WindowTracker::NormalizeAppKey(exePath, title);
            if (!WindowTracker::AppKeysMatch(key, m_pendingLaunch.expectedAppKey))
                return;
        }

        m_settleHwnd = hwnd;
        m_settleZone = m_pendingLaunch.zone;
        m_settleMonitor = m_pendingLaunch.targetMon;
        ClearPendingLaunch();

        if (m_previewHwnd)
            SetTimer(m_previewHwnd, kTimerSettleSnap, 140, nullptr);
        else
            ApplyNativeSnap(m_settleHwnd, m_settleZone, m_settleMonitor);
    }

    void WindowSnapCtrl::ApplyNativeSnap(HWND target, SnapZone zone, HMONITOR targetMon) noexcept
    {
        if (!target || !IsWindow(target) || zone == SnapZone::None)
            return;

        if (IsIconic(target) || IsZoomed(target))
            ShowWindow(target, SW_RESTORE);

        if (targetMon)
        {
            HMONITOR curMon = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
            if (curMon != targetMon)
            {
                MONITORINFO mi{ sizeof(mi) };
                RECT rc{};
                if (GetMonitorInfoW(targetMon, &mi) && GetWindowRect(target, &rc))
                {
                    const int winW = (std::min)(static_cast<int>(rc.right - rc.left),
                        static_cast<int>(mi.rcWork.right - mi.rcWork.left));
                    const int winH = (std::min)(static_cast<int>(rc.bottom - rc.top),
                        static_cast<int>(mi.rcWork.bottom - mi.rcWork.top));
                    const int cx = mi.rcWork.left + (static_cast<int>(mi.rcWork.right - mi.rcWork.left) - winW) / 2;
                    const int cy = mi.rcWork.top + (static_cast<int>(mi.rcWork.bottom - mi.rcWork.top) - winH) / 2;
                    SetWindowPos(target, HWND_TOP, cx, cy, winW, winH, SWP_SHOWWINDOW);
                }
            }
        }

        BringWindowForeground(target);

        WORD arrowVk = 0;
        switch (zone)
        {
        case SnapZone::Left: arrowVk = VK_LEFT; break;
        case SnapZone::Right: arrowVk = VK_RIGHT; break;
        case SnapZone::TopMaximize: arrowVk = VK_UP; break;
        default: return;
        }

        INPUT inputs[4]{};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wVk = VK_LWIN;
        inputs[0].ki.dwExtraInfo = kWinRingsInjectedTag;

        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wVk = arrowVk;
        inputs[1].ki.dwExtraInfo = kWinRingsInjectedTag;

        inputs[2].type = INPUT_KEYBOARD;
        inputs[2].ki.wVk = arrowVk;
        inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
        inputs[2].ki.dwExtraInfo = kWinRingsInjectedTag;

        inputs[3].type = INPUT_KEYBOARD;
        inputs[3].ki.wVk = VK_LWIN;
        inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
        inputs[3].ki.dwExtraInfo = kWinRingsInjectedTag;

        SendInput(4, inputs, sizeof(INPUT));
        ShellDiagLog(L"Snap", L"Applied native Win+Arrow snap zone=%d hwnd=0x%p",
            static_cast<int>(zone), target);
    }

    LRESULT CALLBACK WindowSnapCtrl::PreviewWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept
    {
        auto* self = reinterpret_cast<WindowSnapCtrl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE)
        {
            self = static_cast<WindowSnapCtrl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self)
                self->m_previewHwnd = hwnd;
        }
        if (self)
        {
            try { return self->HandleMessage(hwnd, msg, wp, lp); }
            catch (...) {}
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT WindowSnapCtrl::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_TIMER:
            if (wp == kTimerLiveProbe)
            {
                KillTimer(hwnd, kTimerLiveProbe);
                HWND target = m_liveProbeHwnd;
                m_liveProbeActive = false;
                m_liveProbeHwnd = nullptr;
                if (target && IsWindow(target))
                {
                    GUITHREADINFO gti{ sizeof(gti) };
                    const DWORD tid = GetWindowThreadProcessId(target, nullptr);
                    const bool inNativeDrag = (tid != 0 && GetGUIThreadInfo(tid, &gti) && (gti.flags & GUI_INMOVESIZE) != 0);
                    if (inNativeDrag)
                    {
                        ShellDiagLog(L"Snap", L"LiveDrag active hwnd=0x%p", target);
                    }
                    else if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0)
                    {
                        ShellDiagLog(L"Snap", L"LiveDrag declined -> Fallback edge snap hwnd=0x%p", target);
                        m_fallbackActive = true;
                        m_fallbackIsClosedApp = false;
                        m_fallbackTargetHwnd = target;
                        POINT cur{};
                        if (GetCursorPos(&cur))
                            UpdatePreviewForPoint(cur);
                    }
                }
                return 0;
            }
            if (wp == kTimerPendingExpire)
            {
                ClearPendingLaunch();
                return 0;
            }
            if (wp == kTimerSettleSnap)
            {
                KillTimer(hwnd, kTimerSettleSnap);
                HWND target = m_settleHwnd;
                const SnapZone z = m_settleZone;
                HMONITOR mon = m_settleMonitor;
                m_settleHwnd = nullptr;
                m_settleZone = SnapZone::None;
                m_settleMonitor = nullptr;
                if (target && z != SnapZone::None)
                    ApplyNativeSnap(target, z, mon);
                return 0;
            }
            break;

        default:
            break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
} // namespace winrings
