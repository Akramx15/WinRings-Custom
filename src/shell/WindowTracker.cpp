#include "WindowTracker.h"

#include <Dwmapi.h>
#include <Shellapi.h>
#include <algorithm>
#include <cwctype>

#include "../shared/ElevationUtil.h"
#include "../shared/RadialMath.h"
#include "../shared/WinRingsIdentity.h"
#include "ShellController.h"

namespace winrings
{
    WindowTracker* WindowTracker::s_instance = nullptr;

    namespace
    {
        constexpr UINT_PTR kGlideTimerId = 9001;
        constexpr UINT kGlideStepMs = 10;
        constexpr int kGlideTotalSteps = 16;

        std::wstring ToLowerCopy(std::wstring s)
        {
            for (auto& ch : s)
                ch = static_cast<wchar_t>(std::towlower(ch));
            return s;
        }

        std::wstring TryGetProcessImagePath(DWORD pid)
        {
            if (!pid)
                return {};
            HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!proc)
                return {};
            wchar_t buf[1024]{};
            DWORD size = _countof(buf);
            std::wstring out;
            if (QueryFullProcessImageNameW(proc, 0, buf, &size) && size > 0)
                out.assign(buf, size);
            CloseHandle(proc);
            return out;
        }

        std::wstring FileNameOf(const std::wstring& path)
        {
            const size_t slash = path.find_last_of(L"\\/");
            return (slash != std::wstring::npos) ? path.substr(slash + 1) : path;
        }

        std::wstring FileStemOf(const std::wstring& path)
        {
            std::wstring name = FileNameOf(path);
            const size_t dot = name.find_last_of(L'.');
            return (dot != std::wstring::npos) ? name.substr(0, dot) : name;
        }

        struct UwpChildQuery
        {
            DWORD hostPid = 0;
            DWORD realPid = 0;
        };

        BOOL CALLBACK FindUwpChildProc(HWND child, LPARAM lp)
        {
            auto* q = reinterpret_cast<UwpChildQuery*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(child, &pid);
            if (pid && pid != q->hostPid)
            {
                q->realPid = pid;
                return FALSE;
            }
            return TRUE;
        }

        void CenterWindowOnCursorMonitor(HWND hWnd)
        {
            POINT cursorPt{};
            RECT winRect{};
            if (!GetCursorPos(&cursorPt) || !GetWindowRect(hWnd, &winRect))
                return;

            HMONITOR monitor = MonitorFromPoint(cursorPt, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{ sizeof(mi) };
            if (!GetMonitorInfoW(monitor, &mi))
                return;

            const int workW = mi.rcWork.right - mi.rcWork.left;
            const int workH = mi.rcWork.bottom - mi.rcWork.top;
            const int winW = winRect.right - winRect.left;
            const int winH = winRect.bottom - winRect.top;
            const int newW = (std::min)(winW, workW);
            const int newH = (std::min)(winH, workH);
            const int newX = mi.rcWork.left + (workW - newW) / 2;
            const int newY = mi.rcWork.top + (workH - newH) / 2;
            if (!IsWindowElevated(hWnd) ||
                !ShellController::RequestHostWindowAction(
                    WINRINGS_ADMIN_ACT_SET_WINDOW_POS, hWnd, 0, newX, newY, newW, newH, SWP_NOZORDER | SWP_SHOWWINDOW))
            {
                SetWindowPos(hWnd, nullptr, newX, newY, newW, newH, SWP_NOZORDER | SWP_SHOWWINDOW);
            }
        }

        bool IsIgnoredShellClass(const wchar_t* cls) noexcept
        {
            return _wcsicmp(cls, L"Progman") == 0 ||
                _wcsicmp(cls, L"WorkerW") == 0 ||
                _wcsicmp(cls, L"Shell_TrayWnd") == 0 ||
                _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
                _wcsicmp(cls, L"Windows.UI.Core.CoreWindow") == 0 ||
                _wcsicmp(cls, WINRINGS_OVERLAY_CLASS_W) == 0 ||
                _wcsicmp(cls, WINRINGS_THUMBCARD_CLASS_W) == 0 ||
                _wcsicmp(cls, WINRINGS_HOST_WINDOW_CLASS_W) == 0 ||
                _wcsicmp(cls, WINRINGS_SHELL_CONTROL_CLASS_W) == 0;
        }
    } // namespace

