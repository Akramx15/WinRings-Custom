#include "SettingsDialog.h"

#include <CommCtrl.h>
#include <commdlg.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <dxgi1_2.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <string>
#include <unordered_set>
#include <vector>

#include "../shared/ClockFormat.h"
#include "../shared/RadialMath.h"
#include "../shared/RingThemeEngine.h"
#include "../shared/WinRingsIdentity.h"

namespace winrings::tools
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        constexpr int kWinW = 1040;
        constexpr int kWinH = 760;
        constexpr int kSidebarW = 196;
        constexpr int kFooterH = 58;
        constexpr int kPreviewW = 356;
        constexpr int kPreviewBarH = 28;
        constexpr int kPageCount = 5;

        // Modern Windows 11 Dark Fluent palette
        constexpr COLORREF kColBg      = RGB(28, 28, 30);
        constexpr COLORREF kColSidebar = RGB(22, 22, 24);
        constexpr COLORREF kColCard    = RGB(37, 37, 40);
        constexpr COLORREF kColEdit    = RGB(46, 46, 52);
        constexpr COLORREF kColBorder  = RGB(54, 54, 60);
        constexpr COLORREF kColText    = RGB(244, 244, 246);
        constexpr COLORREF kColMuted   = RGB(161, 161, 170);
        constexpr COLORREF kColAccent  = RGB(0, 120, 212);

        enum CtrlId
        {
            ID_SIDEBAR = 100,
            ID_BTN_APPLY = 101,
            ID_BTN_SAVE = 102,
            ID_BTN_RESET = 103,
            ID_BTN_CANCEL = 104,
            ID_BTN_PREVIEW = 105,

            // Shared persistent live 1:1 preview & Page 0 Appearance
            ID_APPEARANCE_CTRL = 150,
            ID_PREVIEW_RING_CANVAS = 151,

            // Page 1: Triggers & Input
            ID_CB_MOUSE_TRIGGER = 200,
            ID_CB_SCROLL_MODE,
            ID_CHK_DOUBLE_TAP,
            ID_ED_TAP_MS,
            ID_ED_DTAP_MS,
            ID_CHK_HOTKEY,
            ID_CHK_MOD_ALT,
            ID_CHK_MOD_CTRL,
            ID_CHK_MOD_SHIFT,
            ID_CHK_MOD_WIN,
            ID_CB_HOTKEY_VK,
            ID_CHK_MENU_KEY,
            ID_CHK_WIN_TAB,
            ID_CHK_ALT_TAB,
            ID_CHK_WIN_KEY,
            ID_CHK_DIRECT_SWITCH,
            ID_CB_DIRECT_SWITCH_KEY,
            ID_CHK_TP_PHYS_CLICK,
            ID_CHK_TP_CORNER,
            ID_CB_TP_CORNER_POS,
            ID_ED_TP_CORNER_PCT,
            ID_CHK_SHAKE,
            ID_ED_SHAKE_REV,
            ID_ED_SHAKE_WIN,
            ID_ED_SHAKE_AMP,
            ID_CHK_SHAKE_FS,
            ID_CHK_HOT_CORNER,
            ID_CB_HC_POS,
            ID_CB_HC_ACT,
            ID_ED_HC_PRESS,
            ID_CHK_DRAG_SNAP,
            ID_CHK_DRAG_SNAP_CLOSED,
            ID_ED_DRAG_SNAP_HOLD,
            ID_ED_DRAG_SNAP_TEAROFF,

            // Page 2: Top Bar & Media Island
            ID_CHK_TOPBAR_ON = 400,
            ID_CHK_DOCK_TOP,
            ID_SLD_BAR_H,
            ID_ED_BAR_H,
            ID_CHK_KEEP_FONT,
            ID_CHK_CLOCK_1L,
            ID_ED_CLOCK_FMT,
            ID_CB_CLOCK_CASING,
            ID_LBL_CLOCK_PREVIEW,
            ID_CHK_HIDE_APPS,
            ID_CHK_HIDE_START,
            ID_CHK_HIDE_SEARCH,
            ID_CHK_HIDE_TASKVIEW,
            ID_CHK_HIDE_WIDGETS,
            ID_CB_BACKDROP,
            ID_CHK_ALL_MON,
            ID_CHK_FWA_ON,
            ID_CHK_FWA_PILL,
            ID_CHK_FWA_HK,
            ID_CHK_MEDIA_ON,
            ID_CB_MEDIA_POS,
            ID_CB_MEDIA_FS,
            ID_CHK_MEDIA_EXPAND,
            ID_CHK_MEDIA_VOL,
            ID_CHK_MEDIA_HIDE_PAUSED,
            ID_CHK_MEDIA_MARQUEE,
            ID_BTN_RESET_MEDIA_POS,

            // Page 3: Pins & Folders
            ID_LB_PINS = 500,
            ID_CB_PIN_KIND,
            ID_ED_PIN_LABEL,
            ID_ED_PIN_PATH,
            ID_BTN_BROWSE_FILE,
            ID_BTN_BROWSE_DIR,
            ID_ED_PIN_FOLDER_MAX,
            ID_BTN_PIN_ADD,
            ID_BTN_PIN_UPDATE,
            ID_BTN_PIN_REMOVE,
            ID_BTN_PIN_UP,
            ID_BTN_PIN_DOWN,

            // Page 4: Advanced
            ID_ED_RADIUS = 600,
            ID_ED_DEADZONE,
            ID_ED_ICONSIZE,
            ID_ED_THUMBSCALE,
            ID_ED_ACCENT,
            ID_CHK_NO_ANIM,
            ID_CB_WINVIEW,
            ID_CHK_FANOUT_ONE,
            ID_CHK_PTR_FOLLOW,
            ID_CHK_SHIFT_CENTER,
            ID_CHK_INCLUDE_UNPINNED,
            ID_ED_MAXSLOTS,
            ID_CB_SEL_INDICATOR,
            ID_CHK_NO_BLUR,
        };

        struct KeyEntry
        {
            const wchar_t* label;
            UINT vk;
        };

        constexpr KeyEntry kHotkeyChoices[] = {
            { L"Space", VK_SPACE },
            { L"Tab", VK_TAB },
            { L"Q", 'Q' },
            { L"W", 'W' },
            { L"E", 'E' },
            { L"R", 'R' },
            { L"A", 'A' },
            { L"S", 'S' },
            { L"D", 'D' },
            { L"F", 'F' },
            { L"Z", 'Z' },
            { L"X", 'X' },
            { L"G", 'G' },
            { L"` (~)", VK_OEM_3 },
        };

        constexpr const wchar_t* kPageTitles[kPageCount] = {
            L"Appearance",
            L"Triggers & Input",
            L"Top Bar & Media Island",
            L"Pins & Folders",
            L"Advanced"
        };

        constexpr const wchar_t* kPageSubtitles[kPageCount] = {
            L"Choose a visual style and adjust glass transparency & light/dark tone.",
            L"Configure mouse triggers, Win key, touchpad gestures, shake & hot corners.",
            L"Customize the minimal Windows 11 top bar, FullWorkArea, and Dynamic Island media pill.",
            L"Manage pinned apps, websites, folders, and radial collections.",
            L"Tune ring geometry, selection highlight, animations, and blur mode."
        };

        void EnableDarkAppTheme() noexcept
        {
            if (HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            {
                using SetPreferredAppModeFn = int (WINAPI*)(int);
                using FlushMenuThemesFn = void (WINAPI*)();
                if (auto setMode = reinterpret_cast<SetPreferredAppModeFn>(
                    GetProcAddress(ux, MAKEINTRESOURCEA(135))))
                    setMode(2); // AllowDark
                if (auto flush = reinterpret_cast<FlushMenuThemesFn>(
                    GetProcAddress(ux, MAKEINTRESOURCEA(136))))
                    flush();
                FreeLibrary(ux);
            }
        }

        void ApplyDarkDwmFrame(HWND hwnd) noexcept
        {
            const BOOL dark = TRUE;
            DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
            const int corner = 2; // DWMWCP_ROUND
            DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
            const COLORREF border = RGB(62, 62, 62);
            DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
        }

        D2D1_COLOR_F RgbColor(BYTE r, BYTE g, BYTE b, float a = 1.0f) noexcept
        {
            return D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, a);
        }

        std::wstring GetText(HWND hwnd)
        {
            const int len = GetWindowTextLengthW(hwnd);
            if (len <= 0)
                return {};
            std::wstring s(static_cast<size_t>(len) + 1, L'\0');
            GetWindowTextW(hwnd, s.data(), len + 1);
            s.resize(static_cast<size_t>(len));
            return s;
        }

        double ParseDoubleOr(const std::wstring& s, double fallback)
        {
            try { return std::stod(s); }
            catch (...) { return fallback; }
        }

        int ParseIntOr(const std::wstring& s, int fallback)
        {
            try { return std::stoi(s); }
            catch (...) { return fallback; }
        }

        std::wstring ExpandEnvW(const std::wstring& input)
        {
            if (input.empty())
                return {};
            const DWORD needed = ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);
            if (!needed)
                return input;
            std::wstring out(static_cast<size_t>(needed), L'\0');
            const DWORD actual = ExpandEnvironmentStringsW(input.c_str(), out.data(), needed);
            if (!actual)
                return input;
            if (!out.empty() && out.back() == L'\0')
                out.pop_back();
            return out;
        }

        std::wstring ToLowerW(std::wstring s)
        {
            for (wchar_t& ch : s)
                ch = static_cast<wchar_t>(towlower(ch));
            return s;
        }

        std::wstring FileNameNoExtLower(const std::wstring& path)
        {
            const size_t slash = path.find_last_of(L"\\/");
            std::wstring name = (slash != std::wstring::npos) ? path.substr(slash + 1) : path;
            const size_t dot = name.find_last_of(L'.');
            if (dot != std::wstring::npos && dot > 0)
                name.resize(dot);
            return ToLowerW(std::move(name));
        }

        std::wstring InferSlotLabel(SlotKind kind, const std::wstring& pathOrUrl)
        {
            if (kind == SlotKind::RecycleBin)
                return L"Recycle Bin";
            const size_t slash = pathOrUrl.find_last_of(L"\\/");
            std::wstring name = (slash != std::wstring::npos) ? pathOrUrl.substr(slash + 1) : pathOrUrl;
            if (name.empty())
                return pathOrUrl.empty() ? ToString(kind) : pathOrUrl;
            const size_t dot = name.find_last_of(L'.');
            if (dot != std::wstring::npos && dot > 0 && kind != SlotKind::Folder && kind != SlotKind::Website)
                name.resize(dot);
            return name;
        }

        std::wstring ResolveSiblingHost()
        {
            std::wstring path(MAX_PATH, L'\0');
            const DWORD len = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (!len)
                return {};
            path.resize(len);
            const size_t slash = path.find_last_of(L"\\/");
            return (slash != std::wstring::npos) ? (path.substr(0, slash + 1) + WINRINGS_HOST_FILENAME_W) : L"";
        }

        void TriggerHostCommand(const wchar_t* arg)
        {
            const std::wstring host = ResolveSiblingHost();
            if (host.empty() || GetFileAttributesW(host.c_str()) == INVALID_FILE_ATTRIBUTES)
                return;
            std::wstring cmd = L"\"" + host + L"\" " + arg;
            std::vector<wchar_t> buf(cmd.begin(), cmd.end());
            buf.push_back(L'\0');
            STARTUPINFOW si{ sizeof(si) };
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(host.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
            {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }

        struct PreviewSlotItem
        {
            SlotKind kind = SlotKind::App;
            std::wstring label;
            bool isRunning = false;
            bool isFrontmost = false;
            int windowCount = 0;
            ComPtr<ID2D1Bitmap1> iconBmp;
        };

        struct PreviewRunningApp
        {
            std::wstring exeLower;
            std::wstring label;
            HICON hIcon = nullptr;
            bool isFrontmost = false;
            int count = 1;
        };

        class SettingsWindow
        {
        public:
            bool Run(HINSTANCE inst)
            {
                m_instance = inst;
                m_config = SwitcherConfig::LoadOrCreate();
                EnableDarkAppTheme();

                CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(m_wicFactory.ReleaseAndGetAddressOf()));

                D2D1_FACTORY_OPTIONS opts{};
                D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                    &opts, reinterpret_cast<void**>(m_d2dFactory.ReleaseAndGetAddressOf()));
                DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                    reinterpret_cast<IUnknown**>(m_dwriteFactory.ReleaseAndGetAddressOf()));

                WNDCLASSEXW wc{ sizeof(wc) };
                wc.lpfnWndProc = WndProc;
                wc.hInstance = inst;
                wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
                wc.lpszClassName = L"WinRings.Settings.v3";
                RegisterClassExW(&wc);

                auto regPanel = [&](const wchar_t* cls, WNDPROC proc, HCURSOR cur) {
                    WNDCLASSEXW pw{ sizeof(pw) };
                    pw.style = CS_HREDRAW | CS_VREDRAW;
                    pw.lpfnWndProc = proc;
                    pw.hInstance = inst;
                    pw.hCursor = cur;
                    pw.lpszClassName = cls;
                    RegisterClassExW(&pw);
                };
                regPanel(L"WinRings.SidebarPanel.v1", SidebarProc, LoadCursorW(nullptr, IDC_HAND));
                regPanel(L"WinRings.AppearancePanel.v1", AppearanceProc, LoadCursorW(nullptr, IDC_ARROW));
                regPanel(L"WinRings.RingPreviewCanvas.v3", PreviewCanvasProc, LoadCursorW(nullptr, IDC_ARROW));

                RECT wa{};
                SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
                m_hwnd = CreateWindowExW(
                    0, wc.lpszClassName, L"WinRings — Settings",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                    wa.left + (wa.right - wa.left - kWinW) / 2,
                    wa.top + (wa.bottom - wa.top - kWinH) / 2,
                    kWinW, kWinH,
                    nullptr, nullptr, inst, this);
                if (!m_hwnd)
                    return false;

                // Capture live desktop behind the preview panel before showing the window.
                CaptureDesktopBehindPreview();
                RebuildPreviewSlots();

                ShowWindow(m_hwnd, SW_SHOWNORMAL);

                MSG msg{};
                while (GetMessageW(&msg, nullptr, 0, 0) > 0)
                {
                    if (!IsDialogMessageW(m_hwnd, &msg))
                    {
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                }
                return true;
            }

        private:
            struct DragTarget
            {
                enum Kind { None, GlassSlider, ToneSlider } kind = None;
            };

            static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
            {
                auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (msg == WM_NCCREATE)
                {
                    self = static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
                    self->m_hwnd = hwnd;
                }
                if (self)
                    return self->HandleMsg(msg, wp, lp);
                return DefWindowProcW(hwnd, msg, wp, lp);
            }

            static LRESULT CALLBACK SidebarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
            {
                auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (msg == WM_NCCREATE)
                {
                    self = static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
                }
                if (!self)
                    return DefWindowProcW(hwnd, msg, wp, lp);

                switch (msg)
                {
                case WM_ERASEBKGND:
                    return TRUE;
                case WM_PAINT:
                {
                    PAINTSTRUCT ps{};
                    HDC hdc = BeginPaint(hwnd, &ps);
                    self->PaintSidebar(hwnd, hdc);
                    EndPaint(hwnd, &ps);
                    return 0;
                }
                case WM_PRINTCLIENT:
                    self->PaintSidebar(hwnd, reinterpret_cast<HDC>(wp));
                    return 0;
                case WM_MOUSEMOVE:
                {
                    const int idx = self->SidebarHitTest(GET_Y_LPARAM(lp));
                    if (idx != self->m_hoverSidebar)
                    {
                        self->m_hoverSidebar = idx;
                        InvalidateRect(hwnd, nullptr, FALSE);
                        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                        TrackMouseEvent(&tme);
                    }
                    return 0;
                }
                case WM_MOUSELEAVE:
                    if (self->m_hoverSidebar != -1)
                    {
                        self->m_hoverSidebar = -1;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                case WM_LBUTTONUP:
                {
                    const int idx = self->SidebarHitTest(GET_Y_LPARAM(lp));
                    if (idx >= 0 && idx < kPageCount)
                    {
                        self->ShowPage(idx);
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                }
                }
                return DefWindowProcW(hwnd, msg, wp, lp);
            }

            static LRESULT CALLBACK AppearanceProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
            {
                auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (msg == WM_NCCREATE)
                {
                    self = static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
                }
                if (!self)
                    return DefWindowProcW(hwnd, msg, wp, lp);

                switch (msg)
                {
                case WM_ERASEBKGND:
                    return TRUE;
                case WM_PAINT:
                {
                    PAINTSTRUCT ps{};
                    HDC hdc = BeginPaint(hwnd, &ps);
                    self->PaintAppearancePanel(hwnd, hdc);
                    EndPaint(hwnd, &ps);
                    return 0;
                }
                case WM_PRINTCLIENT:
                    self->PaintAppearancePanel(hwnd, reinterpret_cast<HDC>(wp));
                    return 0;
                case WM_MOUSEMOVE:
                {
                    const int x = GET_X_LPARAM(lp);
                    const int y = GET_Y_LPARAM(lp);
                    if (self->m_dragTarget.kind != DragTarget::None)
                    {
                        self->UpdateSliderFromX(hwnd, x);
                    }
                    else
                    {
                        const int hoverCard = self->AppearanceCardHitTest(hwnd, x, y);
                        if (hoverCard != self->m_hoverCard)
                        {
                            self->m_hoverCard = hoverCard;
                            InvalidateRect(hwnd, nullptr, FALSE);
                        }
                        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                        TrackMouseEvent(&tme);
                    }
                    return 0;
                }
                case WM_MOUSELEAVE:
                    if (self->m_hoverCard != -1)
                    {
                        self->m_hoverCard = -1;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                case WM_LBUTTONDOWN:
                    self->HandleAppearanceDown(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                    return 0;
                case WM_LBUTTONUP:
                    if (self->m_dragTarget.kind != DragTarget::None)
                    {
                        self->m_dragTarget.kind = DragTarget::None;
                        ReleaseCapture();
                    }
                    return 0;
                case WM_CAPTURECHANGED:
                    self->m_dragTarget.kind = DragTarget::None;
                    return 0;
                }
                return DefWindowProcW(hwnd, msg, wp, lp);
            }

            static LRESULT CALLBACK PreviewCanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
            {
                auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (msg == WM_NCCREATE)
                {
                    self = static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
                }
                if (!self)
                    return DefWindowProcW(hwnd, msg, wp, lp);

                switch (msg)
                {
                case WM_ERASEBKGND:
                    return TRUE;
                case WM_PAINT:
                {
                    PAINTSTRUCT ps{};
                    BeginPaint(hwnd, &ps);
                    self->PaintRingPreview(hwnd);
                    EndPaint(hwnd, &ps);
                    return 0;
                }
                case WM_MOUSEMOVE:
                {
                    const int slot = self->PreviewSlotHitTest(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                    if (slot != self->m_previewSelectedSlot)
                    {
                        self->m_previewSelectedSlot = slot;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                    TrackMouseEvent(&tme);
                    return 0;
                }
                case WM_MOUSELEAVE:
                    if (self->m_previewSelectedSlot != -1)
                    {
                        self->m_previewSelectedSlot = -1;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                case WM_LBUTTONDOWN:
                {
                    const int x = GET_X_LPARAM(lp);
                    const int y = GET_Y_LPARAM(lp);
                    RECT rc{};
                    GetClientRect(hwnd, &rc);
                    const int h = rc.bottom - rc.top;
                    if (y >= h - kPreviewBarH)
                    {
                        const bool wantDesktop = (x >= 94 && x <= 188);
                        const bool wantSplit = (x >= 8 && x < 92);
                        if (wantDesktop)
                        {
                            self->m_useDesktopScene = true;
                            if (self->m_desktopPixels.empty())
                                self->CaptureDesktopBehindPreview();
                            InvalidateRect(hwnd, nullptr, FALSE);
                        }
                        else if (wantSplit)
                        {
                            self->m_useDesktopScene = false;
                            InvalidateRect(hwnd, nullptr, FALSE);
                        }
                    }
                    return 0;
                }
                }
                return DefWindowProcW(hwnd, msg, wp, lp);
            }

            int SidebarHitTest(int y) const noexcept
            {
                const int rel = y - 78;
                if (rel < 0)
                    return -1;
                const int idx = rel / 42;
                const int inItem = rel % 42;
                if (idx >= 0 && idx < kPageCount && inItem < 36)
                    return idx;
                return -1;
            }

            int AppearanceCardHitTest(HWND hwnd, int x, int y) const noexcept
            {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                const int maxX = (std::max)(40, static_cast<int>(rc.right - rc.left) - 8);
                if (x < 10 || x > maxX)
                    return -1;
                const int rel = y - 28;
                if (rel < 0)
                    return -1;
                const int idx = rel / 62;
                const int inCard = rel % 62;
                if (idx >= 0 && idx < 3 && inCard < 54)
                    return idx;
                return -1;
            }

            void UpdateSliderFromX(HWND hwnd, int x)
            {
                constexpr float kLeft = 24.0f;
                float rightEdge = 380.0f;
                RECT rc{};
                if (hwnd && GetClientRect(hwnd, &rc) && (rc.right - rc.left) > 48)
                    rightEdge = static_cast<float>(rc.right - rc.left) - 24.0f;

                const float pct = (std::clamp)((static_cast<float>(x) - kLeft) / (rightEdge - kLeft), 0.0f, 1.0f);
                const int val = static_cast<int>(std::lround(pct * 100.0f));
                if (m_dragTarget.kind == DragTarget::GlassSlider)
                    m_config.glassLevel = val;
                else if (m_dragTarget.kind == DragTarget::ToneSlider)
                    m_config.lightDarkMode = val;

                if (m_appearanceCtrl)
                    InvalidateRect(m_appearanceCtrl, nullptr, FALSE);
                if (m_ringCanvas)
                    InvalidateRect(m_ringCanvas, nullptr, FALSE);
            }

            void HandleAppearanceDown(HWND hwnd, int x, int y)
            {
                const int card = AppearanceCardHitTest(hwnd, x, y);
                if (card >= 0 && card <= 2)
                {
                    const RingTheme chosen = static_cast<RingTheme>(card);
                    if (chosen != m_config.theme)
                    {
                        m_config.theme = chosen;
                        if (m_config.theme == RingTheme::A4S)
                            m_config.glassLevel = 50;
                        else if (m_config.theme == RingTheme::DarkWood)
                            m_config.glassLevel = 86;
                        else if (m_config.theme == RingTheme::Blue)
                            m_config.glassLevel = 45;
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                    if (m_ringCanvas)
                        InvalidateRect(m_ringCanvas, nullptr, FALSE);
                    return;
                }

                RECT rc{};
                GetClientRect(hwnd, &rc);
                const int maxX = (std::max)(40, static_cast<int>(rc.right - rc.left) - 8);
                if (x >= 10 && x <= maxX)
                {
                    if (y >= 254 && y <= 296)
                    {
                        m_dragTarget.kind = DragTarget::GlassSlider;
                        SetCapture(hwnd);
                        UpdateSliderFromX(hwnd, x);
                        return;
                    }
                    if (y >= 344 && y <= 386)
                    {
                        m_dragTarget.kind = DragTarget::ToneSlider;
                        SetCapture(hwnd);
                        UpdateSliderFromX(hwnd, x);
                        return;
                    }
                }
            }

            int PreviewSlotHitTest(HWND canvas, int x, int y) const noexcept
            {
                RECT rc{};
                GetClientRect(canvas, &rc);
                const float w = static_cast<float>(rc.right - rc.left);
                const float h = static_cast<float>(rc.bottom - rc.top);
                if (y >= static_cast<int>(h) - kPreviewBarH || m_previewSlots.empty())
                    return -1;

                const int slotCount = static_cast<int>(m_previewSlots.size());
                const RingGeometry geom = ComputeRingGeometry(
                    slotCount, m_config.ringRadiusDip, m_config.iconSizeDip, m_config.deadzoneDip);
                const float usableH = (std::max)(80.0f, h - static_cast<float>(kPreviewBarH) - 12.0f);
                const float fitScale = (std::min)(1.0f,
                    (std::min)(w - 20.0f, usableH) * 0.46f / static_cast<float>((std::max)(1.0, geom.outerRadius)));

                const float cx = w * 0.5f;
                const float cy = (h - static_cast<float>(kPreviewBarH)) * 0.5f;
                const float dxDip = (static_cast<float>(x) - cx) / fitScale;
                const float dyDip = (static_cast<float>(y) - cy) / fitScale;
                const double distDip = std::hypot(dxDip, dyDip);
                if (distDip <= geom.holeRadius || distDip > geom.outerRadius + 12.0)
                    return -1;

                return CalculateSectorIndex(dxDip, dyDip, slotCount, m_config.deadzoneDip);
            }

            void EnsureTextFormats()
            {
                if (!m_dwriteFactory || m_dwTitleFmt)
                    return;
                m_dwriteFactory->CreateTextFormat(L"Segoe UI Variable Display", nullptr,
                    DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    16.0f, L"en-us", m_dwHeadingFmt.ReleaseAndGetAddressOf());
                m_dwriteFactory->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
                    DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    13.0f, L"en-us", m_dwTitleFmt.ReleaseAndGetAddressOf());
                m_dwriteFactory->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
                    DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    11.5f, L"en-us", m_dwSubFmt.ReleaseAndGetAddressOf());
                m_dwriteFactory->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
                    DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    13.0f, L"en-us", m_dwCenterFmt.ReleaseAndGetAddressOf());
                if (m_dwCenterFmt)
                {
                    m_dwCenterFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    m_dwCenterFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
                    ComPtr<IDWriteInlineObject> ellipsis;
                    m_dwriteFactory->CreateEllipsisTrimmingSign(m_dwCenterFmt.Get(), ellipsis.GetAddressOf());
                    m_dwCenterFmt->SetTrimming(&trim, ellipsis.Get());
                    m_dwCenterFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                }
                m_dwriteFactory->CreateTextFormat(L"Segoe UI Variable Display", nullptr,
                    DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    15.0f, L"en-us", m_dwGlyphFmt.ReleaseAndGetAddressOf());
                if (m_dwGlyphFmt)
                {
                    m_dwGlyphFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    m_dwGlyphFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                }
                m_dwriteFactory->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
                    DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    11.0f, L"en-us", m_dwSmallFmt.ReleaseAndGetAddressOf());
                if (m_dwSmallFmt)
                {
                    m_dwSmallFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    m_dwSmallFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                }
            }

            void CaptureDesktopBehindPreview()
            {
                if (!m_ringCanvas)
                    return;
                RECT rc{};
                GetClientRect(m_ringCanvas, &rc);
                const int w = (std::max)(1, static_cast<int>(rc.right - rc.left));
                const int h = (std::max)(1, static_cast<int>(rc.bottom - rc.top));
                POINT topLeft{ 0, 0 };
                MapWindowPoints(m_ringCanvas, nullptr, &topLeft, 1);

                HDC screenDc = GetDC(nullptr);
                if (!screenDc)
                    return;
                HDC memDc = CreateCompatibleDC(screenDc);
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = w;
                bi.bmiHeader.biHeight = -h;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                void* bits = nullptr;
                HBITMAP bmp = CreateDIBSection(memDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
                if (memDc && bmp && bits)
                {
                    HGDIOBJ old = SelectObject(memDc, bmp);
                    BitBlt(memDc, 0, 0, w, h, screenDc, topLeft.x, topLeft.y, SRCCOPY | CAPTUREBLT);
                    SelectObject(memDc, old);

                    m_desktopPixels.resize(static_cast<size_t>(w) * h * 4u);
                    memcpy(m_desktopPixels.data(), bits, m_desktopPixels.size());
                    // Ensure alpha channel is 255
                    for (size_t i = 3; i < m_desktopPixels.size(); i += 4)
                        m_desktopPixels[i] = 255;
                    m_desktopW = w;
                    m_desktopH = h;
                    m_desktopSceneBmp.Reset();
                }
                if (bmp) DeleteObject(bmp);
                if (memDc) DeleteDC(memDc);
                ReleaseDC(nullptr, screenDc);
            }

            ComPtr<ID2D1Bitmap1> ConvertHIconToPreviewBitmap(HICON hIcon)
            {
                if (!hIcon || !m_wicFactory || !m_previewCtx)
                    return nullptr;
                ComPtr<IWICBitmap> wicBmp;
                if (FAILED(m_wicFactory->CreateBitmapFromHICON(hIcon, wicBmp.GetAddressOf())) || !wicBmp)
                    return nullptr;
                ComPtr<IWICFormatConverter> conv;
                if (FAILED(m_wicFactory->CreateFormatConverter(conv.GetAddressOf())) || !conv)
                    return nullptr;
                if (FAILED(conv->Initialize(wicBmp.Get(), GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
                    return nullptr;
                ComPtr<ID2D1Bitmap1> bmp;
                m_previewCtx->CreateBitmapFromWicBitmap(conv.Get(), nullptr, bmp.GetAddressOf());
                return bmp;
            }

            ComPtr<ID2D1Bitmap1> LoadPreviewIconForPath(const std::wstring& path)
            {
                if (path.empty())
                    return nullptr;
                SHFILEINFOW shfi{};
                if (SHGetFileInfoW(path.c_str(), 0, &shfi, sizeof(shfi), SHGFI_ICON | SHGFI_LARGEICON) && shfi.hIcon)
                {
                    auto bmp = ConvertHIconToPreviewBitmap(shfi.hIcon);
                    DestroyIcon(shfi.hIcon);
                    if (bmp)
                        return bmp;
                }
                return nullptr;
            }

            ComPtr<ID2D1Bitmap1> LoadPreviewStockIcon(int stockId)
            {
                SHSTOCKICONINFO sii{ sizeof(sii) };
                if (SUCCEEDED(SHGetStockIconInfo(static_cast<SHSTOCKICONID>(stockId),
                    SHGSI_ICON | SHGSI_LARGEICON, &sii)) && sii.hIcon)
                {
                    auto bmp = ConvertHIconToPreviewBitmap(sii.hIcon);
                    DestroyIcon(sii.hIcon);
                    return bmp;
                }
                return nullptr;
            }

            ComPtr<ID2D1Bitmap1> LoadPreviewImageFile(const std::wstring& path)
            {
                if (!m_wicFactory || !m_previewCtx || path.empty())
                    return nullptr;
                ComPtr<IWICBitmapDecoder> dec;
                if (FAILED(m_wicFactory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                    WICDecodeMetadataCacheOnLoad, dec.GetAddressOf())) || !dec)
                    return nullptr;
                ComPtr<IWICBitmapFrameDecode> frame;
                if (FAILED(dec->GetFrame(0, frame.GetAddressOf())) || !frame)
                    return nullptr;
                ComPtr<IWICFormatConverter> conv;
                if (FAILED(m_wicFactory->CreateFormatConverter(conv.GetAddressOf())) || !conv)
                    return nullptr;
                if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
                    return nullptr;
                ComPtr<ID2D1Bitmap1> bmp;
                m_previewCtx->CreateBitmapFromWicBitmap(conv.Get(), nullptr, bmp.GetAddressOf());
                return bmp;
            }

            void RebuildPreviewSlots()
            {
                m_previewSlots.clear();
                EnsurePreviewGraphicsDevices();

                // Enumerate real open windows for live running dots + optional unpinned apps
                struct EnumCtx
                {
                    HWND selfHwnd = nullptr;
                    HWND fgHwnd = nullptr;
                    std::vector<PreviewRunningApp> apps;
                } ctx{ m_hwnd, GetForegroundWindow(), {} };

                EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
                    auto* c = reinterpret_cast<EnumCtx*>(lp);
                    if (!hwnd || hwnd == c->selfHwnd || !IsWindowVisible(hwnd))
                        return TRUE;
                    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
                    if ((ex & WS_EX_TOOLWINDOW) != 0)
                        return TRUE;
                    if ((ex & WS_EX_APPWINDOW) == 0 && GetWindow(hwnd, GW_OWNER) != nullptr)
                        return TRUE;
                    DWORD cloaked = 0;
                    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0)
                        return TRUE;
                    if (GetWindowTextLengthW(hwnd) <= 0)
                        return TRUE;

                    wchar_t cls[64]{};
                    if (GetClassNameW(hwnd, cls, _countof(cls)) > 0)
                    {
                        if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
                            wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0)
                            return TRUE;
                    }

                    DWORD pid = 0;
                    GetWindowThreadProcessId(hwnd, &pid);
                    std::wstring exePath;
                    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                    if (proc)
                    {
                        wchar_t buf[MAX_PATH]{};
                        DWORD sz = _countof(buf);
                        if (QueryFullProcessImageNameW(proc, 0, buf, &sz))
                            exePath.assign(buf, sz);
                        CloseHandle(proc);
                    }
                    if (exePath.empty())
                        return TRUE;

                    const std::wstring exeLow = ToLowerW(exePath);
                    for (auto& existing : c->apps)
                    {
                        if (existing.exeLower == exeLow)
                        {
                            existing.count++;
                            if (hwnd == c->fgHwnd)
                                existing.isFrontmost = true;
                            return TRUE;
                        }
                    }

                    PreviewRunningApp app;
                    app.exeLower = exeLow;
                    app.label = InferSlotLabel(SlotKind::App, exePath);
                    app.isFrontmost = (hwnd == c->fgHwnd);
                    c->apps.push_back(std::move(app));
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&ctx));

                const int cap = (std::clamp)(m_config.maxSlots, 4, 24);
                std::unordered_set<std::wstring> consumedExes;

                for (const auto& cfg : m_config.pinnedSlots)
                {
                    if (static_cast<int>(m_previewSlots.size()) >= cap)
                        break;

                    const std::wstring expanded = ExpandEnvW(cfg.pathOrUrl);
                    PreviewSlotItem item;
                    item.kind = cfg.kind;
                    item.label = cfg.label.empty() ? InferSlotLabel(cfg.kind, expanded) : cfg.label;

                    if (cfg.kind == SlotKind::App)
                    {
                        const std::wstring cfgExeLow = ToLowerW(expanded);
                        const std::wstring cfgStemLow = FileNameNoExtLower(expanded);
                        for (const auto& ra : ctx.apps)
                        {
                            if (ra.exeLower == cfgExeLow || FileNameNoExtLower(ra.exeLower) == cfgStemLow)
                            {
                                item.isRunning = true;
                                item.isFrontmost = ra.isFrontmost;
                                item.windowCount = ra.count;
                                consumedExes.insert(ra.exeLower);
                                break;
                            }
                        }
                        item.iconBmp = LoadPreviewIconForPath(expanded);
                    }
                    else if (cfg.kind == SlotKind::File || cfg.kind == SlotKind::Folder || cfg.kind == SlotKind::Script)
                    {
                        item.iconBmp = LoadPreviewIconForPath(expanded);
                        if (!item.iconBmp && cfg.kind == SlotKind::Folder)
                            item.iconBmp = LoadPreviewStockIcon(SIID_FOLDER);
                    }
                    else if (cfg.kind == SlotKind::Website && !cfg.iconCacheFile.empty())
                    {
                        const std::wstring full = (cfg.iconCacheFile.find(L':') != std::wstring::npos)
                            ? cfg.iconCacheFile
                            : (SwitcherConfig::AppDataDirectory() + L"\\" + cfg.iconCacheFile);
                        item.iconBmp = LoadPreviewImageFile(full);
                    }
                    else if (cfg.kind == SlotKind::Collection)
                    {
                        item.iconBmp = LoadPreviewStockIcon(SIID_FOLDER);
                    }
                    else if (cfg.kind == SlotKind::RecycleBin)
                    {
                        SHQUERYRBINFO rb{ sizeof(rb) };
                        const bool full = SUCCEEDED(SHQueryRecycleBinW(nullptr, &rb)) && rb.i64NumItems > 0;
                        item.iconBmp = LoadPreviewStockIcon(full ? SIID_RECYCLERFULL : SIID_RECYCLER);
                    }

                    m_previewSlots.push_back(std::move(item));
                }

                if (m_config.includeUnpinnedRunningApps)
                {
                    for (const auto& ra : ctx.apps)
                    {
                        if (static_cast<int>(m_previewSlots.size()) >= cap)
                            break;
                        if (consumedExes.count(ra.exeLower))
                            continue;
                        PreviewSlotItem item;
                        item.kind = SlotKind::App;
                        item.label = ra.label;
                        item.isRunning = true;
                        item.isFrontmost = ra.isFrontmost;
                        item.windowCount = ra.count;
                        item.iconBmp = LoadPreviewIconForPath(ra.exeLower);
                        m_previewSlots.push_back(std::move(item));
                    }
                }
            }

            template <typename DrawFn>
            void RenderPanelToDc(HWND panel, HDC targetDc, DrawFn&& drawFn)
            {
                if (!m_d2dFactory || !panel || !targetDc)
                    return;
                EnsureTextFormats();

                RECT rc{};
                GetClientRect(panel, &rc);
                const int pw = (std::max)(1, static_cast<int>(rc.right - rc.left));
                const int ph = (std::max)(1, static_cast<int>(rc.bottom - rc.top));

                if (!m_dcRt)
                {
                    const D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties(
                        D2D1_RENDER_TARGET_TYPE_DEFAULT,
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                        96.0f, 96.0f);
                    m_d2dFactory->CreateDCRenderTarget(&rtp, m_dcRt.ReleaseAndGetAddressOf());
                }
                if (!m_dcRt)
                    return;

                HDC memDc = CreateCompatibleDC(targetDc);
                if (!memDc)
                    return;
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = pw;
                bi.bmiHeader.biHeight = -ph;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                void* bits = nullptr;
                HBITMAP bmp = CreateDIBSection(memDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
                if (bmp && bits)
                {
                    HGDIOBJ oldBmp = SelectObject(memDc, bmp);
                    if (SUCCEEDED(m_dcRt->BindDC(memDc, &rc)))
                    {
                        m_dcRt->BeginDraw();
                        drawFn(m_dcRt.Get(), static_cast<float>(pw), static_cast<float>(ph));
                        if (m_dcRt->EndDraw() == D2DERR_RECREATE_TARGET)
                            m_dcRt.Reset();
                    }
                    BitBlt(targetDc, 0, 0, pw, ph, memDc, 0, 0, SRCCOPY);
                    SelectObject(memDc, oldBmp);
                }
                if (bmp)
                    DeleteObject(bmp);
                DeleteDC(memDc);
            }

            void PaintSidebar(HWND panel, HDC hdc)
            {
                RenderPanelToDc(panel, hdc, [&](ID2D1RenderTarget* rt, float w, float h) {
                    rt->Clear(RgbColor(22, 22, 24));

                    ComPtr<ID2D1SolidColorBrush> textBr, mutedBr, selBg, hoverBg, accentBr, divBr;
                    rt->CreateSolidColorBrush(RgbColor(244, 244, 246), textBr.GetAddressOf());
                    rt->CreateSolidColorBrush(RgbColor(152, 152, 162), mutedBr.GetAddressOf());
                    rt->CreateSolidColorBrush(RgbColor(42, 42, 47), selBg.GetAddressOf());
                    rt->CreateSolidColorBrush(RgbColor(34, 34, 38), hoverBg.GetAddressOf());
                    rt->CreateSolidColorBrush(RgbColor(0, 120, 212), accentBr.GetAddressOf());
                    rt->CreateSolidColorBrush(RgbColor(43, 43, 47), divBr.GetAddressOf());

                    if (divBr)
                        rt->DrawLine(D2D1::Point2F(w - 0.5f, 0.0f), D2D1::Point2F(w - 0.5f, h), divBr.Get(), 1.0f);

                    if (m_dwHeadingFmt && textBr)
                        rt->DrawTextW(L"WinRings", 8, m_dwHeadingFmt.Get(), D2D1::RectF(18.0f, 18.0f, w - 12.0f, 44.0f), textBr.Get());
                    if (m_dwSubFmt && mutedBr)
                        rt->DrawTextW(L"A4S Visual Suite", 16, m_dwSubFmt.Get(), D2D1::RectF(18.0f, 42.0f, w - 12.0f, 62.0f), mutedBr.Get());

                    for (int i = 0; i < kPageCount; ++i)
                    {
                        const float top = 78.0f + i * 42.0f;
                        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
                            D2D1::RectF(10.0f, top, w - 11.0f, top + 36.0f), 6.0f, 6.0f);
                        const bool active = (i == m_activeTab);
                        const bool hover = (i == m_hoverSidebar);
                        if (active && selBg)
                            rt->FillRoundedRectangle(rr, selBg.Get());
                        else if (hover && hoverBg)
                            rt->FillRoundedRectangle(rr, hoverBg.Get());

                        if (active && accentBr)
                        {
                            const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(
                                D2D1::RectF(11.0f, top + 9.0f, 14.2f, top + 27.0f), 1.6f, 1.6f);
                            rt->FillRoundedRectangle(pill, accentBr.Get());
                        }

                        ID2D1SolidColorBrush* lblBr = active ? textBr.Get() : (hover ? textBr.Get() : mutedBr.Get());
                        if (m_dwTitleFmt && lblBr)
                        {
                            rt->DrawTextW(kPageTitles[i], static_cast<UINT32>(wcslen(kPageTitles[i])),
                                m_dwTitleFmt.Get(), D2D1::RectF(24.0f, top + 8.0f, w - 16.0f, top + 30.0f), lblBr);
                        }
                    }
                });
            }

            void PaintAppearancePanel(HWND panel, HDC hdc)
            {
                RenderPanelToDc(panel, hdc, [&](ID2D1RenderTarget* rt, float w, float) {
                    rt->Clear(RgbColor(28, 28, 30));

                    ComPtr<ID2D1SolidColorBrush> titleBr, subBr, cardBg, cardHover, cardSel, cardBorder, accentBr, trackBg, whiteBr, swatchDark, swatchBlue;
                    rt->CreateSolidColorBrush(RgbColor(244, 244, 246), titleBr.GetAddressOf());
                    rt->CreateSolidColorBrush(RgbColor(161, 161, 170), subBr.GetAddressOf());
                    rt->CreateSolidColorBrush(RgbColor(37, 37, 40), cardBg.GetAddressOf());
                    rt->CreateSolidColorBrush(RgbColor(45, 45, 50), cardHover.GetAddressOf());
                    rt->CreateSolidColorBrush(RgbColor(32, 46, 62), cardSel.GetAddressOf());
                    rt->CreateSolidColorBrush(RgbColor(62, 62, 68), cardBorder.GetAddressOf());
                    rt->CreateSolidColorBrush(RgbColor(0, 120, 212), accentBr.GetAddressOf());
                    rt->CreateSolidColorBrush(RgbColor(68, 68, 76), trackBg.GetAddressOf());
                    rt->CreateSolidColorBrush(RgbColor(255, 255, 255), whiteBr.GetAddressOf());
                    rt->CreateSolidColorBrush(RgbColor(32, 32, 32), swatchDark.GetAddressOf());
                    rt->CreateSolidColorBrush(RgbColor(26, 56, 108), swatchBlue.GetAddressOf());

                    if (m_dwTitleFmt && titleBr)
                        rt->DrawTextW(L"Visual Style Preset", 19, m_dwTitleFmt.Get(), D2D1::RectF(10.0f, 2.0f, w, 24.0f), titleBr.Get());

                    struct PresetInfo { const wchar_t* name; const wchar_t* desc; const wchar_t* tag; };
                    constexpr PresetInfo kPresets[3] = {
                        { L"A4S Menu", L"Live wallpaper-reactive frosted acrylic", L"Default" },
                        { L"Dark Wood", L"Previous matte dark #202020 surface", L"" },
                        { L"Blue", L"Classic #3186FF sapphire glass look", L"" }
                    };

                    for (int i = 0; i < 3; ++i)
                    {
                        const float top = 28.0f + i * 62.0f;
                        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(10.0f, top, w - 8.0f, top + 54.0f), 8.0f, 8.0f);
                        const bool active = (static_cast<int>(m_config.theme) == i);
                        const bool hover = (m_hoverCard == i);

                        ID2D1SolidColorBrush* fill = active ? cardSel.Get() : (hover ? cardHover.Get() : cardBg.Get());
                        ID2D1SolidColorBrush* border = active ? accentBr.Get() : cardBorder.Get();
                        if (fill) rt->FillRoundedRectangle(rr, fill);
                        if (border) rt->DrawRoundedRectangle(rr, border, active ? 1.5f : 1.0f);

                        const D2D1_POINT_2F swC{ 37.0f, top + 27.0f };
                        if (i == 0)
                        {
                            rt->FillEllipse(D2D1::Ellipse(swC, 14.0f, 14.0f), swatchBlue.Get());
                            if (accentBr) rt->FillEllipse(D2D1::Ellipse(swC, 9.0f, 9.0f), accentBr.Get());
                            ComPtr<ID2D1SolidColorBrush> frost;
                            rt->CreateSolidColorBrush(RgbColor(32, 32, 32, 0.56f), frost.GetAddressOf());
                            if (frost) rt->FillEllipse(D2D1::Ellipse(swC, 14.0f, 14.0f), frost.Get());
                            rt->DrawEllipse(D2D1::Ellipse(swC, 14.0f, 14.0f), cardBorder.Get(), 1.0f);
                        }
                        else if (i == 1)
                        {
                            rt->FillEllipse(D2D1::Ellipse(swC, 14.0f, 14.0f), swatchDark.Get());
                            rt->DrawEllipse(D2D1::Ellipse(swC, 14.0f, 14.0f), cardBorder.Get(), 1.0f);
                        }
                        else
                        {
                            rt->FillEllipse(D2D1::Ellipse(swC, 14.0f, 14.0f), swatchBlue.Get());
                            ComPtr<ID2D1SolidColorBrush> blueRim;
                            rt->CreateSolidColorBrush(RgbColor(72, 148, 255), blueRim.GetAddressOf());
                            if (blueRim) rt->DrawEllipse(D2D1::Ellipse(swC, 14.0f, 14.0f), blueRim.Get(), 1.2f);
                        }

                        if (m_dwTitleFmt && titleBr)
                        {
                            rt->DrawTextW(kPresets[i].name, static_cast<UINT32>(wcslen(kPresets[i].name)),
                                m_dwTitleFmt.Get(), D2D1::RectF(62.0f, top + 8.0f, w - 86.0f, top + 28.0f), titleBr.Get());
                        }
                        if (m_dwSubFmt && subBr)
                        {
                            rt->DrawTextW(kPresets[i].desc, static_cast<UINT32>(wcslen(kPresets[i].desc)),
                                m_dwSubFmt.Get(), D2D1::RectF(62.0f, top + 28.0f, w - 18.0f, top + 48.0f), subBr.Get());
                        }
                        if (kPresets[i].tag[0] != L'\0' && m_dwSubFmt && accentBr)
                        {
                            const D2D1_ROUNDED_RECT tagBg = D2D1::RoundedRect(D2D1::RectF(w - 72.0f, top + 9.0f, w - 18.0f, top + 25.0f), 4.0f, 4.0f);
                            ComPtr<ID2D1SolidColorBrush> pillBg;
                            rt->CreateSolidColorBrush(RgbColor(0, 120, 212, 0.20f), pillBg.GetAddressOf());
                            if (pillBg) rt->FillRoundedRectangle(tagBg, pillBg.Get());
                            rt->DrawTextW(kPresets[i].tag, static_cast<UINT32>(wcslen(kPresets[i].tag)),
                                m_dwSubFmt.Get(), D2D1::RectF(w - 66.0f, top + 9.0f, w - 20.0f, top + 25.0f), accentBr.Get());
                        }
                    }

                    // Material & Tone card with ONLY 2 Fluent Sliders
                    const D2D1_ROUNDED_RECT slidersCard = D2D1::RoundedRect(
                        D2D1::RectF(10.0f, 224.0f, w - 8.0f, 412.0f), 8.0f, 8.0f);
                    if (cardBg) rt->FillRoundedRectangle(slidersCard, cardBg.Get());
                    if (cardBorder) rt->DrawRoundedRectangle(slidersCard, cardBorder.Get(), 1.0f);

                    auto drawFluentSlider = [&](float topY, const wchar_t* title, const std::wstring& readout,
                        const wchar_t* leftLbl, const wchar_t* midLbl, const wchar_t* rightLbl, int val) {
                        if (m_dwTitleFmt && titleBr)
                            rt->DrawTextW(title, static_cast<UINT32>(wcslen(title)), m_dwTitleFmt.Get(),
                                D2D1::RectF(22.0f, topY, w - 150.0f, topY + 20.0f), titleBr.Get());
                        if (m_dwSubFmt && subBr)
                        {
                            rt->DrawTextW(readout.c_str(), static_cast<UINT32>(readout.size()), m_dwSubFmt.Get(),
                                D2D1::RectF(w - 146.0f, topY + 1.0f, w - 18.0f, topY + 20.0f), subBr.Get());
                        }

                        const float trY = topY + 36.0f;
                        const float trL = 24.0f;
                        const float trR = w - 24.0f;
                        const float frac = static_cast<float>(std::clamp(val, 0, 100)) / 100.0f;
                        const float thumbX = trL + (trR - trL) * frac;

                        const D2D1_ROUNDED_RECT track = D2D1::RoundedRect(D2D1::RectF(trL, trY - 2.5f, trR, trY + 2.5f), 2.5f, 2.5f);
                        const D2D1_ROUNDED_RECT fill = D2D1::RoundedRect(D2D1::RectF(trL, trY - 2.5f, thumbX, trY + 2.5f), 2.5f, 2.5f);
                        if (trackBg) rt->FillRoundedRectangle(track, trackBg.Get());
                        if (accentBr && thumbX > trL) rt->FillRoundedRectangle(fill, accentBr.Get());
                        if (accentBr && whiteBr)
                        {
                            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, trY), 8.0f, 8.0f), whiteBr.Get());
                            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, trY), 5.0f, 5.0f), accentBr.Get());
                        }

                        if (m_dwSubFmt && subBr)
                        {
                            rt->DrawTextW(leftLbl, static_cast<UINT32>(wcslen(leftLbl)), m_dwSubFmt.Get(),
                                D2D1::RectF(trL, trY + 11.0f, trL + 90.0f, trY + 29.0f), subBr.Get());
                            rt->DrawTextW(midLbl, static_cast<UINT32>(wcslen(midLbl)), m_dwSubFmt.Get(),
                                D2D1::RectF((trL + trR) * 0.5f - 28.0f, trY + 11.0f, (trL + trR) * 0.5f + 42.0f, trY + 29.0f), subBr.Get());
                            rt->DrawTextW(rightLbl, static_cast<UINT32>(wcslen(rightLbl)), m_dwSubFmt.Get(),
                                D2D1::RectF(trR - 40.0f, trY + 11.0f, trR + 4.0f, trY + 29.0f), subBr.Get());
                        }
                    };

                    const std::wstring gKind = (m_config.glassLevel <= 20) ? L"Clear Glass"
                        : (m_config.glassLevel <= 68 ? L"Acrylic" : L"Matte / Solid");
                    const std::wstring gReadout = std::to_wstring(m_config.glassLevel) + L"% (" + gKind + L")";
                    drawFluentSlider(238.0f, L"Glass Level", gReadout, L"Clear Glass", L"Acrylic", L"Solid", m_config.glassLevel);

                    const std::wstring tKind = (m_config.lightDarkMode < 35) ? L"Dark"
                        : (m_config.lightDarkMode <= 65 ? L"Neutral" : L"Light");
                    const std::wstring tReadout = std::to_wstring(m_config.lightDarkMode) + L"% (" + tKind + L")";
                    drawFluentSlider(328.0f, L"Light / Dark Mode", tReadout, L"Dark", L"Balanced", L"Light", m_config.lightDarkMode);
                });
            }

            void ReleasePreviewGraphics()
            {
                m_previewNoiseBrush.Reset();
                m_previewNoiseBmp.Reset();
                m_splitSceneBmp.Reset();
                m_desktopSceneBmp.Reset();
                for (auto& slot : m_previewSlots)
                    slot.iconBmp.Reset();
                if (m_previewCtx)
                    m_previewCtx->SetTarget(nullptr);
                m_previewTargetBmp.Reset();
                m_previewSwapChain.Reset();
                m_previewCtx.Reset();
                m_d2dDevice.Reset();
                m_dxgiDevice.Reset();
                m_d3dDevice.Reset();
                m_splitW = 0;
                m_splitH = 0;
                m_prevSwapW = 0;
                m_prevSwapH = 0;
            }

            bool EnsurePreviewGraphicsDevices()
            {
                if (!m_ringCanvas || !m_d2dFactory)
                    return false;

                if (!m_d3dDevice)
                {
                    const UINT d3dFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
                    D3D_DRIVER_TYPE drivers[] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP };
                    HRESULT hr = E_FAIL;
                    for (auto drv : drivers)
                    {
                        hr = D3D11CreateDevice(nullptr, drv, nullptr, d3dFlags, nullptr, 0,
                            D3D11_SDK_VERSION, m_d3dDevice.ReleaseAndGetAddressOf(), nullptr, nullptr);
                        if (SUCCEEDED(hr))
                            break;
                    }
                    if (FAILED(hr) || !m_d3dDevice)
                        return false;
                    if (FAILED(m_d3dDevice.As(&m_dxgiDevice)) || !m_dxgiDevice)
                        return false;
                    if (FAILED(m_d2dFactory->CreateDevice(m_dxgiDevice.Get(), m_d2dDevice.ReleaseAndGetAddressOf())))
                        return false;
                    if (FAILED(m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, m_previewCtx.ReleaseAndGetAddressOf())))
                        return false;
                }

                RECT rc{};
                GetClientRect(m_ringCanvas, &rc);
                const int w = (std::max)(1, static_cast<int>(rc.right - rc.left));
                const int h = (std::max)(1, static_cast<int>(rc.bottom - rc.top));

                if (!m_previewSwapChain || m_prevSwapW != w || m_prevSwapH != h || !m_previewTargetBmp)
                {
                    m_previewCtx->SetTarget(nullptr);
                    m_previewTargetBmp.Reset();

                    if (!m_previewSwapChain)
                    {
                        ComPtr<IDXGIAdapter> adapter;
                        if (FAILED(m_dxgiDevice->GetAdapter(adapter.GetAddressOf())))
                            return false;
                        ComPtr<IDXGIFactory2> factory;
                        if (FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()))))
                            return false;

                        DXGI_SWAP_CHAIN_DESC1 desc{};
                        desc.Width = static_cast<UINT>(w);
                        desc.Height = static_cast<UINT>(h);
                        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                        desc.SampleDesc.Count = 1;
                        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                        desc.BufferCount = 2;
                        desc.Scaling = DXGI_SCALING_STRETCH;
                        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
                        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
                        if (FAILED(factory->CreateSwapChainForHwnd(m_d3dDevice.Get(), m_ringCanvas, &desc,
                            nullptr, nullptr, m_previewSwapChain.ReleaseAndGetAddressOf())))
                            return false;
                    }
                    else
                    {
                        if (FAILED(m_previewSwapChain->ResizeBuffers(2, static_cast<UINT>(w), static_cast<UINT>(h),
                            DXGI_FORMAT_B8G8R8A8_UNORM, 0)))
                            return false;
                    }

                    ComPtr<IDXGISurface> surf;
                    D2D1_BITMAP_PROPERTIES1 bp{};
                    bp.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE);
                    bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
                    bp.dpiX = 96.0f;
                    bp.dpiY = 96.0f;
                    if (FAILED(m_previewSwapChain->GetBuffer(0, IID_PPV_ARGS(surf.GetAddressOf()))) ||
                        FAILED(m_previewCtx->CreateBitmapFromDxgiSurface(surf.Get(), &bp, m_previewTargetBmp.ReleaseAndGetAddressOf())))
                        return false;
                    m_prevSwapW = w;
                    m_prevSwapH = h;
                }

                if (!m_previewNoiseBrush)
                {
                    const std::vector<uint8_t> noise = GenerateAcrylicNoiseBgra256();
                    D2D1_BITMAP_PROPERTIES1 bp{};
                    bp.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
                    bp.dpiX = 96.0f;
                    bp.dpiY = 96.0f;
                    if (SUCCEEDED(m_previewCtx->CreateBitmap(D2D1::SizeU(256, 256), noise.data(), 256u * 4u,
                        &bp, m_previewNoiseBmp.ReleaseAndGetAddressOf())) && m_previewNoiseBmp)
                    {
                        D2D1_BITMAP_BRUSH_PROPERTIES bbp = D2D1::BitmapBrushProperties(
                            D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
                        m_previewCtx->CreateBitmapBrush(m_previewNoiseBmp.Get(), bbp, m_previewNoiseBrush.ReleaseAndGetAddressOf());
                    }
                }

                return true;
            }

            void EnsurePreviewSceneBitmaps(int w, int h)
            {
                if (!m_previewCtx)
                    return;

                if (!m_desktopSceneBmp && !m_desktopPixels.empty() && m_desktopW > 0 && m_desktopH > 0)
                {
                    D2D1_BITMAP_PROPERTIES1 bp{};
                    bp.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE);
                    bp.dpiX = 96.0f;
                    bp.dpiY = 96.0f;
                    m_previewCtx->CreateBitmap(
                        D2D1::SizeU(static_cast<UINT32>(m_desktopW), static_cast<UINT32>(m_desktopH)),
                        m_desktopPixels.data(), static_cast<UINT32>(m_desktopW * 4),
                        &bp, m_desktopSceneBmp.ReleaseAndGetAddressOf());
                }

                if (m_splitSceneBmp && m_splitW == w && m_splitH == h)
                    return;

                const int sceneH = (std::max)(1, h - kPreviewBarH);
                std::vector<BYTE> sharp(static_cast<size_t>(w) * sceneH * 4u, 255);
                const float fw = static_cast<float>((std::max)(1, w));
                const float fh = static_cast<float>((std::max)(1, sceneH));

                auto putPx = [&](int x, int y, float r, float g, float b) {
                    if (x < 0 || y < 0 || x >= w || y >= sceneH)
                        return;
                    BYTE* p = &sharp[(static_cast<size_t>(y) * w + x) * 4u];
                    p[0] = static_cast<BYTE>(std::clamp(static_cast<int>(std::lround(b)), 0, 255));
                    p[1] = static_cast<BYTE>(std::clamp(static_cast<int>(std::lround(g)), 0, 255));
                    p[2] = static_cast<BYTE>(std::clamp(static_cast<int>(std::lround(r)), 0, 255));
                    p[3] = 255;
                };

                const int midX = w / 2;
                const int midY = sceneH / 2;
                for (int y = 0; y < sceneH; ++y)
                {
                    for (int x = 0; x < w; ++x)
                    {
                        if (x < midX)
                        {
                            float r = 18.0f, g = 20.0f, b = 27.0f;
                            const float d = std::hypot((x - fw * 0.22f) / fw, (y - fh * 0.30f) / fh);
                            const float glow = (std::max)(0.0f, 1.0f - d * 2.2f);
                            r += glow * 26.0f;
                            g += glow * 90.0f;
                            b += glow * 130.0f;
                            if (y > 32 && y < sceneH - 28 && (y % 18) >= 4 && (y % 18) <= 9 && x > 20 && x < midX - 20)
                            {
                                const int row = y / 18;
                                if ((row % 3) == 0 && x < midX - 32) { r = 80; g = 185; b = 248; }
                                else if ((row % 3) == 1 && x < midX - 48) { r = 186; g = 112; b = 246; }
                                else if (x < midX - 62) { r = 74; g = 212; b = 148; }
                            }
                            putPx(x, y, r, g, b);
                        }
                        else if (y < midY)
                        {
                            const float nx = (x - midX) / (std::max)(1.0f, fw * 0.5f);
                            const float ny = y / (std::max)(1.0f, fh * 0.5f);
                            const float b1 = (std::max)(0.0f, 1.0f - std::hypot(nx - 0.55f, ny - 0.42f) * 1.35f);
                            const float b2 = (std::max)(0.0f, 1.0f - std::hypot(nx - 0.22f, ny - 0.72f) * 1.45f);
                            const float b3 = (std::max)(0.0f, 1.0f - std::hypot(nx - 0.80f, ny - 0.78f) * 1.60f);
                            const float r = 12.0f + b1 * 48.0f + b2 * 165.0f + b3 * 36.0f;
                            const float g = 34.0f + b1 * 110.0f + b2 * 64.0f + b3 * 185.0f;
                            const float b = 96.0f + b1 * 155.0f + b2 * 145.0f + b3 * 145.0f;
                            putPx(x, y, r, g, b);
                        }
                        else
                        {
                            float r = 236.0f, g = 241.0f, b = 248.0f;
                            if (x > midX + 16 && x < w - 16)
                            {
                                if (y >= midY + 14 && y <= midY + 40) { r = 255; g = 255; b = 255; }
                                else if (y >= midY + 52 && y <= midY + 76 && x < w - 36) { r = 46; g = 132; b = 245; }
                                else if (y >= midY + 88 && y <= midY + 112 && x < w - 52) { r = 245; g = 158; b = 11; }
                                else if (y >= midY + 124 && y <= sceneH - 18) { r = 255; g = 255; b = 255; }
                            }
                            putPx(x, y, r, g, b);
                        }
                    }
                }

                D2D1_BITMAP_PROPERTIES1 bp{};
                bp.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE);
                bp.dpiX = 96.0f;
                bp.dpiY = 96.0f;
                m_previewCtx->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(w), static_cast<UINT32>(sceneH)),
                    sharp.data(), static_cast<UINT32>(w * 4), &bp, m_splitSceneBmp.ReleaseAndGetAddressOf());
                m_splitW = w;
                m_splitH = h;
            }

            ComPtr<ID2D1Image> BuildPreviewAcrylicEffectImage(ID2D1Bitmap1* srcBmp, const RingMaterialSpec& mat, float fitScale)
            {
                if (!m_previewCtx || !srcBmp)
                    return nullptr;

                ComPtr<ID2D1Effect> curStage;
                if (mat.blurSigma > 0.1f)
                {
                    ComPtr<ID2D1Effect> blur;
                    if (FAILED(m_previewCtx->CreateEffect(CLSID_D2D1GaussianBlur, blur.GetAddressOf())) || !blur)
                        return nullptr;
                    blur->SetInput(0, srcBmp);
                    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, mat.blurSigma * fitScale);
                    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED);
                    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
                    curStage = blur;

                    ComPtr<ID2D1Effect> sat;
                    if (SUCCEEDED(m_previewCtx->CreateEffect(CLSID_D2D1Saturation, sat.GetAddressOf())) && sat)
                    {
                        sat->SetInputEffect(0, curStage.Get());
                        sat->SetValue(D2D1_SATURATION_PROP_SATURATION, mat.saturation);
                        curStage = sat;
                    }
                }
                else
                {
                    ComPtr<ID2D1Effect> sat;
                    if (FAILED(m_previewCtx->CreateEffect(CLSID_D2D1Saturation, sat.GetAddressOf())) || !sat)
                        return nullptr;
                    sat->SetInput(0, srcBmp);
                    sat->SetValue(D2D1_SATURATION_PROP_SATURATION, mat.saturation);
                    curStage = sat;
                }

                const float lumA = std::clamp(mat.luminosityAlpha, 0.0f, 1.0f);
                if (lumA > 0.01f)
                {
                    ComPtr<ID2D1Effect> lumFlood, lumBlend;
                    if (SUCCEEDED(m_previewCtx->CreateEffect(CLSID_D2D1Flood, lumFlood.GetAddressOf())) &&
                        SUCCEEDED(m_previewCtx->CreateEffect(CLSID_D2D1Blend, lumBlend.GetAddressOf())))
                    {
                        const D2D1_VECTOR_4F lc{
                            mat.luminosityR * lumA,
                            mat.luminosityG * lumA,
                            mat.luminosityB * lumA,
                            lumA
                        };
                        lumFlood->SetValue(D2D1_FLOOD_PROP_COLOR, lc);
                        lumBlend->SetInputEffect(0, curStage.Get());
                        lumBlend->SetInputEffect(1, lumFlood.Get());
                        lumBlend->SetValue(D2D1_BLEND_PROP_MODE, D2D1_BLEND_MODE_LUMINOSITY);
                        curStage = lumBlend;
                    }
                }

                const float a = std::clamp(mat.tintAlpha, 0.0f, 1.0f);
                ComPtr<ID2D1Effect> tintFlood, comp;
                if (SUCCEEDED(m_previewCtx->CreateEffect(CLSID_D2D1Flood, tintFlood.GetAddressOf())) &&
                    SUCCEEDED(m_previewCtx->CreateEffect(CLSID_D2D1Composite, comp.GetAddressOf())))
                {
                    const D2D1_VECTOR_4F tc{ mat.tintR * a, mat.tintG * a, mat.tintB * a, a };
                    tintFlood->SetValue(D2D1_FLOOD_PROP_COLOR, tc);
                    comp->SetInputEffect(0, curStage.Get());
                    comp->SetInputEffect(1, tintFlood.Get());
                    comp->SetValue(D2D1_COMPOSITE_PROP_MODE, D2D1_COMPOSITE_MODE_SOURCE_OVER);
                    curStage = comp;
                }

                ComPtr<ID2D1Image> outImg;
                if (curStage)
                    curStage->GetOutput(outImg.GetAddressOf());
                return outImg;
            }

            void PaintRingPreview(HWND canvas)
            {
                if (!m_d2dFactory)
                    return;
                EnsureTextFormats();
                if (!EnsurePreviewGraphicsDevices() || !m_previewCtx || !m_previewTargetBmp)
                    return;

                RECT rc{};
                GetClientRect(canvas, &rc);
                const int wPx = (std::max)(1, static_cast<int>(rc.right - rc.left));
                const int hPx = (std::max)(1, static_cast<int>(rc.bottom - rc.top));

                EnsurePreviewSceneBitmaps(wPx, hPx);

                const float w = static_cast<float>(wPx);
                const float h = static_cast<float>(hPx);
                const float sceneH = (std::max)(1.0f, h - static_cast<float>(kPreviewBarH));

                const int slotCount = (std::max)(1, static_cast<int>(m_previewSlots.size()));
                const RingGeometry geom = ComputeRingGeometry(
                    slotCount, m_config.ringRadiusDip, m_config.iconSizeDip, m_config.deadzoneDip);
                const float fitScale = (std::min)(1.0f,
                    (std::min)(w - 20.0f, sceneH - 12.0f) * 0.46f / static_cast<float>((std::max)(1.0, geom.outerRadius)));

                const D2D1_POINT_2F c{ w * 0.5f, sceneH * 0.5f };
                const float outerR = static_cast<float>(geom.outerRadius) * fitScale;
                const float holeR = static_cast<float>(geom.holeRadius) * fitScale;
                const float ringR = static_cast<float>(geom.ringRadius) * fitScale;
                const float dotDist = static_cast<float>(geom.runningDotRadius) * fitScale;
                const float iconSizePx = static_cast<float>(geom.iconSize) * fitScale;

                const RingMaterialSpec tc = ComputeRingMaterial(m_config, 1.0);
                ID2D1Bitmap1* bgBmp = (m_useDesktopScene && m_desktopSceneBmp) ? m_desktopSceneBmp.Get() : m_splitSceneBmp.Get();
                ComPtr<ID2D1Image> glassImage = m_config.disableRingBlur ? nullptr : BuildPreviewAcrylicEffectImage(bgBmp, tc, fitScale);

                m_previewCtx->SetTarget(m_previewTargetBmp.Get());
                m_previewCtx->BeginDraw();

                if (bgBmp)
                    m_previewCtx->DrawBitmap(bgBmp, D2D1::RectF(0.0f, 0.0f, w, sceneH));
                else
                    m_previewCtx->Clear(RgbColor(20, 22, 28));

                auto mkBrush = [&](D2D1_COLOR_F col) {
                    ComPtr<ID2D1SolidColorBrush> b;
                    m_previewCtx->CreateSolidColorBrush(col, b.GetAddressOf());
                    return b;
                };

                // 1. Hollow annulus geometry (identical geometry math as RingOverlay)
                ComPtr<ID2D1PathGeometry> annulus;
                if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(annulus.GetAddressOf())) && annulus)
                {
                    ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(annulus->Open(sink.GetAddressOf())) && sink)
                    {
                        sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
                        sink->BeginFigure(D2D1::Point2F(c.x + outerR, c.y), D2D1_FIGURE_BEGIN_FILLED);
                        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(c.x - outerR, c.y),
                            D2D1::SizeF(outerR, outerR), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
                        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(c.x + outerR, c.y),
                            D2D1::SizeF(outerR, outerR), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);

                        sink->BeginFigure(D2D1::Point2F(c.x + holeR, c.y), D2D1_FIGURE_BEGIN_FILLED);
                        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(c.x - holeR, c.y),
                            D2D1::SizeF(holeR, holeR), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
                        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(c.x + holeR, c.y),
                            D2D1::SizeF(holeR, holeR), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                        sink->Close();
                    }
                }

                // Active slot label pill (only shown when hovering a slot, just like real RingOverlay)
                const int selIdx = (m_previewSelectedSlot >= 0 && m_previewSelectedSlot < static_cast<int>(m_previewSlots.size()))
                    ? m_previewSelectedSlot : -1;
                D2D1_ROUNDED_RECT centerPill{};
                const bool hasSummary = (selIdx >= 0);
                if (hasSummary)
                {
                    const std::wstring& lbl = m_previewSlots[static_cast<size_t>(selIdx)].label;
                    float textW = static_cast<float>(lbl.size()) * 7.5f;
                    const float maxPillW = (std::max)(88.0f, holeR * 1.72f);
                    if (m_dwCenterFmt && m_dwriteFactory)
                    {
                        ComPtr<IDWriteTextLayout> layout;
                        if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(lbl.c_str(), static_cast<UINT32>(lbl.size()),
                            m_dwCenterFmt.Get(), maxPillW - 22.0f, 32.0f, layout.GetAddressOf())) && layout)
                        {
                            DWRITE_TEXT_METRICS tm{};
                            if (SUCCEEDED(layout->GetMetrics(&tm)))
                                textW = tm.widthIncludingTrailingWhitespace;
                        }
                    }
                    const float pw = (std::clamp)(textW + 24.0f, 64.0f, maxPillW);
                    const float ph = 26.0f;
                    centerPill = D2D1::RoundedRect(
                        D2D1::RectF(c.x - pw * 0.5f, c.y - ph * 0.5f, c.x + pw * 0.5f, c.y + ph * 0.5f),
                        13.0f, 13.0f);
                }

                const bool useNoise = (!m_config.disableRingBlur && m_previewNoiseBrush != nullptr && tc.noiseOpacity > 0.002f);
                if (useNoise)
                    m_previewNoiseBrush->SetOpacity(tc.noiseOpacity);

                // Mask real GPU Acrylic effect onto the hollow annulus
                if (annulus)
                {
                    ComPtr<ID2D1Layer> layer;
                    if (SUCCEEDED(m_previewCtx->CreateLayer(layer.GetAddressOf())) && layer)
                    {
                        m_previewCtx->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), annulus.Get()), layer.Get());
                        if (glassImage)
                            m_previewCtx->DrawImage(glassImage.Get());
                        else if (auto fb = mkBrush(tc.ringFill))
                            m_previewCtx->FillRectangle(D2D1::RectF(0.0f, 0.0f, w, sceneH), fb.Get());
                        m_previewCtx->PopLayer();
                    }
                    if (useNoise)
                        m_previewCtx->FillGeometry(annulus.Get(), m_previewNoiseBrush.Get());
                }

                // Silky antialiased 3D glass rims + soft elevation lip (matches live RingOverlay 1:1)
                if (auto shBr = mkBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, tc.isLightText ? 0.30f : 0.14f)))
                {
                    m_previewCtx->DrawEllipse(D2D1::Ellipse(c, outerR + 0.65f, outerR + 0.65f), shBr.Get(), 1.3f);
                    m_previewCtx->DrawEllipse(D2D1::Ellipse(c, holeR - 0.55f, holeR - 0.55f), shBr.Get(), 1.1f);
                }

                auto makeRimGrad = [&](float yTop, float yBot, D2D1_COLOR_F cTop, D2D1_COLOR_F cMid, D2D1_COLOR_F cBot) {
                    ComPtr<ID2D1LinearGradientBrush> lgb;
                    D2D1_GRADIENT_STOP stops[3] = { { 0.0f, cTop }, { 0.52f, cMid }, { 1.0f, cBot } };
                    ComPtr<ID2D1GradientStopCollection> gsc;
                    if (SUCCEEDED(m_previewCtx->CreateGradientStopCollection(stops, 3, gsc.GetAddressOf())))
                    {
                        m_previewCtx->CreateLinearGradientBrush(
                            D2D1::LinearGradientBrushProperties(D2D1::Point2F(c.x, yTop), D2D1::Point2F(c.x, yBot)),
                            gsc.Get(), lgb.GetAddressOf());
                    }
                    return lgb;
                };

                const D2D1_COLOR_F outerTop = (m_config.theme == RingTheme::Blue)
                    ? D2D1::ColorF(132.0f / 255.0f, 198.0f / 255.0f, 1.0f, 0.62f)
                    : (tc.isLightText ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.25f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f));
                const D2D1_COLOR_F outerMid = (m_config.theme == RingTheme::Blue)
                    ? tc.ringStroke
                    : (tc.isLightText ? D2D1::ColorF(118.0f / 255.0f, 118.0f / 255.0f, 124.0f / 255.0f, 0.42f)
                                      : D2D1::ColorF(170.0f / 255.0f, 174.0f / 255.0f, 182.0f / 255.0f, 0.55f));
                const D2D1_COLOR_F outerBot = (m_config.theme == RingTheme::Blue)
                    ? D2D1::ColorF(52.0f / 255.0f, 120.0f / 255.0f, 230.0f / 255.0f, 0.34f)
                    : (tc.isLightText ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f)
                                      : D2D1::ColorF(120.0f / 255.0f, 126.0f / 255.0f, 138.0f / 255.0f, 0.42f));
                if (auto outerGrad = makeRimGrad(c.y - outerR, c.y + outerR, outerTop, outerMid, outerBot))
                    m_previewCtx->DrawEllipse(D2D1::Ellipse(c, outerR - 0.525f, outerR - 0.525f), outerGrad.Get(), 1.05f);

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
                    : (tc.isLightText ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.72f));
                if (auto innerGrad = makeRimGrad(c.y - holeR, c.y + holeR, innerTop, innerMid, innerBot))
                    m_previewCtx->DrawEllipse(D2D1::Ellipse(c, holeR + 0.475f, holeR + 0.475f), innerGrad.Get(), 0.95f);

                // Selection highlight preview (SectorWedge / PointerLine)
                const int previewSlotCnt = static_cast<int>(m_previewSlots.size());
                const int activeIndSlot = (selIdx >= 0) ? selIdx
                    : ((previewSlotCnt > 0 && m_config.selectionIndicator != SelectionIndicatorStyle::GlowOnly) ? 0 : -1);
                if (activeIndSlot >= 0 && activeIndSlot < previewSlotCnt &&
                    m_config.selectionIndicator != SelectionIndicatorStyle::GlowOnly && m_d2dFactory)
                {
                    const double selAngle = GetSlotAngle(activeIndSlot, previewSlotCnt);
                    const double halfAng = kPi / static_cast<double>((std::max)(previewSlotCnt, 4));
                    if (m_config.selectionIndicator == SelectionIndicatorStyle::SectorWedge)
                    {
                        const double a0 = selAngle - halfAng;
                        const double a1 = selAngle + halfAng;
                        const float rOut = outerR - 1.0f;
                        const float rIn = holeR + 1.0f;
                        auto ptAt = [&](float r, double ang) -> D2D1_POINT_2F {
                            return D2D1::Point2F(
                                c.x + r * static_cast<float>(std::cos(ang)),
                                c.y + r * static_cast<float>(std::sin(ang)));
                        };
                        ComPtr<ID2D1PathGeometry> wedgeGeo;
                        if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(wedgeGeo.GetAddressOf())) && wedgeGeo)
                        {
                            ComPtr<ID2D1GeometrySink> sink;
                            if (SUCCEEDED(wedgeGeo->Open(sink.GetAddressOf())) && sink)
                            {
                                const D2D1_ARC_SIZE arcSz = ((a1 - a0) > kPi) ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;
                                sink->BeginFigure(ptAt(rOut, a0), D2D1_FIGURE_BEGIN_FILLED);
                                sink->AddArc(D2D1::ArcSegment(ptAt(rOut, a1), D2D1::SizeF(rOut, rOut),
                                    0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, arcSz));
                                sink->AddLine(ptAt(rIn, a1));
                                sink->AddArc(D2D1::ArcSegment(ptAt(rIn, a0), D2D1::SizeF(rIn, rIn),
                                    0.0f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, arcSz));
                                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                                sink->Close();
                                D2D1_COLOR_F fc = tc.accent;
                                fc.a = 0.24f;
                                if (auto fb = mkBrush(fc))
                                    m_previewCtx->FillGeometry(wedgeGeo.Get(), fb.Get());
                            }
                        }
                        auto strokeArc = [&](float r, double startAng, double endAng, float width, float alpha) {
                            ComPtr<ID2D1PathGeometry> arcGeo;
                            if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(arcGeo.GetAddressOf())) && arcGeo)
                            {
                                ComPtr<ID2D1GeometrySink> sink;
                                if (SUCCEEDED(arcGeo->Open(sink.GetAddressOf())) && sink)
                                {
                                    const D2D1_ARC_SIZE arcSz = ((endAng - startAng) > kPi) ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;
                                    sink->BeginFigure(ptAt(r, startAng), D2D1_FIGURE_BEGIN_HOLLOW);
                                    sink->AddArc(D2D1::ArcSegment(ptAt(r, endAng), D2D1::SizeF(r, r),
                                        0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, arcSz));
                                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                                    sink->Close();
                                    D2D1_COLOR_F sc = tc.accent;
                                    sc.a = alpha;
                                    if (auto sb = mkBrush(sc))
                                        m_previewCtx->DrawGeometry(arcGeo.Get(), sb.Get(), width);
                                }
                            }
                        };
                        strokeArc(rOut, a0, a1, 1.6f, 0.92f);
                        strokeArc(rIn, a0, a1, 1.1f, 0.65f);
                    }
                    else if (m_config.selectionIndicator == SelectionIndicatorStyle::PointerLine)
                    {
                        const float cs = static_cast<float>(std::cos(selAngle));
                        const float sn = static_cast<float>(std::sin(selAngle));
                        const float rStart = holeR + 2.0f;
                        const float rEnd = (std::max)(rStart + 4.0f, ringR - iconSizePx * 0.56f);
                        D2D1_COLOR_F lc = tc.accent;
                        lc.a = 0.92f;
                        if (auto lb = mkBrush(lc))
                            m_previewCtx->DrawLine(
                                D2D1::Point2F(c.x + rStart * cs, c.y + rStart * sn),
                                D2D1::Point2F(c.x + rEnd * cs, c.y + rEnd * sn),
                                lb.Get(), 2.0f);
                    }
                }

                // Real slots, running dots, hover selection highlight, and real extracted shell icons
                constexpr float kIdleScale = 0.90f;
                constexpr float kSelScale = 1.20f;
                for (int pass = 0; pass < 2; ++pass)
                {
                    for (int i = 0; i < static_cast<int>(m_previewSlots.size()); ++i)
                    {
                        const bool sel = (i == selIdx);
                        if ((pass == 0 && sel) || (pass == 1 && !sel))
                            continue;

                        const auto& slot = m_previewSlots[static_cast<size_t>(i)];
                        const double angle = GetSlotAngle(i, static_cast<int>(m_previewSlots.size()));
                        const D2D1_POINT_2F p{
                            c.x + static_cast<float>(std::cos(angle)) * ringR,
                            c.y + static_cast<float>(std::sin(angle)) * ringR
                        };
                        const float scale = sel ? kSelScale : kIdleScale;
                        const float haloR = iconSizePx * 0.72f * (scale / kSelScale);

                        if (slot.isRunning)
                        {
                            const D2D1_POINT_2F dp{
                                c.x + static_cast<float>(std::cos(angle)) * dotDist,
                                c.y + static_cast<float>(std::sin(angle)) * dotDist
                            };
                            D2D1_COLOR_F dc = tc.textColor;
                            dc.a = slot.isFrontmost ? 1.0f : 0.60f;
                            if (auto db = mkBrush(dc))
                                m_previewCtx->FillEllipse(D2D1::Ellipse(dp, slot.isFrontmost ? 2.4f : 1.7f, slot.isFrontmost ? 2.4f : 1.7f), db.Get());
                        }

                        if (sel)
                        {
                            D2D1_COLOR_F g0 = tc.hoverRevealColor;
                            g0.a = (m_config.theme == RingTheme::A4S) ? 0.30f : 0.36f;
                            D2D1_COLOR_F g1 = g0;
                            g1.a = 0.0f;
                            D2D1_GRADIENT_STOP stops[2] = { { 0.0f, g0 }, { 1.0f, g1 } };
                            ComPtr<ID2D1GradientStopCollection> gsc;
                            if (SUCCEEDED(m_previewCtx->CreateGradientStopCollection(stops, 2, gsc.GetAddressOf())))
                            {
                                ComPtr<ID2D1RadialGradientBrush> rgb;
                                const float gr = haloR * 1.48f;
                                if (SUCCEEDED(m_previewCtx->CreateRadialGradientBrush(
                                    D2D1::RadialGradientBrushProperties(p, D2D1::Point2F(0, 0), gr, gr), gsc.Get(), rgb.GetAddressOf())))
                                    m_previewCtx->FillEllipse(D2D1::Ellipse(p, gr, gr), rgb.Get());
                            }
                            if (auto hb = mkBrush(tc.slotSelectedFill))
                                m_previewCtx->FillEllipse(D2D1::Ellipse(p, haloR, haloR), hb.Get());
                            if (auto rb = mkBrush(tc.hoverRim))
                                m_previewCtx->DrawEllipse(D2D1::Ellipse(p, haloR, haloR), rb.Get(), tc.hairlineWidth);
                        }

                        if (slot.iconBmp)
                        {
                            const float ih = iconSizePx * 0.46f * scale;
                            m_previewCtx->DrawBitmap(slot.iconBmp.Get(), D2D1::RectF(p.x - ih, p.y - ih, p.x + ih, p.y + ih));
                        }
                        else if (m_dwGlyphFmt)
                        {
                            std::wstring glyph = L"?";
                            if (slot.kind == SlotKind::RecycleBin) glyph = L"RB";
                            else if (slot.kind == SlotKind::Collection) glyph = L"COL";
                            else if (slot.kind == SlotKind::Website) glyph = L"WEB";
                            else if (!slot.label.empty()) glyph = std::wstring(1, static_cast<wchar_t>(towupper(slot.label[0])));
                            if (auto gb = mkBrush(tc.textColor))
                                m_previewCtx->DrawTextW(glyph.c_str(), static_cast<UINT32>(glyph.size()),
                                    m_dwGlyphFmt.Get(), D2D1::RectF(p.x - haloR, p.y - haloR, p.x + haloR, p.y + haloR), gb.Get());
                        }

                        if (slot.windowCount > 1 && m_dwSmallFmt)
                        {
                            const float bd = iconSizePx * 0.46f * scale;
                            const D2D1_POINT_2F bc{ p.x + bd, p.y - bd };
                            if (auto ab = mkBrush(tc.accent))
                                m_previewCtx->FillEllipse(D2D1::Ellipse(bc, 8.0f, 8.0f), ab.Get());
                            const std::wstring ct = slot.windowCount > 9 ? L"9+" : std::to_wstring(slot.windowCount);
                            if (auto wb = mkBrush(RgbColor(255, 255, 255)))
                                m_previewCtx->DrawTextW(ct.c_str(), static_cast<UINT32>(ct.size()), m_dwSmallFmt.Get(),
                                    D2D1::RectF(bc.x - 10, bc.y - 9, bc.x + 10, bc.y + 9), wb.Get());
                        }
                    }
                }

                // Center pill ONLY on slot hover; subtle idle center dot otherwise (matches real RingOverlay 1:1)
                if (hasSummary)
                {
                    ComPtr<ID2D1RoundedRectangleGeometry> pillGeo;
                    if (SUCCEEDED(m_d2dFactory->CreateRoundedRectangleGeometry(centerPill, pillGeo.GetAddressOf())) && pillGeo)
                    {
                        ComPtr<ID2D1Layer> pLayer;
                        if (SUCCEEDED(m_previewCtx->CreateLayer(pLayer.GetAddressOf())) && pLayer)
                        {
                            m_previewCtx->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), pillGeo.Get()), pLayer.Get());
                            if (glassImage)
                                m_previewCtx->DrawImage(glassImage.Get());
                            else if (auto cb = mkBrush(tc.centerFill))
                                m_previewCtx->FillRectangle(D2D1::RectF(0.0f, 0.0f, w, sceneH), cb.Get());
                            m_previewCtx->PopLayer();
                        }
                    }
                    if (useNoise)
                        m_previewCtx->FillRoundedRectangle(centerPill, m_previewNoiseBrush.Get());
                    if (auto pb = mkBrush(tc.centerBorder))
                        m_previewCtx->DrawRoundedRectangle(centerPill, pb.Get(), tc.hairlineWidth);
                    if (m_dwCenterFmt)
                    {
                        if (auto tb = mkBrush(tc.textColor))
                        {
                            const std::wstring& lbl = m_previewSlots[static_cast<size_t>(selIdx)].label;
                            m_previewCtx->DrawTextW(lbl.c_str(), static_cast<UINT32>(lbl.size()), m_dwCenterFmt.Get(),
                                D2D1::RectF(centerPill.rect.left + 8.0f, centerPill.rect.top, centerPill.rect.right - 8.0f, centerPill.rect.bottom), tb.Get());
                        }
                    }
                }
                else
                {
                    if (auto dotBr = mkBrush(tc.idleDotColor))
                        m_previewCtx->FillEllipse(D2D1::Ellipse(c, 3.0f, 3.0f), dotBr.Get());
                }

                // Bottom backdrop switcher bar ("Split Test" | "Live Desktop" + slot readout)
                const float barTop = sceneH;
                if (auto barBr = mkBrush(RgbColor(20, 20, 23)))
                    m_previewCtx->FillRectangle(D2D1::RectF(0.0f, barTop, w, h), barBr.Get());
                if (auto divBr = mkBrush(RgbColor(54, 54, 60)))
                    m_previewCtx->DrawLine(D2D1::Point2F(0.0f, barTop), D2D1::Point2F(w, barTop), divBr.Get(), 1.0f);

                auto drawModeChip = [&](float x0, float x1, const wchar_t* label, bool on) {
                    const D2D1_ROUNDED_RECT cr = D2D1::RoundedRect(D2D1::RectF(x0, barTop + 4.0f, x1, h - 4.0f), 4.0f, 4.0f);
                    if (auto bg = mkBrush(on ? RgbColor(0, 120, 212, 0.25f) : RgbColor(38, 38, 42)))
                        m_previewCtx->FillRoundedRectangle(cr, bg.Get());
                    if (auto rm = mkBrush(on ? RgbColor(0, 120, 212) : RgbColor(64, 64, 72)))
                        m_previewCtx->DrawRoundedRectangle(cr, rm.Get(), 1.0f);
                    if (m_dwSmallFmt)
                    {
                        if (auto tb = mkBrush(on ? RgbColor(244, 244, 246) : RgbColor(161, 161, 170)))
                            m_previewCtx->DrawTextW(label, static_cast<UINT32>(wcslen(label)), m_dwSmallFmt.Get(), cr.rect, tb.Get());
                    }
                };
                drawModeChip(8.0f, 88.0f, L"Split Test", !m_useDesktopScene);
                drawModeChip(94.0f, 186.0f, L"Live Desktop", m_useDesktopScene);

                if (m_dwSubFmt)
                {
                    const std::wstring info = std::wstring(ToString(m_config.theme)) + L" \u00B7 " + std::to_wstring(m_previewSlots.size()) + L" slots";
                    if (auto ib = mkBrush(RgbColor(161, 161, 170)))
                        m_previewCtx->DrawTextW(info.c_str(), static_cast<UINT32>(info.size()), m_dwSubFmt.Get(),
                            D2D1::RectF(196.0f, barTop + 6.0f, w - 8.0f, h - 4.0f), ib.Get());
                }

                if (auto frameBr = mkBrush(RgbColor(62, 62, 68)))
                    m_previewCtx->DrawRectangle(D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), frameBr.Get(), 1.0f);

                const HRESULT endHr = m_previewCtx->EndDraw();
                m_previewCtx->SetTarget(nullptr);
                if (endHr == D2DERR_RECREATE_TARGET)
                {
                    ReleasePreviewGraphics();
                    return;
                }
                if (m_previewSwapChain)
                    m_previewSwapChain->Present(0, 0);
            }

            HWND MakeCtrl(int pageIdx, const wchar_t* cls, const wchar_t* text, DWORD style,
                int x, int y, int w, int h, int id)
            {
                HWND ctrl = CreateWindowExW(0, cls, text, WS_CHILD | style,
                    x, y, w, h, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), m_instance, nullptr);
                SendMessageW(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
                SetWindowTheme(ctrl, L"DarkMode_Explorer", nullptr);
                if (pageIdx >= 0 && pageIdx < kPageCount)
                    m_pageCtrls[pageIdx].push_back(ctrl);
                return ctrl;
            }

            void BuildUi()
            {
                ApplyDarkDwmFrame(m_hwnd);

                m_bgBrush = CreateSolidBrush(kColBg);
                m_cardBrush = CreateSolidBrush(kColCard);
                m_editBrush = CreateSolidBrush(kColEdit);
                m_footerBrush = CreateSolidBrush(kColSidebar);
                m_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
                m_boldFont = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
                m_titleFont = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Display");

                RECT cli{};
                GetClientRect(m_hwnd, &cli);
                const int cw = cli.right - cli.left;
                const int ch = cli.bottom - cli.top;
                const int px = kSidebarW + 24;
                const int previewX = cw - kPreviewW - 18;
                const int leftW = previewX - px - 18;
                const int fullW = cw - px - 22;

                m_sidebar = CreateWindowExW(0, L"WinRings.SidebarPanel.v1", L"", WS_CHILD | WS_VISIBLE,
                    0, 0, kSidebarW, ch - kFooterH, m_hwnd, reinterpret_cast<HMENU>(ID_SIDEBAR), m_instance, this);

                m_lblHeaderTitle = CreateWindowExW(0, L"STATIC", kPageTitles[0], WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                    px, 16, fullW, 26, m_hwnd, nullptr, m_instance, nullptr);
                SendMessageW(m_lblHeaderTitle, WM_SETFONT, reinterpret_cast<WPARAM>(m_titleFont), TRUE);
                m_lblHeaderSub = CreateWindowExW(0, L"STATIC", kPageSubtitles[0], WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                    px, 44, fullW, 20, m_hwnd, nullptr, m_instance, nullptr);
                SendMessageW(m_lblHeaderSub, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);

                // Shared persistent 1:1 live preview canvas (visible on Appearance, Pins & Folders, Advanced)
                m_ringCanvas = CreateWindowExW(0, L"WinRings.RingPreviewCanvas.v3", L"", WS_CHILD,
                    previewX, 74, kPreviewW, ch - kFooterH - 88, m_hwnd,
                    reinterpret_cast<HMENU>(ID_PREVIEW_RING_CANVAS), m_instance, this);

                // Page 0: Appearance
                m_appearanceCtrl = CreateWindowExW(0, L"WinRings.AppearancePanel.v1", L"", WS_CHILD,
                    px - 10, 74, leftW + 10, 424, m_hwnd, reinterpret_cast<HMENU>(ID_APPEARANCE_CTRL), m_instance, this);
                m_pageCtrls[0].push_back(m_appearanceCtrl);

                // Page 1: Triggers & Input
                int y = 78;
                MakeCtrl(1, L"STATIC", L"Mouse Trigger:", 0, px, y + 3, 120, 22, 0);
                m_cbMouseTrigger = MakeCtrl(1, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, px + 124, y, 220, 160, ID_CB_MOUSE_TRIGGER);
                for (const wchar_t* s : { L"Side Back (XBUTTON1)", L"Side Forward (XBUTTON2)", L"Middle Button", L"Right + Left Chord", L"Disabled" })
                    SendMessageW(m_cbMouseTrigger, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
                MakeCtrl(1, L"STATIC", L"Scroll Wheel:", 0, px + 368, y + 3, 102, 22, 0);
                m_cbScrollMode = MakeCtrl(1, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, px + 474, y, 220, 140, ID_CB_SCROLL_MODE);
                for (const wchar_t* s : { L"Cycle Slots or Windows", L"Cycle Submenu Only", L"Disabled" })
                    SendMessageW(m_cbScrollMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
                y += 34;

                m_chkDoubleTap = MakeCtrl(1, L"BUTTON", L"Double-tap trigger switches to last window",
                    BS_AUTOCHECKBOX | WS_TABSTOP, px, y, 360, 24, ID_CHK_DOUBLE_TAP);
                MakeCtrl(1, L"STATIC", L"Tap ms:", 0, px + 372, y + 3, 58, 22, 0);
                m_edTapMs = MakeCtrl(1, L"EDIT", L"", WS_BORDER | WS_TABSTOP, px + 432, y, 60, 24, ID_ED_TAP_MS);
                MakeCtrl(1, L"STATIC", L"Dbl ms:", 0, px + 510, y + 3, 58, 22, 0);
                m_edDTapMs = MakeCtrl(1, L"EDIT", L"", WS_BORDER | WS_TABSTOP, px + 570, y, 60, 24, ID_ED_DTAP_MS);
                y += 34;

                m_chkHotkey = MakeCtrl(1, L"BUTTON", L"Keyboard Hotkey:",
                    BS_AUTOCHECKBOX | WS_TABSTOP, px, y, 160, 24, ID_CHK_HOTKEY);
                m_chkModAlt = MakeCtrl(1, L"BUTTON", L"Alt", BS_AUTOCHECKBOX | WS_TABSTOP, px + 170, y, 54, 24, ID_CHK_MOD_ALT);
                m_chkModCtrl = MakeCtrl(1, L"BUTTON", L"Ctrl", BS_AUTOCHECKBOX | WS_TABSTOP, px + 230, y, 56, 24, ID_CHK_MOD_CTRL);
                m_chkModShift = MakeCtrl(1, L"BUTTON", L"Shift", BS_AUTOCHECKBOX | WS_TABSTOP, px + 292, y, 62, 24, ID_CHK_MOD_SHIFT);
                m_chkModWin = MakeCtrl(1, L"BUTTON", L"Win", BS_AUTOCHECKBOX | WS_TABSTOP, px + 360, y, 60, 24, ID_CHK_MOD_WIN);
                MakeCtrl(1, L"STATIC", L"Key:", 0, px + 432, y + 3, 38, 22, 0);
                m_cbHotkeyVk = MakeCtrl(1, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, px + 474, y, 130, 180, ID_CB_HOTKEY_VK);
                for (const auto& k : kHotkeyChoices)
                    SendMessageW(m_cbHotkeyVk, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(k.label));
                y += 34;

                m_chkMenuKey = MakeCtrl(1, L"BUTTON", L"Menu Key (VK_APPS)", BS_AUTOCHECKBOX | WS_TABSTOP, px, y, 200, 24, ID_CHK_MENU_KEY);
                m_chkWinTab = MakeCtrl(1, L"BUTTON", L"Intercept Win + Tab", BS_AUTOCHECKBOX | WS_TABSTOP, px + 214, y, 190, 24, ID_CHK_WIN_TAB);
                m_chkAltTab = MakeCtrl(1, L"BUTTON", L"Intercept Alt + Tab", BS_AUTOCHECKBOX | WS_TABSTOP, px + 418, y, 190, 24, ID_CHK_ALT_TAB);
                y += 34;

                m_chkWinKey = MakeCtrl(1, L"BUTTON", L"Open WinRings on Win Key (replaces Start Menu)",
                    BS_AUTOCHECKBOX | WS_TABSTOP, px, y, 420, 24, ID_CHK_WIN_KEY);
                y += 32;

                m_chkDirectSwitch = MakeCtrl(1, L"BUTTON", L"Direct Window Switch (no popup):",
                    BS_AUTOCHECKBOX | WS_TABSTOP, px, y, 275, 24, ID_CHK_DIRECT_SWITCH);
                m_cbDirectSwitchKey = MakeCtrl(1, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, px + 282, y, 140, 120, ID_CB_DIRECT_SWITCH_KEY);
                SendMessageW(m_cbDirectSwitchKey, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Alt + F"));
                SendMessageW(m_cbDirectSwitchKey, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Win + F"));
                SendMessageW(m_cbDirectSwitchKey, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Alt + Q"));
                y += 36;

                m_chkTpPhysClick = MakeCtrl(1, L"BUTTON", L"Touchpad Physical Left-Click toggles WinRings",
                    BS_AUTOCHECKBOX | WS_TABSTOP, px, y, 420, 24, ID_CHK_TP_PHYS_CLICK);
                y += 32;

                m_chkTpCorner = MakeCtrl(1, L"BUTTON", L"Touchpad Corner Tap:",
                    BS_AUTOCHECKBOX | WS_TABSTOP, px, y, 200, 24, ID_CHK_TP_CORNER);
                m_cbTpCornerPos = MakeCtrl(1, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, px + 206, y, 128, 100, ID_CB_TP_CORNER_POS);
                SendMessageW(m_cbTpCornerPos, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Top-Left"));
                SendMessageW(m_cbTpCornerPos, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Top-Right"));
                MakeCtrl(1, L"STATIC", L"Size %:", 0, px + 350, y + 3, 56, 22, 0);
                m_edTpCornerPct = MakeCtrl(1, L"EDIT", L"", WS_BORDER | WS_TABSTOP, px + 410, y, 52, 24, ID_ED_TP_CORNER_PCT);
                y += 36;

                m_chkShake = MakeCtrl(1, L"BUTTON", L"Mouse Shake Trigger (Aluminium OS):",
                    BS_AUTOCHECKBOX | WS_TABSTOP, px, y, 275, 24, ID_CHK_SHAKE);
                MakeCtrl(1, L"STATIC", L"Rev:", 0, px + 282, y + 3, 36, 22, 0);
                m_edShakeRev = MakeCtrl(1, L"EDIT", L"", WS_BORDER | WS_TABSTOP, px + 320, y, 44, 24, ID_ED_SHAKE_REV);
                MakeCtrl(1, L"STATIC", L"Win ms:", 0, px + 378, y + 3, 58, 22, 0);
                m_edShakeWin = MakeCtrl(1, L"EDIT", L"", WS_BORDER | WS_TABSTOP, px + 440, y, 56, 24, ID_ED_SHAKE_WIN);
                MakeCtrl(1, L"STATIC", L"DIP:", 0, px + 510, y + 3, 34, 22, 0);
                m_edShakeAmp = MakeCtrl(1, L"EDIT", L"", WS_BORDER | WS_TABSTOP, px + 548, y, 48, 24, ID_ED_SHAKE_AMP);
                y += 30;
                m_chkShakeFs = MakeCtrl(1, L"BUTTON", L"Suppress Shake in Fullscreen / Games",
                    BS_AUTOCHECKBOX | WS_TABSTOP, px + 22, y, 360, 24, ID_CHK_SHAKE_FS);
                y += 36;

                m_chkHotCorner = MakeCtrl(1, L"BUTTON", L"GNOME Hot Corner (Flick):",
                    BS_AUTOCHECKBOX | WS_TABSTOP, px, y, 214, 24, ID_CHK_HOT_CORNER);
                m_cbHcPos = MakeCtrl(1, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, px + 220, y, 114, 100, ID_CB_HC_POS);
                SendMessageW(m_cbHcPos, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Top-Left"));
                SendMessageW(m_cbHcPos, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Top-Right"));
                m_cbHcAct = MakeCtrl(1, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, px + 344, y, 174, 100, ID_CB_HC_ACT);
                SendMessageW(m_cbHcAct, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Task View (Win+Tab)"));
                SendMessageW(m_cbHcAct, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Open WinRings"));
                MakeCtrl(1, L"STATIC", L"Press:", 0, px + 530, y + 3, 48, 22, 0);
                m_edHcPress = MakeCtrl(1, L"EDIT", L"", WS_BORDER | WS_TABSTOP, px + 582, y, 58, 24, ID_ED_HC_PRESS);
                y += 42;

                MakeCtrl(1, L"STATIC", L"Drag-to-Snap / Split-Screen (from Ring & Window Cards):", 0, px, y, 480, 22, 0);
                y += 26;
                m_chkDragSnap = MakeCtrl(1, L"BUTTON", L"Enable Drag-to-Snap from Ring & Window Cards",
                    BS_AUTOCHECKBOX | WS_TABSTOP, px + 16, y, 440, 22, ID_CHK_DRAG_SNAP);
                y += 26;
                m_chkDragSnapClosed = MakeCtrl(1, L"BUTTON", L"Launch && Snap closed pinned apps when dragged to screen edge",
                    BS_AUTOCHECKBOX | WS_TABSTOP, px + 16, y, 500, 22, ID_CHK_DRAG_SNAP_CLOSED);
                y += 28;
                MakeCtrl(1, L"STATIC", L"Hold ms:", 0, px + 16, y + 3, 64, 22, 0);
                m_edDragSnapHold = MakeCtrl(1, L"EDIT", L"", WS_BORDER | WS_TABSTOP, px + 84, y, 56, 24, ID_ED_DRAG_SNAP_HOLD);
                MakeCtrl(1, L"STATIC", L"Tear-off DIP (outside ring):", 0, px + 162, y + 3, 184, 22, 0);
                m_edDragSnapTearOff = MakeCtrl(1, L"EDIT", L"", WS_BORDER | WS_TABSTOP, px + 350, y, 56, 24, ID_ED_DRAG_SNAP_TEAROFF);

                // Page 2: Top Bar & Media Island
                y = 84;
                m_chkTopBarOn = MakeCtrl(2, L"BUTTON", L"Enable Unified Minimal Top Bar (Windows 11 Shell Integration)", BS_AUTOCHECKBOX | WS_TABSTOP, px, y, 540, 24, ID_CHK_TOPBAR_ON);
                y += 30;
                m_chkDockTop = MakeCtrl(2, L"BUTTON", L"Dock Taskbar to Top Monitor Edge (updates Desktop Work Area)", BS_AUTOCHECKBOX | WS_TABSTOP, px, y, 540, 24, ID_CHK_DOCK_TOP);
                y += 36;

                MakeCtrl(2, L"STATIC", L"Bar Height (28-40 DIP):", 0, px, y + 3, 175, 22, 0);
                m_sldBarH = MakeCtrl(2, TRACKBAR_CLASSW, L"", TBS_AUTOTICKS | WS_TABSTOP, px + 184, y, 200, 28, ID_SLD_BAR_H);
                SendMessageW(m_sldBarH, TBM_SETRANGE, TRUE, MAKELPARAM(28, 40));
                m_edBarH = MakeCtrl(2, L"EDIT", L"", WS_BORDER | WS_TABSTOP, px + 394, y + 2, 56, 24, ID_ED_BAR_H);
                y += 36;

                m_chkKeepFont = MakeCtrl(2, L"BUTTON", L"Keep Text Font Size Unchanged (Native Taskbar Font Size)", BS_AUTOCHECKBOX | WS_TABSTOP, px, y, 520, 24, ID_CHK_KEEP_FONT);
                y += 30;
                m_chkClock1L = MakeCtrl(2, L"BUTTON", L"Single-Line Compact Clock (replaces two-line date/time)", BS_AUTOCHECKBOX | WS_TABSTOP, px, y, 500, 24, ID_CHK_CLOCK_1L);
                y += 34;

                MakeCtrl(2, L"STATIC", L"Clock Format:", 0, px, y + 3, 120, 22, 0);
                m_edClockFmt = MakeCtrl(2, L"EDIT", L"", WS_BORDER | WS_TABSTOP, px + 128, y, 220, 24, ID_ED_CLOCK_FMT);
                MakeCtrl(2, L"STATIC", L"Casing:", 0, px + 362, y + 3, 60, 22, 0);
                m_cbClockCasing = MakeCtrl(2, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, px + 426, y, 160, 140, ID_CB_CLOCK_CASING);
                for (const wchar_t* c : { L"CompactLower", L"Default", L"Lowercase", L"Uppercase" })
                    SendMessageW(m_cbClockCasing, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(c));
                y += 34;

                MakeCtrl(2, L"STATIC", L"Clock Preview:", 0, px, y + 3, 120, 22, 0);
                m_lblClockPreview = MakeCtrl(2, L"STATIC", L"", SS_SUNKEN | SS_CENTERIMAGE, px + 128, y, 280, 26, ID_LBL_CLOCK_PREVIEW);
                SendMessageW(m_lblClockPreview, WM_SETFONT, reinterpret_cast<WPARAM>(m_boldFont), TRUE);
                y += 38;

                MakeCtrl(2, L"STATIC", L"Taskbar Chrome Elements:", 0, px, y, 240, 22, 0);
                y += 26;
                m_chkHideApps = MakeCtrl(2, L"BUTTON", L"Hide Taskbar App Buttons (preserve System Tray)", BS_AUTOCHECKBOX | WS_TABSTOP, px + 16, y, 360, 22, ID_CHK_HIDE_APPS);
                m_chkHideStart = MakeCtrl(2, L"BUTTON", L"Hide Start Button", BS_AUTOCHECKBOX | WS_TABSTOP, px + 394, y, 180, 22, ID_CHK_HIDE_START);
                y += 28;
                m_chkHideSearch = MakeCtrl(2, L"BUTTON", L"Hide Search", BS_AUTOCHECKBOX | WS_TABSTOP, px + 16, y, 150, 22, ID_CHK_HIDE_SEARCH);
                m_chkHideTaskView = MakeCtrl(2, L"BUTTON", L"Hide Task View", BS_AUTOCHECKBOX | WS_TABSTOP, px + 186, y, 160, 22, ID_CHK_HIDE_TASKVIEW);
                m_chkHideWidgets = MakeCtrl(2, L"BUTTON", L"Hide Widgets", BS_AUTOCHECKBOX | WS_TABSTOP, px + 394, y, 170, 22, ID_CHK_HIDE_WIDGETS);
                y += 36;

                MakeCtrl(2, L"STATIC", L"Backdrop Style:", 0, px, y + 3, 120, 22, 0);
                m_cbBackdrop = MakeCtrl(2, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, px + 128, y, 160, 120, ID_CB_BACKDROP);
                for (const wchar_t* b : { L"Clear", L"Frosted", L"Native" })
                    SendMessageW(m_cbBackdrop, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(b));
                m_chkAllMon = MakeCtrl(2, L"BUTTON", L"Apply to All Monitors", BS_AUTOCHECKBOX | WS_TABSTOP, px + 320, y + 2, 220, 24, ID_CHK_ALL_MON);
                y += 40;

                MakeCtrl(2, L"STATIC", L"FullWorkArea Integration (Windows 11 26H2+):", 0, px, y, 360, 22, 0);
                y += 26;
                m_chkFwaOn = MakeCtrl(2, L"BUTTON", L"Extend maximized apps behind Top Bar", BS_AUTOCHECKBOX | WS_TABSTOP, px + 16, y, 310, 22, ID_CHK_FWA_ON);
                m_chkFwaPill = MakeCtrl(2, L"BUTTON", L"Click-Through Pill Clipping", BS_AUTOCHECKBOX | WS_TABSTOP, px + 338, y, 220, 22, ID_CHK_FWA_PILL);
                m_chkFwaHk = MakeCtrl(2, L"BUTTON", L"Win+` Hotkey", BS_AUTOCHECKBOX | WS_TABSTOP, px + 570, y, 140, 22, ID_CHK_FWA_HK);
                y += 34;

                MakeCtrl(2, L"STATIC", L"Dynamic Island Media Pill (Top Bar, Alt+Drag to move):", 0, px, y, 420, 22, 0);
                y += 26;
                m_chkMediaOn = MakeCtrl(2, L"BUTTON", L"Enable Media Pill (fits bar height)", BS_AUTOCHECKBOX | WS_TABSTOP, px + 16, y, 278, 22, ID_CHK_MEDIA_ON);
                MakeCtrl(2, L"STATIC", L"Position:", 0, px + 304, y + 2, 64, 22, 0);
                m_cbMediaPos = MakeCtrl(2, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, px + 370, y - 1, 110, 120, ID_CB_MEDIA_POS);
                for (const wchar_t* p : { L"Center", L"Left", L"Near Tray" })
                    SendMessageW(m_cbMediaPos, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p));
                MakeCtrl(2, L"STATIC", L"Fullscreen:", 0, px + 492, y + 2, 76, 22, 0);
                m_cbMediaFs = MakeCtrl(2, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, px + 570, y - 1, 170, 120, ID_CB_MEDIA_FS);
                for (const wchar_t* fs : { L"Sync with Taskbar", L"Games Only", L"Never Hide" })
                    SendMessageW(m_cbMediaFs, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(fs));
                y += 30;
                m_chkMediaExpand = MakeCtrl(2, L"BUTTON", L"Expand Card on Hover", BS_AUTOCHECKBOX | WS_TABSTOP, px + 16, y, 185, 22, ID_CHK_MEDIA_EXPAND);
                m_chkMediaVol = MakeCtrl(2, L"BUTTON", L"Scroll Wheel Volume", BS_AUTOCHECKBOX | WS_TABSTOP, px + 212, y, 175, 22, ID_CHK_MEDIA_VOL);
                m_chkMediaHidePaused = MakeCtrl(2, L"BUTTON", L"Hide When Paused", BS_AUTOCHECKBOX | WS_TABSTOP, px + 398, y, 165, 22, ID_CHK_MEDIA_HIDE_PAUSED);
                m_chkMediaMarquee = MakeCtrl(2, L"BUTTON", L"Scroll Long Titles", BS_AUTOCHECKBOX | WS_TABSTOP, px + 575, y, 170, 22, ID_CHK_MEDIA_MARQUEE);
                y += 32;
                m_btnResetMediaPos = MakeCtrl(2, L"BUTTON", L"Reset Island Position", BS_OWNERDRAW | WS_TABSTOP, px + 16, y, 180, 28, ID_BTN_RESET_MEDIA_POS);

                // Page 3: Pins & Folders (paired with persistent live 1:1 preview on the right)
                m_lbPins = MakeCtrl(3, L"LISTBOX", L"", LBS_NOTIFY | WS_BORDER | WS_VSCROLL | WS_TABSTOP, px, 78, leftW, 204, ID_LB_PINS);
                int py = 294;
                MakeCtrl(3, L"STATIC", L"Kind:", 0, px, py + 3, 52, 22, 0);
                m_cbPinKind = MakeCtrl(3, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, px + 56, py, 146, 160, ID_CB_PIN_KIND);
                for (const wchar_t* k : { L"App", L"Website", L"File", L"Folder", L"Script", L"Collection", L"RecycleBin" })
                    SendMessageW(m_cbPinKind, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(k));
                MakeCtrl(3, L"STATIC", L"Max Items:", 0, px + 220, py + 3, 82, 22, 0);
                m_edPinFolderMax = MakeCtrl(3, L"EDIT", L"8", WS_BORDER | WS_TABSTOP, px + 306, py, 56, 24, ID_ED_PIN_FOLDER_MAX);
                py += 36;

                MakeCtrl(3, L"STATIC", L"Label:", 0, px, py + 3, 52, 22, 0);
                m_edPinLabel = MakeCtrl(3, L"EDIT", L"", WS_BORDER | WS_TABSTOP, px + 56, py, leftW - 56, 24, ID_ED_PIN_LABEL);
                py += 36;

                MakeCtrl(3, L"STATIC", L"Path:", 0, px, py + 3, 52, 22, 0);
                m_edPinPath = MakeCtrl(3, L"EDIT", L"", WS_BORDER | WS_TABSTOP, px + 56, py, leftW - 216, 24, ID_ED_PIN_PATH);
                MakeCtrl(3, L"BUTTON", L"File...", BS_OWNERDRAW | WS_TABSTOP, px + leftW - 152, py - 1, 72, 28, ID_BTN_BROWSE_FILE);
                MakeCtrl(3, L"BUTTON", L"Folder...", BS_OWNERDRAW | WS_TABSTOP, px + leftW - 74, py - 1, 74, 28, ID_BTN_BROWSE_DIR);
                py += 40;

                const int btnW3 = (leftW - 20) / 3;
                MakeCtrl(3, L"BUTTON", L"Add Slot", BS_OWNERDRAW | WS_TABSTOP, px, py, btnW3, 30, ID_BTN_PIN_ADD);
                MakeCtrl(3, L"BUTTON", L"Update", BS_OWNERDRAW | WS_TABSTOP, px + btnW3 + 10, py, btnW3, 30, ID_BTN_PIN_UPDATE);
                MakeCtrl(3, L"BUTTON", L"Remove", BS_OWNERDRAW | WS_TABSTOP, px + (btnW3 + 10) * 2, py, btnW3, 30, ID_BTN_PIN_REMOVE);
                py += 38;
                MakeCtrl(3, L"BUTTON", L"Move Up", BS_OWNERDRAW | WS_TABSTOP, px, py, btnW3, 30, ID_BTN_PIN_UP);
                MakeCtrl(3, L"BUTTON", L"Move Down", BS_OWNERDRAW | WS_TABSTOP, px + btnW3 + 10, py, btnW3, 30, ID_BTN_PIN_DOWN);

                // Page 4: Advanced (paired with persistent live 1:1 preview on the right)
                y = 80;
                const int col2X = px + leftW / 2 + 6;
                MakeCtrl(4, L"STATIC", L"Ring Radius:", 0, px, y + 3, 108, 22, 0);
                m_edRadius = MakeCtrl(4, L"EDIT", L"", WS_BORDER | WS_TABSTOP, px + 112, y, 74, 24, ID_ED_RADIUS);
                MakeCtrl(4, L"STATIC", L"Deadzone:", 0, col2X, y + 3, 98, 22, 0);
                m_edDeadzone = MakeCtrl(4, L"EDIT", L"", WS_BORDER | WS_TABSTOP, col2X + 102, y, 74, 24, ID_ED_DEADZONE);
                y += 36;

                MakeCtrl(4, L"STATIC", L"Icon Size:", 0, px, y + 3, 108, 22, 0);
                m_edIconSize = MakeCtrl(4, L"EDIT", L"", WS_BORDER | WS_TABSTOP, px + 112, y, 74, 24, ID_ED_ICONSIZE);
                MakeCtrl(4, L"STATIC", L"Preview Scale:", 0, col2X, y + 3, 98, 22, 0);
                m_edThumbScale = MakeCtrl(4, L"EDIT", L"", WS_BORDER | WS_TABSTOP, col2X + 102, y, 74, 24, ID_ED_THUMBSCALE);
                y += 36;

                MakeCtrl(4, L"STATIC", L"Max Slots:", 0, px, y + 3, 108, 22, 0);
                m_edMaxSlots = MakeCtrl(4, L"EDIT", L"", WS_BORDER | WS_TABSTOP, px + 112, y, 74, 24, ID_ED_MAXSLOTS);
                MakeCtrl(4, L"STATIC", L"Accent Hex:", 0, col2X, y + 3, 98, 22, 0);
                m_edAccent = MakeCtrl(4, L"EDIT", L"", WS_BORDER | WS_TABSTOP, col2X + 102, y, 86, 24, ID_ED_ACCENT);
                y += 38;

                MakeCtrl(4, L"STATIC", L"Window View:", 0, px, y + 3, 108, 22, 0);
                m_cbWinView = MakeCtrl(4, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, px + 112, y, leftW - 112, 120, ID_CB_WINVIEW);
                SendMessageW(m_cbWinView, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"OuterArc (Thumbnails)"));
                SendMessageW(m_cbWinView, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"TitleColumn"));
                y += 40;

                m_chkNoAnim = MakeCtrl(4, L"BUTTON", L"Disable Opening Animation (Frame-0 Instant)", BS_AUTOCHECKBOX | WS_TABSTOP, px, y, leftW, 24, ID_CHK_NO_ANIM);
                y += 28;
                m_chkFanOne = MakeCtrl(4, L"BUTTON", L"Fan out live DWM preview even for 1 window", BS_AUTOCHECKBOX | WS_TABSTOP, px, y, leftW, 24, ID_CHK_FANOUT_ONE);
                y += 28;
                m_chkPtrFollow = MakeCtrl(4, L"BUTTON", L"Pointer Follow (160ms glide on focus switch)", BS_AUTOCHECKBOX | WS_TABSTOP, px, y, leftW, 24, ID_CHK_PTR_FOLLOW);
                y += 28;
                m_chkShiftCenter = MakeCtrl(4, L"BUTTON", L"Shift-Center overlay & window on active monitor", BS_AUTOCHECKBOX | WS_TABSTOP, px, y, leftW, 24, ID_CHK_SHIFT_CENTER);
                y += 28;
                m_chkIncludeUnpinned = MakeCtrl(4, L"BUTTON", L"Include unpinned running apps in Ring slots", BS_AUTOCHECKBOX | WS_TABSTOP, px, y, leftW, 24, ID_CHK_INCLUDE_UNPINNED);
                y += 36;

                MakeCtrl(4, L"STATIC", L"Highlight:", 0, px, y + 3, 108, 22, 0);
                m_cbSelIndicator = MakeCtrl(4, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, px + 112, y, leftW - 112, 120, ID_CB_SEL_INDICATOR);
                SendMessageW(m_cbSelIndicator, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Glow Only"));
                SendMessageW(m_cbSelIndicator, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Sector Wedge (v1 Blue)"));
                SendMessageW(m_cbSelIndicator, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Pointer Line"));
                y += 36;

                m_chkNoBlur = MakeCtrl(4, L"BUTTON", L"Disable Acrylic Blur (Ultra-Fast Mode)", BS_AUTOCHECKBOX | WS_TABSTOP, px, y, leftW, 24, ID_CHK_NO_BLUR);

                // Persistent Footer Bar
                const int fy = ch - kFooterH + 13;
                auto footerBtn = [&](const wchar_t* t, int x, int w, int id) {
                    HWND b = CreateWindowExW(0, L"BUTTON", t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                        x, fy, w, 32, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), m_instance, nullptr);
                    SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
                };
                footerBtn(L"Preview Ring", 16, 118, ID_BTN_PREVIEW);
                footerBtn(L"Reset Defaults", 142, 124, ID_BTN_RESET);
                footerBtn(L"Cancel", cw - 332, 92, ID_BTN_CANCEL);
                footerBtn(L"Apply", cw - 232, 92, ID_BTN_APPLY);
                footerBtn(L"Save & Close", cw - 132, 116, ID_BTN_SAVE);

                PopulateControlsFromConfig();
                ShowPage(0);
            }

            void ShowPage(int index)
            {
                m_activeTab = index;
                if (m_lblHeaderTitle) SetWindowTextW(m_lblHeaderTitle, kPageTitles[index]);
                if (m_lblHeaderSub) SetWindowTextW(m_lblHeaderSub, kPageSubtitles[index]);
                for (int t = 0; t < kPageCount; ++t)
                {
                    const int cmd = (t == index) ? SW_SHOW : SW_HIDE;
                    for (HWND c : m_pageCtrls[t])
                        ShowWindow(c, cmd);
                }
                if (m_ringCanvas)
                {
                    const bool showPreview = (index == 0 || index == 3 || index == 4);
                    ShowWindow(m_ringCanvas, showPreview ? SW_SHOW : SW_HIDE);
                    if (showPreview)
                        InvalidateRect(m_ringCanvas, nullptr, FALSE);
                }
                if (index == 0 && m_appearanceCtrl)
                    InvalidateRect(m_appearanceCtrl, nullptr, FALSE);
            }

            void PopulateControlsFromConfig()
            {
                m_populating = true;

                SendMessageW(m_cbMouseTrigger, CB_SETCURSEL, static_cast<WPARAM>(static_cast<int>(m_config.mouseTrigger)), 0);
                SendMessageW(m_cbScrollMode, CB_SETCURSEL, static_cast<WPARAM>(static_cast<int>(m_config.scrollWheelMode)), 0);
                SendMessageW(m_chkDoubleTap, BM_SETCHECK, m_config.doubleTapLastApp ? BST_CHECKED : BST_UNCHECKED, 0);
                SetWindowTextW(m_edTapMs, std::to_wstring(m_config.tapThresholdMs).c_str());
                SetWindowTextW(m_edDTapMs, std::to_wstring(m_config.doubleTapWindowMs).c_str());
                SendMessageW(m_chkHotkey, BM_SETCHECK, m_config.hotkeyEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkModAlt, BM_SETCHECK, (m_config.hotkeyModifiers & MOD_ALT) ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkModCtrl, BM_SETCHECK, (m_config.hotkeyModifiers & MOD_CONTROL) ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkModShift, BM_SETCHECK, (m_config.hotkeyModifiers & MOD_SHIFT) ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkModWin, BM_SETCHECK, (m_config.hotkeyModifiers & MOD_WIN) ? BST_CHECKED : BST_UNCHECKED, 0);

                int vkIdx = 0;
                for (int i = 0; i < static_cast<int>(_countof(kHotkeyChoices)); ++i)
                {
                    if (kHotkeyChoices[i].vk == m_config.hotkeyVk)
                    {
                        vkIdx = i;
                        break;
                    }
                }
                SendMessageW(m_cbHotkeyVk, CB_SETCURSEL, static_cast<WPARAM>(vkIdx), 0);
                SendMessageW(m_chkMenuKey, BM_SETCHECK, m_config.menuKeyEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkWinTab, BM_SETCHECK, m_config.interceptWinTab ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkAltTab, BM_SETCHECK, m_config.interceptAltTab ? BST_CHECKED : BST_UNCHECKED, 0);

                SendMessageW(m_chkWinKey, BM_SETCHECK, m_config.winKeyTrigger ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkDirectSwitch, BM_SETCHECK, m_config.directSwitchEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
                int dsIdx = 0;
                if ((m_config.directSwitchModifiers & MOD_WIN) && m_config.directSwitchVk == 'F') dsIdx = 1;
                else if (m_config.directSwitchVk == 'Q') dsIdx = 2;
                SendMessageW(m_cbDirectSwitchKey, CB_SETCURSEL, static_cast<WPARAM>(dsIdx), 0);

                SendMessageW(m_chkTpPhysClick, BM_SETCHECK, m_config.touchpadPhysLeftClickTrigger ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkTpCorner, BM_SETCHECK, m_config.touchpadCornerTrigger ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_cbTpCornerPos, CB_SETCURSEL, static_cast<WPARAM>(static_cast<int>(m_config.touchpadCornerPos)), 0);
                SetWindowTextW(m_edTpCornerPct, std::to_wstring(m_config.touchpadCornerSizePct).c_str());

                SendMessageW(m_chkShake, BM_SETCHECK, m_config.shakeTriggerEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
                SetWindowTextW(m_edShakeRev, std::to_wstring(m_config.shakeMinReversals).c_str());
                SetWindowTextW(m_edShakeWin, std::to_wstring(m_config.shakeWindowMs).c_str());
                SetWindowTextW(m_edShakeAmp, std::to_wstring(m_config.shakeMinAmplitudeDip).c_str());
                SendMessageW(m_chkShakeFs, BM_SETCHECK, m_config.shakeSuppressFullscreen ? BST_CHECKED : BST_UNCHECKED, 0);

                SendMessageW(m_chkHotCorner, BM_SETCHECK, m_config.hotCornerEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_cbHcPos, CB_SETCURSEL, static_cast<WPARAM>(static_cast<int>(m_config.hotCornerPos)), 0);
                SendMessageW(m_cbHcAct, CB_SETCURSEL, static_cast<WPARAM>(static_cast<int>(m_config.hotCornerAction)), 0);
                SetWindowTextW(m_edHcPress, std::to_wstring(m_config.hotCornerPressureThreshold).c_str());

                SendMessageW(m_chkDragSnap, BM_SETCHECK, m_config.dragToSnapEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkDragSnapClosed, BM_SETCHECK, m_config.dragToSnapLaunchClosed ? BST_CHECKED : BST_UNCHECKED, 0);
                SetWindowTextW(m_edDragSnapHold, std::to_wstring(m_config.dragToSnapHoldMs).c_str());
                SetWindowTextW(m_edDragSnapTearOff, std::to_wstring(m_config.dragToSnapTearOffDip).c_str());

                SetWindowTextW(m_edAccent, m_config.accentColor.c_str());
                wchar_t num[32]{};
                swprintf_s(num, L"%.0f", m_config.ringRadiusDip);
                SetWindowTextW(m_edRadius, num);
                swprintf_s(num, L"%.0f", m_config.deadzoneDip);
                SetWindowTextW(m_edDeadzone, num);
                swprintf_s(num, L"%.0f", m_config.iconSizeDip);
                SetWindowTextW(m_edIconSize, num);
                swprintf_s(num, L"%.2f", m_config.thumbnailScale);
                SetWindowTextW(m_edThumbScale, num);

                SendMessageW(m_cbWinView, CB_SETCURSEL, static_cast<WPARAM>(static_cast<int>(m_config.windowViewMode)), 0);
                SendMessageW(m_chkNoAnim, BM_SETCHECK, m_config.disableOpeningAnimation ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkFanOne, BM_SETCHECK, m_config.fanOutSingleWindow ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkPtrFollow, BM_SETCHECK, m_config.pointerFollow ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkShiftCenter, BM_SETCHECK, m_config.shiftCenterOnActiveMonitor ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkIncludeUnpinned, BM_SETCHECK, m_config.includeUnpinnedRunningApps ? BST_CHECKED : BST_UNCHECKED, 0);
                SetWindowTextW(m_edMaxSlots, std::to_wstring(m_config.maxSlots).c_str());
                SendMessageW(m_cbSelIndicator, CB_SETCURSEL, static_cast<WPARAM>(static_cast<int>(m_config.selectionIndicator)), 0);
                SendMessageW(m_chkNoBlur, BM_SETCHECK, m_config.disableRingBlur ? BST_CHECKED : BST_UNCHECKED, 0);

                SendMessageW(m_chkTopBarOn, BM_SETCHECK, m_config.topBarEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkDockTop, BM_SETCHECK, m_config.topBarDockTop ? BST_CHECKED : BST_UNCHECKED, 0);
                const int barH = (std::clamp)(m_config.topBarHeightDip, 28, 40);
                SendMessageW(m_sldBarH, TBM_SETPOS, TRUE, static_cast<LPARAM>(barH));
                SetWindowTextW(m_edBarH, std::to_wstring(barH).c_str());
                SendMessageW(m_chkKeepFont, BM_SETCHECK, m_config.topBarPreserveFontSize ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkClock1L, BM_SETCHECK, m_config.topBarClockSingleLine ? BST_CHECKED : BST_UNCHECKED, 0);
                SetWindowTextW(m_edClockFmt, m_config.topBarClockFormat.c_str());
                SendMessageW(m_cbClockCasing, CB_SETCURSEL, static_cast<WPARAM>(static_cast<int>(m_config.topBarClockCasing)), 0);

                SendMessageW(m_chkHideApps, BM_SETCHECK, m_config.topBarHideAppButtons ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkHideStart, BM_SETCHECK, m_config.topBarHideStart ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkHideSearch, BM_SETCHECK, m_config.topBarHideSearch ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkHideTaskView, BM_SETCHECK, m_config.topBarHideTaskView ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkHideWidgets, BM_SETCHECK, m_config.topBarHideWidgets ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_cbBackdrop, CB_SETCURSEL, static_cast<WPARAM>(static_cast<int>(m_config.topBarBackdrop)), 0);
                SendMessageW(m_chkAllMon, BM_SETCHECK, m_config.topBarAllMonitors ? BST_CHECKED : BST_UNCHECKED, 0);

                SendMessageW(m_chkFwaOn, BM_SETCHECK, m_config.fullWorkAreaEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkFwaPill, BM_SETCHECK, m_config.fullWorkAreaPillClip ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkFwaHk, BM_SETCHECK, m_config.fullWorkAreaHotkeys ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkMediaOn, BM_SETCHECK, m_config.mediaIslandEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_cbMediaPos, CB_SETCURSEL, static_cast<WPARAM>(static_cast<int>(m_config.mediaIslandPos)), 0);
                SendMessageW(m_cbMediaFs, CB_SETCURSEL, static_cast<WPARAM>(static_cast<int>(m_config.mediaIslandFullscreenMode)), 0);
                SendMessageW(m_chkMediaExpand, BM_SETCHECK, m_config.mediaIslandHoverExpand ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkMediaVol, BM_SETCHECK, m_config.mediaIslandScrollVolume ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkMediaHidePaused, BM_SETCHECK, m_config.mediaIslandHideWhenPaused ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(m_chkMediaMarquee, BM_SETCHECK, m_config.mediaIslandMarquee ? BST_CHECKED : BST_UNCHECKED, 0);
                if (m_btnResetMediaPos)
                    EnableWindow(m_btnResetMediaPos, m_config.mediaIslandOffsetXPct >= 0 ? TRUE : FALSE);

                RefreshPinsList();
                UpdateClockPreview();
                RebuildPreviewSlots();
                if (m_appearanceCtrl) InvalidateRect(m_appearanceCtrl, nullptr, FALSE);
                if (m_ringCanvas) InvalidateRect(m_ringCanvas, nullptr, FALSE);

                m_populating = false;
            }

            void RefreshPinsList()
            {
                SendMessageW(m_lbPins, LB_RESETCONTENT, 0, 0);
                for (const auto& p : m_config.pinnedSlots)
                {
                    const std::wstring row = std::wstring(L"[") + ToString(p.kind) + L"] " +
                        p.label + (p.pathOrUrl.empty() ? L"" : (L" — " + p.pathOrUrl));
                    SendMessageW(m_lbPins, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
                }
                SendMessageW(m_cbPinKind, CB_SETCURSEL, 0, 0);
            }

            void UpdateClockPreview()
            {
                const std::wstring fmt = GetText(m_edClockFmt);
                const int cIdx = static_cast<int>(SendMessageW(m_cbClockCasing, CB_GETCURSEL, 0, 0));
                const ClockCasing casing = (cIdx >= 0 && cIdx <= 3) ? static_cast<ClockCasing>(cIdx) : ClockCasing::CompactLower;
                SYSTEMTIME st{};
                GetLocalTime(&st);
                const std::wstring text = FormatClock(st, fmt, casing);
                SetWindowTextW(m_lblClockPreview, text.c_str());
            }

            void SyncPreviewRelevantFields()
            {
                const std::wstring acc = GetText(m_edAccent);
                if (!acc.empty())
                    m_config.accentColor = acc;
                m_config.ringRadiusDip = (std::clamp)(ParseDoubleOr(GetText(m_edRadius), m_config.ringRadiusDip), 68.0, 220.0);
                m_config.deadzoneDip = (std::clamp)(ParseDoubleOr(GetText(m_edDeadzone), m_config.deadzoneDip), 8.0, 48.0);
                m_config.iconSizeDip = (std::clamp)(ParseDoubleOr(GetText(m_edIconSize), m_config.iconSizeDip), 24.0, 72.0);
                m_config.maxSlots = (std::clamp)(ParseIntOr(GetText(m_edMaxSlots), m_config.maxSlots), 4, 24);
                m_config.includeUnpinnedRunningApps = (SendMessageW(m_chkIncludeUnpinned, BM_GETCHECK, 0, 0) == BST_CHECKED);
                const int selInd = static_cast<int>(SendMessageW(m_cbSelIndicator, CB_GETCURSEL, 0, 0));
                m_config.selectionIndicator = static_cast<SelectionIndicatorStyle>((std::clamp)(selInd, 0, 2));
                m_config.disableRingBlur = (SendMessageW(m_chkNoBlur, BM_GETCHECK, 0, 0) == BST_CHECKED);
                RebuildPreviewSlots();
                if (m_ringCanvas)
                    InvalidateRect(m_ringCanvas, nullptr, FALSE);
            }

            void CollectControlsIntoConfig()
            {
                auto cbIdx = [](HWND c, int maxVal) {
                    const int v = static_cast<int>(SendMessageW(c, CB_GETCURSEL, 0, 0));
                    return (std::clamp)(v, 0, maxVal);
                };
                auto isChk = [](HWND c) { return SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED; };

                m_config.mouseTrigger = static_cast<MouseTriggerButton>(cbIdx(m_cbMouseTrigger, 4));
                m_config.scrollWheelMode = static_cast<ScrollWheelBehavior>(cbIdx(m_cbScrollMode, 2));
                m_config.doubleTapLastApp = isChk(m_chkDoubleTap);
                m_config.tapThresholdMs = (std::clamp)(ParseIntOr(GetText(m_edTapMs), 180), 60, 600);
                m_config.doubleTapWindowMs = (std::clamp)(ParseIntOr(GetText(m_edDTapMs), 280), 100, 900);
                m_config.hotkeyEnabled = isChk(m_chkHotkey);

                UINT mods = 0;
                if (isChk(m_chkModAlt)) mods |= MOD_ALT;
                if (isChk(m_chkModCtrl)) mods |= MOD_CONTROL;
                if (isChk(m_chkModShift)) mods |= MOD_SHIFT;
                if (isChk(m_chkModWin)) mods |= MOD_WIN;
                m_config.hotkeyModifiers = mods;
                m_config.hotkeyVk = kHotkeyChoices[cbIdx(m_cbHotkeyVk, static_cast<int>(_countof(kHotkeyChoices)) - 1)].vk;
                m_config.menuKeyEnabled = isChk(m_chkMenuKey);
                m_config.interceptWinTab = isChk(m_chkWinTab);
                m_config.interceptAltTab = isChk(m_chkAltTab);

                m_config.winKeyTrigger = isChk(m_chkWinKey);
                m_config.directSwitchEnabled = isChk(m_chkDirectSwitch);
                const int dsIdx = cbIdx(m_cbDirectSwitchKey, 2);
                if (dsIdx == 1) { m_config.directSwitchModifiers = MOD_WIN; m_config.directSwitchVk = 'F'; }
                else if (dsIdx == 2) { m_config.directSwitchModifiers = MOD_ALT; m_config.directSwitchVk = 'Q'; }
                else { m_config.directSwitchModifiers = MOD_ALT; m_config.directSwitchVk = 'F'; }

                m_config.touchpadPhysLeftClickTrigger = isChk(m_chkTpPhysClick);
                m_config.touchpadCornerTrigger = isChk(m_chkTpCorner);
                m_config.touchpadCornerPos = static_cast<TouchpadCornerPos>(cbIdx(m_cbTpCornerPos, 1));
                m_config.touchpadCornerSizePct = (std::clamp)(ParseIntOr(GetText(m_edTpCornerPct), 18), 10, 30);

                m_config.shakeTriggerEnabled = isChk(m_chkShake);
                m_config.shakeMinReversals = (std::clamp)(ParseIntOr(GetText(m_edShakeRev), 4), 3, 8);
                m_config.shakeWindowMs = (std::clamp)(ParseIntOr(GetText(m_edShakeWin), 400), 200, 800);
                m_config.shakeMinAmplitudeDip = (std::clamp)(ParseIntOr(GetText(m_edShakeAmp), 32), 15, 80);
                m_config.shakeSuppressFullscreen = isChk(m_chkShakeFs);

                m_config.hotCornerEnabled = isChk(m_chkHotCorner);
                m_config.hotCornerPos = static_cast<HotCornerPos>(cbIdx(m_cbHcPos, 1));
                m_config.hotCornerAction = static_cast<HotCornerAction>(cbIdx(m_cbHcAct, 1));
                m_config.hotCornerPressureThreshold = (std::clamp)(ParseIntOr(GetText(m_edHcPress), 140), 50, 400);

                m_config.dragToSnapEnabled = isChk(m_chkDragSnap);
                m_config.dragToSnapLaunchClosed = isChk(m_chkDragSnapClosed);
                m_config.dragToSnapHoldMs = (std::clamp)(ParseIntOr(GetText(m_edDragSnapHold), 150), 0, 1000);
                m_config.dragToSnapTearOffDip = (std::clamp)(ParseIntOr(GetText(m_edDragSnapTearOff), 36), 12, 160);

                m_config.glassLevel = (std::clamp)(m_config.glassLevel, 0, 100);
                m_config.lightDarkMode = (std::clamp)(m_config.lightDarkMode, 0, 100);

                const std::wstring acc = GetText(m_edAccent);
                if (!acc.empty()) m_config.accentColor = acc;
                m_config.ringRadiusDip = (std::clamp)(ParseDoubleOr(GetText(m_edRadius), 115.0), 68.0, 220.0);
                m_config.deadzoneDip = (std::clamp)(ParseDoubleOr(GetText(m_edDeadzone), 18.0), 8.0, 48.0);
                m_config.iconSizeDip = (std::clamp)(ParseDoubleOr(GetText(m_edIconSize), 44.0), 24.0, 72.0);
                m_config.thumbnailScale = (std::clamp)(ParseDoubleOr(GetText(m_edThumbScale), 1.0), 0.5, 2.0);
                m_config.windowViewMode = static_cast<WindowViewMode>(cbIdx(m_cbWinView, 1));
                m_config.disableOpeningAnimation = isChk(m_chkNoAnim);
                m_config.fanOutSingleWindow = isChk(m_chkFanOne);
                m_config.pointerFollow = isChk(m_chkPtrFollow);
                m_config.shiftCenterOnActiveMonitor = isChk(m_chkShiftCenter);
                m_config.includeUnpinnedRunningApps = isChk(m_chkIncludeUnpinned);
                m_config.maxSlots = (std::clamp)(ParseIntOr(GetText(m_edMaxSlots), 14), 4, 24);
                m_config.selectionIndicator = static_cast<SelectionIndicatorStyle>(cbIdx(m_cbSelIndicator, 2));
                m_config.disableRingBlur = isChk(m_chkNoBlur);

                m_config.topBarEnabled = isChk(m_chkTopBarOn);
                m_config.topBarDockTop = isChk(m_chkDockTop);
                m_config.topBarHeightDip = (std::clamp)(ParseIntOr(GetText(m_edBarH), 30), 28, 40);
                m_config.topBarPreserveFontSize = isChk(m_chkKeepFont);
                m_config.topBarClockSingleLine = isChk(m_chkClock1L);
                m_config.topBarClockFormat = GetText(m_edClockFmt);
                if (m_config.topBarClockFormat.empty()) m_config.topBarClockFormat = L"ddd d MMM  h:mm tt";
                m_config.topBarClockCasing = static_cast<ClockCasing>(cbIdx(m_cbClockCasing, 3));
                m_config.topBarHideAppButtons = isChk(m_chkHideApps);
                m_config.topBarHideStart = isChk(m_chkHideStart);
                m_config.topBarHideSearch = isChk(m_chkHideSearch);
                m_config.topBarHideTaskView = isChk(m_chkHideTaskView);
                m_config.topBarHideWidgets = isChk(m_chkHideWidgets);
                m_config.topBarBackdrop = static_cast<TopBarBackdrop>(cbIdx(m_cbBackdrop, 2));
                m_config.topBarAllMonitors = isChk(m_chkAllMon);

                m_config.fullWorkAreaEnabled = isChk(m_chkFwaOn);
                m_config.fullWorkAreaPillClip = isChk(m_chkFwaPill);
                m_config.fullWorkAreaHotkeys = isChk(m_chkFwaHk);
                m_config.mediaIslandEnabled = isChk(m_chkMediaOn);
                m_config.mediaIslandPos = static_cast<MediaIslandPos>(cbIdx(m_cbMediaPos, 2));
                m_config.mediaIslandFullscreenMode = static_cast<MediaIslandFullscreenMode>(cbIdx(m_cbMediaFs, 2));
                m_config.mediaIslandHoverExpand = isChk(m_chkMediaExpand);
                m_config.mediaIslandScrollVolume = isChk(m_chkMediaVol);
                m_config.mediaIslandHideWhenPaused = isChk(m_chkMediaHidePaused);
                m_config.mediaIslandMarquee = isChk(m_chkMediaMarquee);
            }

            void DrawOwnerButton(const DRAWITEMSTRUCT* dis) const
            {
                if (!dis)
                    return;
                HDC hdc = dis->hDC;
                RECT rc = dis->rcItem;
                const bool primary = (dis->CtlID == ID_BTN_SAVE);
                const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
                const bool disabled = (dis->itemState & ODS_DISABLED) != 0;

                HBRUSH clearBr = CreateSolidBrush(
                    (dis->CtlID >= ID_BTN_APPLY && dis->CtlID <= ID_BTN_PREVIEW) ? kColSidebar : kColBg);
                FillRect(hdc, &rc, clearBr);
                DeleteObject(clearBr);

                COLORREF bgCol = disabled
                    ? RGB(36, 36, 40)
                    : (primary ? (pressed ? RGB(0, 102, 184) : kColAccent)
                               : (pressed ? RGB(58, 58, 66) : RGB(45, 45, 52)));
                COLORREF rimCol = disabled ? RGB(52, 52, 58) : (primary ? RGB(36, 148, 232) : RGB(72, 72, 80));

                HBRUSH bg = CreateSolidBrush(bgCol);
                HPEN pen = CreatePen(PS_SOLID, 1, rimCol);
                HGDIOBJ oldBr = SelectObject(hdc, bg);
                HGDIOBJ oldPen = SelectObject(hdc, pen);
                RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 10, 10);
                SelectObject(hdc, oldBr);
                SelectObject(hdc, oldPen);
                DeleteObject(bg);
                DeleteObject(pen);

                wchar_t txt[128]{};
                GetWindowTextW(dis->hwndItem, txt, _countof(txt));
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, disabled ? kColMuted : (primary ? RGB(255, 255, 255) : kColText));
                HGDIOBJ oldFont = SelectObject(hdc, primary ? m_boldFont : m_font);
                DrawTextW(hdc, txt, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(hdc, oldFont);
            }

            void SaveAndReload()
            {
                CollectControlsIntoConfig();
                if (!m_config.Save())
                {
                    MessageBoxW(m_hwnd, L"Failed writing configuration file.", L"WinRings Settings", MB_ICONERROR | MB_OK);
                    return;
                }
                TriggerHostCommand(L"--reload");
            }

            LRESULT HandleMsg(UINT msg, WPARAM wp, LPARAM lp)
            {
                switch (msg)
                {
                case WM_CREATE:
                    BuildUi();
                    return 0;

                case WM_DRAWITEM:
                    DrawOwnerButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
                    return TRUE;

                case WM_HSCROLL:
                    if (reinterpret_cast<HWND>(lp) == m_sldBarH)
                    {
                        const int v = static_cast<int>(SendMessageW(m_sldBarH, TBM_GETPOS, 0, 0));
                        m_populating = true;
                        SetWindowTextW(m_edBarH, std::to_wstring(v).c_str());
                        m_populating = false;
                        return 0;
                    }
                    break;

                case WM_CTLCOLORSTATIC:
                {
                    HDC hdc = reinterpret_cast<HDC>(wp);
                    HWND ctrl = reinterpret_cast<HWND>(lp);
                    SetTextColor(hdc, (ctrl == m_lblHeaderSub) ? kColMuted : kColText);
                    SetBkColor(hdc, kColBg);
                    return reinterpret_cast<LRESULT>(m_bgBrush);
                }

                case WM_CTLCOLORBTN:
                {
                    HDC hdc = reinterpret_cast<HDC>(wp);
                    SetTextColor(hdc, kColText);
                    SetBkColor(hdc, kColBg);
                    return reinterpret_cast<LRESULT>(m_bgBrush);
                }

                case WM_CTLCOLOREDIT:
                case WM_CTLCOLORLISTBOX:
                {
                    HDC hdc = reinterpret_cast<HDC>(wp);
                    SetTextColor(hdc, kColText);
                    SetBkColor(hdc, kColEdit);
                    return reinterpret_cast<LRESULT>(m_editBrush);
                }

                case WM_ERASEBKGND:
                {
                    RECT rc{};
                    GetClientRect(m_hwnd, &rc);
                    HDC hdc = reinterpret_cast<HDC>(wp);
                    RECT bodyRc{ kSidebarW, 0, rc.right, rc.bottom - kFooterH };
                    FillRect(hdc, &bodyRc, m_bgBrush);
                    RECT footerRc{ 0, rc.bottom - kFooterH, rc.right, rc.bottom };
                    FillRect(hdc, &footerRc, m_footerBrush);
                    HPEN divPen = CreatePen(PS_SOLID, 1, kColBorder);
                    HGDIOBJ oldP = SelectObject(hdc, divPen);
                    MoveToEx(hdc, 0, rc.bottom - kFooterH, nullptr);
                    LineTo(hdc, rc.right, rc.bottom - kFooterH);
                    SelectObject(hdc, oldP);
                    DeleteObject(divPen);
                    return TRUE;
                }

                case WM_COMMAND:
                {
                    const int id = LOWORD(wp);
                    const int code = HIWORD(wp);

                    if (!m_populating)
                    {
                        if ((id == ID_ED_CLOCK_FMT && code == EN_CHANGE) ||
                            (id == ID_CB_CLOCK_CASING && code == CBN_SELCHANGE))
                        {
                            UpdateClockPreview();
                        }
                        else if (id == ID_ED_BAR_H && code == EN_CHANGE)
                        {
                            const int v = (std::clamp)(ParseIntOr(GetText(m_edBarH), 30), 28, 40);
                            SendMessageW(m_sldBarH, TBM_SETPOS, TRUE, static_cast<LPARAM>(v));
                        }
                        else if (((id == ID_ED_RADIUS || id == ID_ED_DEADZONE || id == ID_ED_ICONSIZE ||
                                   id == ID_ED_MAXSLOTS || id == ID_ED_ACCENT) && code == EN_CHANGE) ||
                                 ((id == ID_CHK_INCLUDE_UNPINNED || id == ID_CHK_NO_BLUR) && code == BN_CLICKED) ||
                                 (id == ID_CB_SEL_INDICATOR && code == CBN_SELCHANGE))
                        {
                            SyncPreviewRelevantFields();
                        }
                    }

                    if (id == ID_LB_PINS && code == LBN_SELCHANGE)
                    {
                        const int sel = static_cast<int>(SendMessageW(m_lbPins, LB_GETCURSEL, 0, 0));
                        if (sel >= 0 && sel < static_cast<int>(m_config.pinnedSlots.size()))
                        {
                            const auto& s = m_config.pinnedSlots[static_cast<size_t>(sel)];
                            SendMessageW(m_cbPinKind, CB_SETCURSEL, static_cast<WPARAM>(static_cast<int>(s.kind)), 0);
                            SetWindowTextW(m_edPinLabel, s.label.c_str());
                            SetWindowTextW(m_edPinPath, s.pathOrUrl.c_str());
                            SetWindowTextW(m_edPinFolderMax, std::to_wstring(s.maxFolderItems).c_str());
                        }
                        return 0;
                    }

                    switch (id)
                    {
                    case ID_BTN_APPLY:
                        SaveAndReload();
                        return 0;
                    case ID_BTN_SAVE:
                        SaveAndReload();
                        DestroyWindow(m_hwnd);
                        return 0;
                    case ID_BTN_RESET:
                        if (MessageBoxW(m_hwnd, L"Reset all WinRings settings and default pins?", L"WinRings Settings",
                            MB_YESNO | MB_ICONQUESTION) == IDYES)
                        {
                            m_config = SwitcherConfig::CreateDefault();
                            PopulateControlsFromConfig();
                        }
                        return 0;
                    case ID_BTN_CANCEL:
                        DestroyWindow(m_hwnd);
                        return 0;
                    case ID_BTN_PREVIEW:
                        SaveAndReload();
                        TriggerHostCommand(L"--preview");
                        return 0;
                    case ID_BTN_RESET_MEDIA_POS:
                        m_config.mediaIslandOffsetXPct = -1;
                        if (m_btnResetMediaPos)
                            EnableWindow(m_btnResetMediaPos, FALSE);
                        return 0;

                    case ID_BTN_BROWSE_FILE:
                    {
                        wchar_t fileBuf[MAX_PATH]{};
                        OPENFILENAMEW ofn{ sizeof(ofn) };
                        ofn.hwndOwner = m_hwnd;
                        ofn.lpstrFilter = L"All Files\0*.*\0Executables\0*.exe;*.lnk;*.bat;*.cmd;*.ps1\0";
                        ofn.lpstrFile = fileBuf;
                        ofn.nMaxFile = _countof(fileBuf);
                        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
                        if (GetOpenFileNameW(&ofn))
                            SetWindowTextW(m_edPinPath, fileBuf);
                        return 0;
                    }
                    case ID_BTN_BROWSE_DIR:
                    {
                        BROWSEINFOW bi{};
                        bi.hwndOwner = m_hwnd;
                        bi.lpszTitle = L"Select Folder to Pin";
                        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
                        if (pidl)
                        {
                            wchar_t dirBuf[MAX_PATH]{};
                            if (SHGetPathFromIDListW(pidl, dirBuf))
                                SetWindowTextW(m_edPinPath, dirBuf);
                            CoTaskMemFree(pidl);
                        }
                        return 0;
                    }

                    case ID_BTN_PIN_ADD:
                    case ID_BTN_PIN_UPDATE:
                    {
                        PinnedSlotConfig slot;
                        const int kIdx = (std::clamp)(static_cast<int>(SendMessageW(m_cbPinKind, CB_GETCURSEL, 0, 0)), 0, 6);
                        slot.kind = static_cast<SlotKind>(kIdx);
                        slot.label = GetText(m_edPinLabel);
                        slot.pathOrUrl = GetText(m_edPinPath);
                        slot.maxFolderItems = (std::clamp)(ParseIntOr(GetText(m_edPinFolderMax), 8), 1, 9);
                        if (slot.label.empty())
                            slot.label = ToString(slot.kind);
                        slot.id = L"pin-" + std::to_wstring(GetTickCount64());

                        if (id == ID_BTN_PIN_ADD)
                        {
                            m_config.pinnedSlots.push_back(std::move(slot));
                            RefreshPinsList();
                            SendMessageW(m_lbPins, LB_SETCURSEL, static_cast<WPARAM>(m_config.pinnedSlots.size() - 1), 0);
                        }
                        else
                        {
                            const int sel = static_cast<int>(SendMessageW(m_lbPins, LB_GETCURSEL, 0, 0));
                            if (sel >= 0 && sel < static_cast<int>(m_config.pinnedSlots.size()))
                            {
                                slot.id = m_config.pinnedSlots[static_cast<size_t>(sel)].id;
                                slot.items = m_config.pinnedSlots[static_cast<size_t>(sel)].items;
                                m_config.pinnedSlots[static_cast<size_t>(sel)] = std::move(slot);
                                RefreshPinsList();
                                SendMessageW(m_lbPins, LB_SETCURSEL, static_cast<WPARAM>(sel), 0);
                            }
                        }
                        RebuildPreviewSlots();
                        if (m_ringCanvas) InvalidateRect(m_ringCanvas, nullptr, FALSE);
                        return 0;
                    }
                    case ID_BTN_PIN_REMOVE:
                    {
                        const int sel = static_cast<int>(SendMessageW(m_lbPins, LB_GETCURSEL, 0, 0));
                        if (sel >= 0 && sel < static_cast<int>(m_config.pinnedSlots.size()))
                        {
                            m_config.pinnedSlots.erase(m_config.pinnedSlots.begin() + sel);
                            RefreshPinsList();
                            RebuildPreviewSlots();
                            if (m_ringCanvas) InvalidateRect(m_ringCanvas, nullptr, FALSE);
                        }
                        return 0;
                    }
                    case ID_BTN_PIN_UP:
                    {
                        const int sel = static_cast<int>(SendMessageW(m_lbPins, LB_GETCURSEL, 0, 0));
                        if (sel > 0 && sel < static_cast<int>(m_config.pinnedSlots.size()))
                        {
                            std::swap(m_config.pinnedSlots[static_cast<size_t>(sel)],
                                m_config.pinnedSlots[static_cast<size_t>(sel - 1)]);
                            RefreshPinsList();
                            SendMessageW(m_lbPins, LB_SETCURSEL, static_cast<WPARAM>(sel - 1), 0);
                            RebuildPreviewSlots();
                            if (m_ringCanvas) InvalidateRect(m_ringCanvas, nullptr, FALSE);
                        }
                        return 0;
                    }
                    case ID_BTN_PIN_DOWN:
                    {
                        const int sel = static_cast<int>(SendMessageW(m_lbPins, LB_GETCURSEL, 0, 0));
                        if (sel >= 0 && sel + 1 < static_cast<int>(m_config.pinnedSlots.size()))
                        {
                            std::swap(m_config.pinnedSlots[static_cast<size_t>(sel)],
                                m_config.pinnedSlots[static_cast<size_t>(sel + 1)]);
                            RefreshPinsList();
                            SendMessageW(m_lbPins, LB_SETCURSEL, static_cast<WPARAM>(sel + 1), 0);
                            RebuildPreviewSlots();
                            if (m_ringCanvas) InvalidateRect(m_ringCanvas, nullptr, FALSE);
                        }
                        return 0;
                    }
                    }
                    break;
                }

                case WM_DESTROY:
                    ReleasePreviewGraphics();
                    m_dcRt.Reset();
                    if (m_bgBrush) { DeleteObject(m_bgBrush); m_bgBrush = nullptr; }
                    if (m_cardBrush) { DeleteObject(m_cardBrush); m_cardBrush = nullptr; }
                    if (m_editBrush) { DeleteObject(m_editBrush); m_editBrush = nullptr; }
                    if (m_footerBrush) { DeleteObject(m_footerBrush); m_footerBrush = nullptr; }
                    if (m_font) { DeleteObject(m_font); m_font = nullptr; }
                    if (m_boldFont) { DeleteObject(m_boldFont); m_boldFont = nullptr; }
                    if (m_titleFont) { DeleteObject(m_titleFont); m_titleFont = nullptr; }
                    PostQuitMessage(0);
                    return 0;
                }
                return DefWindowProcW(m_hwnd, msg, wp, lp);
            }

            HINSTANCE m_instance = nullptr;
            HWND m_hwnd = nullptr;
            HWND m_sidebar = nullptr;
            HWND m_appearanceCtrl = nullptr;
            HWND m_lblHeaderTitle = nullptr;
            HWND m_lblHeaderSub = nullptr;
            HBRUSH m_bgBrush = nullptr;
            HBRUSH m_cardBrush = nullptr;
            HBRUSH m_editBrush = nullptr;
            HBRUSH m_footerBrush = nullptr;
            HFONT m_font = nullptr;
            HFONT m_boldFont = nullptr;
            HFONT m_titleFont = nullptr;
            bool m_populating = false;
            int m_activeTab = 0;
            int m_hoverSidebar = -1;
            int m_hoverCard = -1;
            int m_previewSelectedSlot = -1;
            bool m_useDesktopScene = false;
            DragTarget m_dragTarget{};

            std::vector<HWND> m_pageCtrls[kPageCount];
            SwitcherConfig m_config;

            ComPtr<IWICImagingFactory> m_wicFactory;
            ComPtr<ID2D1Factory1> m_d2dFactory;
            ComPtr<IDWriteFactory> m_dwriteFactory;
            ComPtr<IDWriteTextFormat> m_dwHeadingFmt;
            ComPtr<IDWriteTextFormat> m_dwTitleFmt;
            ComPtr<IDWriteTextFormat> m_dwSubFmt;
            ComPtr<IDWriteTextFormat> m_dwCenterFmt;
            ComPtr<IDWriteTextFormat> m_dwGlyphFmt;
            ComPtr<IDWriteTextFormat> m_dwSmallFmt;
            ComPtr<ID2D1DCRenderTarget> m_dcRt;

            // Real D3D11 + D2D1.1 Effect-Graph Preview state
            ComPtr<ID3D11Device> m_d3dDevice;
            ComPtr<IDXGIDevice> m_dxgiDevice;
            ComPtr<ID2D1Device> m_d2dDevice;
            ComPtr<ID2D1DeviceContext> m_previewCtx;
            ComPtr<IDXGISwapChain1> m_previewSwapChain;
            ComPtr<ID2D1Bitmap1> m_previewTargetBmp;
            ComPtr<ID2D1Bitmap1> m_splitSceneBmp;
            ComPtr<ID2D1Bitmap1> m_desktopSceneBmp;
            ComPtr<ID2D1Bitmap1> m_previewNoiseBmp;
            ComPtr<ID2D1BitmapBrush> m_previewNoiseBrush;
            std::vector<BYTE> m_desktopPixels;
            std::vector<PreviewSlotItem> m_previewSlots;
            int m_desktopW = 0;
            int m_desktopH = 0;
            int m_splitW = 0;
            int m_splitH = 0;
            int m_prevSwapW = 0;
            int m_prevSwapH = 0;

            // Controls (Pages 1-4)
            HWND m_cbMouseTrigger{}, m_cbScrollMode{}, m_chkDoubleTap{}, m_edTapMs{}, m_edDTapMs{};
            HWND m_chkHotkey{}, m_chkModAlt{}, m_chkModCtrl{}, m_chkModShift{}, m_chkModWin{}, m_cbHotkeyVk{};
            HWND m_chkMenuKey{}, m_chkWinTab{}, m_chkAltTab{};
            HWND m_chkWinKey{}, m_chkDirectSwitch{}, m_cbDirectSwitchKey{};
            HWND m_chkTpPhysClick{}, m_chkTpCorner{}, m_cbTpCornerPos{}, m_edTpCornerPct{};
            HWND m_chkShake{}, m_edShakeRev{}, m_edShakeWin{}, m_edShakeAmp{}, m_chkShakeFs{};
            HWND m_chkHotCorner{}, m_cbHcPos{}, m_cbHcAct{}, m_edHcPress{};
            HWND m_chkDragSnap{}, m_chkDragSnapClosed{}, m_edDragSnapHold{}, m_edDragSnapTearOff{};

            HWND m_edAccent{}, m_edRadius{}, m_edDeadzone{}, m_edIconSize{}, m_edThumbScale{};
            HWND m_cbWinView{}, m_chkNoAnim{}, m_chkFanOne{}, m_chkPtrFollow{}, m_chkShiftCenter{}, m_chkIncludeUnpinned{};
            HWND m_edMaxSlots{}, m_cbSelIndicator{}, m_chkNoBlur{}, m_ringCanvas{};

            HWND m_chkTopBarOn{}, m_chkDockTop{}, m_sldBarH{}, m_edBarH{}, m_chkKeepFont{}, m_chkClock1L{};
            HWND m_edClockFmt{}, m_cbClockCasing{}, m_lblClockPreview{};
            HWND m_chkHideApps{}, m_chkHideStart{}, m_chkHideSearch{}, m_chkHideTaskView{}, m_chkHideWidgets{};
            HWND m_cbBackdrop{}, m_chkAllMon{};
            HWND m_chkFwaOn{}, m_chkFwaPill{}, m_chkFwaHk{};
            HWND m_chkMediaOn{}, m_cbMediaPos{}, m_cbMediaFs{}, m_chkMediaExpand{}, m_chkMediaVol{};
            HWND m_chkMediaHidePaused{}, m_chkMediaMarquee{}, m_btnResetMediaPos{};

            HWND m_lbPins{}, m_cbPinKind{}, m_edPinLabel{}, m_edPinPath{}, m_edPinFolderMax{};
        };
    } // namespace

    int RunSettingsDialog(HINSTANCE instance)
    {
        SettingsWindow win;
        return win.Run(instance) ? 0 : 1;
    }
} // namespace winrings::tools
