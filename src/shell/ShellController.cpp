#include "ShellController.h"

#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../shared/WinRingsApi.h"
#include "../shared/WinRingsConfig.h"
#include "../shared/WinRingsIdentity.h"
#include "AppIndex.h"
#include "FullWorkAreaCtrl.h"
#include "GestureEngine.h"
#include "InputEngine.h"
#include "MediaIsland.h"
#include "RingOverlay.h"
#include "SlotModel.h"
#include "TopBarHook.h"
#include "WindowSnapCtrl.h"
#include "WindowTracker.h"
#include "XamlTaskbarCtrl.h"

namespace winrings
{
    void ShellDiagLog(const wchar_t* tag, const wchar_t* fmt, ...) noexcept
    {
        try
        {
            static std::mutex s_logMutex;
            std::lock_guard<std::mutex> lock(s_logMutex);

            wchar_t body[1024]{};
            va_list ap{};
            va_start(ap, fmt);
            _vsnwprintf_s(body, _countof(body), _TRUNCATE, fmt ? fmt : L"", ap);
            va_end(ap);

            SYSTEMTIME st{};
            GetLocalTime(&st);
            wchar_t lineW[1280]{};
            _snwprintf_s(lineW, _countof(lineW), _TRUNCATE,
                L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] [pid=%lu tid=%lu] [%s] %s\r\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                GetCurrentProcessId(), GetCurrentThreadId(),
                tag ? tag : L"Shell", body);

            const std::wstring dir = SwitcherConfig::AppDataDirectory();
            if (dir.empty())
                return;
            SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
            const std::wstring logPath = dir + L"\\shell_diag.log";

            DWORD disposition = OPEN_ALWAYS;
            WIN32_FILE_ATTRIBUTE_DATA attr{};
            if (GetFileAttributesExW(logPath.c_str(), GetFileExInfoStandard, &attr))
            {
                const ULONGLONG sz = (static_cast<ULONGLONG>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
                if (sz > 262144ull)
                    disposition = CREATE_ALWAYS;
            }

            HANDLE file = CreateFileW(logPath.c_str(), FILE_APPEND_DATA | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return;

            LARGE_INTEGER zero{};
            SetFilePointerEx(file, zero, nullptr, FILE_END);
            const std::string utf8 = WideToUtf8(lineW);
            if (!utf8.empty())
            {
                DWORD written = 0;
                WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
            }
            CloseHandle(file);
        }
        catch (...) {}
    }

    namespace
    {
        constexpr UINT_PTR kWatchdogTimerId = 1;
        constexpr UINT kWatchdogIntervalMs = 1000;
        constexpr UINT_PTR kTopBarRetryTimerId = 2;
        constexpr UINT kMsgXamlFirstAttached = WM_APP + 300;
        constexpr UINT kMsgShakeTriggered = WM_APP + 301;
        constexpr UINT kMsgStartOpenSnap = WM_APP + 302;
        constexpr UINT kMsgStartClosedSnap = WM_APP + 303;
        constexpr wchar_t kPreviewControlClassW[] = L"WinRings.Preview.Control.v1";

        double QueryPrimaryDpiScale() noexcept
        {
            const POINT zero{ 0, 0 };
            HMONITOR mon = MonitorFromPoint(zero, MONITOR_DEFAULTTOPRIMARY);
            if (HMODULE shcore = GetModuleHandleW(L"shcore.dll"))
            {
                using GetDpiForMonFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
                if (auto fn = reinterpret_cast<GetDpiForMonFn>(GetProcAddress(shcore, "GetDpiForMonitor")))
                {
                    UINT dx = 96, dy = 96;
                    if (SUCCEEDED(fn(mon, 0, &dx, &dy)) && dx >= 72)
                        return static_cast<double>(dx) / 96.0;
                }
            }
            return 1.0;
        }

        bool IsCurrentProcessExplorer() noexcept
        {
            wchar_t path[MAX_PATH]{};
            const DWORD len = GetModuleFileNameW(nullptr, path, _countof(path));
            if (!len || len >= _countof(path))
                return false;
            const wchar_t* slash = wcsrchr(path, L'\\');
            const wchar_t* name = slash ? (slash + 1) : path;
            return _wcsicmp(name, L"explorer.exe") == 0;
        }

        std::wstring ResolveSiblingPath(HMODULE mod, const wchar_t* fileName)
        {
            std::wstring path(MAX_PATH, L'\0');
            DWORD len = GetModuleFileNameW(mod, path.data(), static_cast<DWORD>(path.size()));
            while (len >= path.size() && path.size() < 32768)
            {
                path.resize(path.size() * 2);
                len = GetModuleFileNameW(mod, path.data(), static_cast<DWORD>(path.size()));
            }
            if (!len)
                return {};
            path.resize(len);
            const size_t slash = path.find_last_of(L"\\/");
            if (slash == std::wstring::npos)
                return {};
            return path.substr(0, slash + 1) + fileName;
        }

        void LaunchToolsExecutable(HMODULE mod, const std::wstring& args)
        {
            const std::wstring exe = ResolveSiblingPath(mod, WINRINGS_TOOLS_FILENAME_W);
            if (exe.empty())
                return;
            std::wstring cmdLine = L"\"" + exe + L"\"";
            if (!args.empty())
            {
                cmdLine.push_back(L' ');
                cmdLine += args;
            }
            STARTUPINFOW si{ sizeof(si) };
            PROCESS_INFORMATION pi{};
            std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
            mutableCmd.push_back(L'\0');
            if (CreateProcessW(exe.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE,
                0, nullptr, nullptr, &si, &pi))
            {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }

        POINT GetActiveMonitorCenterPhysical() noexcept
        {
            POINT pt{};
            GetCursorPos(&pt);
            HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{ sizeof(mi) };
            if (GetMonitorInfoW(mon, &mi))
            {
                return POINT{
                    (mi.rcWork.left + mi.rcWork.right) / 2,
                    (mi.rcWork.top + mi.rcWork.bottom) / 2
                };
            }
            return pt;
        }

        class ControllerRuntime
        {
        public:
            ControllerRuntime(HMODULE shellModule, bool previewMode)
                : m_shellModule(shellModule)
                , m_previewMode(previewMode)
            {
            }

            ~ControllerRuntime()
            {
                CleanupHostHandle();
            }

            int Run()
            {
                const HRESULT hrOle = OleInitialize(nullptr);

                m_statusMessage = RegisterWindowMessageW(WINRINGS_SHELL_STATUS_MESSAGE_W);
                m_commandMessage = RegisterWindowMessageW(WINRINGS_SHELL_COMMAND_MESSAGE_W);
                m_shellHookMessage = RegisterWindowMessageW(L"SHELLHOOK");

                const wchar_t* clsName = m_previewMode ? kPreviewControlClassW : WINRINGS_SHELL_CONTROL_CLASS_W;
                WNDCLASSEXW wc{ sizeof(wc) };
                wc.lpfnWndProc = ControlWndProc;
                wc.hInstance = m_shellModule;
                wc.lpszClassName = clsName;
                RegisterClassExW(&wc);

                m_controlHwnd = CreateWindowExW(
                    0, clsName, L"", 0,
                    0, 0, 0, 0,
                    HWND_MESSAGE, nullptr, m_shellModule, this);
                if (!m_controlHwnd)
                {
                    if (SUCCEEDED(hrOle)) OleUninitialize();
                    return 1;
                }

                if (m_previewMode)
                {
                    Arm(true);
                    const POINT center = GetActiveMonitorCenterPhysical();
                    OpenRingAt(center.x, center.y, true);
                }
                else
                {
                    AcquireHostHandshake();
                    SetTimer(m_controlHwnd, kWatchdogTimerId, kWatchdogIntervalMs, nullptr);
                    Arm(false);
                }

                MSG msg{};
                while (GetMessageW(&msg, nullptr, 0, 0) > 0)
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }

                Disarm(false, false);
                if (m_controlHwnd && IsWindow(m_controlHwnd))
                {
                    DestroyWindow(m_controlHwnd);
                    m_controlHwnd = nullptr;
                }
                CleanupHostHandle();
                if (SUCCEEDED(hrOle))
                    OleUninitialize();
                return 0;
            }

        private:
            static LRESULT CALLBACK ControlWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept
            {
                auto* self = reinterpret_cast<ControllerRuntime*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (msg == WM_NCCREATE)
                {
                    self = static_cast<ControllerRuntime*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
                    self->m_controlHwnd = hwnd;
                }
                if (self)
                {
                    try { return self->HandleControlMessage(hwnd, msg, wp, lp); }
                    catch (...) {}
                }
                return DefWindowProcW(hwnd, msg, wp, lp);
            }

            LRESULT HandleControlMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
            {
                if (msg == m_shellHookMessage)
                {
                    if (m_armed)
                    {
                        m_tracker.OnShellHookMessage(wp, lp);
                        if (!m_previewMode)
                        {
                            if (wp == HSHELL_WINDOWACTIVATED || wp == HSHELL_RUDEAPPACTIVATED)
                                m_mediaIsland.OnForegroundChanged();
                            if (wp == HSHELL_WINDOWCREATED || wp == HSHELL_WINDOWACTIVATED || wp == HSHELL_RUDEAPPACTIVATED)
                                m_windowSnap.OnWindowCreatedOrActivated(reinterpret_cast<HWND>(lp));
                        }
                    }
                    return 0;
                }

                if (msg == m_commandMessage && !m_previewMode)
                {
                    switch (static_cast<UINT32>(wp))
                    {
                    case WINRINGS_CMD_TOGGLE_RING:
                        if (m_armed)
                            ToggleStickyRing();
                        return TRUE;

                    case WINRINGS_CMD_EXIT:
                        Disarm(true, true);
                        return TRUE;

                    case WINRINGS_CMD_SUSPEND_INPUT:
                        m_input.SetSuspended(true);
                        if (m_overlay.IsOpen())
                            m_overlay.Dismiss();
                        return TRUE;

                    case WINRINGS_CMD_REFRESH_INPUT:
                        m_input.SetSuspended(false);
                        m_input.ResetTransientState();
                        return TRUE;

                    case WINRINGS_CMD_RELOAD_CONFIG:
                        ReloadConfiguration();
                        return TRUE;

                    case WINRINGS_CMD_TOGGLE_TOPBAR:
                        ToggleTopBar();
                        return TRUE;

                    case WINRINGS_CMD_TOGGLE_MEDIA_ISLAND:
                        ToggleMediaIsland();
                        return TRUE;

                    case WINRINGS_CMD_QUERY_SHELL_STATE:
                        return static_cast<LRESULT>(QueryShellStateBits());

                    case WINRINGS_CMD_REATTACH:
                        HandleReattach(reinterpret_cast<HWND>(lp));
                        return TRUE;

                    default:
                        return FALSE;
                    }
                }

                switch (msg)
                {
                case WM_TIMER:
                    if (wp == 9001)
                    {
                        if (m_armed)
                            m_tracker.OnGlideTimerTick();
                        return 0;
                    }
                    if (wp == kWatchdogTimerId && !m_previewMode)
                    {
                        CheckHostWatchdog();
                        if (m_armed)
                        {
                            m_gestures.EnsureDeviceRegistration();
                            m_input.UpdateCachedDpiScale(QueryPrimaryDpiScale());
                        }
                        return 0;
                    }
                    if (wp == kTopBarRetryTimerId && !m_previewMode)
                    {
                        ApplyTopBarState(false);
                        if (XamlTaskbarCtrl::IsAttached() &&
                            (!TopBarHook::IsHooked() || m_topBarHookAppliedGen == m_topBarDesiredGen))
                        {
                            KillTimer(hwnd, kTopBarRetryTimerId);
                        }
                        return 0;
                    }
                    break;

                case kMsgXamlFirstAttached:
                    if (m_armed && !m_previewMode)
                    {
                        ShellDiagLog(L"ShellCtrl", L"Received kMsgXamlFirstAttached -> forcing TopBar sync");
                        ApplyTopBarState(true);
                    }
                    return 0;

                case kMsgShakeTriggered:
                    if (m_armed && !m_previewMode && !m_overlay.IsOpen())
                    {
                        bool suppressed = false;
                        if (m_config.shakeSuppressFullscreen)
                        {
                            QUERY_USER_NOTIFICATION_STATE quns = QUNS_ACCEPTS_NOTIFICATIONS;
                            if (SUCCEEDED(SHQueryUserNotificationState(&quns)) &&
                                (quns == QUNS_RUNNING_D3D_FULL_SCREEN || quns == QUNS_PRESENTATION_MODE))
                            {
                                suppressed = true;
                            }
                        }
                        if (!suppressed)
                            OpenRingAt(m_pendingShakePt.x, m_pendingShakePt.y, true);
                    }
                    return 0;

                case kMsgStartOpenSnap:
                    if (m_armed && !m_previewMode && m_pendingOpenSnapHwnd)
                    {
                        HWND target = m_pendingOpenSnapHwnd;
                        const POINT pt = m_pendingOpenSnapPt;
                        m_pendingOpenSnapHwnd = nullptr;
                        m_windowSnap.BeginOpenWindowDrag(target, pt);
                    }
                    return 0;

                case kMsgStartClosedSnap:
                    if (m_armed && !m_previewMode && m_hasPendingClosedSnap)
                    {
                        PendingClosedSnap req = std::move(m_pendingClosedSnap);
                        m_hasPendingClosedSnap = false;
                        m_windowSnap.BeginClosedAppDrag(req.kind, std::move(req.label), std::move(req.pathOrUrl), req.screenPt);
                    }
                    return 0;

                case WM_INPUT:
                    if (m_armed && !m_previewMode)
                        m_gestures.OnRawInput(lp);
                    return DefWindowProcW(hwnd, msg, wp, lp);

                case WM_DISPLAYCHANGE:
                case WM_SETTINGCHANGE:
                    if (m_armed && !m_previewMode)
                    {
                        m_input.UpdateCachedDpiScale(QueryPrimaryDpiScale());
                        if (m_config.topBarEnabled)
                            ApplyTopBarState(true);
                        m_mediaIsland.RefreshLayoutAndVisibility();
                    }
                    return 0;

                default:
                    break;
                }
                return DefWindowProcW(hwnd, msg, wp, lp);
            }

            void CleanupHostHandle() noexcept
            {
                if (m_hostProcess)
                {
                    CloseHandle(m_hostProcess);
                    m_hostProcess = nullptr;
                }
            }

            void AcquireHostHandshake(HWND explicitHostWindow = nullptr)
            {
                CleanupHostHandle();
                m_hostWindow = explicitHostWindow;
                m_attemptCookie = 0;

                if (!m_hostWindow)
                {
                    for (int i = 0; i < 15 && !m_hostWindow; ++i)
                    {
                        m_hostWindow = FindWindowW(WINRINGS_HOST_WINDOW_CLASS_W, WINRINGS_HOST_WINDOW_TITLE_W);
                        if (!m_hostWindow)
                            Sleep(40);
                    }
                }
                if (m_hostWindow && IsWindow(m_hostWindow))
                {
                    m_attemptCookie = reinterpret_cast<UINT_PTR>(
                        GetPropW(m_hostWindow, WINRINGS_HOST_ATTEMPT_COOKIE_PROP_W));
                    DWORD hostPid = 0;
                    GetWindowThreadProcessId(m_hostWindow, &hostPid);
                    if (hostPid)
                        m_hostProcess = OpenProcess(SYNCHRONIZE, FALSE, hostPid);
                }
            }

            void PostStatusToHost(WinRingsDllStatus status) noexcept
            {
                if (m_previewMode || !m_hostWindow || !IsWindow(m_hostWindow) || !m_attemptCookie || !m_statusMessage)
                    return;
                const UINT64 packed = (static_cast<UINT64>(GetCurrentProcessId()) << 32) | static_cast<UINT64>(status);
                PostMessageW(m_hostWindow, m_statusMessage,
                    static_cast<WPARAM>(packed),
                    static_cast<LPARAM>(m_attemptCookie));
            }

            void CheckHostWatchdog()
            {
                if (!m_armed)
                    return;
                bool hostGone = false;
                if (!m_hostWindow || !IsWindow(m_hostWindow))
                    hostGone = true;
                else if (m_hostProcess && WaitForSingleObject(m_hostProcess, 0) == WAIT_OBJECT_0)
                    hostGone = true;

                if (hostGone)
                    Disarm(true, false);
            }

            void Arm(bool forPreview)
            {
                if (m_armed)
                    return;

                m_config = SwitcherConfig::LoadOrCreate();
                m_tracker.Initialize(m_controlHwnd);

                RingOverlay::Callbacks overlayCb{};
                overlayCb.launchTools = [this](const std::wstring& args) {
                    LaunchToolsExecutable(m_shellModule, args);
                };
                overlayCb.saveAndReloadConfig = [this](const SwitcherConfig& updated) {
                    updated.Save();
                    m_config = updated;
                    ApplyLoadedConfig();
                };
                overlayCb.onToggleTopBarRequested = [this]() {
                    ToggleTopBar();
                };
                overlayCb.onToggleMediaIslandRequested = [this]() {
                    ToggleMediaIsland();
                };
                overlayCb.onStartOpenWindowSnapDrag = [this](HWND targetHwnd, POINT screenPt) {
                    if (!m_previewMode && m_controlHwnd && targetHwnd)
                    {
                        m_pendingOpenSnapHwnd = targetHwnd;
                        m_pendingOpenSnapPt = screenPt;
                        PostMessageW(m_controlHwnd, kMsgStartOpenSnap, 0, 0);
                    }
                };
                overlayCb.onStartClosedAppSnapDrag = [this](SlotKind kind, std::wstring label, std::wstring pathOrUrl, POINT screenPt) {
                    if (!m_previewMode && m_controlHwnd)
                    {
                        m_pendingClosedSnap = PendingClosedSnap{ kind, std::move(label), std::move(pathOrUrl), screenPt };
                        m_hasPendingClosedSnap = true;
                        PostMessageW(m_controlHwnd, kMsgStartClosedSnap, 0, 0);
                    }
                };
                overlayCb.onExitRequested = [this]() {
                    if (m_previewMode)
                        PostQuitMessage(0);
                    else if (m_controlHwnd)
                        PostMessageW(m_controlHwnd, m_commandMessage, WINRINGS_CMD_EXIT, 0);
                };
                overlayCb.onOverlayClosed = [this]() {
                    m_input.ResetTransientState();
                    m_tracker.CancelPointerGlide();
                    if (m_previewMode)
                        PostQuitMessage(0);
                };

                if (!m_overlay.Initialize(m_shellModule, m_shellModule, &m_tracker, std::move(overlayCb), &m_appIndex))
                {
                    m_tracker.Shutdown();
                    if (!forPreview)
                        PostStatusToHost(WINRINGS_DLL_STATUS_FAILED);
                    return;
                }
                m_appIndex.StartAsyncRefresh();
                m_overlay.SetConfig(m_config);

                InputEngine::Callbacks inputCb{};
                inputCb.isRingOpen = [this]() { return m_overlay.IsOpen(); };
                inputCb.isRingLockedOpen = [this]() { return m_overlay.IsLockedOpen(); };
                inputCb.isRingDragActive = [this]() { return m_overlay.IsDragOrReorderActive(); };
                inputCb.isPointInOverlay = [this](POINT pt) { return m_overlay.IsPointInActiveBounds(pt); };
                inputCb.getOverlayDpiScale = [this]() { return m_overlay.DpiScale(); };
                inputCb.onTriggerPressed = [this](int x, int y) {
                    const bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                    POINT openPt{ x, y };
                    if (shiftHeld && m_config.shiftCenterOnActiveMonitor)
                        openPt = GetActiveMonitorCenterPhysical();
                    OpenRingAt(openPt.x, openPt.y, false);
                };
                inputCb.onTriggerReleased = [this](int x, int y, bool isTap) {
                    if (!m_overlay.IsOpen())
                        return;
                    if (isTap)
                    {
                        m_overlay.LockOpen();
                    }
                    else
                    {
                        m_overlay.UpdatePointer(x, y);
                        m_overlay.CommitSelectionAndClose();
                    }
                };
                inputCb.onDoubleTapLastApp = [this]() {
                    if (m_overlay.IsOpen())
                        m_overlay.Dismiss();
                    HWND prev = m_tracker.PreviousActiveWindow();
                    if (!prev || !IsWindow(prev))
                        prev = m_tracker.LastActiveWindow();
                    if (prev && IsWindow(prev))
                    {
                        const bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                        m_tracker.ActivateWindow(prev,
                            shiftHeld && m_config.shiftCenterOnActiveMonitor,
                            m_config.pointerFollow);
                    }
                };
                inputCb.onMouseMoved = [this](int x, int y) {
                    if (m_overlay.IsOpen())
                        m_overlay.UpdatePointer(x, y);
                };
                inputCb.onMouseWheel = [this](int delta) {
                    if (m_overlay.IsOpen())
                        m_overlay.HandleScrollWheel(delta);
                };
                inputCb.onCancelRequested = [this]() {
                    if (m_overlay.IsOpen())
                        m_overlay.HandleEscapeKey();
                };
                inputCb.onToggleRequested = [this]() {
                    ToggleStickyRing();
                };
                inputCb.onCommitRequested = [this]() {
                    if (m_overlay.IsOpen())
                        m_overlay.CommitSelectionAndClose();
                };
                inputCb.onArrowStep = [this](int delta) {
                    if (m_overlay.IsOpen())
                        m_overlay.HandleScrollWheel(delta);
                };
                inputCb.onFilterChar = [this](wchar_t ch) {
                    if (m_overlay.IsOpen())
                        m_overlay.AppendFilterChar(ch);
                };
                inputCb.onFilterBackspace = [this]() {
                    if (m_overlay.IsOpen())
                        m_overlay.BackspaceFilter();
                };
                inputCb.onUserPointerDelta = [this]() {
                };
                inputCb.onDirectSwitchStep = [this]() {
                    if (m_directSwitchList.empty())
                    {
                        m_tracker.SetMruFrozen(true);
                        m_directSwitchList = m_tracker.GetEligibleMruWindows(m_overlay.Hwnd());
                        if (m_directSwitchList.empty())
                        {
                            m_tracker.SetMruFrozen(false);
                            return;
                        }
                        m_directSwitchIndex = (m_directSwitchList.size() > 1) ? 1u : 0u;
                    }
                    else
                    {
                        m_directSwitchIndex = (m_directSwitchIndex + 1u) % m_directSwitchList.size();
                    }
                    m_tracker.ActivateWindowDirect(m_directSwitchList[m_directSwitchIndex], false);
                };
                inputCb.onDirectSwitchCommit = [this]() {
                    m_tracker.SetMruFrozen(false);
                    if (!m_directSwitchList.empty() && m_directSwitchIndex < m_directSwitchList.size())
                    {
                        if (m_directSwitchList.size() > 1 && m_directSwitchIndex != 0)
                            m_tracker.OnWinEventForeground(m_directSwitchList[0]);
                        m_tracker.OnWinEventForeground(m_directSwitchList[m_directSwitchIndex]);
                    }
                    m_directSwitchList.clear();
                    m_directSwitchIndex = 0;
                };
                inputCb.shouldSuppressTouchpadMouse = [this](UINT mouseMsg) {
                    return !m_previewMode && m_gestures.ShouldSuppressMouseButton(mouseMsg);
                };
                inputCb.onMediaIslandWheel = [this](POINT pt, short delta) {
                    if (m_previewMode || !m_mediaIsland.HitTestScreenPoint(pt))
                        return false;
                    m_mediaIsland.PostExternalWheel(delta);
                    return true;
                };
                inputCb.onShakeDetected = [this](POINT pt) {
                    if (!m_previewMode && m_controlHwnd)
                    {
                        m_pendingShakePt = pt;
                        PostMessageW(m_controlHwnd, kMsgShakeTriggered, 0, 0);
                    }
                };
                inputCb.isSnapFallbackDragActive = [this]() {
                    return !m_previewMode && m_windowSnap.IsFallbackDragActive();
                };
                inputCb.onSnapFallbackDragMove = [this](POINT pt) {
                    if (!m_previewMode)
                        m_windowSnap.OnFallbackDragMove(pt);
                };
                inputCb.onSnapFallbackDragRelease = [this](POINT pt) {
                    return !m_previewMode && m_windowSnap.OnFallbackDragRelease(pt);
                };
                inputCb.onSnapFallbackDragCancel = [this]() {
                    if (!m_previewMode)
                        m_windowSnap.CancelActiveDrag();
                };

                m_input.Install(m_config, std::move(inputCb));
                m_input.UpdateCachedDpiScale(QueryPrimaryDpiScale());
                m_input.SetSuspended(false);
                m_armed = true;

                if (!forPreview)
                {
                    m_windowSnap.Initialize(m_shellModule);
                    m_fullWorkArea.Initialize(m_shellModule, m_config, [this]() {
                        m_mediaIsland.OnTaskbarVisibilityChanged(m_fullWorkArea.IsTaskbarHidden());
                    });
                    MediaIsland::Callbacks mediaCb{};
                    mediaCb.getSystemTrayWidthDip = []() {
                        return XamlTaskbarCtrl::GetSystemTrayWidthDip();
                    };
                    mediaCb.onSaveOffsetXPct = [this](int pct) {
                        m_config.mediaIslandOffsetXPct = pct;
                        m_config.Save();
                        m_mediaIsland.UpdateConfig(m_config);
                    };
                    mediaCb.onToggleEnabled = [this]() {
                        ToggleMediaIsland();
                    };
                    mediaCb.onOpenSettings = [this]() {
                        LaunchToolsExecutable(m_shellModule, L"--settings");
                    };
                    m_mediaIsland.Initialize(m_shellModule, m_config, std::move(mediaCb));
                    m_mediaIsland.OnTaskbarVisibilityChanged(m_fullWorkArea.IsTaskbarHidden());
                    XamlTaskbarCtrl::SetTrayMetricsCallback([this]() {
                        if (!m_previewMode)
                            m_mediaIsland.NotifyLayoutRefresh();
                    });

                    m_gestures.Initialize(m_controlHwnd, m_config, {
                        [this]() { return m_overlay.IsOpen(); },
                        [this]() { ToggleStickyRing(); },
                        [this]() {
                            if (!m_overlay.IsOpen())
                            {
                                POINT pt{};
                                GetCursorPos(&pt);
                                OpenRingAt(pt.x, pt.y, true);
                            }
                        },
                        [this](POINT pt) {
                            return !m_previewMode && m_mediaIsland.HitTestScreenPoint(pt);
                        },
                        [this](short delta) {
                            if (!m_previewMode)
                                m_mediaIsland.PostTouchpadWheel(delta);
                        }
                    });

                    const bool hookOk = m_config.topBarEnabled ? TopBarHook::Install() : false;
                    ShellDiagLog(L"ShellCtrl",
                        L"Arm: Radial Switcher ready (theme=%s glass=%d tone=%d topBarEnabled=%d dockTop=%d heightDip=%d hideApps=%d hookInstall=%d)",
                        ToString(m_config.theme), m_config.glassLevel, m_config.lightDarkMode,
                        m_config.topBarEnabled ? 1 : 0, m_config.topBarDockTop ? 1 : 0,
                        m_config.topBarHeightDip, m_config.topBarHideAppButtons ? 1 : 0,
                        hookOk ? 1 : 0);
                    ShellDiagLog(L"ShellCtrl",
                        L"Custom modules armed: FullWorkArea=%d MediaIsland=%d WinKey=%d DirectSwitch=%d TpClick=%d Shake=%d HotCorner=%d DragSnap=%d",
                        m_config.fullWorkAreaEnabled ? 1 : 0, m_config.mediaIslandEnabled ? 1 : 0,
                        m_config.winKeyTrigger ? 1 : 0, m_config.directSwitchEnabled ? 1 : 0,
                        m_config.touchpadPhysLeftClickTrigger ? 1 : 0, m_config.shakeTriggerEnabled ? 1 : 0,
                        m_config.hotCornerEnabled ? 1 : 0, m_config.dragToSnapEnabled ? 1 : 0);

                    HWND ctrlHwnd = m_controlHwnd;
                    XamlTaskbarCtrl::StartAttachment(m_shellModule, m_config, [ctrlHwnd]() {
                        if (ctrlHwnd && IsWindow(ctrlHwnd))
                            PostMessageW(ctrlHwnd, kMsgXamlFirstAttached, 0, 0);
                    });
                    ++m_topBarDesiredGen;
                    SetTimer(m_controlHwnd, kTopBarRetryTimerId, 1000, nullptr);
                    ApplyTopBarState(true);
                    PostStatusToHost(WINRINGS_DLL_STATUS_READY);
                }
            }

            void Disarm(bool restoreTopBar, bool notifyHost)
            {
                if (!m_armed)
                {
                    if (notifyHost)
                        PostStatusToHost(WINRINGS_DLL_STATUS_DISARMED);
                    return;
                }

                ShellDiagLog(L"ShellCtrl", L"Disarm(restoreTopBar=%d notifyHost=%d)",
                    restoreTopBar ? 1 : 0, notifyHost ? 1 : 0);

                XamlTaskbarCtrl::SetTrayMetricsCallback(nullptr);
                if (m_controlHwnd && !m_previewMode)
                    KillTimer(m_controlHwnd, kTopBarRetryTimerId);

                m_armed = false;
                m_topBarHookAppliedGen = 0;
                m_tracker.SetMruFrozen(false);
                m_directSwitchList.clear();
                m_directSwitchIndex = 0;
                m_pendingOpenSnapHwnd = nullptr;
                m_hasPendingClosedSnap = false;
                m_input.Uninstall();
                if (!m_previewMode)
                {
                    m_windowSnap.Shutdown();
                    m_gestures.Shutdown();
                    m_mediaIsland.Shutdown();
                    m_fullWorkArea.Shutdown();
                }
                if (m_overlay.IsOpen())
                    m_overlay.Dismiss();
                m_overlay.Shutdown();
                m_appIndex.Shutdown();
                m_tracker.Shutdown();

                if (restoreTopBar && !m_previewMode)
                {
                    XamlTaskbarCtrl::RestoreAll();
                    HMODULE shellMod = m_shellModule;
                    TopBarHook::RestoreAndUnhook([shellMod](std::function<void()> fn) {
                        if (XamlTaskbarCtrl::RunOnUiThread(fn, 1500))
                            return true;
                        return TopBarHook::RunOnPrimaryTaskbarThread(shellMod, std::move(fn), 1500);
                    });
                }

                if (notifyHost && !m_previewMode)
                    PostStatusToHost(WINRINGS_DLL_STATUS_DISARMED);
            }

            void HandleReattach(HWND newHostWindow)
            {
                Disarm(true, false);
                AcquireHostHandshake(newHostWindow);
                Arm(false);
            }

            void OpenRingAt(int x, int y, bool lockOpen)
            {
                m_tracker.RecordForegroundBeforeOpen(m_overlay.Hwnd());
                m_appIndex.EnsureFreshAsync();
                auto slots = SlotModel::BuildRingSlots(
                    m_overlay.Hwnd(), m_config, m_tracker, m_tracker.WicFactory(), &m_appIndex);
                m_overlay.OpenAt(x, y, std::move(slots), lockOpen);
            }

            void ToggleStickyRing()
            {
                if (m_overlay.IsOpen())
                {
                    m_overlay.Dismiss();
                    return;
                }
                POINT pt{};
                GetCursorPos(&pt);
                const bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                if (shiftHeld && m_config.shiftCenterOnActiveMonitor)
                    pt = GetActiveMonitorCenterPhysical();
                OpenRingAt(pt.x, pt.y, true);
            }

            void ReloadConfiguration()
            {
                if (!m_armed)
                    return;
                m_config = SwitcherConfig::LoadOrCreate();
                ShellDiagLog(L"ShellCtrl",
                    L"ReloadConfiguration: theme=%s glass=%d tone=%d topBarEnabled=%d dockTop=%d heightDip=%d media=%d",
                    ToString(m_config.theme), m_config.glassLevel, m_config.lightDarkMode,
                    m_config.topBarEnabled ? 1 : 0, m_config.topBarDockTop ? 1 : 0,
                    m_config.topBarHeightDip, m_config.mediaIslandEnabled ? 1 : 0);
                ApplyLoadedConfig();
            }

            void ToggleTopBar()
            {
                if (!m_armed)
                    return;
                const bool prev = m_config.topBarEnabled;
                m_config.topBarEnabled = !m_config.topBarEnabled;
                const bool saved = m_config.Save();
                ShellDiagLog(L"ShellCtrl", L"ToggleTopBar: %d -> %d (saved=%d xamlAttached=%d hooked=%d)",
                    prev ? 1 : 0, m_config.topBarEnabled ? 1 : 0, saved ? 1 : 0,
                    XamlTaskbarCtrl::IsAttached() ? 1 : 0, TopBarHook::IsHooked() ? 1 : 0);
                ApplyLoadedConfig();
            }

            void ToggleMediaIsland()
            {
                if (!m_armed)
                    return;
                const bool prev = m_config.mediaIslandEnabled;
                m_config.mediaIslandEnabled = !m_config.mediaIslandEnabled;
                const bool saved = m_config.Save();
                ShellDiagLog(L"ShellCtrl", L"ToggleMediaIsland: %d -> %d (saved=%d)",
                    prev ? 1 : 0, m_config.mediaIslandEnabled ? 1 : 0, saved ? 1 : 0);
                ApplyLoadedConfig();
            }

            void ApplyLoadedConfig()
            {
                m_overlay.SetConfig(m_config);
                m_input.UpdateConfig(m_config);
                m_input.UpdateCachedDpiScale(QueryPrimaryDpiScale());
                if (!m_previewMode)
                {
                    m_gestures.UpdateConfig(m_config);
                    m_fullWorkArea.UpdateConfig(m_config);
                    m_mediaIsland.UpdateConfig(m_config);
                    ++m_topBarDesiredGen;
                    if (m_config.topBarEnabled && !TopBarHook::IsHooked())
                        TopBarHook::Install();
                    ApplyTopBarState(true);
                }
            }

            void ApplyTopBarState(bool forceHookReapply)
            {
                if (m_previewMode)
                    return;

                XamlTaskbarCtrl::ApplyConfig(m_config);
                if (!XamlTaskbarCtrl::IsAttached())
                    XamlTaskbarCtrl::EnsureAttachRetry(m_shellModule, m_config);

                if (m_config.topBarEnabled && !TopBarHook::IsHooked())
                    TopBarHook::Install();

                if (TopBarHook::IsHooked() && (forceHookReapply || m_topBarHookAppliedGen != m_topBarDesiredGen))
                {
                    const bool en = m_config.topBarEnabled;
                    const bool top = m_config.topBarDockTop;
                    const int h = m_config.topBarHeightDip;
                    const bool allMon = m_config.topBarAllMonitors;
                    auto applied = std::make_shared<std::atomic<bool>>(false);
                    auto applyFn = [en, top, h, allMon, applied]() {
                        if (TopBarHook::ApplyOnTaskbarUiThread(en, top, h, allMon))
                            applied->store(true, std::memory_order_release);
                    };

                    const wchar_t* dispatchVia = L"Failed";
                    bool dispatched = false;
                    if (XamlTaskbarCtrl::RunOnUiThread(applyFn, 1500))
                    {
                        dispatched = true;
                        dispatchVia = L"XamlUiThread";
                    }
                    else if (TopBarHook::RunOnPrimaryTaskbarThread(m_shellModule, applyFn, 1500))
                    {
                        dispatched = true;
                        dispatchVia = L"TrayThreadFallback";
                    }

                    const bool appliedOk = dispatched && applied->load(std::memory_order_acquire);
                    ShellDiagLog(L"ShellCtrl",
                        L"ApplyTopBarState: en=%d top=%d h=%d xamlAttached=%d dispatch=%s applied=%d",
                        en ? 1 : 0, top ? 1 : 0, h, XamlTaskbarCtrl::IsAttached() ? 1 : 0, dispatchVia,
                        appliedOk ? 1 : 0);

                    if (appliedOk)
                        m_topBarHookAppliedGen = m_topBarDesiredGen;
                }

                if (!XamlTaskbarCtrl::IsAttached() ||
                    (TopBarHook::IsHooked() && m_topBarHookAppliedGen != m_topBarDesiredGen))
                {
                    SetTimer(m_controlHwnd, kTopBarRetryTimerId, 1000, nullptr);
                }
            }

            UINT32 QueryShellStateBits() const noexcept
            {
                UINT32 bits = 0;
                if (m_armed)
                    bits |= WINRINGS_STATE_RING_READY;
                bits |= XamlTaskbarCtrl::GetStateBits();
                if (TopBarHook::IsHooked())
                    bits |= WINRINGS_STATE_TOPBAR_HOOKED;
                if (m_mediaIsland.IsActiveVisible())
                    bits |= WINRINGS_STATE_MEDIA_ISLAND_ACTIVE;
                return bits;
            }

            HMODULE m_shellModule = nullptr;
            bool m_previewMode = false;
            bool m_armed = false;

            HWND m_controlHwnd = nullptr;
            HWND m_hostWindow = nullptr;
            HANDLE m_hostProcess = nullptr;
            UINT_PTR m_attemptCookie = 0;

            UINT m_statusMessage = 0;
            UINT m_commandMessage = 0;
            UINT m_shellHookMessage = 0;
            uint64_t m_topBarDesiredGen = 0;
            uint64_t m_topBarHookAppliedGen = 0;

            struct PendingClosedSnap
            {
                SlotKind kind = SlotKind::App;
                std::wstring label;
                std::wstring pathOrUrl;
                POINT screenPt{};
            };

            SwitcherConfig m_config;
            AppIndex m_appIndex;
            WindowTracker m_tracker;
            RingOverlay m_overlay;
            InputEngine m_input;
            FullWorkAreaCtrl m_fullWorkArea;
            GestureEngine m_gestures;
            MediaIsland m_mediaIsland;
            WindowSnapCtrl m_windowSnap;
            std::vector<HWND> m_directSwitchList;
            size_t m_directSwitchIndex = 0;
            POINT m_pendingShakePt{};
            HWND m_pendingOpenSnapHwnd = nullptr;
            POINT m_pendingOpenSnapPt{};
            PendingClosedSnap m_pendingClosedSnap;
            bool m_hasPendingClosedSnap = false;
        };

        std::atomic<bool> g_explorerStarted{ false };

        DWORD WINAPI ExplorerUiThreadProc(void* param)
        {
            HMODULE shellMod = static_cast<HMODULE>(param);
            ControllerRuntime runtime(shellMod, false);
            return static_cast<DWORD>(runtime.Run());
        }
    } // namespace

    void ShellController::BootstrapInExplorer(HMODULE shellModule)
    {
        if (!IsCurrentProcessExplorer())
            return;

        if (g_explorerStarted.exchange(true, std::memory_order_acq_rel))
            return;

        // Pin WinRingsShell.dll inside explorer.exe for its lifetime; --exit only disarms.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&ShellController::BootstrapInExplorer),
            &pinned);

        HANDLE th = CreateThread(nullptr, 0, ExplorerUiThreadProc, shellModule, 0, nullptr);
        if (th)
            CloseHandle(th);
        else
            g_explorerStarted.store(false, std::memory_order_release);
    }

    int ShellController::RunStandalonePreview(HMODULE shellModule)
    {
        ControllerRuntime runtime(shellModule, true);
        return runtime.Run();
    }

    bool ShellController::RequestShutdownFromApi(HWND controlWindow) noexcept
    {
        if (!controlWindow || !IsWindow(controlWindow))
            return false;
        const UINT cmdMsg = RegisterWindowMessageW(WINRINGS_SHELL_COMMAND_MESSAGE_W);
        if (!cmdMsg)
            return false;
        return PostMessageW(controlWindow, cmdMsg, WINRINGS_CMD_EXIT, 0) != FALSE;
    }
} // namespace winrings
