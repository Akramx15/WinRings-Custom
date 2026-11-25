#include <Windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <wtsapi32.h>

#pragma comment(lib, "gdi32.lib")

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "../shared/CrashGuard.h"
#include "../shared/ElevationUtil.h"
#include "../shared/WinRingsApi.h"
#include "../shared/WinRingsConfig.h"
#include "../shell/GestureEngine.h"
#include "../shell/InputEngine.h"

using namespace winrings;

namespace
{
    constexpr UINT_PTR kRetryTimer = 1;
    constexpr UINT_PTR kInjectionTimer = 2;
    constexpr UINT_PTR kStopTimer = 3;
    constexpr UINT_PTR kTrayRetryTimer = 4;
    constexpr UINT_PTR kAdminWatchdogTimer = 5;
    constexpr UINT_PTR kAdminConfigSyncTimer = 6;
    constexpr DWORD kInjectionTimeoutMs = 6000;
    constexpr DWORD kStopTimeoutMs = 5500;
    constexpr DWORD kRetryDelayMs[] = { 100, 250, 500, 1000, 2000, 4000, 4000 };

    constexpr UINT kTrayCallbackMsg = WM_APP + 100;
    constexpr UINT kMsgAdminShakeTriggered = WM_APP + 101;
    constexpr UINT kTrayIconId = 1;
    constexpr UINT kMenuCmdOpenRing = 1001;
    constexpr UINT kMenuCmdToggleTopBar = 1002;
    constexpr UINT kMenuCmdSettings = 1003;
    constexpr UINT kMenuCmdReload = 1004;
    constexpr UINT kMenuCmdConfigFolder = 1005;
    constexpr UINT kMenuCmdExit = 1006;
    constexpr UINT kMenuCmdToggleMediaIsland = 1007;

    constexpr wchar_t kSafetyDisabledValue[] = L"InjectionDisabled";
    constexpr wchar_t kCrashCountValue[] = L"AbnormalExplorerExitCount";
    constexpr wchar_t kCrashWindowValue[] = L"AbnormalExplorerWindowStart";
    constexpr wchar_t kCrashTimesValue[] = L"AbnormalExplorerExitTimes";
    constexpr wchar_t kLastExitCodeValue[] = L"LastExplorerExitCode";

    using WTSRegisterSessionNotificationFn = BOOL (WINAPI*)(HWND, DWORD);
    using WTSUnregisterSessionNotificationFn = BOOL (WINAPI*)(HWND);

    class UniqueHandle
    {
    public:
        UniqueHandle() noexcept = default;
        explicit UniqueHandle(HANDLE value) noexcept : m_value(value) {}
        ~UniqueHandle() { reset(); }
        UniqueHandle(const UniqueHandle&) = delete;
        UniqueHandle& operator=(const UniqueHandle&) = delete;
        UniqueHandle(UniqueHandle&& other) noexcept : m_value(other.release()) {}
        UniqueHandle& operator=(UniqueHandle&& other) noexcept
        {
            if (this != &other)
                reset(other.release());
            return *this;
        }
        HANDLE get() const noexcept { return m_value; }
        explicit operator bool() const noexcept { return m_value && m_value != INVALID_HANDLE_VALUE; }
        HANDLE release() noexcept
        {
            HANDLE v = m_value;
            m_value = nullptr;
            return v;
        }
        void reset(HANDLE value = nullptr) noexcept
        {
            if (m_value && m_value != INVALID_HANDLE_VALUE)
                CloseHandle(m_value);
            m_value = value;
        }

    private:
        HANDLE m_value = nullptr;
    };

    struct ExplorerTarget
    {
        HWND taskbar = nullptr;
        HWND shellWindow = nullptr;
        DWORD processId = 0;
        DWORD threadId = 0;
        UniqueHandle process;

        void clear()
        {
            taskbar = nullptr;
            shellWindow = nullptr;
            processId = 0;
            threadId = 0;
            process.reset();
        }
    };

    struct HostRuntime
    {
        HINSTANCE instance = nullptr;
        HWND window = nullptr;
        HWND shellControlWindow = nullptr;
        UINT hostControlMessage = 0;
        UINT shellStatusMessage = 0;
        UINT shellCommandMessage = 0;
        UINT taskbarCreatedMessage = 0;
        WinRingsHostStatus status = WINRINGS_HOST_STATUS_STARTING;
        HMODULE shellDll = nullptr;
        const WinRingsApi* api = nullptr;
        ExplorerTarget explorer;
        size_t retryIndex = 0;
        bool pendingToggle = false;
        bool exitPending = false;
        bool activationCommitted = false;
        bool initializationPending = false;
        bool replacementPending = false;
        bool safetyDisabled = false;
        std::array<ULONGLONG, kCrashLoopThreshold> abnormalExplorerExitTimes{};
        DWORD abnormalExplorerExitCount = 0;
        bool sessionInputInteractive = true;
        bool powerInputInteractive = true;
        bool interactiveInput = true;
        ULONGLONG inputGeneration = 0;
        ULONGLONG injectionInputGeneration = 0;
        ULONGLONG appliedInputGeneration = 0;
        bool inputStateApplied = false;
        bool sessionEnding = false;
        bool authenticatedDisarmed = false;
        HMODULE wtsApi = nullptr;
        WTSRegisterSessionNotificationFn registerSessionNotification = nullptr;
        WTSUnregisterSessionNotificationFn unregisterSessionNotification = nullptr;
        bool sessionNotificationRegistered = false;
        UINT_PTR attemptCookie = 0;
        bool trayAdded = false;
        HICON trayIcon = nullptr;
        unsigned trayRetryCount = 0;
        bool isElevated = false;
        bool adminBridgeArmed = false;
        SwitcherConfig config;
        InputEngine adminInput;
        GestureEngine adminGestures;
        POINT pendingAdminShakePt{};
    };

    HostRuntime g_runtime;

    void DebugLog(const wchar_t* text)
    {
        std::wstring line = L"WinRingsHost: ";
        line += text;
        line += L"\r\n";
        OutputDebugStringW(line.c_str());
    }

