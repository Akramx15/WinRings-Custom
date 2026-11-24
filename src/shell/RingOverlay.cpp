#include "RingOverlay.h"

#include <ShellScalingApi.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_set>

#include "../shared/SearchMatch.h"
#include "../shared/WinRingsIdentity.h"
#include "AppIndex.h"

namespace winrings
{
    namespace
    {
        constexpr double kOverlayDip = 760.0;
        constexpr double kCenterDip = kOverlayDip * 0.5;
        constexpr UINT_PTR kAnimTimerId = 101;
        constexpr UINT kAnimIntervalMs = 16;
        constexpr double kOpenDurationMs = 120.0;
        constexpr double kIdleSlotScale = 0.90;
        constexpr double kSelectedSlotScale = 1.20;
        constexpr double kSearchMouseUnlockDip = 4.0;
        constexpr wchar_t kWcaBackdropClass[] = L"WinRings.WcaBackdrop.v1";

        struct WcaAccentPolicy
        {
            int state;
            int flags;
            DWORD gradientColor;
            int animationId;
        };

        struct WcaAttrData
        {
            DWORD attrib;
            PVOID pvData;
            SIZE_T cbData;
        };

        using SetWindowCompositionAttributeFn = BOOL (WINAPI*)(HWND, WcaAttrData*);

        inline uint64_t HashMix(uint64_t h, uint64_t v) noexcept
        {
            h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        }

        D2D1_COLOR_F Argb(BYTE a, BYTE r, BYTE g, BYTE b) noexcept
        {
            return D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
        }

        bool IsClientAreaAnimationEnabled() noexcept
        {
            BOOL enabled = TRUE;
            SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
            return enabled != FALSE;
        }
    } // namespace

    RingOverlay::~RingOverlay()
    {
        Shutdown();
    }

    bool RingOverlay::Initialize(HINSTANCE instance, HMODULE shellModule, WindowTracker* tracker,
        Callbacks callbacks, AppIndex* appIndex)
    {
        m_instance = instance;
        m_shellModule = shellModule;
        m_tracker = tracker;
        m_appIndex = appIndex;
        m_callbacks = std::move(callbacks);

        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        m_qpcFreq = freq.QuadPart > 0 ? freq.QuadPart : 1;

        if (!m_classRegistered)
        {
            WNDCLASSEXW wc{ sizeof(wc) };
            wc.style = CS_DBLCLKS;
            wc.lpfnWndProc = OverlayWndProc;
            wc.hInstance = m_instance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = WINRINGS_OVERLAY_CLASS_W;
            if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                return false;
            m_classRegistered = true;
        }

        m_hwnd = CreateWindowExW(
            WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
            WINRINGS_OVERLAY_CLASS_W,
            L"WinRings Overlay",
            WS_POPUP,
            0, 0, 760, 760,
            nullptr, nullptr, m_instance, this);
        if (!m_hwnd)
            return false;

        m_wcaBackdropActive = CreateWcaBackdropWindow();

        if (!CreateGraphicsDevices())
            return false;

        m_thumbPool.Initialize(m_instance, m_hwnd);

        m_dropTarget = new RingDropTarget({
            [this]() { OnDragEnterOverlay(); },
            [this](POINT pt) { OnDragOverOverlay(pt); },
            [this]() { OnDragLeaveOverlay(); },
            [this](POINT pt, std::vector<std::wstring> files, std::wstring text) {
                OnDropOverlay(pt, std::move(files), std::move(text));
            }
        });
        RegisterDragDrop(m_hwnd, m_dropTarget);

        return true;
    }

    void RingOverlay::Shutdown()
    {
        Dismiss();
        if (m_hwnd)
        {
            RevokeDragDrop(m_hwnd);
        }
        if (m_dropTarget)
        {
            m_dropTarget->Release();
            m_dropTarget = nullptr;
        }
        m_thumbPool.Shutdown();
        ReleaseGraphicsDevices();
        DestroyWcaBackdropWindow();
        if (m_hwnd)
        {
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
        }
    }

    LRESULT CALLBACK RingOverlay::WcaBackdropWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_PAINT:
        {
            // Identical transparent-DIB paint pass to A4S-Menu (MenuPaint.cpp:3650-3655):
            // BPPF_ERASE clears the 32-bit surface to alpha=0 so native WCA ACCENT_ENABLE_ACRYLICBLURBEHIND
            // composites directly with zero GDI fill interference.
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            BP_PAINTPARAMS pp{ sizeof(pp), BPPF_ERASE, nullptr, nullptr };
            HDC memDc = nullptr;
            if (HPAINTBUFFER hbp = BeginBufferedPaint(hdc, &ps.rcPaint, BPBF_TOPDOWNDIB, &pp, &memDc))
                EndBufferedPaint(hbp, TRUE);
            EndPaint(hwnd, &ps);
            return 0;
        }
        default:
            break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    bool RingOverlay::CreateWcaBackdropWindow()
    {
        DestroyWcaBackdropWindow();

        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        auto setComp = user32
            ? reinterpret_cast<SetWindowCompositionAttributeFn>(GetProcAddress(user32, "SetWindowCompositionAttribute"))
            : nullptr;
        if (!setComp)
            return false;

        if (!m_bufferedPaintInit)
        {
            BufferedPaintInit();
            m_bufferedPaintInit = true;
        }

        if (!m_wcaClassRegistered)
        {
            WNDCLASSEXW wc{ sizeof(wc) };
            wc.lpfnWndProc = WcaBackdropWndProc;
            wc.hInstance = m_instance;
            wc.hbrBackground = nullptr;
            wc.lpszClassName = kWcaBackdropClass;
            if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                return false;
            m_wcaClassRegistered = true;
        }

        m_wcaBackdropHwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
            kWcaBackdropClass,
            L"",
            WS_POPUP,
            0, 0, 760, 760,
            nullptr, nullptr, m_instance, nullptr);
        if (!m_wcaBackdropHwnd)
            return false;

        // Suppress square DWM rounding and square outer border on the 760x760 bounding HWND;
        // SetWindowRgn clips WCA acrylic precisely to the ring annulus, outer arc, and pills.
        const int noRound = 1; // DWMWCP_DONOTROUND
        DwmSetWindowAttribute(m_wcaBackdropHwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &noRound, sizeof(noRound));
        const DWORD noBorder = 0xFFFFFFFEu; // DWMWA_COLOR_NONE
        DwmSetWindowAttribute(m_wcaBackdropHwnd, 34 /* DWMWA_BORDER_COLOR */, &noBorder, sizeof(noBorder));

        BOOL forceActive = TRUE;
        WcaAttrData activeData{ 15 /* WCA_FORCE_ACTIVEWINDOW_APPEARANCE */, &forceActive, sizeof(forceActive) };
        setComp(m_wcaBackdropHwnd, &activeData);