    WindowTracker::~WindowTracker()
    {
        Shutdown();
    }

    bool WindowTracker::Initialize(HWND eventWindow)
    {
        m_eventWindow = eventWindow;
        s_instance = this;

        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(m_wicFactory.ReleaseAndGetAddressOf()));

        SeedMruFromEnumWindows();

        if (m_eventWindow)
        {
            m_shellHookRegistered = RegisterShellHookWindow(m_eventWindow) != FALSE;
        }
        m_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNTHREAD);

        HWND fg = GetForegroundWindow();
        if (fg)
            OnWinEventForeground(fg);
        return true;
    }

    void WindowTracker::Shutdown()
    {
        CancelPointerGlide();
        if (m_fgHook)
        {
            UnhookWinEvent(m_fgHook);
            m_fgHook = nullptr;
        }
        if (m_shellHookRegistered && m_eventWindow)
        {
            DeregisterShellHookWindow(m_eventWindow);
            m_shellHookRegistered = false;
        }
        m_iconCache.clear();
        m_mruOrder.clear();
        m_wicFactory.Reset();
        if (s_instance == this)
            s_instance = nullptr;
        m_eventWindow = nullptr;
    }

    void CALLBACK WindowTracker::WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
        LONG idObject, LONG, DWORD, DWORD)
    {
        try
        {
            if (event == EVENT_SYSTEM_FOREGROUND && idObject == OBJID_WINDOW && s_instance && hwnd)
                s_instance->OnWinEventForeground(hwnd);
        }
        catch (...) {}
    }

    void WindowTracker::OnWinEventForeground(HWND hwnd)
    {
        if (m_mruFrozen)
            return;
        if (!hwnd || !IsWindow(hwnd))
            return;
        wchar_t cls[128]{};
        GetClassNameW(hwnd, cls, _countof(cls));
        if (IsIgnoredShellClass(cls))
            return;

        if (hwnd != m_lastActiveWindow)
        {
            if (m_lastActiveWindow && IsWindow(m_lastActiveWindow))
                m_previousActiveWindow = m_lastActiveWindow;
            m_lastActiveWindow = hwnd;
        }
        PromoteInMru(hwnd);
    }

    void WindowTracker::OnShellHookMessage(WPARAM eventCode, LPARAM lParam)
    {
        HWND hwnd = reinterpret_cast<HWND>(lParam);
        if (!hwnd)
            return;
        if (eventCode == HSHELL_WINDOWACTIVATED || eventCode == HSHELL_RUDEAPPACTIVATED)
            OnWinEventForeground(hwnd);
        else if (eventCode == HSHELL_WINDOWDESTROYED)
            RemoveFromMru(hwnd);
        else if (eventCode == HSHELL_WINDOWCREATED)
            PromoteInMru(hwnd);
    }

    void WindowTracker::RecordForegroundBeforeOpen(HWND ignoreOverlayHwnd)
    {
        HWND fg = GetForegroundWindow();
        if (!fg || fg == ignoreOverlayHwnd)
            return;
        OnWinEventForeground(fg);
    }

    void WindowTracker::SwapActiveHistory(HWND activated) noexcept
    {
        if (!activated || activated == m_lastActiveWindow)
            return;
        m_previousActiveWindow = m_lastActiveWindow;
        m_lastActiveWindow = activated;
        PromoteInMru(activated);
    }

    void WindowTracker::PromoteInMru(HWND hwnd)
    {
        RemoveFromMru(hwnd);
        m_mruOrder.insert(m_mruOrder.begin(), hwnd);
        if (m_mruOrder.size() > 128)
            m_mruOrder.resize(128);
    }

    void WindowTracker::RemoveFromMru(HWND hwnd)
    {
        m_mruOrder.erase(std::remove(m_mruOrder.begin(), m_mruOrder.end(), hwnd), m_mruOrder.end());
        if (m_previousActiveWindow == hwnd)
            m_previousActiveWindow = nullptr;
        if (m_lastActiveWindow == hwnd)
            m_lastActiveWindow = m_previousActiveWindow;
    }

    void WindowTracker::SeedMruFromEnumWindows()
    {
        m_mruOrder.clear();
        struct SeedCtx { std::vector<HWND>* list; HWND shell; };
        SeedCtx ctx{ &m_mruOrder, GetShellWindow() };
        EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<SeedCtx*>(lp);
            std::wstring title, exe;
            DWORD pid = 0;
            if (IsEligibleWindow(hwnd, c->shell, nullptr, 0, title, pid, exe))
                c->list->push_back(hwnd);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
    }

    bool WindowTracker::IsEligibleWindow(HWND hwnd, HWND shellWindow, HWND ignoreHwnd,
        DWORD, std::wstring& outTitle, DWORD& outPid, std::wstring& outExePath)
    {
        if (!hwnd || hwnd == shellWindow || hwnd == ignoreHwnd)
            return false;
        if (!IsWindowVisible(hwnd))
            return false;
        if (GetAncestor(hwnd, GA_ROOT) != hwnd)
            return false;

        wchar_t cls[128]{};
        GetClassNameW(hwnd, cls, _countof(cls));
        if (IsIgnoredShellClass(cls))
            return false;

        const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if ((exStyle & WS_EX_TOOLWINDOW) != 0 && (exStyle & WS_EX_APPWINDOW) == 0)
            return false;

        // Require unowned root window unless it explicitly opts into the taskbar/switcher via WS_EX_APPWINDOW.
        HWND owner = GetWindow(hwnd, GW_OWNER);
        if (owner != nullptr && (exStyle & WS_EX_APPWINDOW) == 0)
            return false;

        int cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0)
            return false;

        const int len = GetWindowTextLengthW(hwnd);
        if (len <= 0)
            return false;
        std::wstring title(static_cast<size_t>(len) + 1, L'\0');
        const int actual = GetWindowTextW(hwnd, title.data(), static_cast<int>(title.size()));
        if (actual <= 0)
            return false;
        title.resize(static_cast<size_t>(actual));

        // Trim whitespace
        while (!title.empty() && std::iswspace(title.back())) title.pop_back();
        size_t start = 0;
        while (start < title.size() && std::iswspace(title[start])) ++start;
        if (start > 0) title.erase(0, start);
        if (title.empty())
            return false;

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid)
            return false;

        if (_wcsicmp(cls, L"ApplicationFrameWindow") == 0)
        {
            UwpChildQuery q{ pid, 0 };
            EnumChildWindows(hwnd, FindUwpChildProc, reinterpret_cast<LPARAM>(&q));
            if (q.realPid)
                pid = q.realPid;
        }

        outPid = pid;
        outTitle = std::move(title);
        outExePath = TryGetProcessImagePath(pid);
        return true;
    }

    std::wstring WindowTracker::NormalizeAppKey(const std::wstring& exePath, const std::wstring& fallbackName)
    {
        std::wstring raw = !exePath.empty() ? ToLowerCopy(exePath) : ToLowerCopy(fallbackName);
        for (auto& ch : raw)
        {
            if (ch == L'/')
                ch = L'\\';
        }
        return raw;
    }

    bool WindowTracker::AppKeysMatch(const std::wstring& a, const std::wstring& b)
    {
        if (a.empty() || b.empty())
            return false;
        if (a == b)
            return true;
        const bool aHasSlash = (a.find(L'\\') != std::wstring::npos);
        const bool bHasSlash = (b.find(L'\\') != std::wstring::npos);
        if (!aHasSlash || !bHasSlash)
        {
            const std::wstring nameA = FileNameOf(a);
            const std::wstring nameB = FileNameOf(b);
            return !nameA.empty() && nameA == nameB;
        }
        return false;
    }

    std::vector<AppWindowGroup> WindowTracker::EnumerateGroupedWindows(HWND ignoreHwnd)
    {
        struct EnumCtx
        {
            WindowTracker* self;
            HWND shell;
            HWND ignore;
            std::vector<AppWindowGroup> groups;
            std::unordered_map<std::wstring, size_t> indexByKey;
        } ctx{ this, GetShellWindow(), ignoreHwnd, {}, {} };

        auto addCandidate = [&](HWND hwnd)
        {
            std::wstring title, exePath;
            DWORD pid = 0;
            if (!IsEligibleWindow(hwnd, ctx.shell, ctx.ignore, 0, title, pid, exePath))
                return;

            std::wstring procName = !exePath.empty() ? FileStemOf(exePath) : L"App";
            std::wstring key = NormalizeAppKey(exePath, procName);
            const bool minimized = IsIconic(hwnd) != FALSE;

            auto it = ctx.indexByKey.find(key);
            if (it == ctx.indexByKey.end())
            {
                AppWindowGroup group;
                group.normalizedKey = key;
                group.exePath = exePath;
                group.processName = procName;
                group.iconBitmap = GetWindowIconBitmap(hwnd, exePath);
                group.windows.push_back(WindowEntry{ hwnd, pid, std::move(title), minimized });
                ctx.indexByKey.emplace(key, ctx.groups.size());
                ctx.groups.push_back(std::move(group));
            }
            else
            {
                auto& wins = ctx.groups[it->second].windows;
                for (const auto& existing : wins)
                {
                    if (existing.hwnd == hwnd)
                        return;
                }
                wins.push_back(WindowEntry{ hwnd, pid, std::move(title), minimized });
            }
        };

        // Prune dead windows from MRU and seed in MRU order first.
        std::vector<HWND> aliveMru;
        aliveMru.reserve(m_mruOrder.size());
        for (HWND hwnd : m_mruOrder)
        {
            if (IsWindow(hwnd))
            {
                aliveMru.push_back(hwnd);
                addCandidate(hwnd);
            }
        }
        m_mruOrder.swap(aliveMru);

        // Append any newly discovered Z-order windows not yet in MRU.
        EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
            auto* fn = reinterpret_cast<decltype(addCandidate)*>(lp);
            (*fn)(hwnd);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&addCandidate));

        for (auto& g : ctx.groups)
        {
            std::stable_sort(g.windows.begin(), g.windows.end(),
                [](const WindowEntry& a, const WindowEntry& b) {
                    return !a.isMinimized && b.isMinimized;
                });
        }
        return ctx.groups;
    }

    std::vector<HWND> WindowTracker::GetEligibleMruWindows(HWND ignoreHwnd)
    {
        struct Ctx
        {
            HWND shell;
            HWND ignore;
            std::vector<HWND> result;
            std::unordered_map<HWND, bool> seen;
        } ctx{ GetShellWindow(), ignoreHwnd, {}, {} };

        auto tryAdd = [&](HWND hwnd)
        {
            if (!hwnd || ctx.seen.find(hwnd) != ctx.seen.end())
                return;
            std::wstring title, exePath;
            DWORD pid = 0;
            if (!IsEligibleWindow(hwnd, ctx.shell, ctx.ignore, 0, title, pid, exePath))
                return;
            ctx.seen.emplace(hwnd, true);
            ctx.result.push_back(hwnd);
        };

        if (HWND fg = GetForegroundWindow())
            tryAdd(fg);

        for (HWND hwnd : m_mruOrder)
        {
            if (IsWindow(hwnd))
                tryAdd(hwnd);
        }

        EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
            auto* fn = reinterpret_cast<decltype(tryAdd)*>(lp);
            (*fn)(hwnd);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&tryAdd));

        return ctx.result;
    }

    Microsoft::WRL::ComPtr<IWICBitmapSource> WindowTracker::ConvertHIconToWic(HICON hIcon)
    {
        if (!hIcon || !m_wicFactory)
            return nullptr;

        Microsoft::WRL::ComPtr<IWICBitmap> wicBmp;
        if (FAILED(m_wicFactory->CreateBitmapFromHICON(hIcon, wicBmp.GetAddressOf())) || !wicBmp)
            return nullptr;

        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        if (FAILED(m_wicFactory->CreateFormatConverter(converter.GetAddressOf())) || !converter)
            return nullptr;

        if (FAILED(converter->Initialize(wicBmp.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
            return nullptr;

        return converter;
    }

    Microsoft::WRL::ComPtr<IWICBitmapSource> WindowTracker::GetShellIconBitmap(const std::wstring& path)
    {
        if (path.empty())
            return nullptr;
        const std::wstring cacheKey = L"path:" + ToLowerCopy(path);
        if (auto it = m_iconCache.find(cacheKey); it != m_iconCache.end())
            return it->second;

        SHFILEINFOW shfi{};
        if (!SHGetFileInfoW(path.c_str(), 0, &shfi, sizeof(shfi), SHGFI_ICON | SHGFI_LARGEICON) || !shfi.hIcon)
            return nullptr;

        auto bmp = ConvertHIconToWic(shfi.hIcon);
        DestroyIcon(shfi.hIcon);
        if (bmp)
            m_iconCache.emplace(cacheKey, bmp);
        return bmp;
    }

    Microsoft::WRL::ComPtr<IWICBitmapSource> WindowTracker::GetStockIconBitmap(int stockIconId)
    {
        const std::wstring cacheKey = L"stock:" + std::to_wstring(stockIconId);
        if (auto it = m_iconCache.find(cacheKey); it != m_iconCache.end())
            return it->second;

        SHSTOCKICONINFO sii{ sizeof(sii) };
        if (FAILED(SHGetStockIconInfo(static_cast<SHSTOCKICONID>(stockIconId),
            SHGSI_ICON | SHGSI_LARGEICON, &sii)) || !sii.hIcon)
            return nullptr;

        auto bmp = ConvertHIconToWic(sii.hIcon);
        DestroyIcon(sii.hIcon);
        if (bmp)
            m_iconCache.emplace(cacheKey, bmp);
        return bmp;
    }

    Microsoft::WRL::ComPtr<IWICBitmapSource> WindowTracker::GetWindowIconBitmap(HWND hwnd, const std::wstring& exePath)
    {
        if (!exePath.empty() && _wcsicmp(FileNameOf(exePath).c_str(), L"explorer.exe") == 0)
            return GetShellIconBitmap(exePath);

        if (!exePath.empty())
        {
            const std::wstring cacheKey = L"win:" + ToLowerCopy(exePath);
            if (auto it = m_iconCache.find(cacheKey); it != m_iconCache.end())
                return it->second;
        }

        HICON hIcon = nullptr;
        DWORD_PTR res = 0;
        if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 40, &res) && res)
            hIcon = reinterpret_cast<HICON>(res);
        else if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 40, &res) && res)
            hIcon = reinterpret_cast<HICON>(res);
        if (!hIcon)
            hIcon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON));
        if (!hIcon)
            hIcon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICONSM));

        if (!hIcon)
            return GetShellIconBitmap(exePath);

        auto bmp = ConvertHIconToWic(hIcon);
        if (!bmp)
            return GetShellIconBitmap(exePath);

        if (!exePath.empty())
            m_iconCache.emplace(L"win:" + ToLowerCopy(exePath), bmp);
        return bmp;
    }

    void WindowTracker::ActivateWindow(HWND target, bool centerOnActiveMonitor, bool pointerFollow)
    {
        if (!target || !IsWindow(target))
            return;

        const bool elevated = IsWindowElevated(target);
        if (IsIconic(target))
        {
            if (!elevated || !ShellController::RequestHostWindowAction(WINRINGS_ADMIN_ACT_RESTORE, target))
                ShowWindow(target, SW_RESTORE);
        }

        if (centerOnActiveMonitor)
            CenterWindowOnCursorMonitor(target);

        const bool relayed = elevated &&
            ShellController::RequestHostWindowAction(WINRINGS_ADMIN_ACT_ACTIVATE, target);
        if (!relayed && !SetForegroundWindow(target))
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

        SwapActiveHistory(target);

        if (pointerFollow && m_eventWindow)
        {
            POINT start{};
            RECT rect{};
            if (GetCursorPos(&start) && GetWindowRect(target, &rect))
            {
                const bool alreadyInside = start.x >= rect.left && start.x <= rect.right &&
                    start.y >= rect.top && start.y <= rect.bottom;
                if (!alreadyInside)
                {
                    m_gliding = true;
                    m_glideStart = start;
                    m_glideLastExpected = start;
                    m_glideTarget = { (rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2 };
                    m_glideStep = 0;
                    SetTimer(m_eventWindow, kGlideTimerId, kGlideStepMs, nullptr);
                }
            }
        }
    }

    void WindowTracker::ActivateWindowDirect(HWND target, bool commitToMru)
    {
        if (!target || !IsWindow(target))
            return;

        const bool elevated = IsWindowElevated(target);
        const bool relayed = elevated &&
            ShellController::RequestHostWindowAction(WINRINGS_ADMIN_ACT_ACTIVATE, target);
        if (!relayed)
        {
            if (IsIconic(target))
                ShowWindowAsync(target, SW_RESTORE);

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

        if (commitToMru)
            SwapActiveHistory(target);
    }

    void WindowTracker::CancelPointerGlide() noexcept
    {
        if (m_gliding && m_eventWindow)
            KillTimer(m_eventWindow, kGlideTimerId);
        m_gliding = false;
        m_glideStep = 0;
    }

    void WindowTracker::OnGlideTimerTick() noexcept
    {
        if (!m_gliding)
            return;

        POINT cur{};
        if (!GetCursorPos(&cur))
        {
            CancelPointerGlide();
            return;
        }
        if (std::abs(cur.x - m_glideLastExpected.x) > 6 || std::abs(cur.y - m_glideLastExpected.y) > 6)
        {
            CancelPointerGlide();
            return;
        }

        ++m_glideStep;
        const double t = EaseOutCubic(static_cast<double>(m_glideStep) / static_cast<double>(kGlideTotalSteps));
        const int nextX = static_cast<int>(std::llround(m_glideStart.x + (m_glideTarget.x - m_glideStart.x) * t));
        const int nextY = static_cast<int>(std::llround(m_glideStart.y + (m_glideTarget.y - m_glideStart.y) * t));
        SetCursorPos(nextX, nextY);
        m_glideLastExpected = { nextX, nextY };

        if (m_glideStep >= kGlideTotalSteps)
            CancelPointerGlide();
    }

    void WindowTracker::MinimizeWindow(HWND hwnd) noexcept
    {
        if (!hwnd || !IsWindow(hwnd))
            return;
        if (IsWindowElevated(hwnd) && ShellController::RequestHostWindowAction(WINRINGS_ADMIN_ACT_MINIMIZE, hwnd))
            return;
        ShowWindow(hwnd, SW_MINIMIZE);
    }

    void WindowTracker::CloseWindow(HWND hwnd) noexcept
    {
        if (!hwnd || !IsWindow(hwnd))
            return;
        if (IsWindowElevated(hwnd) && ShellController::RequestHostWindowAction(WINRINGS_ADMIN_ACT_CLOSE, hwnd))
            return;
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }

    void WindowTracker::TerminateProcessByPid(DWORD pid) noexcept
    {
        if (pid == 0 || pid == GetCurrentProcessId())
            return;
        HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (hProc)
        {
            TerminateProcess(hProc, 1);
            CloseHandle(hProc);
            return;
        }
        ShellController::RequestHostWindowAction(WINRINGS_ADMIN_ACT_TERMINATE_PID, nullptr, pid);
    }
} // namespace winrings