    void HostDiagLog(const wchar_t* fmt, ...) noexcept
    {
        try
        {
            wchar_t body[512]{};
            va_list ap{};
            va_start(ap, fmt);
            _vsnwprintf_s(body, _countof(body), _TRUNCATE, fmt ? fmt : L"", ap);
            va_end(ap);

            SYSTEMTIME st{};
            GetLocalTime(&st);
            wchar_t lineW[768]{};
            _snwprintf_s(lineW, _countof(lineW), _TRUNCATE,
                L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] [pid=%lu tid=%lu] [Host] %s\r\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                GetCurrentProcessId(), GetCurrentThreadId(), body);

            const std::wstring dir = SwitcherConfig::AppDataDirectory();
            if (dir.empty())
                return;
            SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
            const std::wstring logPath = dir + L"\\shell_diag.log";

            HANDLE file = CreateFileW(logPath.c_str(), FILE_APPEND_DATA | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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

    double QueryHostPrimaryDpiScale() noexcept
    {
        using GetDpiForMonFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        static GetDpiForMonFn pGetDpiForMon = []() -> GetDpiForMonFn {
            HMODULE shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            return shcore ? reinterpret_cast<GetDpiForMonFn>(GetProcAddress(shcore, "GetDpiForMonitor")) : nullptr;
        }();
        if (pGetDpiForMon)
        {
            const POINT zero{ 0, 0 };
            HMONITOR mon = MonitorFromPoint(zero, MONITOR_DEFAULTTOPRIMARY);
            UINT dx = 96, dy = 96;
            if (SUCCEEDED(pGetDpiForMon(mon, 0, &dx, &dy)) && dx >= 72)
                return static_cast<double>(dx) / 96.0;
        }
        return 1.0;
    }

    void ForceForegroundElevated(HWND target) noexcept
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

    bool IsOverlayWindowVisible() noexcept
    {
        HWND overlay = FindWindowW(WINRINGS_OVERLAY_CLASS_W, nullptr);
        return overlay && IsWindowVisible(overlay) != FALSE;
    }

    bool IsPointOverMediaIslandBar(POINT pt) noexcept
    {
        if (!g_runtime.config.mediaIslandEnabled || !g_runtime.config.mediaIslandScrollVolume)
            return false;
        HWND island = FindWindowW(L"WinRings.MediaIsland.v1", nullptr);
        if (!island || !IsWindowVisible(island))
            return false;
        RECT rc{};
        if (!GetWindowRect(island, &rc))
            return false;

        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        LONG hitTop = rc.top;
        LONG hitBottom = rc.bottom;
        if (GetMonitorInfoW(mon, &mi))
        {
            const double scale = QueryHostPrimaryDpiScale();
            const int barDip = (std::clamp)(g_runtime.config.topBarHeightDip, 24, 48);
            const int barPx = static_cast<int>(std::lround(static_cast<double>(barDip) * scale));
            hitTop = (std::min)(rc.top, mi.rcMonitor.top);
            hitBottom = (std::max)(rc.bottom, mi.rcMonitor.top + barPx);
        }
        return pt.x >= rc.left && pt.x <= rc.right &&
            pt.y >= hitTop && pt.y <= hitBottom;
    }

    bool ShouldAdminBridgeHandle(POINT pt, bool isKeyboardOrRaw) noexcept
    {
        const bool shellReady = (g_runtime.status == WINRINGS_HOST_STATUS_READY);
        if (!g_runtime.isElevated || !shellReady || !g_runtime.interactiveInput)
            return false;
        if (IsOverlayWindowVisible())
            return false;
        const bool fgElev = IsWindowElevated(GetForegroundWindow());
        const bool cursorElev = isKeyboardOrRaw ? false : IsWindowElevated(WindowFromPoint(pt));
        return ShouldAdminBridgeInterceptInput(
            g_runtime.isElevated,
            shellReady,
            g_runtime.interactiveInput,
            false,
            fgElev,
            cursorElev,
            isKeyboardOrRaw);
    }

    ULONGLONG CurrentUtcFileTime()
    {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER u{};
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        return u.QuadPart;
    }

    class SafetyMutationLock
    {
    public:
        bool acquire()
        {
            HANDLE token = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
                return false;
            UniqueHandle tokenHandle(token);
            DWORD bytes = 0;
            GetTokenInformation(tokenHandle.get(), TokenUser, nullptr, 0, &bytes);
            if (!bytes || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
                return false;
            std::vector<BYTE> storage(bytes);
            if (!GetTokenInformation(tokenHandle.get(), TokenUser, storage.data(), bytes, &bytes))
                return false;
            const auto* user = reinterpret_cast<const TOKEN_USER*>(storage.data());
            if (!IsValidSid(user->User.Sid))
                return false;
            LPWSTR sidText = nullptr;
            if (!ConvertSidToStringSidW(user->User.Sid, &sidText) || !sidText)
                return false;
            std::wstring name = WINRINGS_HOST_SAFETY_MUTEX_PREFIX_W;
            name += sidText;
            LocalFree(sidText);
            m_mutex.reset(CreateMutexW(nullptr, FALSE, name.c_str()));
            if (!m_mutex)
                return false;
            const DWORD wait = WaitForSingleObject(m_mutex.get(), 2000);
            m_owned = (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED);
            return m_owned;
        }

        ~SafetyMutationLock()
        {
            if (m_owned)
                ReleaseMutex(m_mutex.get());
        }

        SafetyMutationLock() = default;
        SafetyMutationLock(const SafetyMutationLock&) = delete;
        SafetyMutationLock& operator=(const SafetyMutationLock&) = delete;

    private:
        UniqueHandle m_mutex;
        bool m_owned = false;
    };

    void ApplySafetyStateToRuntime(const CrashGuardState& state)
    {
        g_runtime.safetyDisabled = state.disabled;
        g_runtime.abnormalExplorerExitTimes = state.abnormalExitTimes;
        g_runtime.abnormalExplorerExitCount = state.abnormalExitCount;
    }

    bool ReadSafetyStateLocked(CrashGuardState& state)
    {
        state = kEmptyCrashGuard;
        HKEY key = nullptr;
        const LONG openResult = RegOpenKeyExW(HKEY_CURRENT_USER, WINRINGS_HOST_SAFETY_REGISTRY_W,
            0, KEY_QUERY_VALUE, &key);
        if (openResult == ERROR_FILE_NOT_FOUND || openResult == ERROR_PATH_NOT_FOUND)
            return true;
        if (openResult != ERROR_SUCCESS)
            return false;

        DWORD value = 0;
        DWORD type = 0;
        DWORD size = sizeof(value);
        if (RegQueryValueExW(key, kSafetyDisabledValue, nullptr, &type,
            reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS && type == REG_DWORD && size == sizeof(value))
            state.disabled = value != 0;

        std::array<ULONGLONG, kCrashLoopThreshold> timestamps{};
        type = 0;
        size = static_cast<DWORD>(sizeof(timestamps));
        if (RegQueryValueExW(key, kCrashTimesValue, nullptr, &type,
            reinterpret_cast<BYTE*>(timestamps.data()), &size) == ERROR_SUCCESS &&
            type == REG_BINARY && size == sizeof(timestamps))
        {
            for (ULONGLONG ts : timestamps)
            {
                if (ts && state.abnormalExitCount < state.abnormalExitTimes.size())
                    state.abnormalExitTimes[state.abnormalExitCount++] = ts;
            }
        }
        RegCloseKey(key);
        return true;
    }

    void LoadSafetyState()
    {
        CrashGuardState state = kEmptyCrashGuard;
        SafetyMutationLock lock;
        if (!lock.acquire() || !ReadSafetyStateLocked(state))
        {
            state.disabled = true;
            ApplySafetyStateToRuntime(state);
            DebugLog(L"could not serialize HostSafety state; injection remains disabled");
            return;
        }
        state = AdvanceCrashGuard(state, false, CurrentUtcFileTime());
        ApplySafetyStateToRuntime(state);
    }

    bool PersistSafetyStateLocked(const CrashGuardState& state, DWORD lastExitCode)
    {
        HKEY key = nullptr;
        DWORD disposition = 0;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, WINRINGS_HOST_SAFETY_REGISTRY_W, 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &key, &disposition) != ERROR_SUCCESS)
            return false;

        const DWORD disabled = state.disabled ? 1u : 0u;
        const ULONGLONG windowStart = state.abnormalExitCount ? state.abnormalExitTimes[0] : 0;
        const LONG r1 = RegSetValueExW(key, kSafetyDisabledValue, 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&disabled), sizeof(disabled));
        const LONG r2 = RegSetValueExW(key, kCrashCountValue, 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&state.abnormalExitCount), sizeof(state.abnormalExitCount));
        const LONG r3 = RegSetValueExW(key, kCrashWindowValue, 0, REG_QWORD,
            reinterpret_cast<const BYTE*>(&windowStart), sizeof(windowStart));
        const LONG r4 = RegSetValueExW(key, kLastExitCodeValue, 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&lastExitCode), sizeof(lastExitCode));
        const LONG r5 = RegSetValueExW(key, kCrashTimesValue, 0, REG_BINARY,
            reinterpret_cast<const BYTE*>(state.abnormalExitTimes.data()),
            static_cast<DWORD>(sizeof(state.abnormalExitTimes)));
        RegCloseKey(key);
        return r1 == ERROR_SUCCESS && r2 == ERROR_SUCCESS && r3 == ERROR_SUCCESS &&
            r4 == ERROR_SUCCESS && r5 == ERROR_SUCCESS;
    }

    bool ResetSafetyState()
    {
        SafetyMutationLock lock;
        if (!lock.acquire())
            return false;
        const LONG res = RegDeleteTreeW(HKEY_CURRENT_USER, WINRINGS_HOST_SAFETY_REGISTRY_W);
        if (res != ERROR_SUCCESS && res != ERROR_FILE_NOT_FOUND && res != ERROR_PATH_NOT_FOUND)
            return false;
        g_runtime.safetyDisabled = false;
        g_runtime.abnormalExplorerExitTimes.fill(0);
        g_runtime.abnormalExplorerExitCount = 0;
        return true;
    }

    bool RecordExplorerExitForCrashGuard(bool countAbnormalExit, DWORD exitCode)
    {
        SafetyMutationLock lock;
        if (!lock.acquire())
        {
            g_runtime.safetyDisabled = true;
            return true;
        }
        CrashGuardState state = kEmptyCrashGuard;
        if (!ReadSafetyStateLocked(state))
        {
            g_runtime.safetyDisabled = true;
            return true;
        }
        state = AdvanceCrashGuard(state, countAbnormalExit, CurrentUtcFileTime());
        if (!PersistSafetyStateLocked(state, exitCode))
        {
            g_runtime.safetyDisabled = true;
            return true;
        }
        ApplySafetyStateToRuntime(state);
        return state.disabled;
    }

    bool RegisterSessionNotifications()
    {
        g_runtime.wtsApi = LoadLibraryExW(L"wtsapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!g_runtime.wtsApi)
            return false;
        g_runtime.registerSessionNotification = reinterpret_cast<WTSRegisterSessionNotificationFn>(
            GetProcAddress(g_runtime.wtsApi, "WTSRegisterSessionNotification"));
        g_runtime.unregisterSessionNotification = reinterpret_cast<WTSUnregisterSessionNotificationFn>(
            GetProcAddress(g_runtime.wtsApi, "WTSUnRegisterSessionNotification"));
        if (!g_runtime.registerSessionNotification || !g_runtime.unregisterSessionNotification ||
            !g_runtime.registerSessionNotification(g_runtime.window, NOTIFY_FOR_THIS_SESSION))
        {
            FreeLibrary(g_runtime.wtsApi);
            g_runtime.wtsApi = nullptr;
            g_runtime.registerSessionNotification = nullptr;
            g_runtime.unregisterSessionNotification = nullptr;
            return false;
        }
        g_runtime.sessionNotificationRegistered = true;
        return true;
    }

    void UnregisterSessionNotifications()
    {
        if (g_runtime.sessionNotificationRegistered && g_runtime.unregisterSessionNotification && g_runtime.window)
            g_runtime.unregisterSessionNotification(g_runtime.window);
        g_runtime.sessionNotificationRegistered = false;
        g_runtime.registerSessionNotification = nullptr;
        g_runtime.unregisterSessionNotification = nullptr;
        if (g_runtime.wtsApi)
            FreeLibrary(g_runtime.wtsApi);
        g_runtime.wtsApi = nullptr;
    }

    bool GenerateAttemptCookie(UINT_PTR& cookie)
    {
        using RtlGenRandomFn = BOOLEAN (WINAPI*)(PVOID, ULONG);
        HMODULE advapi = LoadLibraryExW(L"advapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        auto random = advapi ? reinterpret_cast<RtlGenRandomFn>(GetProcAddress(advapi, "SystemFunction036")) : nullptr;
        cookie = 0;
        const bool ok = random && random(&cookie, sizeof(cookie)) && cookie != 0;
        if (advapi)
            FreeLibrary(advapi);
        return ok;
    }

    void ClearAttemptCookie()
    {
        if (g_runtime.window)
            RemovePropW(g_runtime.window, WINRINGS_HOST_ATTEMPT_COOKIE_PROP_W);
        g_runtime.attemptCookie = 0;
    }

    bool PublishAttemptCookie()
    {
        ClearAttemptCookie();
        UINT_PTR cookie = 0;
        if (!GenerateAttemptCookie(cookie) ||
            !SetPropW(g_runtime.window, WINRINGS_HOST_ATTEMPT_COOKIE_PROP_W, reinterpret_cast<HANDLE>(cookie)))
            return false;
        g_runtime.attemptCookie = cookie;
        return true;
    }

    void WriteParentConsole(const std::wstring& text)
    {
        bool attached = false;
        if (!GetConsoleWindow())
            attached = AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;
        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        if (output && output != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            const std::wstring line = text + L"\r\n";
            if (GetFileType(output) == FILE_TYPE_CHAR)
            {
                WriteConsoleW(output, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
            }
            else
            {
                const std::string utf8 = WideToUtf8(line);
                if (!utf8.empty())
                    WriteFile(output, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
            }
        }
        if (attached)
            FreeConsole();
    }

    std::wstring GetModulePath()
    {
        std::vector<wchar_t> buffer(512);
        for (;;)
        {
            const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (!length)
                return {};
            if (length < buffer.size() - 1)
                return std::wstring(buffer.data(), length);
            if (buffer.size() >= 32768)
                return {};
            buffer.resize((std::min<size_t>)(buffer.size() * 2, 32768));
        }
    }

    std::wstring StripExtendedPrefix(std::wstring path)
    {
        if (path.rfind(L"\\\\?\\UNC\\", 0) == 0)
            return L"\\\\" + path.substr(8);
        if (path.rfind(L"\\\\?\\", 0) == 0)
            return path.substr(4);
        return path;
    }

    bool CanonicalizeExistingFile(const std::wstring& path, std::wstring& canonical)
    {
        UniqueHandle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file)
            return false;

        const DWORD needed = GetFinalPathNameByHandleW(file.get(), nullptr, 0,
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!needed)
            return false;
        std::vector<wchar_t> buffer(static_cast<size_t>(needed) + 1);
        const DWORD length = GetFinalPathNameByHandleW(file.get(), buffer.data(),
            static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!length || length >= buffer.size())
            return false;
        canonical = StripExtendedPrefix(std::wstring(buffer.data(), length));
        return true;
    }

    std::wstring DirectoryOf(const std::wstring& path)
    {
        const size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
    }

    bool EqualPath(const std::wstring& left, const std::wstring& right)
    {
        return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
    }

    bool ResolveSiblingBinary(const wchar_t* fileName, std::wstring& outPath)
    {
        std::wstring canonicalHost;
        if (!CanonicalizeExistingFile(GetModulePath(), canonicalHost))
            return false;
        const std::wstring hostDirectory = DirectoryOf(canonicalHost);
        if (hostDirectory.empty())
            return false;

        const std::wstring candidate = hostDirectory + L"\\" + fileName;
        std::wstring canonicalFile;
        if (!CanonicalizeExistingFile(candidate, canonicalFile))
            return false;
        if (!EqualPath(DirectoryOf(canonicalFile), hostDirectory))
            return false;
        const size_t slash = canonicalFile.find_last_of(L"\\/");
        if (slash == std::wstring::npos || _wcsicmp(canonicalFile.c_str() + slash + 1, fileName) != 0)
            return false;
        outPath = canonicalFile;
        return true;
    }

    bool QueryTokenUser(HANDLE process, std::vector<BYTE>& storage, PSID& sid)
    {
        HANDLE rawToken = nullptr;
        if (!OpenProcessToken(process, TOKEN_QUERY, &rawToken))
            return false;
        UniqueHandle token(rawToken);
        DWORD bytes = 0;
        GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
        if (!bytes || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            return false;
        storage.resize(bytes);
        if (!GetTokenInformation(token.get(), TokenUser, storage.data(), bytes, &bytes))
            return false;
        sid = reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid;
        return IsValidSid(sid) != FALSE;
    }

    bool IsSameUser(HANDLE targetProcess)
    {
        std::vector<BYTE> s1, s2;
        PSID p1 = nullptr, p2 = nullptr;
        return QueryTokenUser(GetCurrentProcess(), s1, p1) &&
            QueryTokenUser(targetProcess, s2, p2) && EqualSid(p1, p2) != FALSE;
    }

    bool IsSameArchitecture(HANDLE targetProcess)
    {
        using IsWow64Process2Fn = BOOL (WINAPI*)(HANDLE, USHORT*, USHORT*);
        auto fn = reinterpret_cast<IsWow64Process2Fn>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
        if (fn)
        {
            USHORT pm1 = 0, nm1 = 0, pm2 = 0, nm2 = 0;
            if (!fn(GetCurrentProcess(), &pm1, &nm1) || !fn(targetProcess, &pm2, &nm2))
                return false;
            const USHORT e1 = pm1 ? pm1 : nm1;
            const USHORT e2 = pm2 ? pm2 : nm2;
            return e1 == e2;
        }
        BOOL w1 = FALSE, w2 = FALSE;
        return IsWow64Process(GetCurrentProcess(), &w1) &&
            IsWow64Process(targetProcess, &w2) && w1 == w2;
    }

    bool IsWindowsExplorer(HANDLE process)
    {
        std::vector<wchar_t> buf(32768);
        DWORD count = static_cast<DWORD>(buf.size());
        if (!QueryFullProcessImageNameW(process, 0, buf.data(), &count))
            return false;
        std::wstring canonicalProcess;
        if (!CanonicalizeExistingFile(std::wstring(buf.data(), count), canonicalProcess))
            return false;

        std::vector<wchar_t> winDir(32768);
        const UINT len = GetWindowsDirectoryW(winDir.data(), static_cast<UINT>(winDir.size()));
        if (!len || len >= winDir.size())
            return false;
        std::wstring expected = std::wstring(winDir.data(), len) + L"\\explorer.exe";
        std::wstring canonicalExpected;
        return CanonicalizeExistingFile(expected, canonicalExpected) &&
            EqualPath(canonicalProcess, canonicalExpected);
    }

    bool IsValidatedExplorerProcess(DWORD pid, UniqueHandle& process)
    {
        if (!pid)
            return false;
        DWORD s1 = 0, s2 = 0;
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &s1) ||
            !ProcessIdToSessionId(pid, &s2) || s1 != s2)
            return false;
        UniqueHandle handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
        if (!handle || !IsSameUser(handle.get()) || !IsWindowsExplorer(handle.get()) ||
            !IsSameArchitecture(handle.get()))
            return false;
        process = std::move(handle);
        return true;
    }

    BOOL CALLBACK CollectShellCandidatesProc(HWND hwnd, LPARAM param)
    {
        auto* target = reinterpret_cast<ExplorerTarget*>(param);
        wchar_t cls[128]{};
        GetClassNameW(hwnd, cls, _countof(cls));
        if (_wcsicmp(cls, L"Progman") != 0 && _wcsicmp(cls, L"WorkerW") != 0)
            return TRUE;
        DWORD pid = 0;
        const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
        if (!tid || !pid)
            return TRUE;
        if (target->processId && target->processId != pid)
            return TRUE;
        target->shellWindow = hwnd;
        target->processId = pid;
        if (!target->threadId)
            target->threadId = tid;
        return FALSE;
    }

    bool FindValidatedExplorer(ExplorerTarget& result)
    {
        HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
        DWORD taskbarPid = 0;
        const DWORD taskbarTid = taskbar ? GetWindowThreadProcessId(taskbar, &taskbarPid) : 0;

        ExplorerTarget candidate;
        candidate.taskbar = taskbar;
        candidate.processId = taskbarPid;
        candidate.threadId = taskbarTid;
        EnumWindows(CollectShellCandidatesProc, reinterpret_cast<LPARAM>(&candidate));
        if (!candidate.shellWindow || !candidate.processId)
        {
            if (!taskbar || !taskbarPid)
                return false;
            candidate.shellWindow = taskbar;
            candidate.processId = taskbarPid;
            candidate.threadId = taskbarTid;
        }

        UniqueHandle process;
        if (!IsValidatedExplorerProcess(candidate.processId, process))
            return false;

        result.clear();
        result.taskbar = (taskbarPid == candidate.processId) ? taskbar : nullptr;
        result.shellWindow = candidate.shellWindow;
        result.processId = candidate.processId;
        result.threadId = candidate.threadId;
        result.process = std::move(process);
        return result.threadId != 0;
    }

    bool LoadShellDllLocally()
    {
        if (g_runtime.shellDll)
            return true;
        std::wstring dllPath;
        if (!ResolveSiblingBinary(WINRINGS_SHELL_FILENAME_W, dllPath))
        {
            DebugLog(L"sibling WinRingsShell.dll not found");
            return false;
        }
        SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
        g_runtime.shellDll = LoadLibraryExW(dllPath.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!g_runtime.shellDll)
        {
            DebugLog(L"LoadLibraryExW rejected WinRingsShell.dll");
            return false;
        }

        auto getApi = reinterpret_cast<WinRingsGetApiFn>(
            GetProcAddress(g_runtime.shellDll, WINRINGS_GET_API_EXPORT_A));
        g_runtime.api = getApi ? getApi(WINRINGS_ABI_VERSION) : nullptr;
        if (!g_runtime.api ||
            g_runtime.api->cbSize < sizeof(WinRingsApi) ||
            g_runtime.api->abiVersion != WINRINGS_ABI_VERSION ||
            g_runtime.api->productVersion != WINRINGS_PRODUCT_VERSION ||
            !g_runtime.api->RequestShutdown)
        {
            DebugLog(L"WinRingsShell.dll ABI mismatch");
            FreeLibrary(g_runtime.shellDll);
            g_runtime.shellDll = nullptr;
            g_runtime.api = nullptr;
            return false;
        }
        return true;
    }

    HWND FindShellControlWindow(DWORD explorerPid)
    {
        HWND found = nullptr;
        while ((found = FindWindowExW(HWND_MESSAGE, found, WINRINGS_SHELL_CONTROL_CLASS_W, nullptr)) != nullptr)
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(found, &pid);
            if (pid == explorerPid)
                return found;
        }
        return nullptr;
    }

    bool IsDifferentShellDllAlreadyMapped(DWORD pid, const std::wstring& expectedPath, bool& alreadyMappedExact)
    {
        alreadyMappedExact = false;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
        if (snap == INVALID_HANDLE_VALUE)
            return false;
        MODULEENTRY32W me{ sizeof(me) };
        bool mismatch = false;
        for (BOOL more = Module32FirstW(snap, &me); more; more = Module32NextW(snap, &me))
        {
            if (_wcsicmp(me.szModule, WINRINGS_SHELL_FILENAME_W) == 0)
            {
                std::wstring canonicalMapped;
                if (CanonicalizeExistingFile(me.szExePath, canonicalMapped) &&
                    EqualPath(canonicalMapped, expectedPath))
                {
                    alreadyMappedExact = true;
                }
                else
                {
                    mismatch = true;
                }
                break;
            }
        }
        CloseHandle(snap);
        return mismatch;
    }

    bool MapShellDllIntoExplorer(DWORD pid, const std::wstring& dllPath)
    {
        UniqueHandle process(OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
            PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid));
        if (!process)
        {
            DebugLog(L"OpenProcess failed for Explorer");
            return false;
        }

        auto localLoad = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        if (!localLoad)
            return false;

        MEMORY_BASIC_INFORMATION mbi{};
        wchar_t modulePath[MAX_PATH]{};
        if (!VirtualQuery(reinterpret_cast<const void*>(localLoad), &mbi, sizeof(mbi)) ||
            !GetModuleFileNameW(static_cast<HMODULE>(mbi.AllocationBase), modulePath, ARRAYSIZE(modulePath)))
            return false;

        const wchar_t* moduleName = wcsrchr(modulePath, L'\\');
        uintptr_t remoteLoad = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
        if (snap != INVALID_HANDLE_VALUE)
        {
            MODULEENTRY32W entry{ sizeof(entry) };
            for (BOOL more = Module32FirstW(snap, &entry); more; more = Module32NextW(snap, &entry))
            {
                if (moduleName && _wcsicmp(entry.szModule, moduleName + 1) == 0)
                {
                    remoteLoad = reinterpret_cast<uintptr_t>(entry.modBaseAddr) +
                        (reinterpret_cast<uintptr_t>(localLoad) - reinterpret_cast<uintptr_t>(mbi.AllocationBase));
                    break;
                }
            }
            CloseHandle(snap);
        }
        if (!remoteLoad)
            return false;

        const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
        void* remotePath = VirtualAllocEx(process.get(), nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remotePath)
            return false;

        bool ok = false;
        SIZE_T written = 0;
        HANDLE thread = nullptr;
        if (WriteProcessMemory(process.get(), remotePath, dllPath.c_str(), bytes, &written) && written == bytes)
        {
            thread = CreateRemoteThread(process.get(), nullptr, 0,
                reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteLoad), remotePath, 0, nullptr);
        }
        if (thread)
        {
            const DWORD wait = WaitForSingleObject(thread, 10000);
            DWORD exitCode = 0;
            if (wait == WAIT_OBJECT_0 && GetExitCodeThread(thread, &exitCode))
            {
                VirtualFreeEx(process.get(), remotePath, 0, MEM_RELEASE);
                ok = exitCode != 0;
            }
            // If CreateRemoteThread timed out, leave remotePath alive until Explorer exits.
            CloseHandle(thread);
        }
        else
        {
            VirtualFreeEx(process.get(), remotePath, 0, MEM_RELEASE);
        }
        return ok;
    }

    bool ExplorerProcessStillAlive()
    {
        return g_runtime.explorer.process &&
            WaitForSingleObject(g_runtime.explorer.process.get(), 0) == WAIT_TIMEOUT;
    }

    void CancelTimers()
    {
        if (!g_runtime.window)
            return;
        KillTimer(g_runtime.window, kRetryTimer);
        KillTimer(g_runtime.window, kInjectionTimer);
    }

    void DisarmAdminInputBridge();

    bool ResetConnection()
    {
        DisarmAdminInputBridge();
        CancelTimers();
        g_runtime.shellControlWindow = nullptr;
        g_runtime.explorer.clear();
        ClearAttemptCookie();
        return true;
    }

    bool SendShellCommand(WinRingsCommand cmd, LRESULT* outResult = nullptr)
    {
        if (!ExplorerProcessStillAlive())
            return false;
        if ((!g_runtime.shellControlWindow || !IsWindow(g_runtime.shellControlWindow)) &&
            g_runtime.explorer.processId)
        {
            g_runtime.shellControlWindow = FindShellControlWindow(g_runtime.explorer.processId);
            if (g_runtime.shellControlWindow)
            {
                HostDiagLog(L"Re-resolved shellControlWindow=0x%p for Explorer pid=%lu",
                    g_runtime.shellControlWindow, g_runtime.explorer.processId);
            }
        }
        if (!g_runtime.shellControlWindow || !IsWindow(g_runtime.shellControlWindow))
            return false;
        if (cmd == WINRINGS_CMD_TOGGLE_RING)
            AllowSetForegroundWindow(g_runtime.explorer.processId);

        if (outResult)
        {
            DWORD_PTR res = 0;
            if (!SendMessageTimeoutW(g_runtime.shellControlWindow, g_runtime.shellCommandMessage,
                static_cast<WPARAM>(cmd), 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &res))
                return false;
            *outResult = static_cast<LRESULT>(res);
            return true;
        }
        return PostMessageW(g_runtime.shellControlWindow, g_runtime.shellCommandMessage,
            static_cast<WPARAM>(cmd), 0) != FALSE;
    }

    LPARAM PackPointLParam(POINT pt) noexcept
    {
        return MAKELPARAM(
            static_cast<WORD>(static_cast<short>(pt.x)),
            static_cast<WORD>(static_cast<short>(pt.y)));
    }

    bool SendShellAdminCommand(WinRingsCommand cmd, LPARAM lp = 0) noexcept
    {
        if (g_runtime.status != WINRINGS_HOST_STATUS_READY || !ExplorerProcessStillAlive())
            return false;
        if ((!g_runtime.shellControlWindow || !IsWindow(g_runtime.shellControlWindow)) &&
            g_runtime.explorer.processId)
        {
            g_runtime.shellControlWindow = FindShellControlWindow(g_runtime.explorer.processId);
        }
        if (!g_runtime.shellControlWindow || !IsWindow(g_runtime.shellControlWindow))
            return false;
        if (g_runtime.explorer.processId)
            AllowSetForegroundWindow(g_runtime.explorer.processId);
        return PostMessageW(g_runtime.shellControlWindow, g_runtime.shellCommandMessage,
            static_cast<WPARAM>(cmd), lp) != FALSE;
    }

    void DisarmAdminInputBridge()
    {
        if (g_runtime.window && IsWindow(g_runtime.window))
        {
            KillTimer(g_runtime.window, kAdminWatchdogTimer);
            KillTimer(g_runtime.window, kAdminConfigSyncTimer);
        }
        if (g_runtime.adminBridgeArmed)
        {
            g_runtime.adminInput.Uninstall();
            g_runtime.adminGestures.Shutdown();
            g_runtime.adminBridgeArmed = false;
            HostDiagLog(L"AdminInputBridge disarmed");
        }
    }

    void ReloadAdminBridgeConfig()
    {
        if (!g_runtime.isElevated || !g_runtime.adminBridgeArmed)
            return;
        g_runtime.config = SwitcherConfig::LoadOrCreate();
        g_runtime.adminInput.UpdateConfig(g_runtime.config);
        g_runtime.adminInput.UpdateCachedDpiScale(QueryHostPrimaryDpiScale());
        g_runtime.adminGestures.UpdateConfig(g_runtime.config);
    }

    void ArmAdminInputBridge()
    {
        if (!g_runtime.isElevated || g_runtime.status != WINRINGS_HOST_STATUS_READY || !g_runtime.window)
            return;

        if (g_runtime.adminBridgeArmed)
        {
            ReloadAdminBridgeConfig();
            return;
        }

        g_runtime.config = SwitcherConfig::LoadOrCreate();

        GestureEngine::Callbacks gCb{};
        gCb.isRingOpen = []() { return IsOverlayWindowVisible(); };
        gCb.onToggleRing = []() { SendShellAdminCommand(WINRINGS_CMD_ADMIN_TOGGLE_RING); };
        gCb.onOpenRing = []() {
            POINT pt{};
            GetCursorPos(&pt);
            SendShellAdminCommand(WINRINGS_CMD_OPEN_RING_AT, PackPointLParam(pt));
        };
        gCb.isPointOverMediaIsland = [](POINT pt) { return IsPointOverMediaIslandBar(pt); };
        gCb.onMediaIslandTouchpadWheel = [](short delta) {
            SendShellAdminCommand(WINRINGS_CMD_MEDIA_WHEEL, static_cast<LPARAM>(delta));
        };
        gCb.shouldProcessRawInput = []() { return ShouldAdminBridgeHandle(POINT{}, true); };
        g_runtime.adminGestures.Initialize(g_runtime.window, g_runtime.config, std::move(gCb));

        InputEngine::Callbacks iCb{};
        iCb.isRingOpen = []() { return IsOverlayWindowVisible(); };
        iCb.shouldProcessInput = [](POINT pt, bool isKeyboard) {
            return ShouldAdminBridgeHandle(pt, isKeyboard);
        };
        iCb.shouldSuppressTouchpadMouse = [](UINT msg) {
            return g_runtime.adminGestures.ShouldSuppressMouseButton(msg);
        };
        iCb.onTriggerPressed = [](int, int) {
            SendShellAdminCommand(WINRINGS_CMD_ADMIN_TOGGLE_RING);
        };
        iCb.onToggleRequested = []() {
            SendShellAdminCommand(WINRINGS_CMD_ADMIN_TOGGLE_RING);
        };
        iCb.onDoubleTapLastApp = []() {
            SendShellAdminCommand(WINRINGS_CMD_DOUBLE_TAP_LAST_APP);
        };
        iCb.onDirectSwitchStep = []() {
            SendShellAdminCommand(WINRINGS_CMD_DIRECT_SWITCH_STEP);
        };
        iCb.onDirectSwitchCommit = []() {
            SendShellAdminCommand(WINRINGS_CMD_DIRECT_SWITCH_COMMIT);
        };
        iCb.onMediaIslandWheel = [](POINT pt, short delta) {
            if (!IsPointOverMediaIslandBar(pt))
                return false;
            SendShellAdminCommand(WINRINGS_CMD_MEDIA_WHEEL, static_cast<LPARAM>(delta));
            return true;
        };
        iCb.onShakeDetected = [](POINT pt) {
            if (g_runtime.window)
            {
                g_runtime.pendingAdminShakePt = pt;
                PostMessageW(g_runtime.window, kMsgAdminShakeTriggered, 0, 0);
            }
        };

        const bool installed = g_runtime.adminInput.Install(g_runtime.config, std::move(iCb));
        g_runtime.adminInput.UpdateCachedDpiScale(QueryHostPrimaryDpiScale());
        g_runtime.adminInput.SetSuspended(!g_runtime.interactiveInput);
        g_runtime.adminBridgeArmed = installed;
        SetTimer(g_runtime.window, kAdminWatchdogTimer, 2000, nullptr);
        HostDiagLog(L"AdminInputBridge armed (elevated=1 hooks=%d)", installed ? 1 : 0);
    }

    bool HandleAdminWindowActionCopyData(HWND senderHwnd, const COPYDATASTRUCT* cds) noexcept
    {
        if (!g_runtime.isElevated || !cds ||
            cds->dwData != WINRINGS_COPYDATA_WINDOW_ACTION ||
            cds->cbData != sizeof(WinRingsAdminWindowActionPayload) ||
            !cds->lpData)
        {
            return false;
        }

        WinRingsAdminWindowActionPayload payload{};
        std::memcpy(&payload, cds->lpData, sizeof(payload));
        if (payload.cbSize != sizeof(WinRingsAdminWindowActionPayload) ||
            !g_runtime.attemptCookie ||
            payload.attemptCookie != g_runtime.attemptCookie ||
            !g_runtime.explorer.processId)
        {
            return false;
        }

        DWORD senderPid = 0;
        if (!senderHwnd || !IsWindow(senderHwnd) ||
            !GetWindowThreadProcessId(senderHwnd, &senderPid) ||
            senderPid != g_runtime.explorer.processId)
        {
            return false;
        }

        HWND target = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(payload.targetHwnd));
        switch (static_cast<WinRingsAdminWindowActionKind>(payload.action))
        {
        case WINRINGS_ADMIN_ACT_ACTIVATE:
            if (!target || !IsWindow(target))
                return false;
            if (IsIconic(target))
                ShowWindow(target, SW_RESTORE);
            ForceForegroundElevated(target);
            return true;

        case WINRINGS_ADMIN_ACT_MINIMIZE:
            if (!target || !IsWindow(target))
                return false;
            ShowWindowAsync(target, SW_MINIMIZE);
            return true;

        case WINRINGS_ADMIN_ACT_CLOSE:
            if (!target || !IsWindow(target))
                return false;
            return PostMessageW(target, WM_CLOSE, 0, 0) != FALSE;

        case WINRINGS_ADMIN_ACT_TERMINATE_PID:
        {
            if (payload.targetPid == 0 ||
                payload.targetPid == GetCurrentProcessId() ||
                payload.targetPid == g_runtime.explorer.processId)
            {
                return false;
            }
            HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, payload.targetPid);
            if (!hProc)
                return false;
            const BOOL ok = TerminateProcess(hProc, 1);
            CloseHandle(hProc);
            return ok != FALSE;
        }

        case WINRINGS_ADMIN_ACT_SET_WINDOW_POS:
            if (!target || !IsWindow(target))
                return false;
            return SetWindowPos(
                target,
                (payload.swpFlags & SWP_NOZORDER) ? nullptr : HWND_TOP,
                payload.x, payload.y, payload.cx, payload.cy,
                payload.swpFlags) != FALSE;

        case WINRINGS_ADMIN_ACT_MAXIMIZE:
            if (!target || !IsWindow(target))
                return false;
            ShowWindow(target, SW_MAXIMIZE);
            ForceForegroundElevated(target);
            return true;

        case WINRINGS_ADMIN_ACT_RESTORE:
            if (!target || !IsWindow(target))
                return false;
            return ShowWindow(target, SW_RESTORE) != FALSE;

        default:
            return false;
        }
    }

    bool ReconcileInteractiveInput()
    {
        const bool missed = g_runtime.inputGeneration != g_runtime.injectionInputGeneration;
        const bool required = g_runtime.inputStateApplied
            ? (g_runtime.appliedInputGeneration != g_runtime.inputGeneration)
            : (!g_runtime.interactiveInput || missed);
        if (!required)
        {
            g_runtime.appliedInputGeneration = g_runtime.inputGeneration;
            g_runtime.inputStateApplied = true;
            return true;
        }
        const WinRingsCommand cmd = g_runtime.interactiveInput
            ? WINRINGS_CMD_REFRESH_INPUT
            : WINRINGS_CMD_SUSPEND_INPUT;
        if (!SendShellCommand(cmd))
        {
            g_runtime.status = WINRINGS_HOST_STATUS_FAILED;
            return false;
        }
        g_runtime.appliedInputGeneration = g_runtime.inputGeneration;
        g_runtime.inputStateApplied = true;
        return true;
    }

    void AdvanceInteractiveInputGeneration()
    {
        g_runtime.interactiveInput = g_runtime.sessionInputInteractive &&
            g_runtime.powerInputInteractive && !g_runtime.sessionEnding;
        if (++g_runtime.inputGeneration == 0)
        {
            g_runtime.inputGeneration = 1;
            g_runtime.appliedInputGeneration = 0;
            g_runtime.injectionInputGeneration = 0;
            g_runtime.inputStateApplied = false;
        }
        if (!g_runtime.interactiveInput)
            g_runtime.pendingToggle = false;
        if (g_runtime.adminBridgeArmed)
        {
            g_runtime.adminInput.SetSuspended(!g_runtime.interactiveInput);
            if (g_runtime.interactiveInput)
            {
                g_runtime.adminInput.ResetTransientState();
                g_runtime.adminGestures.EnsureDeviceRegistration(true);
            }
        }
        if (g_runtime.status == WINRINGS_HOST_STATUS_READY && g_runtime.activationCommitted)
            ReconcileInteractiveInput();
    }

    void ScheduleRetry();

    bool TryInject()
    {
        if (g_runtime.safetyDisabled)
        {
            g_runtime.status = WINRINGS_HOST_STATUS_SAFETY_DISABLED;
            return false;
        }
        if (!LoadShellDllLocally())
            return false;

        std::wstring dllPath;
        if (!ResolveSiblingBinary(WINRINGS_SHELL_FILENAME_W, dllPath))
            return false;

        ExplorerTarget target;
        if (!FindValidatedExplorer(target))
            return false;

        bool alreadyMappedExact = false;
        if (IsDifferentShellDllAlreadyMapped(target.processId, dllPath, alreadyMappedExact))
        {
            DebugLog(L"different WinRingsShell.dll build is already mapped in Explorer; restart Explorer to upgrade");
            g_runtime.explorer = std::move(target);
            g_runtime.status = WINRINGS_HOST_STATUS_FAILED;
            return false;
        }

        if (!PublishAttemptCookie())
            return false;

        g_runtime.explorer = std::move(target);
        g_runtime.injectionInputGeneration = g_runtime.inputGeneration;
        g_runtime.inputStateApplied = false;
        g_runtime.initializationPending = true;
        g_runtime.activationCommitted = false;
        g_runtime.authenticatedDisarmed = false;
        g_runtime.status = WINRINGS_HOST_STATUS_INJECTING;

        if (alreadyMappedExact)
        {
            HWND existingCtrl = FindShellControlWindow(g_runtime.explorer.processId);
            if (existingCtrl)
            {
                // Re-attach to our exact mapped WinRingsShell.dll after an earlier --exit.
                DWORD_PTR accepted = 0;
                if (SendMessageTimeoutW(existingCtrl, g_runtime.shellCommandMessage,
                    WINRINGS_CMD_REATTACH, reinterpret_cast<LPARAM>(g_runtime.window),
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &accepted) && accepted)
                {
                    SetTimer(g_runtime.window, kInjectionTimer, kInjectionTimeoutMs, nullptr);
                    return true;
                }
            }
            g_runtime.initializationPending = false;
            g_runtime.status = WINRINGS_HOST_STATUS_FAILED;
            return false;
        }

        if (!MapShellDllIntoExplorer(g_runtime.explorer.processId, dllPath))
        {
            g_runtime.initializationPending = false;
            ClearAttemptCookie();
            g_runtime.explorer.clear();
            return false;
        }

        SetTimer(g_runtime.window, kInjectionTimer, kInjectionTimeoutMs, nullptr);
        return true;
    }

    void ScheduleRetry()
    {
        if (g_runtime.safetyDisabled)
        {
            ResetConnection();
            g_runtime.status = WINRINGS_HOST_STATUS_SAFETY_DISABLED;
            return;
        }
        ResetConnection();
        g_runtime.initializationPending = false;
        if (g_runtime.retryIndex >= _countof(kRetryDelayMs))
        {
            g_runtime.status = WINRINGS_HOST_STATUS_FAILED;
            DebugLog(L"bounded Explorer connection retries exhausted");
            return;
        }
        g_runtime.status = WINRINGS_HOST_STATUS_STARTING;
        const DWORD delay = kRetryDelayMs[g_runtime.retryIndex++];
        if (!SetTimer(g_runtime.window, kRetryTimer, delay, nullptr))
            g_runtime.status = WINRINGS_HOST_STATUS_FAILED;
    }

    void BeginConnection(bool immediate)
    {
        if (g_runtime.safetyDisabled)
        {
            ResetConnection();
            g_runtime.status = WINRINGS_HOST_STATUS_SAFETY_DISABLED;
            return;
        }
        ResetConnection();
        g_runtime.initializationPending = false;
        g_runtime.status = WINRINGS_HOST_STATUS_STARTING;
        if (immediate && TryInject())
            return;
        ScheduleRetry();
    }

    void IssueShutdownRequest(bool preserveReplacement = false)
    {
        DisarmAdminInputBridge();
        g_runtime.status = WINRINGS_HOST_STATUS_STOPPING;
        CancelTimers();
        bool requested = false;
        if ((!g_runtime.shellControlWindow || !IsWindow(g_runtime.shellControlWindow)) &&
            ExplorerProcessStillAlive() && g_runtime.explorer.processId)
        {
            g_runtime.shellControlWindow = FindShellControlWindow(g_runtime.explorer.processId);
            if (g_runtime.shellControlWindow)
            {
                HostDiagLog(L"IssueShutdownRequest re-resolved shellControlWindow=0x%p for Explorer pid=%lu",
                    g_runtime.shellControlWindow, g_runtime.explorer.processId);
            }
        }
        if (g_runtime.shellControlWindow && IsWindow(g_runtime.shellControlWindow) &&
            g_runtime.api && g_runtime.api->RequestShutdown)
        {
            requested = g_runtime.api->RequestShutdown(g_runtime.shellControlWindow) != FALSE;
        }
        if (!requested)
            requested = SendShellCommand(WINRINGS_CMD_EXIT);
        if (!requested)
        {
            if (!ExplorerProcessStillAlive())
            {
                g_runtime.activationCommitted = false;
                DestroyWindow(g_runtime.window);
            }
            else
            {
                g_runtime.replacementPending = preserveReplacement;
                g_runtime.status = WINRINGS_HOST_STATUS_FAILED;
            }
            return;
        }
        SetTimer(g_runtime.window, kStopTimer, kStopTimeoutMs, nullptr);
    }

    void RequestStop()
    {
        g_runtime.exitPending = true;
        if (g_runtime.initializationPending)
        {
            g_runtime.status = WINRINGS_HOST_STATUS_STOPPING;
            return;
        }
        if (g_runtime.activationCommitted || g_runtime.status == WINRINGS_HOST_STATUS_READY)
        {
            IssueShutdownRequest(g_runtime.replacementPending);
            return;
        }
        CancelTimers();
        DestroyWindow(g_runtime.window);
    }

    void HandleReadyStatus()
    {
        g_runtime.initializationPending = false;
        KillTimer(g_runtime.window, kInjectionTimer);
        KillTimer(g_runtime.window, kRetryTimer);
        g_runtime.retryIndex = 0;
        g_runtime.activationCommitted = true;
        g_runtime.authenticatedDisarmed = false;
        g_runtime.shellControlWindow = FindShellControlWindow(g_runtime.explorer.processId);

        if (g_runtime.replacementPending)
        {
            if (g_runtime.exitPending)
                IssueShutdownRequest(true);
            else
                g_runtime.status = WINRINGS_HOST_STATUS_STARTING;
            return;
        }
        g_runtime.status = WINRINGS_HOST_STATUS_READY;
        ArmAdminInputBridge();
        if (g_runtime.exitPending)
        {
            IssueShutdownRequest();
            return;
        }
        if (!ReconcileInteractiveInput())
            return;
        if (g_runtime.pendingToggle)
        {
            if (SendShellCommand(WINRINGS_CMD_TOGGLE_RING))
                g_runtime.pendingToggle = false;
        }
    }

    void HandleDllStatus(WinRingsDllStatus status, DWORD explorerPid, UINT_PTR cookie)
    {
        if (!g_runtime.explorer.processId || explorerPid != g_runtime.explorer.processId ||
            !cookie || cookie != g_runtime.attemptCookie)
            return;

        switch (status)
        {
        case WINRINGS_DLL_STATUS_READY:
            if (!g_runtime.initializationPending && g_runtime.status != WINRINGS_HOST_STATUS_READY)
                return;
            HandleReadyStatus();
            break;
        case WINRINGS_DLL_STATUS_FAILED:
            g_runtime.initializationPending = false;
            g_runtime.activationCommitted = false;
            CancelTimers();
            g_runtime.status = WINRINGS_HOST_STATUS_FAILED;
            if (g_runtime.exitPending)
                DestroyWindow(g_runtime.window);
            break;
        case WINRINGS_DLL_STATUS_DISARMED:
            g_runtime.authenticatedDisarmed = true;
            g_runtime.initializationPending = false;
            g_runtime.activationCommitted = false;
            KillTimer(g_runtime.window, kStopTimer);
            CancelTimers();
            if (g_runtime.exitPending)
                DestroyWindow(g_runtime.window);
            else if (g_runtime.replacementPending)
                g_runtime.status = WINRINGS_HOST_STATUS_STARTING;
            else
                g_runtime.status = WINRINGS_HOST_STATUS_FAILED;
            break;
        }
    }

    void HandleExplorerProcessExit()
    {
        if (!g_runtime.explorer.process ||
            WaitForSingleObject(g_runtime.explorer.process.get(), 0) != WAIT_OBJECT_0)
            return;

        DebugLog(L"validated Explorer exited");
        DWORD exitCode = ERROR_GEN_FAILURE;
        GetExitCodeProcess(g_runtime.explorer.process.get(), &exitCode);
        const bool attempted = g_runtime.initializationPending || g_runtime.activationCommitted;
        const bool abnormal = attempted && exitCode != ERROR_SUCCESS &&
            !g_runtime.sessionEnding && !g_runtime.authenticatedDisarmed;
        const bool safetyTripped = RecordExplorerExitForCrashGuard(abnormal, exitCode);

        g_runtime.initializationPending = false;
        g_runtime.activationCommitted = false;
        g_runtime.inputStateApplied = false;
        g_runtime.replacementPending = false;
        g_runtime.shellControlWindow = nullptr;

        if (g_runtime.exitPending)
        {
            ResetConnection();
            if (g_runtime.window && IsWindow(g_runtime.window))
                DestroyWindow(g_runtime.window);
        }
        else if (g_runtime.sessionEnding)
        {
            ResetConnection();
            g_runtime.status = WINRINGS_HOST_STATUS_STOPPING;
        }
        else if (safetyTripped)
        {
            ResetConnection();
            g_runtime.status = WINRINGS_HOST_STATUS_SAFETY_DISABLED;
        }
        else
        {
            g_runtime.retryIndex = 0;
            BeginConnection(true);
        }
    }

    void LaunchSiblingTools(const wchar_t* args)
    {
        std::wstring toolsPath;
        if (!ResolveSiblingBinary(WINRINGS_TOOLS_FILENAME_W, toolsPath))
            return;
        SHELLEXECUTEINFOW sei{ sizeof(sei) };
        sei.lpFile = toolsPath.c_str();
        sei.lpParameters = args;
        sei.nShow = SW_SHOWNORMAL;
        ShellExecuteExW(&sei);
    }

    HICON CreateWinRingsTrayIcon() noexcept
    {
        constexpr int kSize = 32;
        BITMAPV5HEADER bi{};
        bi.bV5Size = sizeof(bi);
        bi.bV5Width = kSize;
        bi.bV5Height = -kSize; // top-down
        bi.bV5Planes = 1;
        bi.bV5BitCount = 32;
        bi.bV5Compression = BI_BITFIELDS;
        bi.bV5RedMask = 0x00FF0000;
        bi.bV5GreenMask = 0x0000FF00;
        bi.bV5BlueMask = 0x000000FF;
        bi.bV5AlphaMask = 0xFF000000;

        void* bits = nullptr;
        HDC screenDc = GetDC(nullptr);
        HBITMAP colorBmp = CreateDIBSection(screenDc, reinterpret_cast<BITMAPINFO*>(&bi),
            DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, screenDc);
        if (!colorBmp || !bits)
        {
            if (colorBmp)
                DeleteObject(colorBmp);
            return LoadIconW(nullptr, IDI_APPLICATION);
        }

        auto* px = static_cast<uint32_t*>(bits);
        constexpr float cx = (kSize - 1) * 0.5f;
        constexpr float cy = (kSize - 1) * 0.5f;
        for (int y = 0; y < kSize; ++y)
        {
            for (int x = 0; x < kSize; ++x)
            {
                const float dx = static_cast<float>(x) - cx;
                const float dy = static_cast<float>(y) - cy;
                const float dist = std::sqrt(dx * dx + dy * dy);

                // Outer ring radius ~11.2, stroke thickness ~3.2, plus small center dot ~2.2
                const float ringDist = std::fabs(dist - 10.8f);
                float alpha = 0.0f;
                if (ringDist < 2.4f)
                    alpha = (std::clamp)(2.4f - ringDist, 0.0f, 1.0f);
                if (dist < 2.8f)
                    alpha = (std::max)(alpha, (std::clamp)(2.8f - dist, 0.0f, 1.0f));

                if (alpha > 0.01f)
                {
                    const uint32_t a = static_cast<uint32_t>(alpha * 255.0f + 0.5f);
                    // Cyan-blue gradient (#31B8FF -> #1F7BFF)
                    const float t = (static_cast<float>(y) / static_cast<float>(kSize));
                    const uint32_t r = static_cast<uint32_t>((49.0f * (1.0f - t) + 31.0f * t) * alpha);
                    const uint32_t g = static_cast<uint32_t>((200.0f * (1.0f - t) + 134.0f * t) * alpha);
                    const uint32_t b = static_cast<uint32_t>(255.0f * alpha);
                    px[y * kSize + x] = (a << 24) | (r << 16) | (g << 8) | b;
                }
                else
                {
                    px[y * kSize + x] = 0;
                }
            }
        }

        HBITMAP maskBmp = CreateBitmap(kSize, kSize, 1, 1, nullptr);
        ICONINFO ii{};
        ii.fIcon = TRUE;
        ii.hbmMask = maskBmp;
        ii.hbmColor = colorBmp;
        HICON icon = CreateIconIndirect(&ii);
        DeleteObject(colorBmp);
        if (maskBmp)
            DeleteObject(maskBmp);
        return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
    }

    void EnsureTrayIconPromoted() noexcept
    {
        HKEY hRoot = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings",
                0, KEY_READ | KEY_WRITE, &hRoot) != ERROR_SUCCESS || !hRoot)
        {
            return;
        }

        wchar_t subKeyName[128]{};
        for (DWORD idx = 0; ; ++idx)
        {
            DWORD nameLen = _countof(subKeyName);
            if (RegEnumKeyExW(hRoot, idx, subKeyName, &nameLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                break;

            HKEY hSub = nullptr;
            if (RegOpenKeyExW(hRoot, subKeyName, 0, KEY_READ | KEY_WRITE, &hSub) != ERROR_SUCCESS || !hSub)
                continue;

            wchar_t exePath[512]{};
            DWORD cbExe = sizeof(exePath);
            DWORD type = 0;
            const bool hasExe = (RegQueryValueExW(hSub, L"ExecutablePath", nullptr, &type,
                reinterpret_cast<LPBYTE>(exePath), &cbExe) == ERROR_SUCCESS &&
                (type == REG_SZ || type == REG_EXPAND_SZ));

            wchar_t tip[256]{};
            DWORD cbTip = sizeof(tip);
            const bool hasTip = (RegQueryValueExW(hSub, L"InitialTooltip", nullptr, &type,
                reinterpret_cast<LPBYTE>(tip), &cbTip) == ERROR_SUCCESS &&
                (type == REG_SZ || type == REG_EXPAND_SZ));

            if ((hasExe && wcsstr(exePath, L"WinRingsHost.exe") != nullptr) ||
                (hasTip && wcsstr(tip, L"WinRings") != nullptr))
            {
                DWORD promoted = 0;
                DWORD cbPromoted = sizeof(promoted);
                DWORD valType = 0;
                const LONG qRes = RegQueryValueExW(hSub, L"IsPromoted", nullptr, &valType,
                    reinterpret_cast<LPBYTE>(&promoted), &cbPromoted);
                if (qRes != ERROR_SUCCESS || promoted != 1u)
                {
                    const DWORD one = 1u;
                    RegSetValueExW(hSub, L"IsPromoted", 0, REG_DWORD,
                        reinterpret_cast<const BYTE*>(&one), sizeof(one));
                }
            }
            RegCloseKey(hSub);
        }
        RegCloseKey(hRoot);
    }

    void AddTrayIcon(HWND hwnd)
    {
        if (!g_runtime.trayIcon)
            g_runtime.trayIcon = CreateWinRingsTrayIcon();

        NOTIFYICONDATAW nid{ sizeof(nid) };
        nid.hWnd = hwnd;
        nid.uID = kTrayIconId;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = kTrayCallbackMsg;
        nid.hIcon = g_runtime.trayIcon;
        wcscpy_s(nid.szTip, L"WinRings — Radial Switcher & Top Bar");
        if (Shell_NotifyIconW(NIM_ADD, &nid) || Shell_NotifyIconW(NIM_MODIFY, &nid))
        {
            g_runtime.trayAdded = true;
            KillTimer(hwnd, kTrayRetryTimer);
            EnsureTrayIconPromoted();
        }
        else if (g_runtime.trayRetryCount < 30)
        {
            ++g_runtime.trayRetryCount;
            SetTimer(hwnd, kTrayRetryTimer, 1000, nullptr);
        }
    }

    void RemoveTrayIcon(HWND hwnd)
    {
        KillTimer(hwnd, kTrayRetryTimer);
        if (g_runtime.trayAdded)
        {
            NOTIFYICONDATAW nid{ sizeof(nid) };
            nid.hWnd = hwnd;
            nid.uID = kTrayIconId;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            g_runtime.trayAdded = false;
        }
        if (g_runtime.trayIcon)
        {
            DestroyIcon(g_runtime.trayIcon);
            g_runtime.trayIcon = nullptr;
        }
    }

    void EnableDarkWin32Menus() noexcept
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

    void ShowTrayContextMenu(HWND hwnd)
    {
        POINT pt{};
        GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        if (!menu)
            return;
        const SwitcherConfig cfgNow = SwitcherConfig::LoadOrCreate();
        AppendMenuW(menu, MF_STRING, kMenuCmdOpenRing, L"Open Ring Now");
        AppendMenuW(menu, MF_STRING | (cfgNow.topBarEnabled ? MF_CHECKED : MF_UNCHECKED),
            kMenuCmdToggleTopBar, L"Toggle Top Minimal Bar");
        AppendMenuW(menu, MF_STRING | (cfgNow.mediaIslandEnabled ? MF_CHECKED : MF_UNCHECKED),
            kMenuCmdToggleMediaIsland, L"Show Media Island");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuCmdSettings, L"Settings...");
        AppendMenuW(menu, MF_STRING, kMenuCmdReload, L"Reload Config");
        AppendMenuW(menu, MF_STRING, kMenuCmdConfigFolder, L"Open Config Folder");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuCmdExit, L"Exit WinRings");
        SetMenuDefaultItem(menu, kMenuCmdSettings, FALSE);

        UINT menuFlags = TPM_RETURNCMD | TPM_RIGHTBUTTON;
        if (cfgNow.topBarEnabled && cfgNow.topBarDockTop)
        {
            HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{ sizeof(mi) };
            if (GetMonitorInfoW(mon, &mi))
            {
                const UINT dpi = GetDpiForWindow(hwnd) >= 72 ? GetDpiForWindow(hwnd) : 96u;
                const int hDip = (std::clamp)(cfgNow.topBarHeightDip, 24, 52);
                LONG barBottom = mi.rcMonitor.top + MulDiv(hDip, static_cast<int>(dpi), 96);
                HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
                RECT rcTray{};
                if (tray && GetWindowRect(tray, &rcTray) &&
                    rcTray.top <= mi.rcMonitor.top + 8 &&
                    rcTray.bottom > mi.rcMonitor.top &&
                    rcTray.bottom <= mi.rcMonitor.top + 120)
                {
                    barBottom = (std::max)(barBottom, rcTray.bottom);
                }
                if (pt.y < barBottom + 4)
                {
                    pt.y = barBottom + 4;
                    menuFlags |= TPM_TOPALIGN;
                }
            }
        }
        else if (HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr))
        {
            HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{ sizeof(mi) };
            RECT rcTray{};
            if (GetMonitorInfoW(mon, &mi) && GetWindowRect(tray, &rcTray) &&
                rcTray.top <= mi.rcMonitor.top + 8 &&
                rcTray.bottom > mi.rcMonitor.top &&
                rcTray.bottom <= mi.rcMonitor.top + 120)
            {
                if (pt.y < rcTray.bottom + 4)
                {
                    pt.y = rcTray.bottom + 4;
                    menuFlags |= TPM_TOPALIGN;
                }
            }
        }

        SetForegroundWindow(hwnd);
        const UINT cmd = TrackPopupMenuEx(menu, menuFlags, pt.x, pt.y, hwnd, nullptr);
        DestroyMenu(menu);
        PostMessageW(hwnd, WM_NULL, 0, 0);

        if (g_runtime.explorer.processId)
            AllowSetForegroundWindow(g_runtime.explorer.processId);

        switch (cmd)
        {
        case kMenuCmdOpenRing:
            if (g_runtime.status == WINRINGS_HOST_STATUS_READY)
                SendShellCommand(WINRINGS_CMD_TOGGLE_RING);
            break;
        case kMenuCmdToggleTopBar:
        {
            const bool sent = (g_runtime.status == WINRINGS_HOST_STATUS_READY) &&
                SendShellCommand(WINRINGS_CMD_TOGGLE_TOPBAR);
            if (sent && g_runtime.adminBridgeArmed)
                SetTimer(hwnd, kAdminConfigSyncTimer, 200, nullptr);
            HostDiagLog(L"TrayMenu ToggleTopBar: status=%d ctrl=0x%p sent=%d",
                static_cast<int>(g_runtime.status), g_runtime.shellControlWindow, sent ? 1 : 0);
            break;
        }
        case kMenuCmdToggleMediaIsland:
        {
            const bool sent = (g_runtime.status == WINRINGS_HOST_STATUS_READY) &&
                SendShellCommand(WINRINGS_CMD_TOGGLE_MEDIA_ISLAND);
            if (sent && g_runtime.adminBridgeArmed)
                SetTimer(hwnd, kAdminConfigSyncTimer, 200, nullptr);
            HostDiagLog(L"TrayMenu ToggleMediaIsland: status=%d ctrl=0x%p sent=%d",
                static_cast<int>(g_runtime.status), g_runtime.shellControlWindow, sent ? 1 : 0);
            break;
        }
        case kMenuCmdSettings:
            LaunchSiblingTools(L"--settings");
            break;
        case kMenuCmdReload:
        {
            const bool sent = (g_runtime.status == WINRINGS_HOST_STATUS_READY) &&
                SendShellCommand(WINRINGS_CMD_RELOAD_CONFIG);
            ReloadAdminBridgeConfig();
            HostDiagLog(L"TrayMenu ReloadConfig: status=%d ctrl=0x%p sent=%d",
                static_cast<int>(g_runtime.status), g_runtime.shellControlWindow, sent ? 1 : 0);
            break;
        }
        case kMenuCmdConfigFolder:
        {
            const std::wstring dir = SwitcherConfig::AppDataDirectory();
            SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
            ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case kMenuCmdExit:
            RequestStop();
            break;
        default:
            break;
        }
    }

    const wchar_t* StatusText(WinRingsHostStatus status)
    {
        switch (status)
        {
        case WINRINGS_HOST_STATUS_STARTING: return L"starting";
        case WINRINGS_HOST_STATUS_INJECTING: return L"injecting";
        case WINRINGS_HOST_STATUS_READY: return L"ready";
        case WINRINGS_HOST_STATUS_FAILED: return L"failed";
        case WINRINGS_HOST_STATUS_STOPPING: return L"stopping";
        case WINRINGS_HOST_STATUS_SAFETY_DISABLED: return L"safety_disabled (3 Explorer crashes in 180s)";
        }
        return L"unknown";
    }

    LRESULT CALLBACK HostWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == g_runtime.hostControlMessage)
        {
            switch (static_cast<WinRingsHostControl>(wParam))
            {
            case WINRINGS_HOST_CTRL_TOGGLE:
                if (!g_runtime.interactiveInput)
                    return FALSE;
                if (g_runtime.status == WINRINGS_HOST_STATUS_READY)
                    return SendShellCommand(WINRINGS_CMD_TOGGLE_RING) ? TRUE : FALSE;
                if (g_runtime.status == WINRINGS_HOST_STATUS_STARTING ||
                    g_runtime.status == WINRINGS_HOST_STATUS_INJECTING)
                {
                    g_runtime.pendingToggle = true;
                    return TRUE;
                }
                return FALSE;
            case WINRINGS_HOST_CTRL_EXIT:
                RequestStop();
                return TRUE;
            case WINRINGS_HOST_CTRL_STATUS:
                return static_cast<LRESULT>(g_runtime.status);
            case WINRINGS_HOST_CTRL_RELOAD:
            {
                const bool ok = (g_runtime.status == WINRINGS_HOST_STATUS_READY &&
                    SendShellCommand(WINRINGS_CMD_RELOAD_CONFIG));
                ReloadAdminBridgeConfig();
                return ok ? TRUE : FALSE;
            }
            case WINRINGS_HOST_CTRL_TOGGLE_TOPBAR:
            {
                const bool ok = (g_runtime.status == WINRINGS_HOST_STATUS_READY) &&
                    SendShellCommand(WINRINGS_CMD_TOGGLE_TOPBAR);
                if (ok && g_runtime.adminBridgeArmed)
                    SetTimer(window, kAdminConfigSyncTimer, 200, nullptr);
                HostDiagLog(L"CLI --toggle-topbar: status=%d ctrl=0x%p sent=%d",
                    static_cast<int>(g_runtime.status), g_runtime.shellControlWindow, ok ? 1 : 0);
                return ok ? TRUE : FALSE;
            }
            case WINRINGS_HOST_CTRL_TOGGLE_MEDIA_ISLAND:
            {
                const bool ok = (g_runtime.status == WINRINGS_HOST_STATUS_READY) &&
                    SendShellCommand(WINRINGS_CMD_TOGGLE_MEDIA_ISLAND);
                if (ok && g_runtime.adminBridgeArmed)
                    SetTimer(window, kAdminConfigSyncTimer, 200, nullptr);
                HostDiagLog(L"CLI --toggle-media-island: status=%d ctrl=0x%p sent=%d",
                    static_cast<int>(g_runtime.status), g_runtime.shellControlWindow, ok ? 1 : 0);
                return ok ? TRUE : FALSE;
            }
            case WINRINGS_HOST_CTRL_QUERY_DETAILS:
            {
                LRESULT shellBits = 0;
                if (g_runtime.status == WINRINGS_HOST_STATUS_READY)
                    SendShellCommand(WINRINGS_CMD_QUERY_SHELL_STATE, &shellBits);
                if (g_runtime.adminBridgeArmed)
                    shellBits |= WINRINGS_STATE_ADMIN_BRIDGE_ACTIVE;
                return static_cast<LRESULT>(static_cast<DWORD>(g_runtime.status) |
                    (static_cast<DWORD>(shellBits & 0xFFFF) << 16));
            }
            case WINRINGS_HOST_CTRL_RESET_SAFETY:
                if (!ResetSafetyState())
                    return FALSE;
                if (g_runtime.status == WINRINGS_HOST_STATUS_SAFETY_DISABLED)
                    BeginConnection(true);
                return TRUE;
            default:
                return FALSE;
            }
        }

        if (message == g_runtime.shellStatusMessage)
        {
            const UINT64 payload = static_cast<UINT64>(wParam);
            HandleDllStatus(static_cast<WinRingsDllStatus>(static_cast<UINT32>(payload)),
                static_cast<DWORD>(payload >> 32), static_cast<UINT_PTR>(lParam));
            return 0;
        }

        if (message == g_runtime.taskbarCreatedMessage)
        {
            g_runtime.trayAdded = false;
            AddTrayIcon(window);
            if (!g_runtime.exitPending && !ExplorerProcessStillAlive())
            {
                g_runtime.retryIndex = 0;
                g_runtime.activationCommitted = false;
                BeginConnection(false);
            }
            return 0;
        }

        switch (message)
        {
        case kMsgAdminShakeTriggered:
            if (g_runtime.adminBridgeArmed && !IsOverlayWindowVisible())
            {
                bool suppressed = false;
                if (g_runtime.config.shakeSuppressFullscreen)
                {
                    QUERY_USER_NOTIFICATION_STATE quns = QUNS_ACCEPTS_NOTIFICATIONS;
                    if (SUCCEEDED(SHQueryUserNotificationState(&quns)) &&
                        (quns == QUNS_RUNNING_D3D_FULL_SCREEN || quns == QUNS_PRESENTATION_MODE))
                    {
                        suppressed = true;
                    }
                }
                if (!suppressed)
                    SendShellAdminCommand(WINRINGS_CMD_OPEN_RING_AT, PackPointLParam(g_runtime.pendingAdminShakePt));
            }
            return 0;

        case WM_COPYDATA:
            return HandleAdminWindowActionCopyData(
                reinterpret_cast<HWND>(wParam),
                reinterpret_cast<const COPYDATASTRUCT*>(lParam)) ? TRUE : FALSE;

        case WM_INPUT:
            if (g_runtime.adminBridgeArmed)
                g_runtime.adminGestures.OnRawInput(lParam);
            break;

        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            if (g_runtime.adminBridgeArmed)
                g_runtime.adminInput.UpdateCachedDpiScale(QueryHostPrimaryDpiScale());
            break;

        case kTrayCallbackMsg:
            if (lParam == WM_LBUTTONUP || lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU)
                ShowTrayContextMenu(window);
            return 0;

        case WM_WTSSESSION_CHANGE:
            if (wParam == WTS_SESSION_LOGOFF) g_runtime.sessionEnding = true;
            else if (wParam == WTS_SESSION_LOGON) g_runtime.sessionEnding = false;
            if (wParam == WTS_SESSION_LOCK || wParam == WTS_SESSION_LOGOFF || wParam == WTS_REMOTE_DISCONNECT)
            {
                g_runtime.sessionInputInteractive = false;
                AdvanceInteractiveInputGeneration();
            }
            else if (wParam == WTS_SESSION_UNLOCK || wParam == WTS_SESSION_LOGON || wParam == WTS_REMOTE_CONNECT)
            {
                g_runtime.sessionInputInteractive = true;
                AdvanceInteractiveInputGeneration();
            }
            return 0;

        case WM_POWERBROADCAST:
            if (wParam == PBT_APMSUSPEND)
            {
                g_runtime.powerInputInteractive = false;
                AdvanceInteractiveInputGeneration();
            }
            else if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND || wParam == PBT_APMRESUMECRITICAL)
            {
                g_runtime.powerInputInteractive = true;
                AdvanceInteractiveInputGeneration();
            }
            return TRUE;

        case WM_TIMER:
            if (wParam == kRetryTimer)
            {
                KillTimer(window, kRetryTimer);
                if (!TryInject())
                    ScheduleRetry();
                return 0;
            }
            if (wParam == kInjectionTimer)
            {
                KillTimer(window, kInjectionTimer);
                if (g_runtime.initializationPending)
                {
                    g_runtime.initializationPending = false;
                    g_runtime.status = WINRINGS_HOST_STATUS_FAILED;
                    DebugLog(L"Explorer initialization timed out");
                }
                return 0;
            }
            if (wParam == kStopTimer)
            {
                KillTimer(window, kStopTimer);
                g_runtime.status = WINRINGS_HOST_STATUS_FAILED;
                DestroyWindow(window);
                return 0;
            }
            if (wParam == kTrayRetryTimer)
            {
                AddTrayIcon(window);
                return 0;
            }
            if (wParam == kAdminWatchdogTimer)
            {
                if (g_runtime.adminBridgeArmed)
                {
                    g_runtime.adminGestures.EnsureDeviceRegistration();
                    g_runtime.adminInput.UpdateCachedDpiScale(QueryHostPrimaryDpiScale());
                }
                return 0;
            }
            if (wParam == kAdminConfigSyncTimer)
            {
                KillTimer(window, kAdminConfigSyncTimer);
                ReloadAdminBridgeConfig();
                return 0;
            }
            break;

        case WM_CLOSE:
            if (!g_runtime.exitPending)
                RequestStop();
            return 0;

        case WM_QUERYENDSESSION:
            g_runtime.sessionEnding = true;
            AdvanceInteractiveInputGeneration();
            return TRUE;

        case WM_ENDSESSION:
            if (wParam)
                DestroyWindow(window);
            else
            {
                g_runtime.sessionEnding = false;
                AdvanceInteractiveInputGeneration();
            }
            return 0;

        case WM_DESTROY:
            DisarmAdminInputBridge();
            RemoveTrayIcon(window);
            UnregisterSessionNotifications();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    enum class LaunchCommand
    {
        Host,
        Toggle,
        ToggleTopBar,
        ToggleMediaIsland,
        Exit,
        Status,
        Reload,
        ResetSafety,
        Settings,
        Version,
        Preview,
        Invalid
    };

    LaunchCommand ParseCommand(int argc, wchar_t** argv)
    {
        if (argc == 1) return LaunchCommand::Host;
        if (argc != 2) return LaunchCommand::Invalid;
        if (_wcsicmp(argv[1], L"--toggle") == 0) return LaunchCommand::Toggle;
        if (_wcsicmp(argv[1], L"--toggle-topbar") == 0) return LaunchCommand::ToggleTopBar;
        if (_wcsicmp(argv[1], L"--toggle-media-island") == 0) return LaunchCommand::ToggleMediaIsland;
        if (_wcsicmp(argv[1], L"--exit") == 0) return LaunchCommand::Exit;
        if (_wcsicmp(argv[1], L"--status") == 0) return LaunchCommand::Status;
        if (_wcsicmp(argv[1], L"--reload") == 0) return LaunchCommand::Reload;
        if (_wcsicmp(argv[1], L"--reset-safety") == 0) return LaunchCommand::ResetSafety;
        if (_wcsicmp(argv[1], L"--settings") == 0) return LaunchCommand::Settings;
        if (_wcsicmp(argv[1], L"--version") == 0) return LaunchCommand::Version;
        if (_wcsicmp(argv[1], L"--preview") == 0) return LaunchCommand::Preview;
        return LaunchCommand::Invalid;
    }

    int ForwardToExistingHost(LaunchCommand cmd, UINT controlMessage)
    {
        HWND window = nullptr;
        for (unsigned attempt = 0; attempt < 40; ++attempt)
        {
            window = FindWindowW(WINRINGS_HOST_WINDOW_CLASS_W, WINRINGS_HOST_WINDOW_TITLE_W);
            if (window) break;
            Sleep(25);
        }
        if (!window)
            return ERROR_SERVICE_NOT_ACTIVE;

        if (cmd == LaunchCommand::Toggle)
        {
            DWORD hostPid = 0;
            GetWindowThreadProcessId(window, &hostPid);
            if (hostPid)
                AllowSetForegroundWindow(hostPid);
        }

        WinRingsHostControl ctrl = WINRINGS_HOST_CTRL_STATUS;
        switch (cmd)
        {
        case LaunchCommand::Toggle: ctrl = WINRINGS_HOST_CTRL_TOGGLE; break;
        case LaunchCommand::ToggleTopBar: ctrl = WINRINGS_HOST_CTRL_TOGGLE_TOPBAR; break;
        case LaunchCommand::ToggleMediaIsland: ctrl = WINRINGS_HOST_CTRL_TOGGLE_MEDIA_ISLAND; break;
        case LaunchCommand::Exit: ctrl = WINRINGS_HOST_CTRL_EXIT; break;
        case LaunchCommand::Status: ctrl = WINRINGS_HOST_CTRL_QUERY_DETAILS; break;
        case LaunchCommand::Reload: ctrl = WINRINGS_HOST_CTRL_RELOAD; break;
        case LaunchCommand::ResetSafety: ctrl = WINRINGS_HOST_CTRL_RESET_SAFETY; break;
        default: break;
        }

        DWORD_PTR result = 0;
        if (!SendMessageTimeoutW(window, controlMessage, static_cast<WPARAM>(ctrl), 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &result))
            return static_cast<int>(GetLastError());

        if (cmd == LaunchCommand::Status)
        {
            const UINT32 packed = static_cast<UINT32>(result);
            const auto hostStatus = static_cast<WinRingsHostStatus>(packed & 0xFFFF);
            const UINT32 bits = (packed >> 16) & 0xFFFF;
            wchar_t msg[256]{};
            swprintf_s(msg, L"WinRingsHost: %s (ring=%d xaml=%d hook=%d backdrop=%d clock=%d appsHidden=%d media=%d admin=%d)",
                StatusText(hostStatus),
                (bits & WINRINGS_STATE_RING_READY) ? 1 : 0,
                (bits & WINRINGS_STATE_XAML_ATTACHED) ? 1 : 0,
                (bits & WINRINGS_STATE_TOPBAR_HOOKED) ? 1 : 0,
                (bits & WINRINGS_STATE_BACKDROP_ACTIVE) ? 1 : 0,
                (bits & WINRINGS_STATE_CLOCK_ACTIVE) ? 1 : 0,
                (bits & WINRINGS_STATE_APPS_HIDDEN) ? 1 : 0,
                (bits & WINRINGS_STATE_MEDIA_ISLAND_ACTIVE) ? 1 : 0,
                (bits & WINRINGS_STATE_ADMIN_BRIDGE_ACTIVE) ? 1 : 0);
            WriteParentConsole(msg);
            return hostStatus == WINRINGS_HOST_STATUS_READY ? 0 : static_cast<int>(hostStatus);
        }
        return result ? 0 : ERROR_GEN_FAILURE;
    }

    int RunPreviewMode()
    {
        if (!LoadShellDllLocally())
            return ERROR_MOD_NOT_FOUND;
        auto runPreview = reinterpret_cast<WinRingsRunPreviewFn>(
            GetProcAddress(g_runtime.shellDll, WINRINGS_RUN_PREVIEW_EXPORT_A));
        if (!runPreview)
            return ERROR_PROC_NOT_FOUND;
        return runPreview();
    }
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
#ifndef _WIN64
#error WinRingsHost requires x64.
#endif
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return static_cast<int>(GetLastError());
    const LaunchCommand cmd = ParseCommand(argc, argv);
    LocalFree(argv);

    if (cmd == LaunchCommand::Version)
    {
        WriteParentConsole(L"WinRingsHost " WINRINGS_PRODUCT_VERSION_STR_W L" (ABI 1.0)");
        return 0;
    }
    if (cmd == LaunchCommand::Settings)
    {
        LaunchSiblingTools(L"--settings");
        return 0;
    }
    if (cmd == LaunchCommand::Preview)
    {
        return RunPreviewMode();
    }
    if (cmd == LaunchCommand::Invalid)
    {
        return ERROR_BAD_ARGUMENTS;
    }

    g_runtime.instance = instance;
    g_runtime.isElevated = IsCurrentProcessElevated();
    g_runtime.hostControlMessage = RegisterWindowMessageW(WINRINGS_HOST_CONTROL_MESSAGE_W);
    g_runtime.shellStatusMessage = RegisterWindowMessageW(WINRINGS_SHELL_STATUS_MESSAGE_W);
    g_runtime.shellCommandMessage = RegisterWindowMessageW(WINRINGS_SHELL_COMMAND_MESSAGE_W);
    g_runtime.taskbarCreatedMessage = RegisterWindowMessageW(WINRINGS_TASKBAR_CREATED_MESSAGE_W);
    if (!g_runtime.hostControlMessage || !g_runtime.shellStatusMessage ||
        !g_runtime.shellCommandMessage || !g_runtime.taskbarCreatedMessage)
        return static_cast<int>(GetLastError());

    SetLastError(ERROR_SUCCESS);
    UniqueHandle instanceMutex(CreateMutexW(nullptr, TRUE, WINRINGS_HOST_MUTEX_W));
    const DWORD mutexErr = GetLastError();
    if (!instanceMutex && mutexErr != ERROR_ACCESS_DENIED)
        return static_cast<int>(mutexErr);
    if (mutexErr == ERROR_ALREADY_EXISTS || mutexErr == ERROR_ACCESS_DENIED)
    {
        if (cmd == LaunchCommand::Host)
            return 0;
        return ForwardToExistingHost(cmd, g_runtime.hostControlMessage);
    }

    if (cmd == LaunchCommand::Status)
    {
        WriteParentConsole(L"WinRingsHost: stopped");
        ReleaseMutex(instanceMutex.get());
        return ERROR_SERVICE_NOT_ACTIVE;
    }
    if (cmd == LaunchCommand::Exit)
    {
        ReleaseMutex(instanceMutex.get());
        return 0;
    }

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = HostWindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = WINRINGS_HOST_WINDOW_CLASS_W;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        ReleaseMutex(instanceMutex.get());
        return static_cast<int>(GetLastError());
    }

    EnableDarkWin32Menus();

    g_runtime.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        WINRINGS_HOST_WINDOW_CLASS_W, WINRINGS_HOST_WINDOW_TITLE_W, WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!g_runtime.window)
    {
        ReleaseMutex(instanceMutex.get());
        return static_cast<int>(GetLastError());
    }

    ChangeWindowMessageFilterEx(g_runtime.window, g_runtime.hostControlMessage, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_runtime.window, g_runtime.shellStatusMessage, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_runtime.window, g_runtime.taskbarCreatedMessage, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_runtime.window, kTrayCallbackMsg, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_runtime.window, WM_COPYDATA, MSGFLT_ALLOW, nullptr);

    HostDiagLog(L"Startup v%s elevated=%d", WINRINGS_PRODUCT_VERSION_STR_W, g_runtime.isElevated ? 1 : 0);

    LoadSafetyState();
    if (cmd == LaunchCommand::ResetSafety && !ResetSafetyState())
    {
        DestroyWindow(g_runtime.window);
        ReleaseMutex(instanceMutex.get());
        return ERROR_WRITE_FAULT;
    }

    RegisterSessionNotifications();
    AddTrayIcon(g_runtime.window);
    if (cmd == LaunchCommand::Toggle)
        g_runtime.pendingToggle = true;

    BeginConnection(true);

    MSG msg{};
    int result = 0;
    bool running = true;
    while (running)
    {
        HANDLE handles[1] = { g_runtime.explorer.process.get() };
        const DWORD handleCount = handles[0] ? 1u : 0u;
        const DWORD waitResult = MsgWaitForMultipleObjectsEx(handleCount,
            handleCount ? handles : nullptr, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (waitResult == WAIT_FAILED)
        {
            result = static_cast<int>(GetLastError());
            break;
        }
        if (handleCount && waitResult == WAIT_OBJECT_0)
        {
            HandleExplorerProcessExit();
            continue;
        }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    CancelTimers();
    DisarmAdminInputBridge();
    g_runtime.explorer.clear();
    if (g_runtime.shellDll)
        FreeLibrary(g_runtime.shellDll);
    if (g_runtime.window && IsWindow(g_runtime.window))
        DestroyWindow(g_runtime.window);
    ReleaseMutex(instanceMutex.get());
    return result;
}

namespace winrings
{
    void ShellDiagLog(const wchar_t* tag, const wchar_t* fmt, ...) noexcept
    {
        try
        {
            wchar_t body[512]{};
            va_list ap{};
            va_start(ap, fmt);
            _vsnwprintf_s(body, _countof(body), _TRUNCATE, fmt ? fmt : L"", ap);
            va_end(ap);
            HostDiagLog(L"[%s] %s", tag ? tag : L"Admin", body);
        }
        catch (...) {}
    }
} // namespace winrings