        m_lastWcaState = -1;
        m_lastWcaGradColor = 0;
        m_lastWcaDark = -1;
        m_lastWcaRegionHash = 0;
        UpdateWcaMaterial(BuildThemeColors());
        return (m_lastWcaState >= 0);
    }

    void RingOverlay::DestroyWcaBackdropWindow()
    {
        if (m_wcaBackdropHwnd)
        {
            DestroyWindow(m_wcaBackdropHwnd);
            m_wcaBackdropHwnd = nullptr;
        }
        m_wcaBackdropActive = false;
        m_lastWcaState = -1;
        m_lastWcaGradColor = 0;
        m_lastWcaDark = -1;
        m_lastWcaRegionHash = 0;
        if (m_bufferedPaintInit)
        {
            BufferedPaintUnInit();
            m_bufferedPaintInit = false;
        }
    }

    void RingOverlay::UpdateWcaMaterial(const ThemeColors& tc)
    {
        if (!m_wcaBackdropHwnd)
            return;

        const BYTE r = static_cast<BYTE>(std::clamp(static_cast<int>(std::lround(tc.tintR * 255.0f)), 0, 255));
        const BYTE g = static_cast<BYTE>(std::clamp(static_cast<int>(std::lround(tc.tintG * 255.0f)), 0, 255));
        const BYTE b = static_cast<BYTE>(std::clamp(static_cast<int>(std::lround(tc.tintB * 255.0f)), 0, 255));
        const BYTE a = static_cast<BYTE>(std::clamp(static_cast<int>(std::lround(tc.tintAlpha * 255.0f)), 2, 255));
        const DWORD gradColor = (static_cast<DWORD>(a) << 24) | RGB(r, g, b);
        const int state = (tc.blurSigma > 1.0f) ? 4 /* ACCENT_ENABLE_ACRYLICBLURBEHIND */ : 2 /* ACCENT_ENABLE_TRANSPARENTGRADIENT */;
        const int darkVal = tc.isLightText ? 1 : 0;

        if (state == m_lastWcaState && gradColor == m_lastWcaGradColor && darkVal == m_lastWcaDark)
            return;

        const BOOL dark = darkVal ? TRUE : FALSE;
        DwmSetWindowAttribute(m_wcaBackdropHwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));

        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        auto setComp = user32
            ? reinterpret_cast<SetWindowCompositionAttributeFn>(GetProcAddress(user32, "SetWindowCompositionAttribute"))
            : nullptr;
        if (!setComp)
            return;

        BOOL forceActive = TRUE;
        WcaAttrData activeData{ 15 /* WCA_FORCE_ACTIVEWINDOW_APPEARANCE */, &forceActive, sizeof(forceActive) };
        setComp(m_wcaBackdropHwnd, &activeData);

        // Exact same WCA_ACCENT_POLICY invocation as A4S-Menu (MenuContainer.cpp:126-129):
        // At default A4S (glassLevel=50, lightDarkMode=0): state=4, flags=0x13, gradientColor=0x80202020.
        WcaAccentPolicy policy{ state, 0x13, gradColor, 0 };
        WcaAttrData accentData{ 19 /* WCA_ACCENT_POLICY */, &policy, sizeof(policy) };
        if (setComp(m_wcaBackdropHwnd, &accentData))
        {
            m_lastWcaState = state;
            m_lastWcaGradColor = gradColor;
            m_lastWcaDark = darkVal;
            InvalidateRect(m_wcaBackdropHwnd, nullptr, FALSE);
        }
    }

    void RingOverlay::UpdateWcaRegion(float dpiScale, float openScale, float outerR, float holeR,
        bool hasCurvedArc, bool hasQuery, const D2D1_ROUNDED_RECT& searchPill,
        bool hasSummary, const D2D1_ROUNDED_RECT& centerPill)
    {
        if (!m_wcaBackdropHwnd)
            return;

        auto dipToPx = [&](double v) -> double {
            return (kCenterDip + (v - kCenterDip) * static_cast<double>(openScale)) * static_cast<double>(dpiScale);
        };
        const double cxPx = kCenterDip * static_cast<double>(dpiScale);
        const double cyPx = kCenterDip * static_cast<double>(dpiScale);
        const double scalePx = static_cast<double>(dpiScale) * static_cast<double>(openScale);

        // Hash every region parameter at 0.25 px precision to avoid redundant SetWindowRgn calls.
        uint64_t h = 0x517CC1B727220A95ULL;
        h = HashMix(h, static_cast<uint64_t>(std::llround(dpiScale * 100.0f)));
        h = HashMix(h, static_cast<uint64_t>(std::llround(openScale * 1000.0f)));
        h = HashMix(h, static_cast<uint64_t>(std::llround(outerR * 4.0f)));
        h = HashMix(h, static_cast<uint64_t>(std::llround(holeR * 4.0f)));
        h = HashMix(h, hasCurvedArc ? 1ULL : 0ULL);
        if (hasCurvedArc)
        {
            h = HashMix(h, static_cast<uint64_t>(std::llround(m_submenuArcLayout.arcRadius * 4.0)));
            h = HashMix(h, static_cast<uint64_t>(std::llround(m_submenuArcLayout.startAngleRad * 1000.0)));
            h = HashMix(h, static_cast<uint64_t>(std::llround(m_submenuArcLayout.endAngleRad * 1000.0)));
            h = HashMix(h, static_cast<uint64_t>(std::llround(m_submenuArcLayout.trackWidthDip * 4.0)));
        }
        if ((m_expandedSlotIndex >= 0 || m_searchArcActive) && !m_submenuCards.empty())
        {
            for (const auto& card : m_submenuCards)
            {
                if (!card.window)
                    continue;
                h = HashMix(h, static_cast<uint64_t>(std::llround(card.boundsDip.left * 2.0)));
                h = HashMix(h, static_cast<uint64_t>(std::llround(card.boundsDip.top * 2.0)));
                h = HashMix(h, static_cast<uint64_t>(std::llround(card.boundsDip.width * 2.0)));
                h = HashMix(h, static_cast<uint64_t>(std::llround(card.boundsDip.height * 2.0)));
                h = HashMix(h, static_cast<uint64_t>(std::llround(card.currentScale * 100.0)));
            }
        }
        h = HashMix(h, m_dragActive ? 1ULL : 0ULL);
        h = HashMix(h, hasQuery ? 1ULL : 0ULL);
        if (hasQuery)
        {
            h = HashMix(h, static_cast<uint64_t>(std::llround(searchPill.rect.left * 2.0f)));
            h = HashMix(h, static_cast<uint64_t>(std::llround(searchPill.rect.top * 2.0f)));
            h = HashMix(h, static_cast<uint64_t>(std::llround(searchPill.rect.right * 2.0f)));
            h = HashMix(h, static_cast<uint64_t>(std::llround(searchPill.rect.bottom * 2.0f)));
        }
        h = HashMix(h, hasSummary ? 1ULL : 0ULL);
        if (hasSummary)
        {
            h = HashMix(h, static_cast<uint64_t>(std::llround(centerPill.rect.left * 2.0f)));
            h = HashMix(h, static_cast<uint64_t>(std::llround(centerPill.rect.top * 2.0f)));
            h = HashMix(h, static_cast<uint64_t>(std::llround(centerPill.rect.right * 2.0f)));
            h = HashMix(h, static_cast<uint64_t>(std::llround(centerPill.rect.bottom * 2.0f)));
        }

        if (h == m_lastWcaRegionHash)
            return;

        // Inset region by 1.0 px on every perimeter so the 1-bit HRGN staircase sits strictly inside
        // the silky antialiased Direct2D edge-bridge & 3D glass bezel drawn on m_hwnd.
        const double outerPx = (std::max)(2.0, static_cast<double>(outerR) * scalePx - 1.0);
        const double holePx = (std::max)(1.0, static_cast<double>(holeR) * scalePx + 1.0);

        HRGN combined = CreateEllipticRgn(
            static_cast<int>(std::lround(cxPx - outerPx)),
            static_cast<int>(std::lround(cyPx - outerPx)),
            static_cast<int>(std::lround(cxPx + outerPx)) + 1,
            static_cast<int>(std::lround(cyPx + outerPx)) + 1);
        if (!combined)
            return;

        if (holePx < outerPx - 1.0)
        {
            HRGN inner = CreateEllipticRgn(
                static_cast<int>(std::lround(cxPx - holePx)),
                static_cast<int>(std::lround(cyPx - holePx)),
                static_cast<int>(std::lround(cxPx + holePx)) + 1,
                static_cast<int>(std::lround(cyPx + holePx)) + 1);
            if (inner)
            {
                CombineRgn(combined, combined, inner, RGN_DIFF);
                DeleteObject(inner);
            }
        }

        auto addRoundRect = [&](double lDip, double tDip, double rDip, double bDip, double radDip) {
            const int l = static_cast<int>(std::lround(dipToPx(lDip) + 1.0));
            const int t = static_cast<int>(std::lround(dipToPx(tDip) + 1.0));
            const int r = static_cast<int>(std::lround(dipToPx(rDip) - 1.0)) + 1;
            const int b = static_cast<int>(std::lround(dipToPx(bDip) - 1.0)) + 1;
            const int d = (std::max)(2, static_cast<int>(std::lround(radDip * 2.0 * scalePx)));
            if (r > l && b > t)
            {
                if (HRGN rr = CreateRoundRectRgn(l, t, r, b, d, d))
                {
                    CombineRgn(combined, combined, rr, RGN_OR);
                    DeleteObject(rr);
                }
            }
        };

        if (hasCurvedArc && m_submenuArcLayout.trackWidthDip > 2.0)
        {
            const double arcR = m_submenuArcLayout.arcRadius * scalePx;
            const double halfW = (std::max)(1.0, m_submenuArcLayout.trackWidthDip * 0.5 * scalePx - 1.0);
            const double a0 = m_submenuArcLayout.startAngleRad;
            const double a1 = m_submenuArcLayout.endAngleRad;
            const double acx = cxPx;
            const double acy = cyPx;

            if (std::abs(a1 - a0) < 0.01)
            {
                const double ex = acx + arcR * std::cos(a0);
                const double ey = acy + arcR * std::sin(a0);
                if (HRGN cap = CreateEllipticRgn(
                    static_cast<int>(std::lround(ex - halfW)),
                    static_cast<int>(std::lround(ey - halfW)),
                    static_cast<int>(std::lround(ex + halfW)) + 1,
                    static_cast<int>(std::lround(ey + halfW)) + 1))
                {
                    CombineRgn(combined, combined, cap, RGN_OR);
                    DeleteObject(cap);
                }
            }
            else
            {
                constexpr int kArcSteps = 44;
                constexpr int kCapSteps = 14;
                std::vector<POINT> pts;
                pts.reserve(static_cast<size_t>((kArcSteps + 1) * 2 + (kCapSteps + 1) * 2));
                const double rOut = arcR + halfW;
                const double rIn = (std::max)(1.0, arcR - halfW);
                const double dir = (a1 >= a0) ? 1.0 : -1.0;

                for (int i = 0; i <= kArcSteps; ++i)
                {
                    const double ang = a0 + (a1 - a0) * (static_cast<double>(i) / kArcSteps);
                    pts.push_back(POINT{
                        static_cast<LONG>(std::lround(acx + rOut * std::cos(ang))),
                        static_cast<LONG>(std::lround(acy + rOut * std::sin(ang)))
                    });
                }
                const double c1x = acx + arcR * std::cos(a1);
                const double c1y = acy + arcR * std::sin(a1);
                for (int i = 1; i < kCapSteps; ++i)
                {
                    const double ang = a1 + dir * kPi * (static_cast<double>(i) / kCapSteps);
                    pts.push_back(POINT{
                        static_cast<LONG>(std::lround(c1x + halfW * std::cos(ang))),
                        static_cast<LONG>(std::lround(c1y + halfW * std::sin(ang)))
                    });
                }
                for (int i = kArcSteps; i >= 0; --i)
                {
                    const double ang = a0 + (a1 - a0) * (static_cast<double>(i) / kArcSteps);
                    pts.push_back(POINT{
                        static_cast<LONG>(std::lround(acx + rIn * std::cos(ang))),
                        static_cast<LONG>(std::lround(acy + rIn * std::sin(ang)))
                    });
                }
                const double c0x = acx + arcR * std::cos(a0);
                const double c0y = acy + arcR * std::sin(a0);
                for (int i = 1; i < kCapSteps; ++i)
                {
                    const double ang = a0 + dir * kPi + dir * kPi * (static_cast<double>(i) / kCapSteps);
                    pts.push_back(POINT{
                        static_cast<LONG>(std::lround(c0x + halfW * std::cos(ang))),
                        static_cast<LONG>(std::lround(c0y + halfW * std::sin(ang)))
                    });
                }
                if (HRGN arcRgn = CreatePolygonRgn(pts.data(), static_cast<int>(pts.size()), WINDING))
                {
                    CombineRgn(combined, combined, arcRgn, RGN_OR);
                    DeleteObject(arcRgn);
                }
            }
        }

        if ((m_expandedSlotIndex >= 0 || m_searchArcActive) && !m_submenuCards.empty())
        {
            for (const auto& card : m_submenuCards)
            {
                if (!card.window)
                    continue;
                const double cx = card.boundsDip.left + card.boundsDip.width * 0.5;
                const double cy = card.boundsDip.top + card.boundsDip.height * 0.5;
                const double hw = card.boundsDip.width * 0.5 * card.currentScale;
                const double hh = card.boundsDip.height * 0.5 * card.currentScale;
                addRoundRect(cx - hw, cy - hh, cx + hw, cy + hh, 8.0);
            }
        }

        if (hasQuery)
            addRoundRect(searchPill.rect.left, searchPill.rect.top, searchPill.rect.right, searchPill.rect.bottom, searchPill.radiusX);
        if (hasSummary)
            addRoundRect(centerPill.rect.left, centerPill.rect.top, centerPill.rect.right, centerPill.rect.bottom, centerPill.radiusX);

        if (SetWindowRgn(m_wcaBackdropHwnd, combined, TRUE))
            m_lastWcaRegionHash = h;
        else
            DeleteObject(combined);
    }

    void RingOverlay::SetConfig(const SwitcherConfig& config)
    {
        const bool blurChanged = (m_config.disableRingBlur != config.disableRingBlur);
        m_config = config;
        if (blurChanged)
        {
            if (m_config.disableRingBlur && m_wcaBackdropHwnd && IsWindow(m_wcaBackdropHwnd))
                ShowWindow(m_wcaBackdropHwnd, SW_HIDE);
            if (m_hwnd)
            {
                ReleaseGraphicsDevices();
                CreateGraphicsDevices();
            }
        }
        if (m_isOpen)
            Render();
    }

    bool RingOverlay::CreateGraphicsDevices()
    {
        const UINT d3dFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_DRIVER_TYPE drivers[] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP };
        HRESULT hr = E_FAIL;
        for (auto driver : drivers)
        {
            hr = D3D11CreateDevice(nullptr, driver, nullptr, d3dFlags, nullptr, 0,
                D3D11_SDK_VERSION, m_d3dDevice.ReleaseAndGetAddressOf(), nullptr, nullptr);
            if (SUCCEEDED(hr))
                break;
        }
        if (FAILED(hr) || !m_d3dDevice)
            return false;

        if (FAILED(m_d3dDevice.As(&m_dxgiDevice)) || !m_dxgiDevice)
            return false;

        D2D1_FACTORY_OPTIONS opts{};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
            &opts, reinterpret_cast<void**>(m_d2dFactory.ReleaseAndGetAddressOf()))))
            return false;

        if (FAILED(m_d2dFactory->CreateDevice(m_dxgiDevice.Get(), m_d2dDevice.ReleaseAndGetAddressOf())))
            return false;

        if (FAILED(m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
            m_d2dContext.ReleaseAndGetAddressOf())))
            return false;

        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwriteFactory.ReleaseAndGetAddressOf()))))
            return false;

        m_dwriteFactory->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            13.0f, L"en-us", m_titleFormat.ReleaseAndGetAddressOf());
        if (m_titleFormat)
        {
            m_titleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_titleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
            m_dwriteFactory->CreateEllipsisTrimmingSign(m_titleFormat.Get(), ellipsis.GetAddressOf());
            m_titleFormat->SetTrimming(&trim, ellipsis.Get());
            m_titleFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }

        m_dwriteFactory->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            11.0f, L"en-us", m_smallFormat.ReleaseAndGetAddressOf());
        if (m_smallFormat)
        {
            m_smallFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_smallFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
            m_dwriteFactory->CreateEllipsisTrimmingSign(m_smallFormat.Get(), ellipsis.GetAddressOf());
            m_smallFormat->SetTrimming(&trim, ellipsis.Get());
            m_smallFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }

        m_dwriteFactory->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            13.0f, L"en-us", m_searchFormat.ReleaseAndGetAddressOf());
        if (m_searchFormat)
        {
            m_searchFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            m_searchFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
            m_dwriteFactory->CreateEllipsisTrimmingSign(m_searchFormat.Get(), ellipsis.GetAddressOf());
            m_searchFormat->SetTrimming(&trim, ellipsis.Get());
            m_searchFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }

        m_dwriteFactory->CreateTextFormat(L"Segoe UI Variable Display", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            16.0f, L"en-us", m_glyphFormat.ReleaseAndGetAddressOf());
        if (m_glyphFormat)
        {
            m_glyphFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_glyphFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        if (!EnsureSwapChainSize(760, 760))
            return false;

        EnsureNoiseBrush();

        if (m_config.disableRingBlur || m_wcaBackdropActive ||
            !m_acrylicBackdrop.Initialize(m_hwnd, m_swapChain.Get(), m_maskSwapChain.Get(),
                m_swapWidthPx, m_swapHeightPx))
        {
            // When native WCA ACCENT_ENABLE_ACRYLICBLURBEHIND is active on m_wcaBackdropHwnd (exact A4S-Menu path),
            // m_hwnd renders crisp antialiased foreground chrome & icons over it via lightweight IDCompositionTarget.
            if (FAILED(DCompositionCreateDevice(m_dxgiDevice.Get(), IID_PPV_ARGS(m_dcompDevice.ReleaseAndGetAddressOf()))))
                return false;
            if (FAILED(m_dcompDevice->CreateTargetForHwnd(m_hwnd, TRUE, m_dcompTarget.ReleaseAndGetAddressOf())))
                return false;
            if (FAILED(m_dcompDevice->CreateVisual(m_dcompVisual.ReleaseAndGetAddressOf())))
                return false;
            if (FAILED(m_dcompVisual->SetContent(m_swapChain.Get())))
                return false;
            if (FAILED(m_dcompTarget->SetRoot(m_dcompVisual.Get())))
                return false;
        }
        else
        {
            m_acrylicBackdrop.UpdateMaterial(BuildAcrylicMaterial());
        }

        return true;
    }

    void RingOverlay::ReleaseGraphicsDevices()
    {
        m_d2dBitmapCache.clear();
        m_noiseBrush.Reset();
        m_noiseBitmap.Reset();
        if (m_d2dContext)
            m_d2dContext->SetTarget(nullptr);
        m_acrylicBackdrop.Shutdown();
        m_maskTargetBitmap.Reset();
        m_maskSwapChain.Reset();
        m_targetBitmap.Reset();
        m_swapChain.Reset();
        m_dcompVisual.Reset();
        m_dcompTarget.Reset();
        m_dcompDevice.Reset();
        m_glyphFormat.Reset();
        m_searchFormat.Reset();
        m_smallFormat.Reset();
        m_titleFormat.Reset();
        m_dwriteFactory.Reset();
        m_d2dContext.Reset();
        m_d2dDevice.Reset();
        m_d2dFactory.Reset();
        m_dxgiDevice.Reset();
        m_d3dDevice.Reset();
        m_swapWidthPx = 0;
        m_swapHeightPx = 0;
    }

    bool RingOverlay::EnsureSwapChainSize(int widthPx, int heightPx)
    {
        if (!m_dxgiDevice || !m_d2dContext)
            return false;

        widthPx = (std::max)(1, widthPx);
        heightPx = (std::max)(1, heightPx);
        if (m_swapChain && m_maskSwapChain && m_swapWidthPx == widthPx && m_swapHeightPx == heightPx &&
            m_targetBitmap && m_maskTargetBitmap)
            return true;

        m_d2dContext->SetTarget(nullptr);
        m_targetBitmap.Reset();
        m_maskTargetBitmap.Reset();

        if (!m_swapChain || !m_maskSwapChain)
        {
            m_swapChain.Reset();
            m_maskSwapChain.Reset();

            Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
            if (FAILED(m_dxgiDevice->GetAdapter(adapter.GetAddressOf())))
                return false;
            Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
            if (FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()))))
                return false;

            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = static_cast<UINT>(widthPx);
            desc.Height = static_cast<UINT>(heightPx);
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.Scaling = DXGI_SCALING_STRETCH;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

            if (FAILED(factory->CreateSwapChainForComposition(m_dxgiDevice.Get(), &desc, nullptr,
                m_swapChain.ReleaseAndGetAddressOf())))
                return false;
            if (FAILED(factory->CreateSwapChainForComposition(m_dxgiDevice.Get(), &desc, nullptr,
                m_maskSwapChain.ReleaseAndGetAddressOf())))
                return false;

            if (m_dcompVisual && m_dcompTarget)
            {
                if (FAILED(m_dcompVisual->SetContent(m_swapChain.Get())))
                    return false;
                if (FAILED(m_dcompTarget->SetRoot(m_dcompVisual.Get())))
                    return false;
            }
        }
        else
        {
            if (FAILED(m_swapChain->ResizeBuffers(2, static_cast<UINT>(widthPx), static_cast<UINT>(heightPx),
                DXGI_FORMAT_B8G8R8A8_UNORM, 0)))
                return false;
            if (FAILED(m_maskSwapChain->ResizeBuffers(2, static_cast<UINT>(widthPx), static_cast<UINT>(heightPx),
                DXGI_FORMAT_B8G8R8A8_UNORM, 0)))
                return false;
        }

        D2D1_BITMAP_PROPERTIES1 bp{};
        bp.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
        bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        bp.dpiX = 96.0f;
        bp.dpiY = 96.0f;

        Microsoft::WRL::ComPtr<IDXGISurface> fgSurface;
        if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(fgSurface.GetAddressOf()))) ||
            FAILED(m_d2dContext->CreateBitmapFromDxgiSurface(fgSurface.Get(), &bp,
                m_targetBitmap.ReleaseAndGetAddressOf())))
            return false;

        Microsoft::WRL::ComPtr<IDXGISurface> maskSurface;
        if (FAILED(m_maskSwapChain->GetBuffer(0, IID_PPV_ARGS(maskSurface.GetAddressOf()))) ||
            FAILED(m_d2dContext->CreateBitmapFromDxgiSurface(maskSurface.Get(), &bp,
                m_maskTargetBitmap.ReleaseAndGetAddressOf())))
            return false;

        m_swapWidthPx = widthPx;
        m_swapHeightPx = heightPx;
        if (m_acrylicBackdrop.IsActive())
            m_acrylicBackdrop.Resize(widthPx, heightPx);
        return true;
    }

    Microsoft::WRL::ComPtr<ID2D1Bitmap1> RingOverlay::GetOrCreateD2DBitmap(IWICBitmapSource* wicSource)
    {
        if (!wicSource || !m_d2dContext)
            return nullptr;
        if (auto it = m_d2dBitmapCache.find(wicSource); it != m_d2dBitmapCache.end())
            return it->second;

        Microsoft::WRL::ComPtr<ID2D1Bitmap1> bmp;
        if (FAILED(m_d2dContext->CreateBitmapFromWicBitmap(wicSource, nullptr, bmp.GetAddressOf())) || !bmp)
            return nullptr;

        if (m_d2dBitmapCache.size() > 256)
            m_d2dBitmapCache.clear();
        m_d2dBitmapCache.emplace(wicSource, bmp);
        return bmp;
    }

    RingGeometry RingOverlay::ActiveGeometry() const noexcept
    {
        const int slotCount = (std::max)(1, static_cast<int>(m_visibleSlots.size()));
        return ComputeRingGeometry(slotCount, m_config.ringRadiusDip, m_config.iconSizeDip, m_config.deadzoneDip);
    }

    void RingOverlay::UpdateDpiAndPosition(int physicalX, int physicalY)
    {
        POINT pt{ physicalX, physicalY };
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);

        UINT dpiX = 96, dpiY = 96;
        using GetDpiForMonitorFn = HRESULT (WINAPI*)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);
        static auto pGetDpiForMonitor = reinterpret_cast<GetDpiForMonitorFn>(
            GetProcAddress(LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32), "GetDpiForMonitor"));
        if (pGetDpiForMonitor && SUCCEEDED(pGetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX > 0)
            m_dpiScale = static_cast<double>(dpiX) / 96.0;
        else
            m_dpiScale = 1.0;

        MONITORINFO mi{ sizeof(mi) };
        RectD workAreaDip{ 0.0, 0.0, 1920.0, 1080.0 };
        if (GetMonitorInfoW(mon, &mi))
        {
            workAreaDip = RectD{
                mi.rcWork.left / m_dpiScale,
                mi.rcWork.top / m_dpiScale,
                (mi.rcWork.right - mi.rcWork.left) / m_dpiScale,
                (mi.rcWork.bottom - mi.rcWork.top) / m_dpiScale
            };
        }

        const RingGeometry geom = ActiveGeometry();
        const double padDip = (std::min)(geom.outerRadius + 78.0,
            (std::min)(workAreaDip.width, workAreaDip.height) * 0.48);
        const double cursorDipX = physicalX / m_dpiScale;
        const double cursorDipY = physicalY / m_dpiScale;
        const PointD clampedDip = ClampRingCenter(cursorDipX, cursorDipY, padDip, workAreaDip);

        const int physSize = static_cast<int>(std::llround(kOverlayDip * m_dpiScale));
        const int leftPx = static_cast<int>(std::llround((clampedDip.x - kCenterDip) * m_dpiScale));
        const int topPx = static_cast<int>(std::llround((clampedDip.y - kCenterDip) * m_dpiScale));

        m_centerScreenPhysical = { clampedDip.x * m_dpiScale, clampedDip.y * m_dpiScale };

        EnsureSwapChainSize(physSize, physSize);
        SetWindowPos(m_hwnd, HWND_TOPMOST, leftPx, topPx, physSize, physSize, SWP_NOACTIVATE);
        if (m_wcaBackdropHwnd)
            SetWindowPos(m_wcaBackdropHwnd, m_hwnd, leftPx, topPx, physSize, physSize, SWP_NOACTIVATE);
    }

    bool RingOverlay::IsPointInActiveBounds(POINT screenPt) const noexcept
    {
        if (!m_isOpen || !m_hwnd)
            return false;
        const RingGeometry geom = ActiveGeometry();
        const double dx = (screenPt.x - m_centerScreenPhysical.x) / m_dpiScale;
        const double dy = (screenPt.y - m_centerScreenPhysical.y) / m_dpiScale;
        const double dist = std::hypot(dx, dy);
        if (dist <= geom.outerRadius + 16.0)
            return true;
        if (!m_submenuCardRects.empty())
        {
            const PointD local{ kCenterDip + dx, kCenterDip + dy };
            for (const auto& cardRect : m_submenuCardRects)
            {
                const RectD expanded{
                    cardRect.left - 18.0, cardRect.top - 18.0,
                    cardRect.width + 36.0, cardRect.height + 36.0
                };
                if (expanded.Contains(local))
                    return true;
            }
        }
        return false;
    }

    void RingOverlay::EnsureNoiseBrush()
    {
        if (m_noiseBrush || !m_d2dContext)
            return;

        const std::vector<uint8_t> noise = GenerateAcrylicNoiseBgra256();
        D2D1_BITMAP_PROPERTIES1 bp{};
        bp.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
        bp.dpiX = 96.0f;
        bp.dpiY = 96.0f;
        if (FAILED(m_d2dContext->CreateBitmap(
            D2D1::SizeU(256, 256), noise.data(), 256u * 4u, &bp, m_noiseBitmap.ReleaseAndGetAddressOf())) || !m_noiseBitmap)
            return;

        D2D1_BITMAP_BRUSH_PROPERTIES bbp = D2D1::BitmapBrushProperties(
            D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        m_d2dContext->CreateBitmapBrush(m_noiseBitmap.Get(), bbp, m_noiseBrush.ReleaseAndGetAddressOf());
    }

    AcrylicMaterialParams RingOverlay::BuildAcrylicMaterial() const noexcept
    {
        AcrylicMaterialParams p = AcrylicParamsFromMaterial(ComputeRingMaterial(m_config, m_dpiScale));
        p.blurSigma *= static_cast<float>((std::max)(1.0, m_dpiScale));
        return p;
    }

    RingOverlay::ThemeColors RingOverlay::BuildThemeColors() const noexcept
    {
        return ComputeRingMaterial(m_config, m_dpiScale);
    }

    void RingOverlay::OpenAt(int physicalX, int physicalY, std::vector<RingSlot> slots, bool lockOpen)
    {
        if (!m_hwnd)
            return;

        if (m_appIndex)
            m_appIndex->EnsureFreshAsync();

        m_allSlots = std::move(slots);
        m_visibleSlots = m_allSlots;
        m_slotSearchMatched.assign(m_visibleSlots.size(), true);
        m_slotScales.assign(m_visibleSlots.size(), kIdleSlotScale);
        m_searchResultSlots.clear();
        m_rankedMatches.clear();
        m_rankedMatchCursor = -1;
        m_filterQuery.clear();
        m_selectedIndex = -1;
        ClearSubmenu();
        m_corridorOriginDip = { kCenterDip, kCenterDip };
        m_hasLastPointer = false;
        m_lastPointerPhysical = {};
        m_searchMouseLocked = false;
        m_searchMouseAnchorDip = {};
        m_isLockedOpen = lockOpen;
        m_contextMenuOpen = false;
        m_dragActive = false;
        m_dragOverCenter = false;
        m_pressKind = PressKind::None;
        m_pressSlotIndex = -1;
        m_pressSubIndex = -1;
        m_pressScreenPt = {};
        m_pressStartTick = 0;
        m_reorderActive = false;
        m_reorderFromIndex = -1;
        m_reorderCurrentIndex = -1;
        m_reorderCursorDip = {};
        ResetDragSpringState();
        m_isOpen = true;

        UpdateDpiAndPosition(physicalX, physicalY);

        const bool staticOpen = m_config.disableOpeningAnimation || !IsClientAreaAnimationEnabled();
        if (staticOpen)
        {
            m_openProgress = 1.0;
        }
        else
        {
            m_openProgress = 0.0;
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            m_openStartQpc = now.QuadPart;
            if (!m_animTimerRunning)
            {
                SetTimer(m_hwnd, kAnimTimerId, kAnimIntervalMs, nullptr);
                m_animTimerRunning = true;
            }
        }

        Render();
        const bool showWca = (m_wcaBackdropHwnd && m_wcaBackdropActive && !m_config.disableRingBlur);
        if (showWca)
        {
            ShowWindow(m_wcaBackdropHwnd, SW_SHOWNOACTIVATE);
            UpdateWindow(m_wcaBackdropHwnd);
        }
        else if (m_wcaBackdropHwnd && IsWindow(m_wcaBackdropHwnd))
        {
            ShowWindow(m_wcaBackdropHwnd, SW_HIDE);
        }
        ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
        if (showWca)
            SetWindowPos(m_wcaBackdropHwnd, m_hwnd, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void RingOverlay::HandleEscapeKey()
    {
        if (!m_isOpen)
            return;
        if (!m_filterQuery.empty())
        {
            m_filterQuery.clear();
            RebuildLayoutAfterFilter();
            return;
        }
        Dismiss();
    }

    void RingOverlay::Dismiss()
    {
        if (!m_isOpen && !m_animTimerRunning)
            return;
        m_isOpen = false;
        m_isLockedOpen = false;
        m_contextMenuOpen = false;
        m_dragActive = false;
        m_dragOverCenter = false;
        m_pressKind = PressKind::None;
        m_pressSlotIndex = -1;
        m_pressSubIndex = -1;
        m_pressStartTick = 0;
        m_reorderActive = false;
        m_reorderFromIndex = -1;
        m_reorderCurrentIndex = -1;
        ResetDragSpringState();
        m_selectedIndex = -1;
        m_filterQuery.clear();
        m_rankedMatches.clear();
        m_rankedMatchCursor = -1;
        m_searchMouseLocked = false;

        if (m_hwnd && GetCapture() == m_hwnd)
            ReleaseCapture();

        if (m_animTimerRunning && m_hwnd)
        {
            KillTimer(m_hwnd, kAnimTimerId);
            m_animTimerRunning = false;
        }
        ClearSubmenu();
        if (m_wcaBackdropHwnd && IsWindow(m_wcaBackdropHwnd))
            ShowWindow(m_wcaBackdropHwnd, SW_HIDE);
        if (m_hwnd && IsWindow(m_hwnd))
            ShowWindow(m_hwnd, SW_HIDE);
        if (m_callbacks.onOverlayClosed)
            m_callbacks.onOverlayClosed();
    }

    bool RingOverlay::SlotHasSubmenuItems(const RingSlot& slot) const noexcept
    {
        return slot.ShowsWindowPreview(m_config.fanOutSingleWindow) || !slot.subItems.empty();
    }

    void RingOverlay::ClearSubmenu()
    {
        m_thumbPool.HideAll();
        m_submenuCards.clear();
        m_submenuCardRects.clear();
        m_searchResultSlots.clear();
        m_expandedSlotIndex = -1;
        m_selectedSubIndex = -1;
        m_hoveredCloseSubIndex = -1;
        m_searchArcActive = false;
        m_submenuHasArcTrack = false;
        m_submenuArcLayout = {};
        m_submenuBoundsDip = {};
        m_submenuIsTitleColumn = false;
    }

    D2D1_RECT_F RingOverlay::GetSubmenuCloseRectDip(const D2D1_RECT_F& cardRectDip, WindowViewMode mode) const noexcept
    {
        constexpr float sz = 16.0f;
        if (mode == WindowViewMode::TitleColumn)
        {
            const float cy = (cardRectDip.top + cardRectDip.bottom) * 0.5f;
            return D2D1::RectF(cardRectDip.right - 22.0f, cy - sz * 0.5f, cardRectDip.right - 6.0f, cy + sz * 0.5f);
        }
        return D2D1::RectF(cardRectDip.right - 20.0f, cardRectDip.bottom - 18.0f, cardRectDip.right - 4.0f, cardRectDip.bottom - 2.0f);
    }

    int RingOverlay::HitTestSubmenuCloseButton(PointD localDip) const noexcept
    {
        if (m_submenuCards.empty())
            return -1;
        for (size_t i = 0; i < m_submenuCards.size(); ++i)
        {
            const auto& card = m_submenuCards[i];
            if (!card.window)
                continue;
            const float cx = static_cast<float>(card.boundsDip.left + card.boundsDip.width * 0.5);
            const float cy = static_cast<float>(card.boundsDip.top + card.boundsDip.height * 0.5);
            const float hw = static_cast<float>(card.boundsDip.width * 0.5 * card.currentScale);
            const float hh = static_cast<float>(card.boundsDip.height * 0.5 * card.currentScale);
            const D2D1_RECT_F cardRectDip = D2D1::RectF(cx - hw, cy - hh, cx + hw, cy + hh);
            const D2D1_RECT_F btn = GetSubmenuCloseRectDip(cardRectDip, m_config.windowViewMode);
            if (localDip.x >= btn.left - 2.0 && localDip.x <= btn.right + 2.0 &&
                localDip.y >= btn.top - 2.0 && localDip.y <= btn.bottom + 2.0)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    void RingOverlay::OpenSubmenuForSlot(int slotIndex, PointD originDip)
    {
        ClearSubmenu();
        if (slotIndex < 0 || slotIndex >= static_cast<int>(m_visibleSlots.size()))
            return;

        const RingSlot& slot = m_visibleSlots[static_cast<size_t>(slotIndex)];
        if (!SlotHasSubmenuItems(slot))
            return;

        const RingGeometry geom = ActiveGeometry();
        m_expandedSlotIndex = slotIndex;
        const int slotCount = static_cast<int>(m_visibleSlots.size());
        const double originDist = std::hypot(originDip.x - kCenterDip, originDip.y - kCenterDip);
        m_corridorOriginDip = (originDist <= geom.outerRadius)
            ? originDip
            : GetSlotCenter(kCenterDip, kCenterDip, geom.ringRadius, slotIndex, slotCount);
        const double baseAngle = GetSlotAngle(slotIndex, slotCount);

        if (slot.ShowsWindowPreview(m_config.fanOutSingleWindow))
        {
            const int count = (std::min)(static_cast<int>(slot.windows.size()), 9);
            m_submenuHasArcTrack = false;
            if (m_config.windowViewMode == WindowViewMode::TitleColumn)
            {
                m_submenuIsTitleColumn = true;
                m_submenuCardRects = ComputeTitleColumnRects(
                    kCenterDip, kCenterDip, kOverlayDip, baseAngle,
                    geom.ringRadius, geom.iconSize, count);
            }
            else
            {
                m_submenuIsTitleColumn = false;
                m_submenuCardRects = ComputeWindowOuterArcRects(
                    kCenterDip, kCenterDip, baseAngle,
                    geom.ringRadius, geom.iconSize, m_config.thumbnailScale, count);
            }

            m_submenuCards.reserve(m_submenuCardRects.size());
            for (size_t i = 0; i < m_submenuCardRects.size(); ++i)
            {
                m_submenuCards.push_back(SubmenuCard{
                    m_submenuCardRects[i], &slot.windows[i], nullptr, 1.0, 1.0
                });
            }
        }
        else if (!slot.subItems.empty())
        {
            const int count = (std::min)(static_cast<int>(slot.subItems.size()), 9);
            const bool isFolderArc = (slot.kind == SlotKind::Folder);
            m_submenuIsTitleColumn = false;
            m_submenuHasArcTrack = true;
            m_submenuArcLayout = ComputeIconArcLayout(
                kCenterDip, kCenterDip, baseAngle, geom.outerRadius,
                count, 40.0, 12.0, 4.01426, isFolderArc);
            m_submenuCardRects = m_submenuArcLayout.rects;

            m_submenuCards.reserve(m_submenuCardRects.size());
            for (size_t i = 0; i < m_submenuCardRects.size(); ++i)
            {
                m_submenuCards.push_back(SubmenuCard{
                    m_submenuCardRects[i], nullptr, &slot.subItems[i], 1.0, 1.0
                });
            }
        }

        if (m_submenuCards.empty())
        {
            ClearSubmenu();
            return;
        }

        m_submenuBoundsDip = ComputeUnionBounds(m_submenuCardRects);
        SyncDwmThumbnails();
    }

    void RingOverlay::SyncDwmThumbnails()
    {
        if (!m_isOpen || m_expandedSlotIndex < 0 ||
            m_config.windowViewMode != WindowViewMode::OuterArc)
        {
            m_thumbPool.HideAll();
            return;
        }

        const ThemeColors tc = BuildThemeColors();
        const int overlayLeftPx = static_cast<int>(std::llround(m_centerScreenPhysical.x - kCenterDip * m_dpiScale));
        const int overlayTopPx = static_cast<int>(std::llround(m_centerScreenPhysical.y - kCenterDip * m_dpiScale));

        for (size_t i = 0; i < DwmThumbPool::kPoolSize; ++i)
        {
            if (i >= m_submenuCards.size() || !m_submenuCards[i].window || m_submenuCards[i].window->isMinimized)
            {
                m_thumbPool.HideCard(i);
                continue;
            }
            const auto& card = m_submenuCards[i];
            const bool selected = (static_cast<int>(i) == m_selectedSubIndex);
            const double scale = card.currentScale;
            const double cx = card.boundsDip.left + card.boundsDip.width * 0.5;
            const double cy = card.boundsDip.top + card.boundsDip.height * 0.5;
            const double wDip = (card.boundsDip.width - 12.0) * scale;
            const double hDip = (card.boundsDip.height - 24.0) * scale;
            const double leftDip = cx - wDip * 0.5;
            const double topDip = cy - (card.boundsDip.height * 0.5 - 6.0) * scale;

            const int pxLeft = overlayLeftPx + static_cast<int>(std::llround(leftDip * m_dpiScale));
            const int pxTop = overlayTopPx + static_cast<int>(std::llround(topDip * m_dpiScale));
            const int pxW = static_cast<int>(std::llround(wDip * m_dpiScale));
            const int pxH = static_cast<int>(std::llround(hDip * m_dpiScale));

            m_thumbPool.ShowCard(i, card.window->hwnd, pxLeft, pxTop, pxW, pxH,
                selected ? tc.accentColorRef : tc.borderColorRef, 248);
        }
    }

    int RingOverlay::HitTestSubmenu(PointD localDip) const noexcept
    {
        return HitTestRects(localDip, m_submenuCardRects);
    }

    void RingOverlay::UpdatePointer(int physicalX, int physicalY)
    {
        if (!m_isOpen || m_visibleSlots.empty() || m_contextMenuOpen || m_dragActive ||
            m_reorderActive || m_pressKind != PressKind::None)
            return;

        m_hasLastPointer = true;
        m_lastPointerPhysical = POINT{ physicalX, physicalY };

        const double dx = (physicalX - m_centerScreenPhysical.x) / m_dpiScale;
        const double dy = (physicalY - m_centerScreenPhysical.y) / m_dpiScale;
        const PointD localPt{ kCenterDip + dx, kCenterDip + dy };
        const PointD centerPt{ kCenterDip, kCenterDip };
        const int slotCount = static_cast<int>(m_visibleSlots.size());

        if (m_searchMouseLocked)
        {
            if (std::hypot(localPt.x - m_searchMouseAnchorDip.x, localPt.y - m_searchMouseAnchorDip.y) <= kSearchMouseUnlockDip)
                return;
            m_searchMouseLocked = false;
        }

        const RingGeometry geom = ActiveGeometry();
        const double outerReach = m_isLockedOpen ? 12.0 : 48.0;
        RingHoverResult next = ResolveRingHover(
            localPt,
            centerPt,
            slotCount,
            m_config.deadzoneDip,
            geom.ringRadius,
            geom.iconSize,
            m_selectedIndex,
            m_expandedSlotIndex,
            m_corridorOriginDip,
            m_submenuCardRects,
            m_submenuBoundsDip,
            m_submenuIsTitleColumn,
            outerReach);

        const int earlyCloseHit = (m_expandedSlotIndex >= 0 && !m_searchArcActive && m_filterQuery.empty())
            ? HitTestSubmenuCloseButton(localPt)
            : -1;
        if (earlyCloseHit >= 0)
        {
            next.selectedSlot = m_expandedSlotIndex;
            next.selectedSubIndex = earlyCloseHit;
            next.keepExistingSubmenu = true;
        }

        const bool allowHoverAnim = IsClientAreaAnimationEnabled();
        auto ensureAnimRunning = [&]() {
            if (allowHoverAnim && !m_animTimerRunning && m_hwnd)
            {
                SetTimer(m_hwnd, kAnimTimerId, kAnimIntervalMs, nullptr);
                m_animTimerRunning = true;
            }
        };

        bool needsRender = false;
        bool needsThumbSync = false;

        if (m_searchArcActive || !m_filterQuery.empty())
        {
            int nextRingIdx = -1;
            int nextSubIdx = -1;
            if (next.selectedSlot == -2 && next.selectedSubIndex >= 0)
            {
                nextSubIdx = next.selectedSubIndex;
            }
            else if (next.selectedSlot >= 0 &&
                     next.selectedSlot < static_cast<int>(m_slotSearchMatched.size()) &&
                     m_slotSearchMatched[static_cast<size_t>(next.selectedSlot)])
            {
                nextRingIdx = next.selectedSlot;
            }

            if (nextRingIdx != m_selectedIndex || nextSubIdx != m_selectedSubIndex)
            {
                m_selectedIndex = nextRingIdx;
                m_selectedSubIndex = nextSubIdx;
                for (size_t i = 0; i < m_submenuCards.size(); ++i)
                    m_submenuCards[i].targetScale = (static_cast<int>(i) == m_selectedSubIndex) ? 1.12 : 1.0;

                if (!allowHoverAnim)
                {
                    for (size_t i = 0; i < m_slotScales.size(); ++i)
                        m_slotScales[i] = (static_cast<int>(i) == m_selectedIndex) ? kSelectedSlotScale : kIdleSlotScale;
                    for (auto& card : m_submenuCards)
                        card.currentScale = card.targetScale;
                }
                else
                {
                    ensureAnimRunning();
                }
                needsRender = true;
            }
            if (next.updateCorridorOrigin)
                m_corridorOriginDip = localPt;
            m_hoveredCloseSubIndex = -1;
            if (needsRender)
                Render();
            return;
        }

        if (next.selectedSlot != m_selectedIndex)
        {
            m_selectedIndex = next.selectedSlot;
            m_selectedSubIndex = -1;
            m_hoveredCloseSubIndex = -1;

            if (m_selectedIndex >= 0 && SlotHasSubmenuItems(m_visibleSlots[static_cast<size_t>(m_selectedIndex)]))
                OpenSubmenuForSlot(m_selectedIndex, localPt);
            else
                ClearSubmenu();

            if (!allowHoverAnim)
            {
                for (size_t i = 0; i < m_slotScales.size(); ++i)
                    m_slotScales[i] = (static_cast<int>(i) == m_selectedIndex) ? kSelectedSlotScale : kIdleSlotScale;
            }
            else
            {
                ensureAnimRunning();
            }
            needsRender = true;
        }
        else
        {
            if (m_selectedIndex >= 0)
            {
                if (!next.keepExistingSubmenu && m_expandedSlotIndex >= 0)
                {
                    ClearSubmenu();
                    needsRender = true;
                }
                else if (m_expandedSlotIndex != m_selectedIndex &&
                    SlotHasSubmenuItems(m_visibleSlots[static_cast<size_t>(m_selectedIndex)]))
                {
                    OpenSubmenuForSlot(m_selectedIndex, localPt);
                    needsRender = true;
                }
            }
            else if (m_expandedSlotIndex >= 0)
            {
                ClearSubmenu();
                needsRender = true;
            }

            if (next.updateCorridorOrigin)
                m_corridorOriginDip = localPt;

            if (next.selectedSubIndex != m_selectedSubIndex)
            {
                m_selectedSubIndex = next.selectedSubIndex;
                for (size_t i = 0; i < m_submenuCards.size(); ++i)
                    m_submenuCards[i].targetScale = (static_cast<int>(i) == m_selectedSubIndex) ? 1.12 : 1.0;
                if (!allowHoverAnim)
                {
                    for (auto& card : m_submenuCards)
                        card.currentScale = card.targetScale;
                }
                else
                {
                    ensureAnimRunning();
                }
                needsThumbSync = true;
                needsRender = true;
            }

            const int nextCloseIdx = HitTestSubmenuCloseButton(localPt);
            if (nextCloseIdx != m_hoveredCloseSubIndex)
            {
                m_hoveredCloseSubIndex = nextCloseIdx;
                needsRender = true;
            }
        }

        if (needsThumbSync)
            SyncDwmThumbnails();
        if (needsRender)
            Render();
    }

    void RingOverlay::HandleScrollWheel(int wheelDelta)
    {
        if (!m_isOpen || m_visibleSlots.empty() || m_contextMenuOpen ||
            m_config.scrollWheelMode == ScrollWheelBehavior::Disabled)
            return;

        // When search is active, cycle through the ranked search matches (pinned/open ring items + top half-ring system apps)
        if (!m_filterQuery.empty())
        {
            if (m_rankedMatches.empty())
                return;
            const int cnt = static_cast<int>(m_rankedMatches.size());
            m_rankedMatchCursor = StepIndex(m_rankedMatchCursor, cnt, wheelDelta);
            const RankedMatch& m = m_rankedMatches[static_cast<size_t>(m_rankedMatchCursor)];
            if (m.isRingSlot)
            {
                m_selectedIndex = m.index;
                m_selectedSubIndex = -1;
            }
            else
            {
                m_selectedIndex = -1;
                m_selectedSubIndex = m.index;
            }
            for (size_t i = 0; i < m_slotScales.size(); ++i)
                m_slotScales[i] = (static_cast<int>(i) == m_selectedIndex) ? kSelectedSlotScale : kIdleSlotScale;
            for (size_t i = 0; i < m_submenuCards.size(); ++i)
                m_submenuCards[i].currentScale = m_submenuCards[i].targetScale =
                    (static_cast<int>(i) == m_selectedSubIndex) ? 1.12 : 1.0;
            Render();
            return;
        }

        const RingGeometry geom = ActiveGeometry();
        const int slotCount = static_cast<int>(m_visibleSlots.size());

        if (m_expandedSlotIndex >= 0 && !m_submenuCards.empty())
        {
            m_selectedSubIndex = StepIndex(m_selectedSubIndex, static_cast<int>(m_submenuCards.size()), wheelDelta);
            for (size_t i = 0; i < m_submenuCards.size(); ++i)
                m_submenuCards[i].currentScale = m_submenuCards[i].targetScale =
                    (static_cast<int>(i) == m_selectedSubIndex) ? 1.12 : 1.0;
            SyncDwmThumbnails();
            Render();
            return;
        }

        if (m_config.scrollWheelMode == ScrollWheelBehavior::CycleSubmenuOnly)
        {
            if (m_selectedIndex >= 0 && m_selectedIndex < slotCount &&
                SlotHasSubmenuItems(m_visibleSlots[static_cast<size_t>(m_selectedIndex)]))
            {
                const PointD slotOrigin = GetSlotCenter(kCenterDip, kCenterDip, geom.ringRadius, m_selectedIndex, slotCount);
                OpenSubmenuForSlot(m_selectedIndex, slotOrigin);
                if (!m_submenuCards.empty())
                {
                    m_selectedSubIndex = 0;
                    m_submenuCards[0].currentScale = m_submenuCards[0].targetScale = 1.12;
                    SyncDwmThumbnails();
                }
                Render();
            }
            return;
        }

        m_selectedIndex = StepIndex(m_selectedIndex, slotCount, wheelDelta);
        m_selectedSubIndex = -1;
        for (size_t i = 0; i < m_slotScales.size(); ++i)
            m_slotScales[i] = (static_cast<int>(i) == m_selectedIndex) ? kSelectedSlotScale : kIdleSlotScale;

        if (m_selectedIndex >= 0 && SlotHasSubmenuItems(m_visibleSlots[static_cast<size_t>(m_selectedIndex)]))
        {
            const PointD slotOrigin = GetSlotCenter(kCenterDip, kCenterDip, geom.ringRadius, m_selectedIndex, slotCount);
            OpenSubmenuForSlot(m_selectedIndex, slotOrigin);
        }
        else
            ClearSubmenu();

        Render();
    }

    void RingOverlay::CommitSelectionAndClose()
    {
        const bool shiftHeld = m_config.shiftCenterOnActiveMonitor &&
            ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);

        RingSlot chosenSlotCopy;
        WindowEntry chosenWindowCopy;
        bool hasSlot = false;
        bool hasWindow = false;

        if ((m_expandedSlotIndex >= 0 || m_searchArcActive) && m_selectedSubIndex >= 0 &&
            m_selectedSubIndex < static_cast<int>(m_submenuCards.size()))
        {
            const auto& card = m_submenuCards[static_cast<size_t>(m_selectedSubIndex)];
            if (card.subSlot)
            {
                chosenSlotCopy = *card.subSlot;
                hasSlot = true;
            }
            else if (card.window && m_expandedSlotIndex >= 0)
            {
                chosenSlotCopy = m_visibleSlots[static_cast<size_t>(m_expandedSlotIndex)];
                chosenWindowCopy = *card.window;
                hasSlot = true;
                hasWindow = true;
            }
        }
        else if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_visibleSlots.size()))
        {
            chosenSlotCopy = m_visibleSlots[static_cast<size_t>(m_selectedIndex)];
            hasSlot = true;
            if (!chosenSlotCopy.windows.empty())
            {
                chosenWindowCopy = chosenSlotCopy.windows.front();
                hasWindow = true;
            }
        }

        Dismiss();
        if (hasSlot)
            ActivateSlotOrWindow(&chosenSlotCopy, hasWindow ? &chosenWindowCopy : nullptr, shiftHeld);
    }

    void RingOverlay::ActivateSlotOrWindow(const RingSlot* slot, const WindowEntry* win, bool shiftHeld)
    {
        if (win && win->hwnd && m_tracker)
        {
            m_tracker->ActivateWindow(win->hwnd, shiftHeld, m_config.pointerFollow);
            return;
        }
        if (!slot)
            return;
        if (slot->kind == SlotKind::RecycleBin)
        {
            SlotModel::LaunchPathOrUrl(L"shell:RecycleBinFolder");
            return;
        }
        if (!slot->pathOrUrl.empty())
            SlotModel::LaunchPathOrUrl(slot->pathOrUrl);
    }

    void RingOverlay::AppendFilterChar(wchar_t ch)
    {
        if (!m_isOpen)
            return;
        if (ch == L' ' && m_filterQuery.empty())
            return;
        m_filterQuery.push_back(ch);
        RebuildLayoutAfterFilter();
    }

    void RingOverlay::BackspaceFilter()
    {
        if (!m_isOpen || m_filterQuery.empty())
            return;
        m_filterQuery.pop_back();
        RebuildLayoutAfterFilter();
    }

    void RingOverlay::RebuildLayoutAfterFilter()
    {
        ClearSubmenu();
        m_visibleSlots = m_allSlots;
        m_slotSearchMatched.assign(m_visibleSlots.size(), true);
        m_rankedMatches.clear();
        m_rankedMatchCursor = -1;

        const std::wstring trimmed = TrimCopyW(m_filterQuery);
        if (trimmed.empty())
        {
            m_searchMouseLocked = false;
            m_selectedIndex = -1;
            m_selectedSubIndex = -1;
            for (size_t i = 0; i < m_slotScales.size(); ++i)
                m_slotScales[i] = kIdleSlotScale;
            Render();
            return;
        }

        std::unordered_set<std::wstring> excludeTargets;
        std::unordered_set<std::wstring> excludeNames;
        for (size_t i = 0; i < m_visibleSlots.size(); ++i)
        {
            const auto& slot = m_visibleSlots[i];
            if (!slot.pathOrUrl.empty())
                excludeTargets.insert(AppIndex::CanonicalPathKey(slot.pathOrUrl));
            if (!slot.label.empty())
                excludeNames.insert(ToLowerFold(slot.label));
            for (const auto& alias : slot.searchAliases)
                excludeNames.insert(ToLowerFold(alias));

            const int tier = ScoreAliases(slot.searchAliases, trimmed);
            const bool matched = (tier >= 0);
            m_slotSearchMatched[i] = matched;
            if (matched)
            {
                m_rankedMatches.push_back(RankedMatch{
                    true, static_cast<int>(i), tier, slot.label.size()
                });
            }
        }

        if (m_appIndex)
        {
            IWICImagingFactory* wic = m_tracker ? m_tracker->WicFactory() : nullptr;
            const auto foundApps = m_appIndex->Search(trimmed, excludeTargets, excludeNames, 9, wic);
            m_searchResultSlots.reserve(foundApps.size());
            for (size_t j = 0; j < foundApps.size(); ++j)
            {
                const auto& app = foundApps[j];
                RingSlot s;
                s.id = L"sys-" + std::to_wstring(j);
                s.kind = SlotKind::App;
                s.label = app.name;
                s.pathOrUrl = app.launchTarget;
                s.iconBitmap = app.iconBitmap;
                s.isPinned = false;
                m_searchResultSlots.push_back(std::move(s));

                const int sysTier = ScoreAliases(app.aliases, trimmed);
                m_rankedMatches.push_back(RankedMatch{
                    false, static_cast<int>(j), (sysTier >= 0 ? sysTier : 3), app.name.size()
                });
            }
        }

        std::sort(m_rankedMatches.begin(), m_rankedMatches.end(), [&](const RankedMatch& a, const RankedMatch& b) {
            if (a.tier != b.tier)
                return a.tier < b.tier;
            if (a.isRingSlot != b.isRingSlot)
                return a.isRingSlot; // Pinned & running main-ring apps win ties over external system apps!
            return a.nameLen < b.nameLen;
        });

        if (!m_searchResultSlots.empty())
        {
            const RingGeometry geom = ActiveGeometry();
            const int sysCount = static_cast<int>(m_searchResultSlots.size());
            m_searchArcActive = true;
            m_expandedSlotIndex = -2;
            m_submenuIsTitleColumn = false;
            m_submenuHasArcTrack = true;
            m_submenuArcLayout = ComputeIconArcLayout(
                kCenterDip, kCenterDip, -kPi * 0.5, geom.outerRadius,
                sysCount, 40.0, 12.0, 4.01426, false);
            m_submenuCardRects = m_submenuArcLayout.rects;
            m_submenuCards.reserve(m_submenuCardRects.size());
            for (size_t j = 0; j < m_submenuCardRects.size(); ++j)
            {
                m_submenuCards.push_back(SubmenuCard{
                    m_submenuCardRects[j], nullptr, &m_searchResultSlots[j], 1.0, 1.0
                });
            }
            m_submenuBoundsDip = ComputeUnionBounds(m_submenuCardRects);
        }

        if (!m_rankedMatches.empty())
        {
            m_rankedMatchCursor = 0;
            const RankedMatch& best = m_rankedMatches[0];
            if (best.isRingSlot)
            {
                m_selectedIndex = best.index;
                m_selectedSubIndex = -1;
            }
            else
            {
                m_selectedIndex = -1;
                m_selectedSubIndex = best.index;
                if (m_selectedSubIndex >= 0 && m_selectedSubIndex < static_cast<int>(m_submenuCards.size()))
                    m_submenuCards[static_cast<size_t>(m_selectedSubIndex)].targetScale = 1.12;
            }
        }
        else
        {
            m_selectedIndex = -1;
            m_selectedSubIndex = -1;
        }

        m_searchMouseLocked = true;
        if (m_hasLastPointer)
        {
            m_searchMouseAnchorDip = {
                kCenterDip + (m_lastPointerPhysical.x - m_centerScreenPhysical.x) / m_dpiScale,
                kCenterDip + (m_lastPointerPhysical.y - m_centerScreenPhysical.y) / m_dpiScale
            };
        }
        else
        {
            m_searchMouseAnchorDip = { kCenterDip, kCenterDip };
        }

        if (IsClientAreaAnimationEnabled())
        {
            if (!m_animTimerRunning && m_hwnd)
            {
                SetTimer(m_hwnd, kAnimTimerId, kAnimIntervalMs, nullptr);
                m_animTimerRunning = true;
            }
        }
        else
        {
            for (size_t i = 0; i < m_slotScales.size(); ++i)
                m_slotScales[i] = (static_cast<int>(i) == m_selectedIndex) ? kSelectedSlotScale : kIdleSlotScale;
            for (auto& card : m_submenuCards)
                card.currentScale = card.targetScale;
        }

        Render();
    }

    void RingOverlay::StepAnimations()
    {
        if (!m_isOpen)
            return;

        bool anyActive = false;
        if (m_openProgress < 1.0)
        {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            const double elapsedMs = static_cast<double>(now.QuadPart - m_openStartQpc) * 1000.0 /
                static_cast<double>(m_qpcFreq);
            m_openProgress = (std::min)(1.0, elapsedMs / kOpenDurationMs);
            if (m_openProgress < 1.0)
                anyActive = true;
        }

        for (size_t i = 0; i < m_slotScales.size(); ++i)
        {
            const double target = (static_cast<int>(i) == m_selectedIndex) ? kSelectedSlotScale : kIdleSlotScale;
            const double diff = target - m_slotScales[i];
            if (std::abs(diff) > 0.005)
            {
                m_slotScales[i] += diff * 0.35;
                anyActive = true;
            }
            else
            {
                m_slotScales[i] = target;
            }
        }

        bool subChanged = false;
        for (auto& card : m_submenuCards)
        {
            const double diff = card.targetScale - card.currentScale;
            if (std::abs(diff) > 0.005)
            {
                card.currentScale += diff * 0.35;
                anyActive = true;
                subChanged = true;
            }
            else
            {
                card.currentScale = card.targetScale;
            }
        }

        if (subChanged)
            SyncDwmThumbnails();

        if (m_dragActive && m_springSlotIndex >= 0 && !m_springTriggered &&
            m_springSlotIndex < static_cast<int>(m_visibleSlots.size()))
        {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            const double elapsedMs = static_cast<double>(now.QuadPart - m_springHoverStartQpc) * 1000.0 /
                static_cast<double>(m_qpcFreq);
            const SpringHoverEval ev = EvaluateSpringHover(elapsedMs);
            m_springPulseScale = ev.scaleMultiplier;
            anyActive = true;
            if (ev.shouldActivate)
            {
                m_springTriggered = true;
                TriggerSpringLoadForSlot(m_springSlotIndex);
                return;
            }
        }
        else
        {
            m_springPulseScale = 1.0;
        }

        Render();

        if (!anyActive && m_animTimerRunning)
        {
            KillTimer(m_hwnd, kAnimTimerId);
            m_animTimerRunning = false;
        }
    }

    std::wstring RingOverlay::BuildFilterSummary() const
    {
        if (m_reorderActive && m_reorderCurrentIndex >= 0 &&
            m_reorderCurrentIndex < static_cast<int>(m_visibleSlots.size()))
        {
            return L"Move: " + m_visibleSlots[static_cast<size_t>(m_reorderCurrentIndex)].label;
        }
        if (m_dragActive)
        {
            if (m_dragOverCenter)
                return L"Pin to Ring";
            if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_visibleSlots.size()))
            {
                const auto& slot = m_visibleSlots[static_cast<size_t>(m_selectedIndex)];
                if (slot.kind == SlotKind::RecycleBin)
                    return L"Delete";
                return slot.label;
            }
            return {};
        }
        if (m_selectedSubIndex >= 0 && m_selectedSubIndex < static_cast<int>(m_submenuCards.size()))
        {
            const auto& card = m_submenuCards[static_cast<size_t>(m_selectedSubIndex)];
            if (card.window) return card.window->title;
            if (card.subSlot) return card.subSlot->label;
        }
        if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_visibleSlots.size()))
            return m_visibleSlots[static_cast<size_t>(m_selectedIndex)].label;
        if (!m_filterQuery.empty() && m_rankedMatches.empty())
            return L"No matches";
        return {};
    }

    Microsoft::WRL::ComPtr<ID2D1Geometry> RingOverlay::BuildAnnulusGeometry(float outerR, float holeR) const
    {
        if (!m_d2dFactory)
            return nullptr;

        const D2D1_POINT_2F centerPt = D2D1::Point2F(static_cast<float>(kCenterDip), static_cast<float>(kCenterDip));
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> annulus;
        if (FAILED(m_d2dFactory->CreatePathGeometry(annulus.GetAddressOf())) || !annulus)
            return nullptr;
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(annulus->Open(sink.GetAddressOf())) || !sink)
            return nullptr;

        sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
        sink->BeginFigure(D2D1::Point2F(centerPt.x + outerR, centerPt.y), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(centerPt.x - outerR, centerPt.y),
            D2D1::SizeF(outerR, outerR), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(centerPt.x + outerR, centerPt.y),
            D2D1::SizeF(outerR, outerR), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);

        sink->BeginFigure(D2D1::Point2F(centerPt.x + holeR, centerPt.y), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(centerPt.x - holeR, centerPt.y),
            D2D1::SizeF(holeR, holeR), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(centerPt.x + holeR, centerPt.y),
            D2D1::SizeF(holeR, holeR), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        return annulus;
    }

    Microsoft::WRL::ComPtr<ID2D1Geometry> RingOverlay::BuildCurvedArcGeometry(const IconArcLayout& arc) const
    {
        if (!m_d2dFactory || arc.rects.empty())
            return nullptr;

        if (arc.rects.size() == 1)
        {
            const float cx = static_cast<float>(arc.rects[0].left + arc.rects[0].width * 0.5);
            const float cy = static_cast<float>(arc.rects[0].top + arc.rects[0].height * 0.5);
            const float r = static_cast<float>(arc.trackWidthDip * 0.5);
            Microsoft::WRL::ComPtr<ID2D1EllipseGeometry> eg;
            if (SUCCEEDED(m_d2dFactory->CreateEllipseGeometry(
                D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), eg.GetAddressOf())))
                return eg;
            return nullptr;
        }

        double span = arc.endAngleRad - arc.startAngleRad;
        if (span <= 0.001)
            return nullptr;
        if (span >= kTwoPi - 0.05)
            span = kTwoPi - 0.05;

        const float centerXY = static_cast<float>(kCenterDip);
        const float r = static_cast<float>(arc.arcRadius);
        const D2D1_POINT_2F pStart{
            centerXY + r * static_cast<float>(std::cos(arc.startAngleRad)),
            centerXY + r * static_cast<float>(std::sin(arc.startAngleRad))
        };
        const D2D1_POINT_2F pEnd{
            centerXY + r * static_cast<float>(std::cos(arc.startAngleRad + span)),
            centerXY + r * static_cast<float>(std::sin(arc.startAngleRad + span))
        };

        Microsoft::WRL::ComPtr<ID2D1PathGeometry> arcGeo;
        if (FAILED(m_d2dFactory->CreatePathGeometry(arcGeo.GetAddressOf())) || !arcGeo)
            return nullptr;
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(arcGeo->Open(sink.GetAddressOf())) || !sink)
            return nullptr;
        sink->BeginFigure(pStart, D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment(
            pEnd,
            D2D1::SizeF(r, r),
            0.0f,
            D2D1_SWEEP_DIRECTION_CLOCKWISE,
            (span > kPi) ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();

        D2D1_STROKE_STYLE_PROPERTIES ssp = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_SOLID, 0.0f);
        Microsoft::WRL::ComPtr<ID2D1StrokeStyle> roundStyle;
        m_d2dFactory->CreateStrokeStyle(ssp, nullptr, 0, roundStyle.GetAddressOf());

        Microsoft::WRL::ComPtr<ID2D1PathGeometry> widenedGeo;
        if (FAILED(m_d2dFactory->CreatePathGeometry(widenedGeo.GetAddressOf())) || !widenedGeo)
            return nullptr;
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> widenedSink;
        if (FAILED(widenedGeo->Open(widenedSink.GetAddressOf())) || !widenedSink)
            return nullptr;
        if (SUCCEEDED(arcGeo->Widen(static_cast<float>(arc.trackWidthDip), roundStyle.Get(),
            nullptr, 0.25f, widenedSink.Get())))
        {
            widenedSink->Close();
            return widenedGeo;
        }
        return nullptr;
    }

    void RingOverlay::DrawCurvedArcTrack(ID2D1Geometry* arcGeo, const IconArcLayout& arc, const ThemeColors& tc, bool paintFill, bool paintNoise)
    {
        if (!m_d2dContext || !arcGeo || arc.rects.empty())
            return;

        auto mkBrush = [&](D2D1_COLOR_F col) {
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> b;
            m_d2dContext->CreateSolidColorBrush(col, b.GetAddressOf());
            return b;
        };

        if (paintFill)
        {
            if (auto b = mkBrush(tc.ringFill))
                m_d2dContext->FillGeometry(arcGeo, b.Get());
        }
        if (paintNoise && m_noiseBrush && tc.noiseOpacity > 0.002f)
            m_d2dContext->FillGeometry(arcGeo, m_noiseBrush.Get());
        if (m_wcaBackdropActive)
        {
            const D2D1_COLOR_F edgeBridge = (m_config.theme == RingTheme::Blue)
                ? D2D1::ColorF(tc.tintR, tc.tintG, tc.tintB, 0.88f)
                : (tc.isLightText ? D2D1::ColorF(34.0f / 255.0f, 34.0f / 255.0f, 36.0f / 255.0f, 0.88f)
                                  : D2D1::ColorF(236.0f / 255.0f, 236.0f / 255.0f, 240.0f / 255.0f, 0.88f));
            if (auto eb = mkBrush(edgeBridge))
                m_d2dContext->DrawGeometry(arcGeo, eb.Get(), 2.2f);
        }
        if (auto b = mkBrush(tc.ringStroke))
        {
            m_d2dContext->DrawGeometry(arcGeo, b.Get(), 0.95f);
        }
    }

    void RingOverlay::Render()
    {
        if (!m_d2dContext || !m_targetBitmap || !m_swapChain)
            return;

        const ThemeColors tc = BuildThemeColors();
        const bool wcaActive = !m_config.disableRingBlur && m_wcaBackdropActive && m_wcaBackdropHwnd != nullptr;
        const bool acrylicActive = !m_config.disableRingBlur && !wcaActive &&
            m_acrylicBackdrop.IsActive() && m_maskSwapChain && m_maskTargetBitmap;
        const bool hasLiveBackdrop = wcaActive || acrylicActive;
        if (wcaActive)
            UpdateWcaMaterial(tc);
        else
        {
            if (m_config.disableRingBlur && m_wcaBackdropHwnd && IsWindow(m_wcaBackdropHwnd))
                ShowWindow(m_wcaBackdropHwnd, SW_HIDE);
            if (acrylicActive)
                m_acrylicBackdrop.UpdateMaterial(BuildAcrylicMaterial());
        }

        const RingGeometry geom = ActiveGeometry();
        const float dpiScale = static_cast<float>(m_dpiScale);
        const double easedOpen = EaseOutCubic(m_openProgress);
        const float openScale = static_cast<float>(0.86 + 0.14 * easedOpen);

        D2D1::Matrix3x2F transform = D2D1::Matrix3x2F::Scale(dpiScale, dpiScale);
        if (openScale < 0.999f)
        {
            transform = D2D1::Matrix3x2F::Scale(openScale, openScale,
                D2D1::Point2F(static_cast<float>(kCenterDip), static_cast<float>(kCenterDip))) * transform;
        }

        const float outerR = static_cast<float>(geom.outerRadius);
        const float holeR = static_cast<float>(geom.holeRadius);
        const D2D1_POINT_2F centerPt = D2D1::Point2F(static_cast<float>(kCenterDip), static_cast<float>(kCenterDip));

        Microsoft::WRL::ComPtr<ID2D1Geometry> annulusGeo = BuildAnnulusGeometry(outerR, holeR);
        Microsoft::WRL::ComPtr<ID2D1Geometry> curvedArcGeo;
        if ((m_expandedSlotIndex >= 0 || m_searchArcActive) && !m_submenuCards.empty() && m_submenuHasArcTrack)
            curvedArcGeo = BuildCurvedArcGeometry(m_submenuArcLayout);

        auto measureTextWidth = [&](const std::wstring& txt, IDWriteTextFormat* fmt, float maxW) -> float {
            if (txt.empty() || !fmt || !m_dwriteFactory)
                return 0.0f;
            Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                txt.c_str(), static_cast<UINT32>(txt.size()), fmt, maxW, 32.0f, layout.GetAddressOf())) && layout)
            {
                DWRITE_TEXT_METRICS tm{};
                if (SUCCEEDED(layout->GetMetrics(&tm)))
                    return tm.widthIncludingTrailingWhitespace;
            }
            return static_cast<float>(txt.size()) * 7.5f;
        };

        const bool hasQuery = !m_filterQuery.empty();
        D2D1_ROUNDED_RECT searchPill{};
        if (hasQuery)
        {
            constexpr float kSearchH = 26.0f;
            constexpr float kSearchR = 13.0f;
            const float maxSearchW = (std::max)(110.0f, holeR * 1.78f);
            const float queryW = measureTextWidth(m_filterQuery, m_searchFormat.Get(), maxSearchW - 44.0f);
            const float searchW = (std::clamp)(queryW + 46.0f, 96.0f, maxSearchW);
            const float searchBottom = (std::min)(centerPt.y - holeR * 0.42f, centerPt.y - 6.0f);
            const float searchTop = searchBottom - kSearchH;
            searchPill = D2D1::RoundedRect(
                D2D1::RectF(centerPt.x - searchW * 0.5f, searchTop, centerPt.x + searchW * 0.5f, searchBottom),
                kSearchR, kSearchR);
        }

        const std::wstring summary = BuildFilterSummary();
        const bool hasSummary = !summary.empty();
        D2D1_ROUNDED_RECT centerPill{};
        if (hasSummary)
        {
            const float maxPillW = (std::max)(110.0f, holeR * 1.72f);
            const float textW = measureTextWidth(summary, m_titleFormat.Get(), maxPillW - 26.0f);
            const float cw = (std::clamp)(textW + 28.0f, 68.0f, maxPillW);
            const float ch = 28.0f;
            const float cy = centerPt.y + (hasQuery ? 15.0f : 0.0f);
            centerPill = D2D1::RoundedRect(
                D2D1::RectF(centerPt.x - cw * 0.5f, cy - ch * 0.5f, centerPt.x + cw * 0.5f, cy + ch * 0.5f),
                14.0f, 14.0f);
        }

        // Pass A (when native WCA backdrop is active): sync exact SetWindowRgn matching A4S-Menu's native acrylic.
        if (wcaActive)
        {
            UpdateWcaRegion(dpiScale, openScale, outerR, holeR, curvedArcGeo != nullptr,
                hasQuery, searchPill, hasSummary, centerPill);
        }
        // Pass A-fallback (when Windows.UI.Composition acrylic is active): render antialiased alpha mask.
        else if (acrylicActive)
        {
            m_d2dContext->SetTarget(m_maskTargetBitmap.Get());
            m_d2dContext->BeginDraw();
            m_d2dContext->Clear(D2D1::ColorF(0, 0, 0, 0));
            m_d2dContext->SetTransform(transform);

            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> maskBrush;
            m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), maskBrush.GetAddressOf());
            if (maskBrush)
            {
                if (annulusGeo)
                    m_d2dContext->FillGeometry(annulusGeo.Get(), maskBrush.Get());
                if (curvedArcGeo)
                    m_d2dContext->FillGeometry(curvedArcGeo.Get(), maskBrush.Get());

                if ((m_expandedSlotIndex >= 0 || m_searchArcActive) && !m_submenuCards.empty())
                {
                    for (const auto& card : m_submenuCards)
                    {
                        if (!card.window)
                            continue;
                        const float s = static_cast<float>(card.currentScale);
                        const float cx = static_cast<float>(card.boundsDip.left + card.boundsDip.width * 0.5);
                        const float cy = static_cast<float>(card.boundsDip.top + card.boundsDip.height * 0.5);
                        const float hw = static_cast<float>(card.boundsDip.width * 0.5) * s;
                        const float hh = static_cast<float>(card.boundsDip.height * 0.5) * s;
                        m_d2dContext->FillRoundedRectangle(
                            D2D1::RoundedRect(D2D1::RectF(cx - hw, cy - hh, cx + hw, cy + hh), 8.0f, 8.0f),
                            maskBrush.Get());
                    }
                }

                if (hasQuery)
                    m_d2dContext->FillRoundedRectangle(searchPill, maskBrush.Get());
                if (hasSummary)
                    m_d2dContext->FillRoundedRectangle(centerPill, maskBrush.Get());
            }

            const HRESULT maskEndHr = m_d2dContext->EndDraw();
            m_d2dContext->SetTarget(nullptr);
            if (maskEndHr == D2DERR_RECREATE_TARGET)
            {
                ReleaseGraphicsDevices();
                CreateGraphicsDevices();
                return;
            }
            m_maskSwapChain->Present(0, 0);
        }

        // Pass B: Foreground content (hairline rims, icons, running dots, hover reveal, magnifier, typography).
        // When native WCA ACCENT_ENABLE_ACRYLICBLURBEHIND is active, Windows 11 DWM supplies the exact native
        // A4S-Menu texture and glow directly — no synthetic noise overlay is painted on top.
        EnsureNoiseBrush();
        const bool useNoise = (!m_config.disableRingBlur && !wcaActive && m_noiseBrush != nullptr && tc.noiseOpacity > 0.002f);
        if (useNoise)
            m_noiseBrush->SetOpacity(tc.noiseOpacity);

        m_d2dContext->SetTarget(m_targetBitmap.Get());
        m_d2dContext->BeginDraw();
        m_d2dContext->Clear(D2D1::ColorF(0, 0, 0, 0));
        m_d2dContext->SetTransform(transform);

        auto mkBrush = [&](D2D1_COLOR_F col) {
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> b;
            m_d2dContext->CreateSolidColorBrush(col, b.GetAddressOf());
            return b;
        };

        // 1. Single Hollow Annulus Backdrop (in fallback mode) + silky antialiased 3D glass rims
        if (!hasLiveBackdrop && annulusGeo)
        {
            if (auto b = mkBrush(tc.ringFill))
                m_d2dContext->FillGeometry(annulusGeo.Get(), b.Get());
        }
        if (useNoise && annulusGeo)
            m_d2dContext->FillGeometry(annulusGeo.Get(), m_noiseBrush.Get());

        // When native WCA acrylic is clipped by a 1-bit HRGN inset by 1.0 px, paint a silky antialiased
        // 2.0 DIP edge bridge right over the 1px transition band (leaving the approved WCA interior 100% untouched),
        // then layer a subtle top-lit 3D glass bezel + soft drop-edge shadow lip on outer and inner circles.
        if (wcaActive)
        {
            const D2D1_COLOR_F bridgeCol = (m_config.theme == RingTheme::Blue)
                ? D2D1::ColorF(tc.tintR, tc.tintG, tc.tintB, 0.90f)
                : (tc.isLightText ? D2D1::ColorF(34.0f / 255.0f, 34.0f / 255.0f, 36.0f / 255.0f, 0.90f)
                                  : D2D1::ColorF(236.0f / 255.0f, 236.0f / 255.0f, 240.0f / 255.0f, 0.90f));
            if (auto br = mkBrush(bridgeCol))
            {
                m_d2dContext->DrawEllipse(D2D1::Ellipse(centerPt, outerR - 0.9f, outerR - 0.9f), br.Get(), 2.0f);
                m_d2dContext->DrawEllipse(D2D1::Ellipse(centerPt, holeR + 0.9f, holeR + 0.9f), br.Get(), 2.0f);
            }
        }

        // Subtle outer & inner soft shadow feather for smooth 3D elevation against any wallpaper
        if (auto shBr = mkBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, tc.isLightText ? 0.30f : 0.14f)))
        {
            m_d2dContext->DrawEllipse(D2D1::Ellipse(centerPt, outerR + 0.65f, outerR + 0.65f), shBr.Get(), 1.3f);
            m_d2dContext->DrawEllipse(D2D1::Ellipse(centerPt, holeR - 0.55f, holeR - 0.55f), shBr.Get(), 1.1f);
        }

        auto makeRimGradient = [&](float yTop, float yBot, D2D1_COLOR_F cTop, D2D1_COLOR_F cMid, D2D1_COLOR_F cBot) {
            Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> lgb;
            D2D1_GRADIENT_STOP stops[3] = {
                { 0.0f, cTop },
                { 0.52f, cMid },
                { 1.0f, cBot }
            };
            Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> gsc;
            if (SUCCEEDED(m_d2dContext->CreateGradientStopCollection(stops, 3, gsc.GetAddressOf())))
            {
                m_d2dContext->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(centerPt.x, yTop), D2D1::Point2F(centerPt.x, yBot)),
                    gsc.Get(), lgb.GetAddressOf());
            }
            return lgb;
        };

        const D2D1_COLOR_F outerTop = (m_config.theme == RingTheme::Blue)
            ? D2D1::ColorF(132.0f / 255.0f, 198.0f / 255.0f, 1.0f, 0.62f)
            : (tc.isLightText ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.25f)
                              : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f));
        const D2D1_COLOR_F outerMid = (m_config.theme == RingTheme::Blue)
            ? tc.ringStroke
            : (tc.isLightText ? D2D1::ColorF(118.0f / 255.0f, 118.0f / 255.0f, 124.0f / 255.0f, 0.42f)
                              : D2D1::ColorF(170.0f / 255.0f, 174.0f / 255.0f, 182.0f / 255.0f, 0.55f));
        const D2D1_COLOR_F outerBot = (m_config.theme == RingTheme::Blue)
            ? D2D1::ColorF(52.0f / 255.0f, 120.0f / 255.0f, 230.0f / 255.0f, 0.34f)
            : (tc.isLightText ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f)
                              : D2D1::ColorF(120.0f / 255.0f, 126.0f / 255.0f, 138.0f / 255.0f, 0.42f));

        if (auto outerGrad = makeRimGradient(centerPt.y - outerR, centerPt.y + outerR, outerTop, outerMid, outerBot))
        {
            constexpr float kOuterRimW = 1.05f;
            m_d2dContext->DrawEllipse(D2D1::Ellipse(centerPt, outerR - kOuterRimW * 0.5f, outerR - kOuterRimW * 0.5f),
                outerGrad.Get(), kOuterRimW);
        }

        // Optically recessed inner bevel (softer shadow at 12 o'clock, gentle catch-light at 6 o'clock)
        const D2D1_COLOR_F innerTop = (m_config.theme == RingTheme::Blue)
            ? D2D1::ColorF(48.0f / 255.0f, 110.0f / 255.0f, 210.0f / 255.0f, 0.28f)
            : (tc.isLightText ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.07f)
                              : D2D1::ColorF(110.0f / 255.0f, 116.0f / 255.0f, 126.0f / 255.0f, 0.34f));
        const D2D1_COLOR_F innerMid = (m_config.theme == RingTheme::Blue)
            ? tc.innerRimStroke
            : (tc.isLightText ? D2D1::ColorF(108.0f / 255.0f, 108.0f / 255.0f, 114.0f / 255.0f, 0.32f)
                              : D2D1::ColorF(170.0f / 255.0f, 174.0f / 255.0f, 182.0f / 255.0f, 0.45f));
        const D2D1_COLOR_F innerBot = (m_config.theme == RingTheme::Blue)
            ? D2D1::ColorF(126.0f / 255.0f, 190.0f / 255.0f, 1.0f, 0.48f)
            : (tc.isLightText ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f)
                              : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.72f));

        if (auto innerGrad = makeRimGradient(centerPt.y - holeR, centerPt.y + holeR, innerTop, innerMid, innerBot))
        {
            constexpr float kInnerRimW = 0.95f;
            m_d2dContext->DrawEllipse(D2D1::Ellipse(centerPt, holeR + kInnerRimW * 0.5f, holeR + kInnerRimW * 0.5f),
                innerGrad.Get(), kInnerRimW);
        }

        // 1b. Selection Indicator (SectorWedge / PointerLine) styled with tc.accent
        const int slotCount = static_cast<int>(m_visibleSlots.size());
        if (m_selectedIndex >= 0 && m_selectedIndex < slotCount &&
            m_config.selectionIndicator != SelectionIndicatorStyle::GlowOnly)
        {
            const double selAngle = GetSlotAngle(m_selectedIndex, slotCount);
            const double halfAngle = kPi / static_cast<double>((std::max)(slotCount, 4));
            if (m_config.selectionIndicator == SelectionIndicatorStyle::SectorWedge && m_d2dFactory)
            {
                const double a0 = selAngle - halfAngle;
                const double a1 = selAngle + halfAngle;
                const double span = a1 - a0;
                const float rOut = outerR - 1.0f;
                const float rIn = holeR + 1.0f;
                auto ptAt = [&](float r, double ang) -> D2D1_POINT_2F {
                    return D2D1::Point2F(
                        centerPt.x + r * static_cast<float>(std::cos(ang)),
                        centerPt.y + r * static_cast<float>(std::sin(ang)));
                };

                Microsoft::WRL::ComPtr<ID2D1PathGeometry> wedgeGeo;
                if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(wedgeGeo.GetAddressOf())) && wedgeGeo)
                {
                    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(wedgeGeo->Open(sink.GetAddressOf())) && sink)
                    {
                        const D2D1_ARC_SIZE arcSz = (span > kPi) ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;
                        sink->BeginFigure(ptAt(rOut, a0), D2D1_FIGURE_BEGIN_FILLED);
                        sink->AddArc(D2D1::ArcSegment(ptAt(rOut, a1), D2D1::SizeF(rOut, rOut),
                            0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, arcSz));
                        sink->AddLine(ptAt(rIn, a1));
                        sink->AddArc(D2D1::ArcSegment(ptAt(rIn, a0), D2D1::SizeF(rIn, rIn),
                            0.0f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, arcSz));
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                        sink->Close();

                        D2D1_COLOR_F fillCol = tc.accent;
                        fillCol.a = 0.24f;
                        if (auto fb = mkBrush(fillCol))
                            m_d2dContext->FillGeometry(wedgeGeo.Get(), fb.Get());
                    }
                }

                auto strokeArc = [&](float r, double startAng, double endAng, float width, float alpha) {
                    Microsoft::WRL::ComPtr<ID2D1PathGeometry> arcGeo;
                    if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(arcGeo.GetAddressOf())) && arcGeo)
                    {
                        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                        if (SUCCEEDED(arcGeo->Open(sink.GetAddressOf())) && sink)
                        {
                            const D2D1_ARC_SIZE arcSz = ((endAng - startAng) > kPi) ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;
                            sink->BeginFigure(ptAt(r, startAng), D2D1_FIGURE_BEGIN_HOLLOW);
                            sink->AddArc(D2D1::ArcSegment(ptAt(r, endAng), D2D1::SizeF(r, r),
                                0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, arcSz));
                            sink->EndFigure(D2D1_FIGURE_END_OPEN);
                            sink->Close();

                            D2D1_COLOR_F scCol = tc.accent;
                            scCol.a = alpha;
                            if (auto sb = mkBrush(scCol))
                                m_d2dContext->DrawGeometry(arcGeo.Get(), sb.Get(), width);
                        }
                    }
                };
                strokeArc(rOut, a0, a1, 1.8f, 0.92f);
                strokeArc(rIn, a0, a1, 1.2f, 0.65f);
            }
            else if (m_config.selectionIndicator == SelectionIndicatorStyle::PointerLine && m_d2dFactory)
            {
                const float c = static_cast<float>(std::cos(selAngle));
                const float s = static_cast<float>(std::sin(selAngle));
                const float rLineStart = holeR + 2.0f;
                const float rLineEnd = (std::max)(rLineStart + 4.0f, static_cast<float>(geom.ringRadius - geom.iconSize * 0.56));
                D2D1_COLOR_F lineCol = tc.accent;
                lineCol.a = 0.92f;
                if (auto lb = mkBrush(lineCol))
                {
                    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> capStyle;
                    D2D1_STROKE_STYLE_PROPERTIES ssp = D2D1::StrokeStyleProperties(
                        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND);
                    m_d2dFactory->CreateStrokeStyle(ssp, nullptr, 0, capStyle.GetAddressOf());
                    m_d2dContext->DrawLine(
                        D2D1::Point2F(centerPt.x + rLineStart * c, centerPt.y + rLineStart * s),
                        D2D1::Point2F(centerPt.x + rLineEnd * c, centerPt.y + rLineEnd * s),
                        lb.Get(), 2.2f, capStyle.Get());
                }
            }
        }

        // 2. Slot icons, running dots, selection circle & count badge
        const bool searchFiltering = !m_filterQuery.empty();
        for (int pass = 0; pass < 2; ++pass)
        {
            for (int i = 0; i < slotCount; ++i)
            {
                const bool selected = (i == m_selectedIndex);
                if ((pass == 0 && selected) || (pass == 1 && !selected))
                    continue;

                const RingSlot& slot = m_visibleSlots[static_cast<size_t>(i)];
                const bool matchedInSearch = !searchFiltering ||
                    (i < static_cast<int>(m_slotSearchMatched.size()) && m_slotSearchMatched[static_cast<size_t>(i)]);
                const float dimAlpha = matchedInSearch ? 1.0f : 0.22f;

                const double angle = GetSlotAngle(i, slotCount);
                const PointD pos = GetSlotCenter(kCenterDip, kCenterDip, geom.ringRadius, i, slotCount);
                float scale = static_cast<float>(i < static_cast<int>(m_slotScales.size())
                    ? m_slotScales[static_cast<size_t>(i)] : kIdleSlotScale);
                if (m_dragActive && i == m_springSlotIndex)
                    scale *= static_cast<float>(m_springPulseScale);
                const float haloR = static_cast<float>(geom.iconSize * 0.72) * (scale / static_cast<float>(kSelectedSlotScale));
                const D2D1_POINT_2F sc{ static_cast<float>(pos.x), static_cast<float>(pos.y) };

                if (m_reorderActive && i == m_reorderCurrentIndex)
                {
                    const float slotR = static_cast<float>(geom.iconSize * 0.56);
                    D2D1_COLOR_F slotFill = tc.accent;
                    slotFill.a = 0.14f;
                    if (auto fb = mkBrush(slotFill))
                        m_d2dContext->FillEllipse(D2D1::Ellipse(sc, slotR, slotR), fb.Get());
                    if (auto sb = mkBrush(tc.accent))
                        m_d2dContext->DrawEllipse(D2D1::Ellipse(sc, slotR, slotR), sb.Get(), 1.4f);
                    continue;
                }

                // Running dot on inner rim (frontmost slightly larger at 1.0 alpha, others 0.6)
                if (slot.IsRunning())
                {
                    const D2D1_POINT_2F dotPt{
                        centerPt.x + static_cast<float>(geom.runningDotRadius * std::cos(angle)),
                        centerPt.y + static_cast<float>(geom.runningDotRadius * std::sin(angle))
                    };
                    const float dotR = slot.isFrontmost ? 2.4f : 1.7f;
                    D2D1_COLOR_F dotCol = tc.textColor;
                    dotCol.a = (slot.isFrontmost ? 1.0f : 0.60f) * dimAlpha;
                    if (auto b = mkBrush(dotCol))
                        m_d2dContext->FillEllipse(D2D1::Ellipse(dotPt, dotR, dotR), b.Get());
                }

                // Selection circle + subtle A4S Fluent radial glow strictly on the hovered/selected icon
                if (selected)
                {
                    D2D1_COLOR_F g0 = tc.hoverRevealColor; g0.a = (m_config.theme == RingTheme::A4S) ? 0.30f : 0.36f;
                    D2D1_COLOR_F g1 = tc.hoverRevealColor; g1.a = 0.0f;
                    D2D1_GRADIENT_STOP stops[2] = { { 0.0f, g0 }, { 1.0f, g1 } };
                    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> gsc;
                    if (SUCCEEDED(m_d2dContext->CreateGradientStopCollection(stops, 2, gsc.GetAddressOf())))
                    {
                        Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> glow;
                        const float gr = haloR * 1.48f;
                        if (SUCCEEDED(m_d2dContext->CreateRadialGradientBrush(
                            D2D1::RadialGradientBrushProperties(sc, D2D1::Point2F(0, 0), gr, gr), gsc.Get(), glow.GetAddressOf())))
                        {
                            m_d2dContext->FillEllipse(D2D1::Ellipse(sc, gr, gr), glow.Get());
                        }
                    }

                    if (auto b = mkBrush(tc.slotSelectedFill))
                        m_d2dContext->FillEllipse(D2D1::Ellipse(sc, haloR, haloR), b.Get());
                    if (auto b = mkBrush(tc.hoverRim))
                        m_d2dContext->DrawEllipse(D2D1::Ellipse(sc, haloR, haloR), b.Get(), tc.hairlineWidth);
                }

                // Selected pinned-not-open icons render at full 1.0 alpha!
                const float iconAlpha = selected ? 1.0f : dimAlpha;
                if (slot.iconBitmap)
                {
                    if (auto bmp = GetOrCreateD2DBitmap(slot.iconBitmap.Get()))
                    {
                        const float iconHalf = static_cast<float>(geom.iconSize * 0.46) * scale;
                        const D2D1_RECT_F dst = D2D1::RectF(sc.x - iconHalf, sc.y - iconHalf, sc.x + iconHalf, sc.y + iconHalf);
                        m_d2dContext->DrawBitmap(bmp.Get(), dst, iconAlpha);
                    }
                }
                else if (m_glyphFormat)
                {
                    std::wstring glyph = L"?";
                    if (slot.kind == SlotKind::RecycleBin) glyph = L"RB";
                    else if (slot.kind == SlotKind::Collection) glyph = L"COL";
                    else if (slot.kind == SlotKind::Website) glyph = L"WEB";
                    else if (!slot.label.empty()) glyph = std::wstring(1, static_cast<wchar_t>(towupper(slot.label[0])));

                    D2D1_COLOR_F gc = tc.textColor;
                    gc.a *= iconAlpha;
                    if (auto b = mkBrush(gc))
                    {
                        const D2D1_RECT_F tr = D2D1::RectF(sc.x - haloR, sc.y - haloR, sc.x + haloR, sc.y + haloR);
                        m_d2dContext->DrawTextW(glyph.c_str(), static_cast<UINT32>(glyph.size()), m_glyphFormat.Get(), tr, b.Get());
                    }
                }

                if (slot.kind == SlotKind::App && slot.windows.size() > 1 && m_smallFormat)
                {
                    const float badgeDist = static_cast<float>(geom.iconSize * 0.46) * scale;
                    const D2D1_POINT_2F bc{ sc.x + badgeDist, sc.y - badgeDist };
                    D2D1_COLOR_F badgeBg = tc.accent;
                    badgeBg.a *= dimAlpha;
                    if (auto b = mkBrush(badgeBg))
                        m_d2dContext->FillEllipse(D2D1::Ellipse(bc, 8.5f, 8.5f), b.Get());
                    D2D1_COLOR_F countCol = Argb(255, 255, 255, 255);
                    countCol.a *= dimAlpha;
                    const std::wstring countTxt = slot.windows.size() > 9 ? L"9+" : std::to_wstring(slot.windows.size());
                    if (auto wb = mkBrush(countCol))
                    {
                        m_d2dContext->DrawTextW(countTxt.c_str(), static_cast<UINT32>(countTxt.size()),
                            m_smallFormat.Get(), D2D1::RectF(bc.x - 10, bc.y - 9, bc.x + 10, bc.y + 9), wb.Get());
                    }
                }
            }
        }

        // Draw actively dragged slot icon on top during ring reorder
        if (m_reorderActive && m_reorderCurrentIndex >= 0 && m_reorderCurrentIndex < slotCount)
        {
            const RingSlot& dragSlot = m_visibleSlots[static_cast<size_t>(m_reorderCurrentIndex)];
            double ddx = m_reorderCursorDip.x - kCenterDip;
            double ddy = m_reorderCursorDip.y - kCenterDip;
            const double maxDragR = geom.outerRadius + 24.0;
            const double curDist = std::hypot(ddx, ddy);
            if (curDist > maxDragR && curDist > 0.001)
            {
                ddx *= (maxDragR / curDist);
                ddy *= (maxDragR / curDist);
            }
            const D2D1_POINT_2F dc{ static_cast<float>(kCenterDip + ddx), static_cast<float>(kCenterDip + ddy) };
            constexpr float kDragScale = 1.18f;
            const float dragHaloR = static_cast<float>(geom.iconSize * 0.72) * (kDragScale / static_cast<float>(kSelectedSlotScale));

            if (auto b = mkBrush(tc.slotSelectedFill))
                m_d2dContext->FillEllipse(D2D1::Ellipse(dc, dragHaloR, dragHaloR), b.Get());
            if (auto b = mkBrush(tc.accent))
                m_d2dContext->DrawEllipse(D2D1::Ellipse(dc, dragHaloR, dragHaloR), b.Get(), 1.25f);

            if (dragSlot.iconBitmap)
            {
                if (auto bmp = GetOrCreateD2DBitmap(dragSlot.iconBitmap.Get()))
                {
                    const float ih = static_cast<float>(geom.iconSize * 0.46) * kDragScale;
                    m_d2dContext->DrawBitmap(bmp.Get(), D2D1::RectF(dc.x - ih, dc.y - ih, dc.x + ih, dc.y + ih), 1.0f);
                }
            }
            else if (m_glyphFormat && !dragSlot.label.empty())
            {
                const std::wstring glyph(1, static_cast<wchar_t>(towupper(dragSlot.label[0])));
                if (auto b = mkBrush(tc.textColor))
                {
                    m_d2dContext->DrawTextW(glyph.c_str(), 1, m_glyphFormat.Get(),
                        D2D1::RectF(dc.x - dragHaloR, dc.y - dragHaloR, dc.x + dragHaloR, dc.y + dragHaloR), b.Get());
                }
            }
        }

        // 3. Submenu cards (Downloads/Folders/Collections/Search continuous curved half-ring + DWM window frames)
        if ((m_expandedSlotIndex >= 0 || m_searchArcActive) && !m_submenuCards.empty())
        {
            if (curvedArcGeo)
                DrawCurvedArcTrack(curvedArcGeo.Get(), m_submenuArcLayout, tc, !hasLiveBackdrop, useNoise);

            for (size_t i = 0; i < m_submenuCards.size(); ++i)
            {
                const auto& card = m_submenuCards[i];
                const bool sel = (static_cast<int>(i) == m_selectedSubIndex);
                const float s = static_cast<float>(card.currentScale);
                const float cx = static_cast<float>(card.boundsDip.left + card.boundsDip.width * 0.5);
                const float cy = static_cast<float>(card.boundsDip.top + card.boundsDip.height * 0.5);
                const float hw = static_cast<float>(card.boundsDip.width * 0.5) * s;
                const float hh = static_cast<float>(card.boundsDip.height * 0.5) * s;
                const D2D1_RECT_F rect = D2D1::RectF(cx - hw, cy - hh, cx + hw, cy + hh);

                if (card.subSlot)
                {
                    if (sel)
                    {
                        const D2D1_POINT_2F cc{ cx, cy };
                        const float hoverR = (std::max)(hw, hh) + 4.0f;
                        D2D1_COLOR_F haloFill = tc.slotSelectedFill;
                        if (auto b = mkBrush(haloFill))
                            m_d2dContext->FillEllipse(D2D1::Ellipse(cc, hoverR, hoverR), b.Get());
                        if (auto b = mkBrush(tc.sectorStroke))
                            m_d2dContext->DrawEllipse(D2D1::Ellipse(cc, hoverR, hoverR), b.Get(), tc.hairlineWidth);
                    }
                    if (card.subSlot->iconBitmap)
                    {
                        if (auto bmp = GetOrCreateD2DBitmap(card.subSlot->iconBitmap.Get()))
                        {
                            const float ih = (std::min)(hw, hh) * 0.92f;
                            m_d2dContext->DrawBitmap(bmp.Get(), D2D1::RectF(cx - ih, cy - ih, cx + ih, cy + ih), 1.0f);
                        }
                    }
                    else if (m_titleFormat && !card.subSlot->label.empty())
                    {
                        const std::wstring ch(1, static_cast<wchar_t>(towupper(card.subSlot->label[0])));
                        if (auto b = mkBrush(tc.textColor))
                            m_d2dContext->DrawTextW(ch.c_str(), 1, m_titleFormat.Get(), rect, b.Get());
                    }
                }
                else if (card.window)
                {
                    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rect, 8.0f, 8.0f);
                    if (!hasLiveBackdrop)
                    {
                        D2D1_COLOR_F fill = tc.centerFill;
                        if (card.window->isMinimized)
                            fill.a *= 0.65f;
                        if (auto b = mkBrush(fill))
                            m_d2dContext->FillRoundedRectangle(rr, b.Get());
                    }
                    if (useNoise)
                        m_d2dContext->FillRoundedRectangle(rr, m_noiseBrush.Get());
                    if (auto b = mkBrush(sel ? tc.sectorStroke : tc.centerBorder))
                        m_d2dContext->DrawRoundedRectangle(rr, b.Get(), sel ? 1.5f : (wcaActive ? (std::max)(1.10f, tc.hairlineWidth) : tc.hairlineWidth));

                    if (m_smallFormat)
                    {
                        D2D1_RECT_F textRect = (m_config.windowViewMode == WindowViewMode::TitleColumn)
                            ? D2D1::RectF(rect.left + 8, rect.top, rect.right - 26.0f, rect.bottom)
                            : D2D1::RectF(rect.left + 6, rect.bottom - 20.0f, rect.right - 24.0f, rect.bottom - 2.0f);
                        const std::wstring txt = (card.window->isMinimized && m_config.windowViewMode == WindowViewMode::TitleColumn)
                            ? (card.window->title + L" (minimized)")
                            : card.window->title;
                        D2D1_COLOR_F txtCol = tc.textColor;
                        if (card.window->isMinimized) txtCol.a *= 0.72f;
                        if (auto b = mkBrush(txtCol))
                            m_d2dContext->DrawTextW(txt.c_str(), static_cast<UINT32>(txt.size()), m_smallFormat.Get(), textRect, b.Get());
                    }

                    const int cardIdx = static_cast<int>(i);
                    const bool closeHot = (cardIdx == m_hoveredCloseSubIndex);
                    const D2D1_RECT_F closeRc = GetSubmenuCloseRectDip(rect, m_config.windowViewMode);
                    const float ccx = (closeRc.left + closeRc.right) * 0.5f;
                    const float ccy = (closeRc.top + closeRc.bottom) * 0.5f;
                    const float cr = (closeRc.right - closeRc.left) * 0.5f;
                    const D2D1_COLOR_F closeBg = closeHot
                        ? D2D1::ColorF(220.0f / 255.0f, 53.0f / 255.0f, 69.0f / 255.0f, 0.94f)
                        : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f);
                    if (auto cb = mkBrush(closeBg))
                        m_d2dContext->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ccx, ccy), cr, cr), cb.Get());
                    const D2D1_COLOR_F closeFg = closeHot
                        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)
                        : D2D1::ColorF(tc.textColor.r, tc.textColor.g, tc.textColor.b, 0.76f);
                    if (auto xb = mkBrush(closeFg))
                    {
                        const float xs = 3.2f;
                        m_d2dContext->DrawLine(D2D1::Point2F(ccx - xs, ccy - xs), D2D1::Point2F(ccx + xs, ccy + xs), xb.Get(), 1.4f);
                        m_d2dContext->DrawLine(D2D1::Point2F(ccx + xs, ccy - xs), D2D1::Point2F(ccx - xs, ccy + xs), xb.Get(), 1.4f);
                    }
                }
            }
        }

        // 4. Center label / Search Stadium Pill (continuous acrylic interior + razor-thin hairline border)
        if (m_dragActive && m_dragOverCenter)
        {
            D2D1_COLOR_F ringCol = tc.accent;
            ringCol.a = 0.72f;
            if (auto b = mkBrush(ringCol))
                m_d2dContext->DrawEllipse(D2D1::Ellipse(centerPt, (std::max)(8.0f, holeR - 4.0f), (std::max)(8.0f, holeR - 4.0f)), b.Get(), 1.5f);
        }

        if (hasQuery)
        {
            if (!hasLiveBackdrop)
            {
                if (auto b = mkBrush(tc.searchFill))
                    m_d2dContext->FillRoundedRectangle(searchPill, b.Get());
            }
            if (useNoise)
                m_d2dContext->FillRoundedRectangle(searchPill, m_noiseBrush.Get());
            if (auto b = mkBrush(tc.searchBorder))
                m_d2dContext->DrawRoundedRectangle(searchPill, b.Get(), wcaActive ? (std::max)(1.10f, tc.hairlineWidth) : tc.hairlineWidth);

            // A4S-Menu monochrome #909090 vector magnifier on the left of the search pill
            if (auto mb = mkBrush(Argb(255, 144, 144, 144)))
            {
                const float iconX = searchPill.rect.left + 11.0f;
                const float iconY = searchPill.rect.top + 6.0f;
                m_d2dContext->DrawEllipse(
                    D2D1::Ellipse(D2D1::Point2F(iconX + 5.2f, iconY + 5.2f), 4.6f, 4.6f), mb.Get(), 1.15f);
                m_d2dContext->DrawLine(
                    D2D1::Point2F(iconX + 8.6f, iconY + 8.6f),
                    D2D1::Point2F(iconX + 12.6f, iconY + 12.6f), mb.Get(), 1.25f);
            }

            if (m_searchFormat)
            {
                const D2D1_RECT_F tr = D2D1::RectF(
                    searchPill.rect.left + 28.0f, searchPill.rect.top, searchPill.rect.right - 10.0f, searchPill.rect.bottom);
                const D2D1_COLOR_F qCol = m_rankedMatches.empty() ? tc.mutedTextColor : tc.textColor;
                if (auto b = mkBrush(qCol))
                    m_d2dContext->DrawTextW(m_filterQuery.c_str(), static_cast<UINT32>(m_filterQuery.size()),
                        m_searchFormat.Get(), tr, b.Get());
            }
        }

        if (hasSummary)
        {
            if (!hasLiveBackdrop)
            {
                if (auto b = mkBrush(tc.centerFill))
                    m_d2dContext->FillRoundedRectangle(centerPill, b.Get());
            }
            if (useNoise)
                m_d2dContext->FillRoundedRectangle(centerPill, m_noiseBrush.Get());
            const bool pinHot = (m_dragActive && m_dragOverCenter);
            if (auto b = mkBrush(pinHot ? tc.accent : tc.centerBorder))
                m_d2dContext->DrawRoundedRectangle(centerPill, b.Get(), pinHot ? 1.4f : (wcaActive ? (std::max)(1.10f, tc.hairlineWidth) : tc.hairlineWidth));

            if (m_titleFormat)
            {
                const D2D1_COLOR_F lblCol = (hasQuery && m_rankedMatches.empty()) ? tc.mutedTextColor : tc.textColor;
                if (auto b = mkBrush(lblCol))
                {
                    const D2D1_RECT_F tr = D2D1::RectF(
                        centerPill.rect.left + 10.0f, centerPill.rect.top,
                        centerPill.rect.right - 10.0f, centerPill.rect.bottom);
                    m_d2dContext->DrawTextW(summary.c_str(), static_cast<UINT32>(summary.size()),
                        m_titleFormat.Get(), tr, b.Get());
                }
            }
        }
        else if (!hasQuery)
        {
            if (auto b = mkBrush(tc.idleDotColor))
                m_d2dContext->FillEllipse(D2D1::Ellipse(centerPt, 3.0f, 3.0f), b.Get());
        }

        const HRESULT endHr = m_d2dContext->EndDraw();
        m_d2dContext->SetTarget(nullptr);
        if (endHr == D2DERR_RECREATE_TARGET)
        {
            ReleaseGraphicsDevices();
            CreateGraphicsDevices();
            return;
        }
        m_swapChain->Present(0, 0);
        if (!acrylicActive && m_dcompDevice)
            m_dcompDevice->Commit();
    }

    void RingOverlay::ShowContextMenuAt(POINT screenPt)
    {
        if (!m_hwnd)
            return;
        HWND hoveredWinHwnd = nullptr;
        if (m_selectedSubIndex >= 0 && m_selectedSubIndex < static_cast<int>(m_submenuCards.size()))
        {
            const auto& card = m_submenuCards[static_cast<size_t>(m_selectedSubIndex)];
            if (card.window && card.window->hwnd && IsWindow(card.window->hwnd))
                hoveredWinHwnd = card.window->hwnd;
        }
        ClearSubmenu();
        m_contextMenuOpen = true;

        HMENU menu = CreatePopupMenu();
        if (!menu)
        {
            m_contextMenuOpen = false;
            return;
        }

        enum MenuCmd
        {
            CmdWinBase = 100,
            CmdNewWindow = 200,
            CmdMinAll = 201,
            CmdCloseAll = 202,
            CmdOpenLocation = 203,
            CmdShowInExplorer = 204,
            CmdTogglePin = 205,
            CmdEmptyRecycleBin = 206,
            CmdRunAsAdmin = 207,
            CmdCloseSubWindow = 208,
            CmdEndTask = 209,
            CmdNewCollection = 300,
            CmdToggleTopBar = 301,
            CmdSettings = 302,
            CmdExit = 303,
            CmdToggleMediaIsland = 304
        };

        RingSlot targetSlot;
        DWORD targetPid = 0;
        std::wstring adminTarget;
        const bool hasSlot = (m_selectedIndex >= 0 && m_selectedIndex < static_cast<int>(m_visibleSlots.size()));
        if (hasSlot)
        {
            targetSlot = m_visibleSlots[static_cast<size_t>(m_selectedIndex)];
            const HWND actionWinHwnd = hoveredWinHwnd
                ? hoveredWinHwnd
                : (!targetSlot.windows.empty() ? targetSlot.windows[0].hwnd : nullptr);
            if (actionWinHwnd && IsWindow(actionWinHwnd))
                GetWindowThreadProcessId(actionWinHwnd, &targetPid);

            if ((targetSlot.kind == SlotKind::App || targetSlot.kind == SlotKind::Script) &&
                !targetSlot.pathOrUrl.empty() &&
                targetSlot.pathOrUrl.rfind(L"shell:", 0) != 0 &&
                (targetSlot.pathOrUrl.find(L'\\') != std::wstring::npos ||
                 targetSlot.pathOrUrl.find(L'/') != std::wstring::npos))
            {
                adminTarget = targetSlot.pathOrUrl;
            }
            else if (targetSlot.kind == SlotKind::App && targetSlot.pathOrUrl.empty() &&
                     targetPid != 0 && targetPid != GetCurrentProcessId())
            {
                HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, targetPid);
                if (hProc)
                {
                    wchar_t exeBuf[MAX_PATH * 2]{};
                    DWORD len = ARRAYSIZE(exeBuf);
                    if (QueryFullProcessImageNameW(hProc, 0, exeBuf, &len) && len > 0)
                        adminTarget.assign(exeBuf, len);
                    CloseHandle(hProc);
                }
            }

            if (!targetSlot.windows.empty())
            {
                const size_t limit = (std::min)(targetSlot.windows.size(), size_t{ 9 });
                for (size_t i = 0; i < limit; ++i)
                    AppendMenuW(menu, MF_STRING, CmdWinBase + i, targetSlot.windows[i].title.c_str());
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            }
            if (targetSlot.kind == SlotKind::App)
            {
                AppendMenuW(menu, MF_STRING, CmdNewWindow, L"New Window");
                if (!adminTarget.empty())
                    AppendMenuW(menu, MF_STRING, CmdRunAsAdmin, L"Run as Administrator");
                if (!targetSlot.windows.empty())
                {
                    AppendMenuW(menu, MF_STRING, CmdMinAll, L"Minimize All");
                    if (hoveredWinHwnd)
                        AppendMenuW(menu, MF_STRING, CmdCloseSubWindow, L"Close Window");
                    AppendMenuW(menu, MF_STRING, CmdCloseAll, L"Close All");
                    if (targetPid != 0 && targetPid != GetCurrentProcessId())
                        AppendMenuW(menu, MF_STRING, CmdEndTask, L"End Task");
                }
                if (!targetSlot.pathOrUrl.empty())
                {
                    AppendMenuW(menu, MF_STRING, CmdOpenLocation, L"Open File Location");
                    AppendMenuW(menu, MF_STRING, CmdTogglePin,
                        targetSlot.isPinned ? L"Unpin from Ring" : L"Pin to Ring");
                }
            }
            else if (targetSlot.kind == SlotKind::File || targetSlot.kind == SlotKind::Folder || targetSlot.kind == SlotKind::Script)
            {
                if (hoveredWinHwnd)
                    AppendMenuW(menu, MF_STRING, CmdCloseSubWindow, L"Close Window");
                if (!adminTarget.empty())
                    AppendMenuW(menu, MF_STRING, CmdRunAsAdmin, L"Run as Administrator");
                AppendMenuW(menu, MF_STRING, CmdShowInExplorer, L"Show in Explorer");
                if (targetPid != 0 && targetPid != GetCurrentProcessId())
                    AppendMenuW(menu, MF_STRING, CmdEndTask, L"End Task");
                if (targetSlot.isPinned)
                    AppendMenuW(menu, MF_STRING, CmdTogglePin, L"Unpin from Ring");
            }
            else if (targetSlot.kind == SlotKind::RecycleBin)
            {
                AppendMenuW(menu, MF_STRING, CmdEmptyRecycleBin, L"Empty Recycle Bin");
            }
            else if (targetSlot.isPinned)
            {
                AppendMenuW(menu, MF_STRING, CmdTogglePin, L"Unpin from Ring");
            }
        }
        else
        {
            AppendMenuW(menu, MF_STRING, CmdNewCollection, L"New Collection...");
            AppendMenuW(menu, MF_STRING | (m_config.topBarEnabled ? MF_CHECKED : MF_UNCHECKED),
                CmdToggleTopBar, L"Toggle Top Minimal Bar");
            AppendMenuW(menu, MF_STRING | (m_config.mediaIslandEnabled ? MF_CHECKED : MF_UNCHECKED),
                CmdToggleMediaIsland, L"Show Media Island");
            AppendMenuW(menu, MF_STRING, CmdSettings, L"Settings...");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, CmdExit, L"Exit WinRings");
        }

        SetForegroundWindow(m_hwnd);
        const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, m_hwnd, nullptr);
        DestroyMenu(menu);
        PostMessageW(m_hwnd, WM_NULL, 0, 0);
        m_contextMenuOpen = false;

        if (cmd == 0)
            return;

        if (hasSlot && cmd >= CmdWinBase && cmd < CmdWinBase + 9)
        {
            const size_t idx = cmd - CmdWinBase;
            if (idx < targetSlot.windows.size() && m_tracker)
            {
                const HWND winHwnd = targetSlot.windows[idx].hwnd;
                Dismiss();
                m_tracker->ActivateWindow(winHwnd, false, m_config.pointerFollow);
            }
            return;
        }

        switch (cmd)
        {
        case CmdNewWindow:
            Dismiss();
            SlotModel::LaunchPathOrUrl(targetSlot.pathOrUrl);
            break;
        case CmdRunAsAdmin:
            Dismiss();
            if (!adminTarget.empty())
                ShellExecuteW(nullptr, L"runas", adminTarget.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case CmdMinAll:
            for (const auto& w : targetSlot.windows) WindowTracker::MinimizeWindow(w.hwnd);
            Dismiss();
            break;
        case CmdCloseSubWindow:
            if (hoveredWinHwnd)
                WindowTracker::CloseWindow(hoveredWinHwnd);
            Dismiss();
            break;
        case CmdCloseAll:
            for (const auto& w : targetSlot.windows) WindowTracker::CloseWindow(w.hwnd);
            Dismiss();
            break;
        case CmdEndTask:
            if (targetPid != 0 && targetPid != GetCurrentProcessId())
                WindowTracker::TerminateProcessByPid(targetPid);
            Dismiss();
            break;
        case CmdOpenLocation:
        case CmdShowInExplorer:
            Dismiss();
            SlotModel::RevealInExplorer(targetSlot.pathOrUrl);
            break;
        case CmdEmptyRecycleBin:
            Dismiss();
            SlotModel::EmptyRecycleBin(nullptr);
            break;
        case CmdTogglePin:
            if (targetSlot.isPinned)
            {
                m_config.pinnedSlots.erase(
                    std::remove_if(m_config.pinnedSlots.begin(), m_config.pinnedSlots.end(),
                        [&](const PinnedSlotConfig& p) { return p.id == targetSlot.id; }),
                    m_config.pinnedSlots.end());
            }
            else if (!targetSlot.pathOrUrl.empty())
            {
                PinnedSlotConfig p;
                p.id = L"pin-" + std::to_wstring(GetTickCount64());
                p.kind = SlotKind::App;
                p.label = targetSlot.label;
                p.pathOrUrl = targetSlot.pathOrUrl;
                m_config.pinnedSlots.push_back(std::move(p));
            }
            if (m_callbacks.saveAndReloadConfig) m_callbacks.saveAndReloadConfig(m_config);
            Dismiss();
            break;
        case CmdNewCollection:
        {
            PinnedSlotConfig col;
            col.id = L"col-" + std::to_wstring(GetTickCount64());
            col.kind = SlotKind::Collection;
            col.label = L"Collection " + std::to_wstring(m_config.pinnedSlots.size() + 1);
            m_config.pinnedSlots.push_back(std::move(col));
            if (m_callbacks.saveAndReloadConfig) m_callbacks.saveAndReloadConfig(m_config);
            Dismiss();
            break;
        }
        case CmdToggleTopBar:
            Dismiss();
            if (m_callbacks.onToggleTopBarRequested) m_callbacks.onToggleTopBarRequested();
            break;
        case CmdToggleMediaIsland:
            Dismiss();
            if (m_callbacks.onToggleMediaIslandRequested) m_callbacks.onToggleMediaIslandRequested();
            break;
        case CmdSettings:
            Dismiss();
            if (m_callbacks.launchTools) m_callbacks.launchTools(L"--settings");
            break;
        case CmdExit:
            Dismiss();
            if (m_callbacks.onExitRequested) m_callbacks.onExitRequested();
            break;
        }
    }

    int RingOverlay::PinnedVisibleSlotCount() const noexcept
    {
        int count = 0;
        for (const auto& s : m_visibleSlots)
        {
            if (!s.isPinned)
                break;
            ++count;
        }
        return count;
    }

    void RingOverlay::ResetDragSpringState() noexcept
    {
        m_springSlotIndex = -1;
        m_springHoverStartQpc = 0;
        m_springTriggered = false;
        m_springPulseScale = 1.0;
    }

    void RingOverlay::EnsureAnimTimerRunning()
    {
        if (!m_animTimerRunning && m_hwnd)
        {
            SetTimer(m_hwnd, kAnimTimerId, kAnimIntervalMs, nullptr);
            m_animTimerRunning = true;
        }
    }

    bool RingOverlay::CanSlotSpringLoad(const RingSlot& slot) const noexcept
    {
        return slot.kind == SlotKind::App || slot.kind == SlotKind::Folder;
    }

    void RingOverlay::TriggerSpringLoadForSlot(int slotIndex)
    {
        if (slotIndex < 0 || slotIndex >= static_cast<int>(m_visibleSlots.size()))
            return;

        const RingSlot slotCopy = m_visibleSlots[static_cast<size_t>(slotIndex)];
        const bool hasWin = !slotCopy.windows.empty();
        const WindowEntry winCopy = hasWin ? slotCopy.windows.front() : WindowEntry{};
        Dismiss();
        if (hasWin && winCopy.hwnd && m_tracker)
        {
            m_tracker->ActivateWindow(winCopy.hwnd, false, false);
        }
        else if (!slotCopy.pathOrUrl.empty())
        {
            SlotModel::LaunchPathOrUrl(slotCopy.pathOrUrl);
        }
    }

    void RingOverlay::CancelPendingPressAndReorder(bool revertVisualOrder)
    {
        if (revertVisualOrder && m_reorderActive &&
            m_reorderFromIndex >= 0 && m_reorderCurrentIndex >= 0 &&
            m_reorderFromIndex != m_reorderCurrentIndex)
        {
            ReorderVectorElement(m_visibleSlots, static_cast<size_t>(m_reorderCurrentIndex), static_cast<size_t>(m_reorderFromIndex));
            ReorderVectorElement(m_allSlots, static_cast<size_t>(m_reorderCurrentIndex), static_cast<size_t>(m_reorderFromIndex));
            ReorderVectorElement(m_slotScales, static_cast<size_t>(m_reorderCurrentIndex), static_cast<size_t>(m_reorderFromIndex));
        }
        m_pressKind = PressKind::None;
        m_pressSlotIndex = -1;
        m_pressSubIndex = -1;
        m_pressStartTick = 0;
        m_reorderActive = false;
        m_reorderFromIndex = -1;
        m_reorderCurrentIndex = -1;
        if (m_isOpen)
            Render();
    }

    void RingOverlay::OnDragEnterOverlay()
    {
        m_dragActive = true;
        m_dragOverCenter = false;
        ResetDragSpringState();
        ClearSubmenu();
        Render();
    }

    void RingOverlay::OnDragOverOverlay(POINT screenPt)
    {
        const double dx = (screenPt.x - m_centerScreenPhysical.x) / m_dpiScale;
        const double dy = (screenPt.y - m_centerScreenPhysical.y) / m_dpiScale;
        const double dist = std::hypot(dx, dy);
        const RingGeometry geom = ActiveGeometry();
        const double pinRadiusDip = (std::max)(m_config.deadzoneDip + 20.0, geom.holeRadius - 4.0);

        const bool overCenter = (dist <= pinRadiusDip);
        const int nextIndex = overCenter
            ? -1
            : CalculateSectorIndex(dx, dy, static_cast<int>(m_visibleSlots.size()), pinRadiusDip);

        const bool slotChanged = (overCenter != m_dragOverCenter || nextIndex != m_selectedIndex);
        m_dragOverCenter = overCenter;
        m_selectedIndex = nextIndex;

        if (nextIndex >= 0 && nextIndex < static_cast<int>(m_visibleSlots.size()) &&
            CanSlotSpringLoad(m_visibleSlots[static_cast<size_t>(nextIndex)]))
        {
            if (nextIndex != m_springSlotIndex)
            {
                m_springSlotIndex = nextIndex;
                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);
                m_springHoverStartQpc = now.QuadPart;
                m_springTriggered = false;
                m_springPulseScale = 1.0;
            }
            EnsureAnimTimerRunning();
        }
        else
        {
            ResetDragSpringState();
        }

        if (slotChanged)
        {
            for (size_t i = 0; i < m_slotScales.size(); ++i)
                m_slotScales[i] = (static_cast<int>(i) == m_selectedIndex) ? kSelectedSlotScale : kIdleSlotScale;
            Render();
        }
    }

    void RingOverlay::OnDragLeaveOverlay()
    {
        m_dragActive = false;
        m_dragOverCenter = false;
        m_selectedIndex = -1;
        ResetDragSpringState();
        for (size_t i = 0; i < m_slotScales.size(); ++i)
            m_slotScales[i] = kIdleSlotScale;
        Render();
    }

    void RingOverlay::OnDropOverlay(POINT screenPt, std::vector<std::wstring> files, std::wstring text)
    {
        m_dragActive = false;
        m_dragOverCenter = false;
        ResetDragSpringState();

        const double dx = (screenPt.x - m_centerScreenPhysical.x) / m_dpiScale;
        const double dy = (screenPt.y - m_centerScreenPhysical.y) / m_dpiScale;
        const double distFromCenter = std::hypot(dx, dy);
        const double pinRadiusDip = (std::max)(m_config.deadzoneDip + 20.0, ActiveGeometry().holeRadius - 4.0);

        if (distFromCenter <= pinRadiusDip)
        {
            if (!files.empty())
            {
                const std::wstring& first = files[0];
                const DWORD attrs = GetFileAttributesW(first.c_str());
                SlotKind kind = SlotKind::File;
                if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0)
                    kind = SlotKind::Folder;
                else if (first.size() >= 4 && _wcsicmp(first.c_str() + first.size() - 4, L".exe") == 0)
                    kind = SlotKind::App;

                const size_t slash = first.find_last_of(L"\\/");
                std::wstring label = (slash != std::wstring::npos) ? first.substr(slash + 1) : first;
                if (kind == SlotKind::App && IsExplorerExecutablePath(first))
                {
                    label = L"File Explorer";
                }
                else if (kind != SlotKind::Folder)
                {
                    const size_t dot = label.find_last_of(L'.');
                    if (dot != std::wstring::npos && dot > 0)
                        label.resize(dot);
                }

                PinnedSlotConfig pin;
                pin.id = L"pin-" + std::to_wstring(GetTickCount64());
                pin.kind = kind;
                pin.label = label;
                pin.pathOrUrl = first;
                m_config.pinnedSlots.push_back(std::move(pin));
                if (m_callbacks.saveAndReloadConfig)
                    m_callbacks.saveAndReloadConfig(m_config);
            }
            else if (!text.empty() && (text.rfind(L"http://", 0) == 0 || text.rfind(L"https://", 0) == 0))
            {
                const std::wstring pinId = L"site-" + std::to_wstring(GetTickCount64());
                PinnedSlotConfig pin;
                pin.id = pinId;
                pin.kind = SlotKind::Website;
                pin.label = text;
                pin.pathOrUrl = text;
                m_config.pinnedSlots.push_back(pin);
                if (m_callbacks.saveAndReloadConfig)
                    m_callbacks.saveAndReloadConfig(m_config);

                const HWND overlayHwnd = m_hwnd;
                const std::wstring cfgPath = SwitcherConfig::ConfigPath();
                SlotModel::FetchFaviconAsync(text, [overlayHwnd, cfgPath, pinId](std::wstring relPath) {
                    if (relPath.empty())
                        return;
                    SwitcherConfig diskCfg;
                    if (!SwitcherConfig::LoadFromFile(cfgPath, diskCfg))
                        diskCfg = SwitcherConfig::CreateDefault();
                    for (auto& s : diskCfg.pinnedSlots)
                    {
                        if (s.id == pinId)
                        {
                            s.iconCacheFile = relPath;
                            break;
                        }
                    }
                    diskCfg.SaveToFile(cfgPath);
                    if (overlayHwnd && IsWindow(overlayHwnd))
                        PostMessageW(overlayHwnd, WM_APP + 50, 0, 0);
                });
            }
            Dismiss();
            return;
        }

        if (!files.empty())
        {
            const int dropIdx = CalculateSectorIndex(dx, dy, static_cast<int>(m_visibleSlots.size()), pinRadiusDip);
            if (dropIdx >= 0 && dropIdx < static_cast<int>(m_visibleSlots.size()))
            {
                const RingSlot target = m_visibleSlots[static_cast<size_t>(dropIdx)];
                Dismiss();
                if (target.kind == SlotKind::RecycleBin)
                {
                    SlotModel::SendToRecycleBin(files);
                }
                else if (target.kind == SlotKind::Folder && !target.pathOrUrl.empty())
                {
                    SlotModel::CopyFilesToFolder(files, target.pathOrUrl);
                }
                else if ((target.kind == SlotKind::App || target.kind == SlotKind::Script) && !target.pathOrUrl.empty())
                {
                    std::wstring args;
                    for (const auto& f : files)
                    {
                        if (!args.empty())
                            args.push_back(L' ');
                        args += L"\"" + f + L"\"";
                    }
                    SlotModel::LaunchPathOrUrl(target.pathOrUrl, args);
                }
                return;
            }
        }

        Dismiss();
    }

    LRESULT CALLBACK RingOverlay::OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* self = reinterpret_cast<RingOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE)
        {
            self = static_cast<RingOverlay*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->m_hwnd = hwnd;
        }
        if (self)
        {
            try { return self->HandleMessage(msg, wp, lp); }
            catch (...) {}
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT RingOverlay::HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_NCHITTEST:
        {
            if (m_reorderActive)
                return HTCLIENT;
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            return IsPointInActiveBounds(pt) ? HTCLIENT : HTTRANSPARENT;
        }

        case WM_TIMER:
            if (wp == kAnimTimerId)
            {
                StepAnimations();
                return 0;
            }
            break;

        case WM_MOUSEMOVE:
        {
            if (!m_isOpen)
                return 0;

            POINT clientPt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            POINT screenPt = clientPt;
            ClientToScreen(m_hwnd, &screenPt);
            const PointD localDip{ clientPt.x / m_dpiScale, clientPt.y / m_dpiScale };

            if (m_pressKind == PressKind::SubmenuCard)
            {
                const double movedPx = std::hypot(
                    static_cast<double>(screenPt.x - m_pressScreenPt.x),
                    static_cast<double>(screenPt.y - m_pressScreenPt.y));
                if (movedPx >= 5.0 * m_dpiScale)
                {
                    std::wstring dragPath;
                    HWND snapWindowHwnd = nullptr;
                    if (m_expandedSlotIndex >= 0 && m_pressSubIndex >= 0 &&
                        m_pressSubIndex < static_cast<int>(m_submenuCards.size()))
                    {
                        const auto& card = m_submenuCards[static_cast<size_t>(m_pressSubIndex)];
                        if (card.subSlot &&
                            (card.subSlot->kind == SlotKind::File || card.subSlot->kind == SlotKind::Folder) &&
                            !card.subSlot->pathOrUrl.empty())
                        {
                            dragPath = card.subSlot->pathOrUrl;
                        }
                        else if (card.window && card.window->hwnd && IsWindow(card.window->hwnd))
                        {
                            snapWindowHwnd = card.window->hwnd;
                        }
                    }

                    if (!dragPath.empty())
                    {
                        m_pressKind = PressKind::None;
                        m_pressSubIndex = -1;
                        m_pressStartTick = 0;
                        if (GetCapture() == m_hwnd)
                            ReleaseCapture();
                        ClearSubmenu();
                        m_selectedIndex = -1;
                        Render();
                        m_draggingOutFromRing = true;
                        RingDropTarget::StartFileDragOut(nullptr, dragPath);
                        m_draggingOutFromRing = false;
                        if (m_isOpen)
                            Dismiss();
                    }
                    else if (snapWindowHwnd && m_config.dragToSnapEnabled)
                    {
                        const ULONGLONG heldMs = GetTickCount64() - m_pressStartTick;
                        if (movedPx >= 10.0 * m_dpiScale &&
                            (heldMs >= static_cast<ULONGLONG>(m_config.dragToSnapHoldMs) || movedPx >= 24.0 * m_dpiScale))
                        {
                            m_pressKind = PressKind::None;
                            m_pressSubIndex = -1;
                            m_pressStartTick = 0;
                            if (GetCapture() == m_hwnd)
                                ReleaseCapture();
                            Dismiss();
                            if (m_callbacks.onStartOpenWindowSnapDrag)
                                m_callbacks.onStartOpenWindowSnapDrag(snapWindowHwnd, screenPt);
                        }
                    }
                    else
                    {
                        m_pressKind = PressKind::None;
                        m_pressSubIndex = -1;
                        m_pressStartTick = 0;
                        if (GetCapture() == m_hwnd)
                            ReleaseCapture();
                    }
                }
                return 0;
            }

            if (m_pressKind == PressKind::RingSlot)
            {
                const int pinnedCount = PinnedVisibleSlotCount();
                const double movedPx = std::hypot(
                    static_cast<double>(screenPt.x - m_pressScreenPt.x),
                    static_cast<double>(screenPt.y - m_pressScreenPt.y));
                const ULONGLONG heldMs = GetTickCount64() - m_pressStartTick;
                const RingGeometry geom = ActiveGeometry();
                const double distDip = std::hypot(localDip.x - kCenterDip, localDip.y - kCenterDip);

                // Unpinned running slots are not reorderable: start Drag-to-Snap directly.
                if (m_config.dragToSnapEnabled && m_pressSlotIndex >= pinnedCount &&
                    m_pressSlotIndex < static_cast<int>(m_visibleSlots.size()))
                {
                    if (movedPx >= 10.0 * m_dpiScale &&
                        (heldMs >= static_cast<ULONGLONG>(m_config.dragToSnapHoldMs) || movedPx >= 24.0 * m_dpiScale))
                    {
                        HWND targetHwnd = nullptr;
                        const auto& slot = m_visibleSlots[static_cast<size_t>(m_pressSlotIndex)];
                        if (!slot.windows.empty() && IsWindow(slot.windows[0].hwnd))
                            targetHwnd = slot.windows[0].hwnd;
                        m_pressKind = PressKind::None;
                        m_pressSlotIndex = -1;
                        m_pressStartTick = 0;
                        if (GetCapture() == m_hwnd)
                            ReleaseCapture();
                        if (targetHwnd)
                        {
                            Dismiss();
                            if (m_callbacks.onStartOpenWindowSnapDrag)
                                m_callbacks.onStartOpenWindowSnapDrag(targetHwnd, screenPt);
                        }
                    }
                    return 0;
                }

                // Pinned slots: reorder inside ring; tear-off outside outerRadius + threshold transitions to Drag-to-Snap.
                const double tearOffRadiusDip = geom.outerRadius + static_cast<double>(m_config.dragToSnapTearOffDip);
                if (m_config.dragToSnapEnabled &&
                    m_pressSlotIndex >= 0 && m_pressSlotIndex < pinnedCount &&
                    m_filterQuery.empty() && !m_searchArcActive &&
                    movedPx >= 10.0 * m_dpiScale &&
                    distDip >= tearOffRadiusDip &&
                    heldMs >= static_cast<ULONGLONG>(m_config.dragToSnapHoldMs))
                {
                    const int activeIdx = m_reorderActive ? m_reorderCurrentIndex : m_pressSlotIndex;
                    if (activeIdx >= 0 && activeIdx < static_cast<int>(m_visibleSlots.size()))
                    {
                        const RingSlot slotCopy = m_visibleSlots[static_cast<size_t>(activeIdx)];
                        HWND openHwnd = (!slotCopy.windows.empty() && IsWindow(slotCopy.windows[0].hwnd))
                            ? slotCopy.windows[0].hwnd
                            : nullptr;
                        const bool canClosedSnap = !openHwnd && m_config.dragToSnapLaunchClosed &&
                            (slotCopy.kind == SlotKind::App || slotCopy.kind == SlotKind::Folder ||
                             slotCopy.kind == SlotKind::RecycleBin || slotCopy.kind == SlotKind::Website);

                        if (openHwnd || canClosedSnap)
                        {
                            CancelPendingPressAndReorder(true);
                            if (GetCapture() == m_hwnd)
                                ReleaseCapture();
                            Dismiss();
                            if (openHwnd)
                            {
                                if (m_callbacks.onStartOpenWindowSnapDrag)
                                    m_callbacks.onStartOpenWindowSnapDrag(openHwnd, screenPt);
                            }
                            else
                            {
                                if (m_callbacks.onStartClosedAppSnapDrag)
                                    m_callbacks.onStartClosedAppSnapDrag(slotCopy.kind, slotCopy.label, slotCopy.pathOrUrl, screenPt);
                            }
                            return 0;
                        }
                    }
                }

                if (!m_reorderActive)
                {
                    if (movedPx >= 6.0 * m_dpiScale &&
                        m_pressSlotIndex >= 0 && m_pressSlotIndex < pinnedCount &&
                        pinnedCount > 1 && m_filterQuery.empty() && !m_searchArcActive)
                    {
                        m_reorderActive = true;
                        m_reorderFromIndex = m_pressSlotIndex;
                        m_reorderCurrentIndex = m_pressSlotIndex;
                        m_reorderCursorDip = localDip;
                        ClearSubmenu();
                        Render();
                    }
                }
                if (m_reorderActive)
                {
                    m_reorderCursorDip = localDip;
                    const int targetIdx = ComputeReorderTargetSlotIndex(
                        localDip.x - kCenterDip,
                        localDip.y - kCenterDip,
                        pinnedCount,
                        static_cast<int>(m_visibleSlots.size()));
                    if (targetIdx != m_reorderCurrentIndex &&
                        targetIdx >= 0 && targetIdx < static_cast<int>(m_visibleSlots.size()))
                    {
                        ReorderVectorElement(m_visibleSlots, static_cast<size_t>(m_reorderCurrentIndex), static_cast<size_t>(targetIdx));
                        ReorderVectorElement(m_allSlots, static_cast<size_t>(m_reorderCurrentIndex), static_cast<size_t>(targetIdx));
                        ReorderVectorElement(m_slotScales, static_cast<size_t>(m_reorderCurrentIndex), static_cast<size_t>(targetIdx));
                        m_reorderCurrentIndex = targetIdx;
                        m_selectedIndex = targetIdx;
                    }
                    Render();
                }
                return 0;
            }

            if (m_pressKind == PressKind::CenterDeadzone)
                return 0;

            if (!m_contextMenuOpen && !m_dragActive && !m_reorderActive)
            {
                if (!m_hasLastPointer || screenPt.x != m_lastPointerPhysical.x || screenPt.y != m_lastPointerPhysical.y)
                    UpdatePointer(screenPt.x, screenPt.y);
            }
            return 0;
        }

        case WM_LBUTTONDOWN:
        {
            if (!m_isLockedOpen || m_dragActive)
                return 0;
            POINT clientPt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            POINT screenPt = clientPt;
            ClientToScreen(m_hwnd, &screenPt);
            if (!m_hasLastPointer && m_selectedIndex < 0 && m_selectedSubIndex < 0)
                UpdatePointer(screenPt.x, screenPt.y);
            const PointD localDip{ clientPt.x / m_dpiScale, clientPt.y / m_dpiScale };

            const int closeHit = HitTestSubmenuCloseButton(localDip);
            if (closeHit >= 0 && closeHit < static_cast<int>(m_submenuCards.size()))
            {
                const HWND targetHwnd = m_submenuCards[static_cast<size_t>(closeHit)].window
                    ? m_submenuCards[static_cast<size_t>(closeHit)].window->hwnd
                    : nullptr;
                if (targetHwnd)
                {
                    WindowTracker::CloseWindow(targetHwnd);
                    const int slotIdx = m_expandedSlotIndex;
                    auto eraseByHwnd = [&](std::vector<WindowEntry>& wins) {
                        wins.erase(
                            std::remove_if(wins.begin(), wins.end(),
                                [&](const WindowEntry& w) { return w.hwnd == targetHwnd; }),
                            wins.end());
                    };
                    if (slotIdx >= 0 && slotIdx < static_cast<int>(m_visibleSlots.size()))
                        eraseByHwnd(m_visibleSlots[static_cast<size_t>(slotIdx)].windows);
                    for (auto& s : m_allSlots)
                        eraseByHwnd(s.windows);

                    m_hoveredCloseSubIndex = -1;
                    if (slotIdx >= 0 && slotIdx < static_cast<int>(m_visibleSlots.size()) &&
                        SlotHasSubmenuItems(m_visibleSlots[static_cast<size_t>(slotIdx)]))
                    {
                        OpenSubmenuForSlot(slotIdx, localDip);
                    }
                    else
                    {
                        ClearSubmenu();
                    }
                    SyncDwmThumbnails();
                    Render();
                }
                return 0;
            }

            const int hitSub = HitTestSubmenu(localDip);
            if (hitSub >= 0 && hitSub < static_cast<int>(m_submenuCards.size()))
            {
                m_pressKind = PressKind::SubmenuCard;
                m_pressSubIndex = hitSub;
                m_pressSlotIndex = -1;
                m_pressScreenPt = screenPt;
                m_pressStartTick = GetTickCount64();
                m_selectedSubIndex = hitSub;
                SetCapture(m_hwnd);
                return 0;
            }

            const double dx = localDip.x - kCenterDip;
            const double dy = localDip.y - kCenterDip;
            const double dist = std::hypot(dx, dy);
            if (dist <= m_config.deadzoneDip)
            {
                m_pressKind = PressKind::CenterDeadzone;
                m_pressSlotIndex = -1;
                m_pressSubIndex = -1;
                m_pressScreenPt = screenPt;
                m_pressStartTick = GetTickCount64();
                SetCapture(m_hwnd);
                return 0;
            }

            const int hitSlot = CalculateSectorIndex(dx, dy, static_cast<int>(m_visibleSlots.size()), m_config.deadzoneDip);
            if (hitSlot >= 0)
            {
                m_pressKind = PressKind::RingSlot;
                m_pressSlotIndex = hitSlot;
                m_pressSubIndex = -1;
                m_pressScreenPt = screenPt;
                m_pressStartTick = GetTickCount64();
                m_selectedIndex = hitSlot;
                SetCapture(m_hwnd);
            }
            return 0;
        }

        case WM_LBUTTONUP:
        {
            if (!m_isLockedOpen)
                return 0;

            POINT clientPt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            POINT screenPt = clientPt;
            ClientToScreen(m_hwnd, &screenPt);
            const PointD localDip{ clientPt.x / m_dpiScale, clientPt.y / m_dpiScale };
            const double dist = std::hypot(localDip.x - kCenterDip, localDip.y - kCenterDip);

            if (m_reorderActive)
            {
                const int fromIdx = m_reorderFromIndex;
                const int toIdx = m_reorderCurrentIndex;
                m_reorderActive = false;
                m_reorderFromIndex = -1;
                m_reorderCurrentIndex = -1;
                m_pressKind = PressKind::None;
                m_pressSlotIndex = -1;
                if (GetCapture() == m_hwnd)
                    ReleaseCapture();

                if (fromIdx >= 0 && toIdx >= 0 && fromIdx != toIdx &&
                    static_cast<size_t>(fromIdx) < m_config.pinnedSlots.size() &&
                    static_cast<size_t>(toIdx) < m_config.pinnedSlots.size())
                {
                    ReorderVectorElement(m_config.pinnedSlots, static_cast<size_t>(fromIdx), static_cast<size_t>(toIdx));
                    if (m_callbacks.saveAndReloadConfig)
                        m_callbacks.saveAndReloadConfig(m_config);
                }
                Render();
                return 0;
            }

            const PressKind kind = m_pressKind;
            m_pressKind = PressKind::None;
            m_pressSlotIndex = -1;
            m_pressSubIndex = -1;
            if (GetCapture() == m_hwnd)
                ReleaseCapture();

            if (kind == PressKind::CenterDeadzone)
            {
                if (dist <= m_config.deadzoneDip)
                    HandleEscapeKey();
                return 0;
            }
            if (kind == PressKind::SubmenuCard)
            {
                const int hitSub = HitTestSubmenu(localDip);
                if (hitSub >= 0 && hitSub < static_cast<int>(m_submenuCards.size()))
                {
                    m_selectedSubIndex = hitSub;
                    CommitSelectionAndClose();
                }
                return 0;
            }
            if (kind == PressKind::RingSlot)
            {
                if (dist <= m_config.deadzoneDip)
                {
                    HandleEscapeKey();
                    return 0;
                }
                const int releaseSlot = CalculateSectorIndex(
                    localDip.x - kCenterDip, localDip.y - kCenterDip,
                    static_cast<int>(m_visibleSlots.size()), m_config.deadzoneDip);
                if (releaseSlot >= 0)
                {
                    m_selectedIndex = releaseSlot;
                    CommitSelectionAndClose();
                }
                return 0;
            }
            return 0;
        }

        case WM_RBUTTONUP:
        {
            POINT screenPt{};
            GetCursorPos(&screenPt);
            ShowContextMenuAt(screenPt);
            return 0;
        }

        case WM_APP + 50:
            if (m_callbacks.saveAndReloadConfig)
            {
                m_config = SwitcherConfig::LoadOrCreate();
                m_callbacks.saveAndReloadConfig(m_config);
            }
            return 0;

        case WM_CAPTURECHANGED:
            if (m_reorderActive || m_pressKind != PressKind::None)
                CancelPendingPressAndReorder(true);
            break;

        default:
            break;
        }
        return DefWindowProcW(m_hwnd, msg, wp, lp);
    }
} // namespace winrings
