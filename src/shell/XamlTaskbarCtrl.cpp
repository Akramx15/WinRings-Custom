#include "XamlTaskbarCtrl.h"

#include <Unknwn.h>
#include <ocidl.h>
#include <xamlom.h>

#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../shared/ClockFormat.h"
#include "../shared/WinRingsApi.h"
#include "../shared/WinRingsIdentity.h"
#include "ShellController.h"
#include "TopBarHook.h"

namespace winrt
{
    using namespace Windows::Foundation;
    using namespace Windows::UI;
    using namespace Windows::UI::Xaml;
    using namespace Windows::UI::Xaml::Automation;
    using namespace Windows::UI::Xaml::Controls;
    using namespace Windows::UI::Xaml::Media;
    using namespace Windows::UI::Xaml::Shapes;
}

namespace winrings
{
    namespace
    {
        namespace xaml = winrt::Windows::UI::Xaml;
        using winrt::com_ptr;
        using winrt::Windows::Foundation::IInspectable;

        constexpr UINT kMsgApplyConfig   = WM_APP + 210;
        constexpr UINT kMsgRestoreAll    = WM_APP + 211;
        constexpr UINT kMsgRunFn         = WM_APP + 212;
        constexpr UINT_PTR kClockTimer   = 201;
        constexpr UINT_PTR kReapplyTimer = 202;
        constexpr wchar_t kXamlMsgClass[] = L"WinRings.XamlUiBridge.v1";

        struct TrackedProp
        {
            xaml::DependencyObject obj{ nullptr };
            xaml::DependencyProperty prop{ nullptr };
            IInspectable origValue{ nullptr };
            int64_t token = 0;
            bool tokenRegistered = false;
            bool origCaptured = false;
        };

        enum class ElementRole : size_t
        {
            HostRootGrid = 0,
            TaskbarFrame,
            TaskbarRootGrid,
            SystemTrayFrame,
            SystemTrayFrameGrid,
            NotificationCenterButton,
            ClockButton,
            DateTimeIconContent,
            DateTimeStackPanel,
            ControlCenterButton,
            SystemTrayCompactItem,
            DateInnerTextBlock,
            TimeInnerTextBlock,
            TaskbarFrameRepeater,
            AppTaskButton,
            StartButton,
            SearchBoxButton,
            TaskViewButton,
            WidgetsButton,
            BackgroundFill,
            BackgroundStroke,
            ScreenEdgeStroke,
            Count
        };

        const wchar_t* RoleName(ElementRole r) noexcept
        {
            switch (r)
            {
            case ElementRole::HostRootGrid: return L"HostRootGrid";
            case ElementRole::TaskbarFrame: return L"TaskbarFrame";
            case ElementRole::TaskbarRootGrid: return L"TaskbarRootGrid";
            case ElementRole::SystemTrayFrame: return L"SystemTrayFrame";
            case ElementRole::SystemTrayFrameGrid: return L"SystemTrayFrameGrid";
            case ElementRole::NotificationCenterButton: return L"NotificationCenterButton";
            case ElementRole::ClockButton: return L"ClockButton";
            case ElementRole::DateTimeIconContent: return L"DateTimeIconContent";
            case ElementRole::DateTimeStackPanel: return L"DateTimeStackPanel";
            case ElementRole::ControlCenterButton: return L"ControlCenterButton";
            case ElementRole::SystemTrayCompactItem: return L"SystemTrayCompactItem";
            case ElementRole::DateInnerTextBlock: return L"DateInnerTextBlock";
            case ElementRole::TimeInnerTextBlock: return L"TimeInnerTextBlock";
            case ElementRole::TaskbarFrameRepeater: return L"TaskbarFrameRepeater";
            case ElementRole::AppTaskButton: return L"AppTaskButton";
            case ElementRole::StartButton: return L"StartButton";
            case ElementRole::SearchBoxButton: return L"SearchBoxButton";
            case ElementRole::TaskViewButton: return L"TaskViewButton";
            case ElementRole::WidgetsButton: return L"WidgetsButton";
            case ElementRole::BackgroundFill: return L"BackgroundFill";
            case ElementRole::BackgroundStroke: return L"BackgroundStroke";
            case ElementRole::ScreenEdgeStroke: return L"ScreenEdgeStroke";
            default: return L"Unknown";
            }
        }

        struct TrackedElement
        {
            InstanceHandle handle = 0;
            bool synthetic = false;
            ElementRole role = ElementRole::BackgroundFill;
            xaml::UIElement element{ nullptr };
            xaml::DependencyObject rootFrame{ nullptr }; // owning TaskbarFrame or SystemTrayFrame
            std::vector<TrackedProp> props;
        };

        struct UiRunTask
        {
            std::function<void()> fn;
            std::atomic<bool> cancelled{ false };
        };

        class TaskbarWatcher;

        struct SharedState
        {
            std::mutex cfgMutex;
            SwitcherConfig config = SwitcherConfig::CreateDefault();
            HMODULE shellModule = nullptr;
            std::function<void()> onFirstAttached;
            std::function<void()> onTrayMetricsChanged;

            std::atomic<bool> attachWorkerRunning{ false };
            std::atomic<ULONGLONG> lastAttachFinishTick{ 0 };
            std::atomic<bool> active{ false };
            std::atomic<bool> attached{ false };
            std::atomic<UINT32> stateBits{ 0 };
            std::atomic<int> systemTrayWidthDip{ 0 };
            std::atomic<DWORD> uiThreadId{ 0 };
            std::atomic<HWND> uiHwnd{ nullptr };

            std::mutex runTasksMutex;
            std::unordered_map<ULONG_PTR, std::shared_ptr<UiRunTask>> runTasks;
            std::atomic<ULONG_PTR> nextRunCookie{ 1 };

            std::mutex watchersMutex;
            std::vector<com_ptr<TaskbarWatcher>> watchers;
            std::array<bool, 65> connectedEndpoints{};
        };

        SharedState g_xaml;

        bool IsTaskbarOrSystemTrayRootObj(const xaml::DependencyObject& obj, bool& isTaskbar, bool& isSystemTray) noexcept
        {
            isTaskbar = false;
            isSystemTray = false;
            try
            {
                if (auto fe = obj.try_as<xaml::FrameworkElement>())
                {
                    const auto name = fe.Name();
                    const auto cls = winrt::get_class_name(fe);
                    if (name == L"TaskbarFrame" || cls == L"Taskbar.TaskbarFrame")
                    {
                        isTaskbar = true;
                        return true;
                    }
                    if (name == L"SystemTrayFrame" || cls == L"SystemTray.SystemTrayFrame")
                    {
                        isSystemTray = true;
                        return true;
                    }
                }
            }
            catch (...) {}
            return false;
        }

        bool FindTaskbarOrSystemTrayAncestor(xaml::DependencyObject obj, xaml::DependencyObject& outRootFrame)
        {
            outRootFrame = nullptr;
            try
            {
                for (int depth = 0; obj && depth < 48; ++depth)
                {
                    bool isTb = false;
                    bool isTray = false;
                    if (IsTaskbarOrSystemTrayRootObj(obj, isTb, isTray))
                    {
                        outRootFrame = obj;
                        return true;
                    }
                    obj = xaml::Media::VisualTreeHelper::GetParent(obj);
                }
            }
            catch (...) {}
            return false;
        }

        bool ClassifyElementRole(const wchar_t* name, const wchar_t* type, const wchar_t* autoId, ElementRole& outRole) noexcept
        {
            const wchar_t* n = name ? name : L"";
            const wchar_t* t = type ? type : L"";
            const wchar_t* a = autoId ? autoId : L"";

            if (wcscmp(n, L"TaskbarFrame") == 0 || wcsstr(t, L"Taskbar.TaskbarFrame") != nullptr)
            {
                outRole = ElementRole::TaskbarFrame;
                return true;
            }
            if (wcscmp(n, L"SystemTrayFrame") == 0 || wcsstr(t, L"SystemTray.SystemTrayFrame") != nullptr)
            {
                outRole = ElementRole::SystemTrayFrame;
                return true;
            }
            if (wcscmp(n, L"SystemTrayFrameGrid") == 0)
            {
                outRole = ElementRole::SystemTrayFrameGrid;
                return true;
            }
            if (wcscmp(n, L"BackgroundFill") == 0)
            {
                outRole = ElementRole::BackgroundFill;
                return true;
            }
            if (wcscmp(n, L"BackgroundStroke") == 0)
            {
                outRole = ElementRole::BackgroundStroke;
                return true;
            }
            if (wcscmp(n, L"ScreenEdgeStroke") == 0)
            {
                outRole = ElementRole::ScreenEdgeStroke;
                return true;
            }
            if (wcscmp(n, L"TaskbarFrameRepeater") == 0)
            {
                outRole = ElementRole::TaskbarFrameRepeater;
                return true;
            }
            if (wcsstr(t, L"Taskbar.TaskListButton") != nullptr ||
                wcsstr(t, L"Taskbar.OverflowToggleButton") != nullptr)
            {
                outRole = ElementRole::AppTaskButton;
                return true;
            }
            if (wcscmp(n, L"StartButton") == 0 || wcscmp(a, L"StartButton") == 0)
            {
                outRole = ElementRole::StartButton;
                return true;
            }
            if (wcscmp(n, L"SearchBoxButton") == 0 || wcscmp(n, L"SearchButton") == 0 ||
                wcscmp(a, L"SearchButton") == 0 || wcscmp(a, L"SearchBoxButton") == 0 ||
                wcsstr(t, L"Taskbar.SearchBoxButton") != nullptr ||
                wcsstr(t, L"Taskbar.SearchBoxLaunchListButton") != nullptr)
            {
                outRole = ElementRole::SearchBoxButton;
                return true;
            }
            if (wcscmp(n, L"TaskViewButton") == 0 || wcscmp(a, L"TaskViewButton") == 0)
            {
                outRole = ElementRole::TaskViewButton;
                return true;
            }
            if (wcscmp(n, L"WidgetsButton") == 0 || wcscmp(a, L"WidgetsButton") == 0 ||
                wcscmp(a, L"DashboardButton") == 0 ||
                wcsstr(t, L"Taskbar.AugmentedEntryPointButton") != nullptr)
            {
                outRole = ElementRole::WidgetsButton;
                return true;
            }
            if (wcscmp(n, L"DateInnerTextBlock") == 0)
            {
                outRole = ElementRole::DateInnerTextBlock;
                return true;
            }
            if (wcscmp(n, L"TimeInnerTextBlock") == 0)
            {
                outRole = ElementRole::TimeInnerTextBlock;
                return true;
            }
            if (wcscmp(n, L"NotificationCenterButton") == 0)
            {
                outRole = ElementRole::NotificationCenterButton;
                return true;
            }
            if (wcscmp(n, L"ClockButton") == 0 || wcscmp(n, L"SecondaryClockStack") == 0)
            {
                outRole = ElementRole::ClockButton;
                return true;
            }
            if (wcscmp(n, L"DateTimeIconContent") == 0 || wcsstr(t, L"SystemTray.DateTimeIconContent") != nullptr)
            {
                outRole = ElementRole::DateTimeIconContent;
                return true;
            }
            if (wcscmp(n, L"DateTimeStackPanel") == 0)
            {
                outRole = ElementRole::DateTimeStackPanel;
                return true;
            }
            if (wcscmp(n, L"ControlCenterButton") == 0)
            {
                outRole = ElementRole::ControlCenterButton;
                return true;
            }
            if (wcscmp(n, L"NotifyIconStack") == 0 ||
                wcscmp(n, L"NonActivatableStack") == 0 ||
                wcscmp(n, L"MainStack") == 0 ||
                wcscmp(n, L"ShowDesktopStack") == 0 ||
                wcscmp(n, L"IconStack") == 0 ||
                wcscmp(n, L"SystemTrayIcon") == 0 ||
                wcsstr(t, L"SystemTray.Stack") != nullptr ||
                wcsstr(t, L"SystemTray.StackListView") != nullptr ||
                wcsstr(t, L"SystemTray.ChevronIconView") != nullptr ||
                wcsstr(t, L"SystemTray.NotifyIconView") != nullptr ||
                wcsstr(t, L"SystemTray.IconView") != nullptr ||
                wcsstr(t, L"SystemTray.OmniButton") != nullptr ||
                wcsstr(t, L"SystemTray.NormalButton") != nullptr ||
                wcsstr(t, L"SystemTray.AccentButton") != nullptr)
            {
                outRole = ElementRole::SystemTrayCompactItem;
                return true;
            }
            return false;
        }

        bool ClassifyLiveElement(const xaml::UIElement& uiElem, ElementRole& outRole) noexcept
        {
            try
            {
                winrt::hstring nameH;
                if (auto fe = uiElem.try_as<xaml::FrameworkElement>())
                    nameH = fe.Name();
                const winrt::hstring typeH = winrt::get_class_name(uiElem);
                const winrt::hstring autoIdH = xaml::Automation::AutomationProperties::GetAutomationId(uiElem);
                return ClassifyElementRole(nameH.c_str(), typeH.c_str(), autoIdH.c_str(), outRole);
            }
            catch (...)
            {
                return false;
            }
        }

        void RestoreProp(TrackedProp& tp) noexcept
        {
            if (!tp.obj || !tp.prop || !tp.origCaptured)
                return;
            try
            {
                if (!tp.origValue || tp.origValue == xaml::DependencyProperty::UnsetValue())
                    tp.obj.ClearValue(tp.prop);
                else
                    tp.obj.SetValue(tp.prop, tp.origValue);
            }
            catch (...) {}
        }

        void SnapshotProp(TrackedElement& te, const xaml::DependencyObject& obj,
            const xaml::DependencyProperty& prop, bool watchChanges,
            std::function<void(const xaml::DependencyObject&, const xaml::DependencyProperty&)> onChange)
        {
            for (const auto& p : te.props)
            {
                if (p.prop == prop && p.obj == obj)
                    return;
            }
            TrackedProp tp;
            tp.obj = obj;
            tp.prop = prop;
            try
            {
                tp.origValue = obj.ReadLocalValue(prop);
                tp.origCaptured = true;
                if (watchChanges && onChange)
                {
                    tp.token = obj.RegisterPropertyChangedCallback(prop,
                        [cb = std::move(onChange)](const xaml::DependencyObject& s, const xaml::DependencyProperty& d) {
                            cb(s, d);
                        });
                    tp.tokenRegistered = true;
                }
            }
            catch (...) {}
            te.props.push_back(std::move(tp));
        }

        void RefreshOrigSnapshot(TrackedElement& te, const xaml::DependencyObject& obj,
            const xaml::DependencyProperty& prop)
        {
            for (auto& p : te.props)
            {
                if (p.obj == obj && p.prop == prop)
                {
                    try { p.origValue = obj.ReadLocalValue(prop); }
                    catch (...) {}
                    return;
                }
            }
        }

        bool IsTaskbarFramePrimaryOnly(const xaml::DependencyObject& frameObj)
        {
            try
            {
                auto fe = frameObj.try_as<xaml::FrameworkElement>();
                if (!fe)
                    return true;
                auto xr = fe.XamlRoot();
                if (!xr)
                    return true;
                const double scale = xr.RasterizationScale();
                const auto sz = xr.Size();
                HMONITOR primaryMon = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
                MONITORINFO mi{ sizeof(mi) };
                if (GetMonitorInfoW(primaryMon, &mi))
                {
                    const double primWidthDip = (mi.rcMonitor.right - mi.rcMonitor.left) / (scale > 0.1 ? scale : 1.0);
                    return std::abs(sz.Width - primWidthDip) < 4.0;
                }
            }
            catch (...) {}
            return true;
        }

        class TaskbarWatcher : public winrt::implements<TaskbarWatcher, IVisualTreeServiceCallback2, winrt::non_agile>
        {
        public:
            explicit TaskbarWatcher(IUnknown* site)
            {
                winrt::check_hresult(site->QueryInterface(m_diagnostics.put()));
            }

            void AttachUiWindowIfNeeded()
            {
                HWND localHwnd = m_hwnd.load(std::memory_order_acquire);
                if (localHwnd && !IsWindow(localHwnd))
                {
                    m_hwnd.store(nullptr, std::memory_order_release);
                    localHwnd = nullptr;
                }

                if (!localHwnd)
                {
                    WNDCLASSEXW wc{ sizeof(wc) };
                    wc.lpfnWndProc = UiWndProc;
                    wc.hInstance = g_xaml.shellModule;
                    wc.lpszClassName = kXamlMsgClass;
                    RegisterClassExW(&wc);
                    localHwnd = CreateWindowExW(0, kXamlMsgClass, L"", 0, 0, 0, 0, 0,
                        HWND_MESSAGE, nullptr, g_xaml.shellModule, this);
                    if (localHwnd)
                    {
                        m_hwnd.store(localHwnd, std::memory_order_release);
                        const DWORD tid = GetCurrentThreadId();
                        SetTimer(localHwnd, kClockTimer, 1000, nullptr);
                        SetTimer(localHwnd, kReapplyTimer, 1500, nullptr);
                        if (!m_winEventHook)
                        {
                            m_winEventHook = SetWinEventHook(
                                EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE,
                                g_xaml.shellModule, TrayPopupWinEventProc,
                                GetCurrentProcessId(), 0, WINEVENT_INCONTEXT);
                        }
                        ShellDiagLog(L"XamlCtrl", L"Created XAML UI bridge hwnd=0x%p tid=%lu winEventHook=0x%p",
                            localHwnd, tid, m_winEventHook);
                    }
                    else
                    {
                        ShellDiagLog(L"XamlCtrl", L"CreateWindowExW FAILED for UI bridge err=%lu mod=0x%p",
                            GetLastError(), g_xaml.shellModule);
                        return;
                    }
                }

                const DWORD tid = GetCurrentThreadId();
                std::lock_guard<std::mutex> lock(g_xaml.watchersMutex);

                const HWND curHwnd = g_xaml.uiHwnd.load(std::memory_order_acquire);
                if (!curHwnd || !IsWindow(curHwnd))
                {
                    g_xaml.uiHwnd.store(localHwnd, std::memory_order_release);
                    g_xaml.uiThreadId.store(tid, std::memory_order_release);
                }

                g_xaml.watchers.erase(
                    std::remove_if(g_xaml.watchers.begin(), g_xaml.watchers.end(),
                        [](const com_ptr<TaskbarWatcher>& w) {
                            const HWND h = w ? w->UiHwnd() : nullptr;
                            return !h || !IsWindow(h);
                        }),
                    g_xaml.watchers.end());

                bool alreadyRegistered = false;
                for (const auto& existing : g_xaml.watchers)
                {
                    if (existing.get() == this)
                    {
                        alreadyRegistered = true;
                        break;
                    }
                }
                if (!alreadyRegistered && g_xaml.watchers.size() < 16)
                {
                    g_xaml.watchers.push_back(get_strong());
                }
            }

            HWND UiHwnd() const noexcept { return m_hwnd.load(std::memory_order_acquire); }

            void RequestApply() noexcept
            {
                const HWND h = m_hwnd.load(std::memory_order_acquire);
                if (h && IsWindow(h))
                    PostMessageW(h, kMsgApplyConfig, 0, 0);
            }

            void RequestRestoreSync(DWORD timeoutMs) noexcept
            {
                const HWND h = m_hwnd.load(std::memory_order_acquire);
                if (h && IsWindow(h))
                {
                    DWORD_PTR res = 0;
                    SendMessageTimeoutW(h, kMsgRestoreAll, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, timeoutMs, &res);
                }
            }

            void ApplyCurrentConfigOnUiThread()
            {
                SwitcherConfig cfg;
                {
                    std::lock_guard<std::mutex> lock(g_xaml.cfgMutex);
                    cfg = g_xaml.config;
                }

                TopBarHook::InstallSystemTrayHook();

                // Rescan known TaskbarFrame / SystemTrayFrame subtrees so dynamically instantiated
                // repeater items (Start/Search/TaskView/Widgets/Apps) or late-loaded tray children are captured.
                RescanKnownRoots();

                if (!cfg.topBarEnabled)
                {
                    RestoreAllOnUiThread();
                    g_xaml.stateBits.store(WINRINGS_STATE_XAML_ATTACHED, std::memory_order_release);
                    return;
                }

                g_xaml.active.store(true, std::memory_order_release);
                const std::wstring formattedClock = FormatCurrentClock(cfg);
                UINT32 bits = WINRINGS_STATE_XAML_ATTACHED;

                std::array<int, static_cast<size_t>(ElementRole::Count)> roleCounts{};
                m_changing = true;
                for (auto& item : m_elements)
                {
                    if (!item.element)
                        continue;
                    const size_t rIdx = static_cast<size_t>(item.role);
                    if (rIdx < roleCounts.size())
                        roleCounts[rIdx]++;
                    try
                    {
                        if (!cfg.topBarAllMonitors && item.rootFrame && !IsTaskbarFramePrimaryOnly(item.rootFrame))
                        {
                            RestoreElement(item);
                            continue;
                        }
                        ApplyToElement(item, cfg, formattedClock, bits);
                    }
                    catch (...) {}
                }
                m_changing = false;

                m_lastBits = bits;
                g_xaml.stateBits.store(bits, std::memory_order_release);

                const size_t countNow = m_elements.size();
                if (countNow != m_lastLoggedCount || bits != m_lastLoggedBits || m_lastLoggedEnabled != 1)
                {
                    m_lastLoggedCount = countNow;
                    m_lastLoggedBits = bits;
                    m_lastLoggedEnabled = 1;
                    ShellDiagLog(L"XamlCtrl",
                        L"Applied(en=1 top=%d h=%d bd=%d) total=%u bits=0x%X [HostG=%d TbF=%d TbRG=%d TrayF=%d TrayG=%d TrayI=%d Rep=%d App=%d Start=%d Srch=%d TV=%d Wdg=%d BgF=%d BgS=%d NCB=%d CCB=%d Time=%d Date=%d]",
                        cfg.topBarDockTop ? 1 : 0, cfg.topBarHeightDip, static_cast<int>(cfg.topBarBackdrop),
                        static_cast<unsigned>(countNow), bits,
                        roleCounts[static_cast<size_t>(ElementRole::HostRootGrid)],
                        roleCounts[static_cast<size_t>(ElementRole::TaskbarFrame)],
                        roleCounts[static_cast<size_t>(ElementRole::TaskbarRootGrid)],
                        roleCounts[static_cast<size_t>(ElementRole::SystemTrayFrame)],
                        roleCounts[static_cast<size_t>(ElementRole::SystemTrayFrameGrid)],
                        roleCounts[static_cast<size_t>(ElementRole::SystemTrayCompactItem)],
                        roleCounts[static_cast<size_t>(ElementRole::TaskbarFrameRepeater)],
                        roleCounts[static_cast<size_t>(ElementRole::AppTaskButton)],
                        roleCounts[static_cast<size_t>(ElementRole::StartButton)],
                        roleCounts[static_cast<size_t>(ElementRole::SearchBoxButton)],
                        roleCounts[static_cast<size_t>(ElementRole::TaskViewButton)],
                        roleCounts[static_cast<size_t>(ElementRole::WidgetsButton)],
                        roleCounts[static_cast<size_t>(ElementRole::BackgroundFill)],
                        roleCounts[static_cast<size_t>(ElementRole::BackgroundStroke)],
                        roleCounts[static_cast<size_t>(ElementRole::NotificationCenterButton)],
                        roleCounts[static_cast<size_t>(ElementRole::ControlCenterButton)],
                        roleCounts[static_cast<size_t>(ElementRole::TimeInnerTextBlock)],
                        roleCounts[static_cast<size_t>(ElementRole::DateInnerTextBlock)]);
                }
            }

            void RestoreAllOnUiThread()
            {
                g_xaml.active.store(false, std::memory_order_release);
                m_changing = true;
                for (auto& item : m_elements)
                {
                    try { RestoreElement(item); }
                    catch (...) {}
                }
                m_changing = false;
                const UINT32 bits = g_xaml.attached.load(std::memory_order_acquire) ? WINRINGS_STATE_XAML_ATTACHED : 0u;
                m_lastBits = bits;
                g_xaml.stateBits.store(bits, std::memory_order_release);
                if (m_lastLoggedEnabled != 0)
                {
                    m_lastLoggedEnabled = 0;
                    m_lastLoggedBits = bits;
                    ShellDiagLog(L"XamlCtrl", L"Restored all tracked XAML elements (count=%u)",
                        static_cast<unsigned>(m_elements.size()));
                }
            }

            HRESULT STDMETHODCALLTYPE OnVisualTreeChange(
                ParentChildRelation,
                VisualElement value,
                VisualMutationType mutation) noexcept override
            {
                auto freeBstrs = [&]() noexcept {
                    if (mutation == Add)
                    {
                        SysFreeString(value.Name);
                        SysFreeString(value.Type);
                        SysFreeString(value.SrcInfo.FileName);
                        SysFreeString(value.SrcInfo.Hash);
                    }
                };

                try
                {
                    if (mutation == Remove)
                    {
                        for (auto it = m_elements.begin(); it != m_elements.end(); ++it)
                        {
                            if (it->handle != value.Handle)
                                continue;
                            UnregisterElementCallbacks(*it);
                            m_elements.erase(it);
                            break;
                        }
                        return S_OK;
                    }

                    if (mutation == Add && value.Handle)
                    {
                        const wchar_t* name = value.Name ? value.Name : L"";
                        const wchar_t* type = value.Type ? value.Type : L"";

                        ElementRole role{};
                        bool matched = ClassifyElementRole(name, type, L"", role);

                        // Also inspect ExperienceToggleButton / OmniButton live via AutomationId since
                        // StartButton / SearchButton / TaskViewButton do not carry x:Name in Win11 XBF.
                        const bool maybeRoleByAutoId = !matched && (
                            wcsstr(type, L"Taskbar.ExperienceToggleButton") != nullptr ||
                            wcsstr(type, L"SystemTray.OmniButton") != nullptr);

                        if (matched || maybeRoleByAutoId)
                        {
                            com_ptr<::IInspectable> inspectable;
                            if (SUCCEEDED(m_diagnostics->GetIInspectableFromHandle(value.Handle, inspectable.put())) &&
                                inspectable)
                            {
                                auto uiElem = inspectable.try_as<xaml::UIElement>();
                                if (uiElem)
                                {
                                    if (!matched)
                                        matched = ClassifyLiveElement(uiElem, role);

                                    xaml::DependencyObject rootFrame{ nullptr };
                                    const bool isSelfRoot = (role == ElementRole::TaskbarFrame ||
                                                             role == ElementRole::SystemTrayFrame);
                                    if (matched && (isSelfRoot || FindTaskbarOrSystemTrayAncestor(uiElem, rootFrame)))
                                    {
                                        if (!rootFrame && isSelfRoot)
                                            rootFrame = uiElem;

                                        AttachUiWindowIfNeeded();
                                        const bool wasFirst = !g_xaml.attached.exchange(true, std::memory_order_acq_rel);
                                        AddOrUpdateElement(value.Handle, false, role, uiElem, rootFrame);

                                        if (isSelfRoot)
                                        {
                                            TryTrackHostRootGrid(uiElem, rootFrame);
                                            if (role == ElementRole::TaskbarFrame)
                                                TryTrackTaskbarRootGrid(uiElem, rootFrame);
                                            ScanVisualSubtree(uiElem, rootFrame);
                                        }

                                        if (wasFirst)
                                        {
                                            std::function<void()> cb;
                                            {
                                                std::lock_guard<std::mutex> lock(g_xaml.cfgMutex);
                                                cb = g_xaml.onFirstAttached;
                                            }
                                            ShellDiagLog(L"XamlCtrl", L"First taskbar/tray XAML attach! role=%s name=%s type=%s",
                                                RoleName(role), name, type);
                                            if (cb)
                                                cb();
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                catch (...) {}

                freeBstrs();
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE OnElementStateChanged(
                InstanceHandle, VisualElementState, LPCWSTR) noexcept override
            {
                return S_OK;
            }

            com_ptr<IXamlDiagnostics> m_diagnostics;

        private:
            static void CALLBACK TrayPopupWinEventProc(
                HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) noexcept
            {
                if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
                    return;
                if (event != EVENT_OBJECT_SHOW && event != EVENT_OBJECT_LOCATIONCHANGE)
                    return;
                if (!TopBarHook::IsTopDockActive())
                    return;

                wchar_t cls[96]{};
                if (!GetClassNameW(hwnd, cls, _countof(cls)))
                    return;

                if (wcscmp(cls, L"TopLevelWindowForOverflowXamlIsland") == 0 ||
                    wcscmp(cls, L"NotifyIconOverflowWindow") == 0)
                {
                    TopBarHook::RepositionTrayPopupIfNeeded(hwnd);
                }
            }

            static LRESULT CALLBACK UiWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept
            {
                auto* self = reinterpret_cast<TaskbarWatcher*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (msg == WM_NCCREATE)
                {
                    self = static_cast<TaskbarWatcher*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
                }
                if (self)
                {
                    try
                    {
                        if (msg == kMsgApplyConfig)
                        {
                            self->ApplyCurrentConfigOnUiThread();
                            return 0;
                        }
                        if (msg == kMsgRestoreAll)
                        {
                            self->RestoreAllOnUiThread();
                            return 0;
                        }
                        if (msg == kMsgRunFn && wp)
                        {
                            const auto cookie = static_cast<ULONG_PTR>(wp);
                            std::shared_ptr<UiRunTask> task;
                            {
                                std::lock_guard<std::mutex> lock(g_xaml.runTasksMutex);
                                auto it = g_xaml.runTasks.find(cookie);
                                if (it != g_xaml.runTasks.end())
                                {
                                    task = std::move(it->second);
                                    g_xaml.runTasks.erase(it);
                                }
                            }
                            if (task && !task->cancelled.load(std::memory_order_acquire) && task->fn)
                            {
                                task->fn();
                                return TRUE;
                            }
                            return FALSE;
                        }
                        if (msg == WM_NCDESTROY)
                        {
                            if (self->m_winEventHook)
                            {
                                UnhookWinEvent(self->m_winEventHook);
                                self->m_winEventHook = nullptr;
                            }
                            HWND expected = hwnd;
                            self->m_hwnd.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
                            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                        }
                        if (msg == WM_TIMER)
                        {
                            if (wp == kClockTimer)
                                self->TickClockOnly();
                            else if (wp == kReapplyTimer && g_xaml.active.load(std::memory_order_acquire))
                                self->ApplyCurrentConfigOnUiThread();
                            return 0;
                        }
                    }
                    catch (...) {}
                }
                return DefWindowProcW(hwnd, msg, wp, lp);
            }

            static std::wstring FormatCurrentClock(const SwitcherConfig& cfg)
            {
                SYSTEMTIME st{};
                GetLocalTime(&st);
                return FormatClock(st, cfg.topBarClockFormat, cfg.topBarClockCasing);
            }

            static InstanceHandle MakeSyntheticHandle(const xaml::UIElement& elem) noexcept
            {
                return reinterpret_cast<InstanceHandle>(winrt::get_abi(elem)) | (1ULL << 63);
            }

            void TryTrackHostRootGrid(const xaml::UIElement& frameElem, const xaml::DependencyObject& rootFrame)
            {
                try
                {
                    auto parent = xaml::Media::VisualTreeHelper::GetParent(frameElem);
                    if (auto grid = parent.try_as<xaml::Controls::Grid>())
                    {
                        AddOrUpdateElement(MakeSyntheticHandle(grid), true, ElementRole::HostRootGrid, grid, rootFrame);
                    }
                }
                catch (...) {}
            }

            void TryTrackTaskbarRootGrid(const xaml::UIElement& frameElem, const xaml::DependencyObject& rootFrame)
            {
                try
                {
                    const int count = xaml::Media::VisualTreeHelper::GetChildrenCount(frameElem);
                    for (int i = 0; i < count && i < 4; ++i)
                    {
                        auto child = xaml::Media::VisualTreeHelper::GetChild(frameElem, i);
                        auto grid = child.try_as<xaml::Controls::Grid>();
                        if (!grid)
                            continue;
                        AddOrUpdateElement(MakeSyntheticHandle(grid), true, ElementRole::TaskbarRootGrid, grid, rootFrame);
                    }
                }
                catch (...) {}
            }

            void ScanVisualSubtree(const xaml::DependencyObject& root, const xaml::DependencyObject& rootFrame)
            {
                try
                {
                    std::vector<xaml::DependencyObject> queue;
                    queue.reserve(256);
                    queue.push_back(root);
                    size_t visited = 0;
                    for (size_t idx = 0; idx < queue.size() && visited < 2048; ++idx, ++visited)
                    {
                        const auto& cur = queue[idx];
                        if (idx > 0)
                        {
                            if (auto ui = cur.try_as<xaml::UIElement>())
                            {
                                ElementRole role{};
                                if (ClassifyLiveElement(ui, role))
                                {
                                    AddOrUpdateElement(MakeSyntheticHandle(ui), true, role, ui, rootFrame);
                                }
                            }
                        }
                        const int childCount = xaml::Media::VisualTreeHelper::GetChildrenCount(cur);
                        for (int i = 0; i < childCount && queue.size() < 2048; ++i)
                        {
                            auto ch = xaml::Media::VisualTreeHelper::GetChild(cur, i);
                            if (ch)
                                queue.push_back(std::move(ch));
                        }
                    }
                }
                catch (...) {}
            }

            static void UnregisterElementCallbacks(TrackedElement& item) noexcept
            {
                for (auto& tp : item.props)
                {
                    if (tp.tokenRegistered && tp.obj && tp.prop)
                    {
                        try { tp.obj.UnregisterPropertyChangedCallback(tp.prop, tp.token); }
                        catch (...) {}
                        tp.tokenRegistered = false;
                    }
                }
            }

            void PruneDetachedSyntheticElements()
            {
                for (auto it = m_elements.begin(); it != m_elements.end(); )
                {
                    bool remove = false;
                    if (it->synthetic)
                    {
                        if (!it->element)
                        {
                            remove = true;
                        }
                        else if (it->role != ElementRole::HostRootGrid)
                        {
                            try
                            {
                                if (!xaml::Media::VisualTreeHelper::GetParent(it->element))
                                    remove = true;
                            }
                            catch (...)
                            {
                                remove = true;
                            }
                        }
                        else
                        {
                            try
                            {
                                if (xaml::Media::VisualTreeHelper::GetChildrenCount(it->element) == 0)
                                    remove = true;
                            }
                            catch (...)
                            {
                                remove = true;
                            }
                        }
                    }

                    if (remove)
                    {
                        UnregisterElementCallbacks(*it);
                        it = m_elements.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }

            void RescanKnownRoots()
            {
                PruneDetachedSyntheticElements();

                struct RootEntry
                {
                    ElementRole role;
                    xaml::UIElement elem;
                    xaml::DependencyObject rootFrame;
                };
                std::vector<RootEntry> roots;
                for (const auto& item : m_elements)
                {
                    if ((item.role == ElementRole::TaskbarFrame || item.role == ElementRole::SystemTrayFrame) &&
                        item.element)
                    {
                        roots.push_back(RootEntry{
                            item.role,
                            item.element,
                            item.rootFrame ? item.rootFrame : item.element
                        });
                    }
                }
                for (const auto& entry : roots)
                {
                    TryTrackHostRootGrid(entry.elem, entry.rootFrame);
                    if (entry.role == ElementRole::TaskbarFrame)
                        TryTrackTaskbarRootGrid(entry.elem, entry.rootFrame);
                    ScanVisualSubtree(entry.elem, entry.rootFrame);
                }
            }

            void AddOrUpdateElement(InstanceHandle handle, bool synthetic, ElementRole role,
                const xaml::UIElement& uiElem, const xaml::DependencyObject& frameObj)
            {
                for (auto& existing : m_elements)
                {
                    if (existing.handle == handle || existing.element == uiElem)
                    {
                        // Upgrade synthetic handle to real diagnostics handle when OnVisualTreeChange delivers it.
                        if (existing.synthetic && !synthetic)
                        {
                            existing.handle = handle;
                            existing.synthetic = false;
                        }
                        return;
                    }
                }
                if (m_elements.size() >= 1024)
                    return;

                TrackedElement te;
                te.handle = handle;
                te.synthetic = synthetic;
                te.role = role;
                te.element = uiElem;
                te.rootFrame = frameObj;

                auto weakThis = get_weak();
                auto onPropChanged = [weakThis](const xaml::DependencyObject& sender, const xaml::DependencyProperty& prop) {
                    auto self = weakThis.get();
                    if (!self || self->m_changing || !g_xaml.active.load(std::memory_order_acquire))
                        return;
                    SwitcherConfig cfg;
                    {
                        std::lock_guard<std::mutex> lock(g_xaml.cfgMutex);
                        cfg = g_xaml.config;
                    }
                    if (!cfg.topBarEnabled)
                        return;
                    const std::wstring clockStr = FormatCurrentClock(cfg);
                    self->m_changing = true;
                    try
                    {
                        for (auto& item : self->m_elements)
                        {
                            if (item.element == sender)
                            {
                                RefreshOrigSnapshot(item, sender, prop);
                                UINT32 bits = self->m_lastBits;
                                self->ApplyToElement(item, cfg, clockStr, bits);
                                self->m_lastBits = bits;
                                g_xaml.stateBits.store(bits, std::memory_order_release);
                                break;
                            }
                        }
                    }
                    catch (...) {}
                    self->m_changing = false;
                };

                auto fe = uiElem.try_as<xaml::FrameworkElement>();
                switch (role)
                {
                case ElementRole::HostRootGrid:
                case ElementRole::TaskbarFrame:
                case ElementRole::TaskbarRootGrid:
                case ElementRole::SystemTrayFrame:
                case ElementRole::SystemTrayFrameGrid:
                    if (fe)
                    {
                        SnapshotProp(te, fe, xaml::FrameworkElement::HeightProperty(), true, onPropChanged);
                        SnapshotProp(te, fe, xaml::FrameworkElement::MinHeightProperty(), true, onPropChanged);
                        SnapshotProp(te, fe, xaml::FrameworkElement::MaxHeightProperty(), true, onPropChanged);
                        SnapshotProp(te, fe, xaml::FrameworkElement::VerticalAlignmentProperty(), true, onPropChanged);
                    }
                    break;

                case ElementRole::NotificationCenterButton:
                case ElementRole::ClockButton:
                case ElementRole::DateTimeIconContent:
                case ElementRole::DateTimeStackPanel:
                case ElementRole::ControlCenterButton:
                case ElementRole::SystemTrayCompactItem:
                    if (fe)
                    {
                        SnapshotProp(te, fe, xaml::FrameworkElement::HeightProperty(), true, onPropChanged);
                        SnapshotProp(te, fe, xaml::FrameworkElement::MinHeightProperty(), false, nullptr);
                        SnapshotProp(te, fe, xaml::FrameworkElement::MaxHeightProperty(), false, nullptr);
                        SnapshotProp(te, fe, xaml::FrameworkElement::VerticalAlignmentProperty(), false, nullptr);
                        SnapshotProp(te, fe, xaml::FrameworkElement::MarginProperty(), false, nullptr);
                        if (auto ctrl = uiElem.try_as<xaml::Controls::Control>())
                            SnapshotProp(te, ctrl, xaml::Controls::Control::PaddingProperty(), false, nullptr);
                    }
                    break;

                case ElementRole::DateInnerTextBlock:
                case ElementRole::TaskbarFrameRepeater:
                case ElementRole::AppTaskButton:
                case ElementRole::StartButton:
                case ElementRole::SearchBoxButton:
                case ElementRole::TaskViewButton:
                case ElementRole::WidgetsButton:
                    SnapshotProp(te, uiElem, xaml::UIElement::VisibilityProperty(), true, onPropChanged);
                    break;

                case ElementRole::TimeInnerTextBlock:
                    if (auto tb = uiElem.try_as<xaml::Controls::TextBlock>())
                    {
                        SnapshotProp(te, tb, xaml::Controls::TextBlock::TextProperty(), true, onPropChanged);
                        SnapshotProp(te, tb, xaml::Controls::TextBlock::TextWrappingProperty(), false, nullptr);
                        SnapshotProp(te, tb, xaml::FrameworkElement::VerticalAlignmentProperty(), false, nullptr);
                        SnapshotProp(te, tb, xaml::FrameworkElement::MarginProperty(), false, nullptr);
                        // IMPORTANT: never touch FontSizeProperty per user requirement!
                    }
                    break;

                case ElementRole::BackgroundFill:
                    SnapshotProp(te, uiElem, xaml::UIElement::OpacityProperty(), true, onPropChanged);
                    if (auto shape = uiElem.try_as<xaml::Shapes::Shape>())
                        SnapshotProp(te, shape, xaml::Shapes::Shape::FillProperty(), true, onPropChanged);
                    break;

                case ElementRole::BackgroundStroke:
                case ElementRole::ScreenEdgeStroke:
                    SnapshotProp(te, uiElem, xaml::UIElement::OpacityProperty(), true, onPropChanged);
                    if (auto shape = uiElem.try_as<xaml::Shapes::Shape>())
                        SnapshotProp(te, shape, xaml::Shapes::Shape::StrokeProperty(), false, nullptr);
                    break;

                default:
                    break;
                }

                m_elements.push_back(std::move(te));
                SwitcherConfig cfg;
                {
                    std::lock_guard<std::mutex> lock(g_xaml.cfgMutex);
                    cfg = g_xaml.config;
                }
                if (cfg.topBarEnabled)
                {
                    g_xaml.active.store(true, std::memory_order_release);
                    m_changing = true;
                    try
                    {
                        UINT32 bits = g_xaml.stateBits.load(std::memory_order_acquire) | WINRINGS_STATE_XAML_ATTACHED;
                        ApplyToElement(m_elements.back(), cfg, FormatCurrentClock(cfg), bits);
                        m_lastBits = bits;
                        g_xaml.stateBits.store(bits, std::memory_order_release);
                    }
                    catch (...) {}
                    m_changing = false;
                }
            }

            void RestoreElement(TrackedElement& item) noexcept
            {
                for (auto& tp : item.props)
                    RestoreProp(tp);
            }

            void ApplyToElement(TrackedElement& item, const SwitcherConfig& cfg,
                const std::wstring& formattedClock, UINT32& bits)
            {
                const double targetH = static_cast<double>((std::clamp)(cfg.topBarHeightDip, 28, 48));
                const xaml::VerticalAlignment targetAlign = cfg.topBarDockTop
                    ? xaml::VerticalAlignment::Top
                    : xaml::VerticalAlignment::Bottom;

                switch (item.role)
                {
                case ElementRole::HostRootGrid:
                case ElementRole::TaskbarFrame:
                case ElementRole::TaskbarRootGrid:
                    if (auto fe = item.element.try_as<xaml::FrameworkElement>())
                    {
                        fe.MinHeight(0.0);
                        fe.MaxHeight(targetH);
                        fe.Height(targetH);
                        fe.VerticalAlignment(targetAlign);
                    }
                    break;

                case ElementRole::SystemTrayFrame:
                case ElementRole::SystemTrayFrameGrid:
                    if (auto fe = item.element.try_as<xaml::FrameworkElement>())
                    {
                        fe.MinHeight(0.0);
                        fe.MaxHeight(targetH);
                        fe.Height(targetH);
                        fe.VerticalAlignment(item.role == ElementRole::SystemTrayFrameGrid
                            ? xaml::VerticalAlignment::Center
                            : targetAlign);
                        if (item.role == ElementRole::SystemTrayFrame &&
                            (!item.rootFrame || IsTaskbarFramePrimaryOnly(item.rootFrame)))
                        {
                            const double aw = fe.ActualWidth();
                            if (std::isfinite(aw) && aw > 40.0 && aw < 1200.0)
                            {
                                const int wDip = static_cast<int>(std::lround(aw));
                                const int prev = g_xaml.systemTrayWidthDip.load(std::memory_order_acquire);
                                if (std::abs(wDip - prev) >= 4)
                                {
                                    g_xaml.systemTrayWidthDip.store(wDip, std::memory_order_release);
                                    std::function<void()> cb;
                                    {
                                        std::lock_guard<std::mutex> lock(g_xaml.cfgMutex);
                                        cb = g_xaml.onTrayMetricsChanged;
                                    }
                                    if (cb)
                                        cb();
                                }
                            }
                        }
                    }
                    break;

                case ElementRole::NotificationCenterButton:
                case ElementRole::ClockButton:
                case ElementRole::DateTimeIconContent:
                case ElementRole::DateTimeStackPanel:
                case ElementRole::ControlCenterButton:
                case ElementRole::SystemTrayCompactItem:
                    if (auto fe = item.element.try_as<xaml::FrameworkElement>())
                    {
                        fe.MinHeight(0.0);
                        fe.MaxHeight(targetH);
                        fe.Height(std::numeric_limits<double>::quiet_NaN());
                        fe.VerticalAlignment(xaml::VerticalAlignment::Center);
                        auto m = fe.Margin();
                        fe.Margin(xaml::ThicknessHelper::FromLengths(m.Left, 0.0, m.Right, 0.0));
                        if (auto ctrl = item.element.try_as<xaml::Controls::Control>())
                        {
                            auto p = ctrl.Padding();
                            ctrl.Padding(xaml::ThicknessHelper::FromLengths(p.Left, 0.0, p.Right, 0.0));
                        }
                    }
                    break;

                case ElementRole::DateInnerTextBlock:
                    if (cfg.topBarClockSingleLine)
                        item.element.Visibility(xaml::Visibility::Collapsed);
                    else
                        RestoreElement(item);
                    break;

                case ElementRole::TimeInnerTextBlock:
                    if (cfg.topBarClockSingleLine)
                    {
                        if (auto tb = item.element.try_as<xaml::Controls::TextBlock>())
                        {
                            tb.TextWrapping(xaml::TextWrapping::NoWrap);
                            tb.VerticalAlignment(xaml::VerticalAlignment::Center);
                            auto m = tb.Margin();
                            tb.Margin(xaml::ThicknessHelper::FromLengths(m.Left, 0.0, m.Right, 0.0));
                            if (tb.Text() != formattedClock)
                                tb.Text(formattedClock);
                            bits |= WINRINGS_STATE_CLOCK_ACTIVE;
                        }
                    }
                    else
                    {
                        RestoreElement(item);
                    }
                    break;

                case ElementRole::TaskbarFrameRepeater:
                {
                    // Collapse the whole ItemsRepeater when every taskbar button category is hidden,
                    // ensuring even newly introduced Win11 taskbar buttons stay completely hidden.
                    // If the user explicitly wants any button kept visible, restore the repeater
                    // and hide only the selected individual button roles below.
                    const bool hideEverything = cfg.topBarHideAppButtons &&
                        cfg.topBarHideStart && cfg.topBarHideSearch &&
                        cfg.topBarHideTaskView && cfg.topBarHideWidgets;
                    if (hideEverything)
                    {
                        item.element.Visibility(xaml::Visibility::Collapsed);
                        bits |= WINRINGS_STATE_APPS_HIDDEN;
                    }
                    else
                    {
                        RestoreElement(item);
                    }
                    break;
                }

                case ElementRole::AppTaskButton:
                {
                    bool inSystemTray = false;
                    if (item.rootFrame)
                    {
                        bool isTb = false;
                        IsTaskbarOrSystemTrayRootObj(item.rootFrame, isTb, inSystemTray);
                    }
                    if (cfg.topBarHideAppButtons && !inSystemTray)
                    {
                        item.element.Visibility(xaml::Visibility::Collapsed);
                        bits |= WINRINGS_STATE_APPS_HIDDEN;
                    }
                    else
                    {
                        RestoreElement(item);
                    }
                    break;
                }

                case ElementRole::StartButton:
                    if (cfg.topBarHideStart) item.element.Visibility(xaml::Visibility::Collapsed);
                    else RestoreElement(item);
                    break;

                case ElementRole::SearchBoxButton:
                    if (cfg.topBarHideSearch) item.element.Visibility(xaml::Visibility::Collapsed);
                    else RestoreElement(item);
                    break;

                case ElementRole::TaskViewButton:
                    if (cfg.topBarHideTaskView) item.element.Visibility(xaml::Visibility::Collapsed);
                    else RestoreElement(item);
                    break;

                case ElementRole::WidgetsButton:
                    if (cfg.topBarHideWidgets) item.element.Visibility(xaml::Visibility::Collapsed);
                    else RestoreElement(item);
                    break;

                case ElementRole::BackgroundFill:
                    if (cfg.topBarBackdrop == TopBarBackdrop::Clear)
                    {
                        item.element.Opacity(0.0);
                        bits |= WINRINGS_STATE_BACKDROP_ACTIVE;
                    }
                    else if (cfg.topBarBackdrop == TopBarBackdrop::Frosted)
                    {
                        item.element.Opacity(0.55);
                        if (auto shape = item.element.try_as<xaml::Shapes::Shape>())
                        {
                            winrt::Color tint{ 0xE0, 0x12, 0x16, 0x1E };
                            shape.Fill(xaml::Media::SolidColorBrush(tint));
                        }
                        bits |= WINRINGS_STATE_BACKDROP_ACTIVE;
                    }
                    else
                    {
                        RestoreElement(item);
                    }
                    break;

                case ElementRole::BackgroundStroke:
                case ElementRole::ScreenEdgeStroke:
                    if (cfg.topBarBackdrop == TopBarBackdrop::Clear)
                    {
                        item.element.Opacity(0.0);
                        bits |= WINRINGS_STATE_BACKDROP_ACTIVE;
                    }
                    else if (cfg.topBarBackdrop == TopBarBackdrop::Frosted)
                    {
                        item.element.Opacity(item.role == ElementRole::ScreenEdgeStroke ? 0.0 : 0.35);
                        bits |= WINRINGS_STATE_BACKDROP_ACTIVE;
                    }
                    else
                    {
                        RestoreElement(item);
                    }
                    break;

                default:
                    break;
                }
            }

            void TickClockOnly()
            {
                if (!g_xaml.active.load(std::memory_order_acquire))
                    return;
                SwitcherConfig cfg;
                {
                    std::lock_guard<std::mutex> lock(g_xaml.cfgMutex);
                    cfg = g_xaml.config;
                }
                if (!cfg.topBarEnabled)
                    return;

                if (cfg.topBarDockTop)
                {
                    HWND ov = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr);
                    if (ov && IsWindowVisible(ov))
                        TopBarHook::RepositionTrayPopupIfNeeded(ov);
                }

                if (!cfg.topBarClockSingleLine)
                    return;

                const std::wstring text = FormatCurrentClock(cfg);
                m_changing = true;
                for (auto& item : m_elements)
                {
                    if (item.role != ElementRole::TimeInnerTextBlock || !item.element)
                        continue;
                    try
                    {
                        if (auto tb = item.element.try_as<xaml::Controls::TextBlock>())
                        {
                            if (tb.Text() != text)
                                tb.Text(text);
                        }
                    }
                    catch (...) {}
                }
                m_changing = false;
            }

            std::atomic<HWND> m_hwnd{ nullptr };
            HWINEVENTHOOK m_winEventHook = nullptr;
            bool m_changing = false;
            UINT32 m_lastBits = WINRINGS_STATE_XAML_ATTACHED;
            size_t m_lastLoggedCount = static_cast<size_t>(-1);
            UINT32 m_lastLoggedBits = static_cast<UINT32>(-1);
            int m_lastLoggedEnabled = -1;
            std::vector<TrackedElement> m_elements;
        };

        class WinRingsXamlSite : public winrt::implements<WinRingsXamlSite, IObjectWithSite, winrt::non_agile>
        {
        public:
            HRESULT STDMETHODCALLTYPE SetSite(IUnknown* value) noexcept override
            {
                try
                {
                    m_site.copy_from(value);
                    if (!value)
                    {
                        m_watcher = nullptr;
                        return S_OK;
                    }

                    // Pin our DLL so XAML diagnostic callbacks remain valid for Explorer's lifetime.
                    HMODULE pinned{};
                    GetModuleHandleExW(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                        reinterpret_cast<LPCWSTR>(&g_xaml), &pinned);

                    // Keep watcher owned by this diagnostic site; it registers into g_xaml.watchers
                    // inside AttachUiWindowIfNeeded() only once it matches a Taskbar/SystemTray element,
                    // so non-taskbar XAML threads (e.g. File Explorer) can never exhaust the watcher cap.
                    auto watcher = winrt::make_self<TaskbarWatcher>(value);
                    m_watcher = watcher;

                    watcher->AddRef();
                    HANDLE th = CreateThread(nullptr, 0, [](void* ptr) -> DWORD {
                        auto* w = static_cast<TaskbarWatcher*>(ptr);
                        const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                        HRESULT hrAdv = E_FAIL;
                        try
                        {
                            auto service = w->m_diagnostics.as<IVisualTreeService3>();
                            hrAdv = service->AdviseVisualTreeChange(w);
                        }
                        catch (...) { hrAdv = winrt::to_hresult(); }
                        ShellDiagLog(L"XamlCtrl", L"SetSite -> AdviseVisualTreeChange hr=0x%08X",
                            static_cast<unsigned>(hrAdv));
                        w->Release();
                        if (SUCCEEDED(hrCo))
                            CoUninitialize();
                        return 0;
                    }, watcher.get(), 0, nullptr);

                    if (th)
                        CloseHandle(th);
                    else
                    {
                        watcher->Release();
                        m_watcher = nullptr;
                        return HRESULT_FROM_WIN32(GetLastError());
                    }
                    return S_OK;
                }
                catch (...) { return winrt::to_hresult(); }
            }

            HRESULT STDMETHODCALLTYPE GetSite(REFIID iid, void** result) noexcept override
            {
                if (!result) return E_POINTER;
                *result = nullptr;
                return m_site ? m_site->QueryInterface(iid, result) : E_FAIL;
            }

        private:
            com_ptr<IUnknown> m_site;
            com_ptr<TaskbarWatcher> m_watcher;
        };

        class WinRingsXamlFactory : public winrt::implements<WinRingsXamlFactory, IClassFactory, winrt::non_agile>
        {
        public:
            HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** result) noexcept override
            {
                if (!result) return E_POINTER;
                *result = nullptr;
                if (outer) return CLASS_E_NOAGGREGATION;
                try { return winrt::make_self<WinRingsXamlSite>()->QueryInterface(iid, result); }
                catch (...) { return winrt::to_hresult(); }
            }

            HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override { return S_OK; }
        };

        DWORD WINAPI AttachWorkerThread(void*)
        {
            struct WorkerGuard
            {
                ~WorkerGuard()
                {
                    g_xaml.lastAttachFinishTick.store(GetTickCount64(), std::memory_order_release);
                    g_xaml.attachWorkerRunning.store(false, std::memory_order_release);
                }
            } guard;

            std::wstring dllPath(32768, L'\0');
            const DWORD len = GetModuleFileNameW(g_xaml.shellModule, dllPath.data(), static_cast<DWORD>(dllPath.size()));
            if (!len || len >= dllPath.size())
            {
                ShellDiagLog(L"XamlCtrl", L"AttachWorker: GetModuleFileNameW failed (mod=0x%p)", g_xaml.shellModule);
                return 1;
            }
            dllPath.resize(len);

            // On fresh Explorer startup, Shell_TrayWnd exists before Taskbar.View.dll creates
            // its XAML island and registers VisualDiagConnection1. Wait briefly until Taskbar.View.dll
            // (or Windows.UI.Xaml.dll) is loaded so we don't burn ep=1 before XAML is ready.
            for (int waitIdx = 0; waitIdx < 60 && !g_xaml.attached.load(std::memory_order_acquire); ++waitIdx)
            {
                if (FindWindowW(L"Shell_TrayWnd", nullptr) && GetModuleHandleW(L"Taskbar.View.dll"))
                    break;
                Sleep(250);
            }

            HMODULE xamlMod = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!xamlMod)
            {
                ShellDiagLog(L"XamlCtrl", L"AttachWorker: LoadLibraryExW(Windows.UI.Xaml.dll) failed err=%lu", GetLastError());
                return 2;
            }

            using InitDiagFn = decltype(&InitializeXamlDiagnosticsEx);
            auto fnInitDiag = reinterpret_cast<InitDiagFn>(GetProcAddress(xamlMod, "InitializeXamlDiagnosticsEx"));
            if (!fnInitDiag)
            {
                ShellDiagLog(L"XamlCtrl", L"AttachWorker: InitializeXamlDiagnosticsEx export missing");
                FreeLibrary(xamlMod);
                return 3;
            }

            struct DiagAttempt
            {
                InitDiagFn fn;
                const wchar_t* path;
                unsigned endpoint;
                HRESULT hr;
            };

            ShellDiagLog(L"XamlCtrl", L"AttachWorker starting rounds (dll=%s taskbarViewLoaded=%d)",
                dllPath.c_str(), GetModuleHandleW(L"Taskbar.View.dll") ? 1 : 0);

            // Fast-expanding rounds ensure VisualDiagConnection1 is re-probed promptly if Explorer's
            // taskbar XAML island came up a few seconds after DLL injection, while still reaching up
            // to VisualDiagConnection60 if earlier slots were taken.
            static constexpr unsigned kRoundMaxEp[] = { 6, 6, 8, 8, 12, 20, 60 };
            for (size_t round = 0; round < _countof(kRoundMaxEp) && !g_xaml.attached.load(std::memory_order_acquire); ++round)
            {
                const unsigned maxEp = kRoundMaxEp[round];
                HRESULT lastFailHr = S_OK;
                unsigned triedCount = 0;

                for (unsigned ep = 1; ep <= maxEp && !g_xaml.attached.load(std::memory_order_acquire); ++ep)
                {
                    {
                        std::lock_guard<std::mutex> lock(g_xaml.watchersMutex);
                        if (ep < g_xaml.connectedEndpoints.size() && g_xaml.connectedEndpoints[ep])
                            continue;
                    }

                    ++triedCount;
                    DiagAttempt attempt{ fnInitDiag, dllPath.c_str(), ep, E_FAIL };
                    HANDLE th = CreateThread(nullptr, 0, [](void* p) -> DWORD {
                        auto* a = static_cast<DiagAttempt*>(p);
                        wchar_t endpointName[64]{};
                        wsprintfW(endpointName, L"VisualDiagConnection%u", a->endpoint);
                        a->hr = a->fn(endpointName, GetCurrentProcessId(), nullptr, a->path, WinRingsXamlDiagClass, nullptr);
                        return 0;
                    }, &attempt, 0, nullptr);
                    if (!th)
                        break;
                    WaitForSingleObject(th, INFINITE);
                    CloseHandle(th);

                    if (SUCCEEDED(attempt.hr))
                    {
                        {
                            std::lock_guard<std::mutex> lock(g_xaml.watchersMutex);
                            if (ep < g_xaml.connectedEndpoints.size())
                                g_xaml.connectedEndpoints[ep] = true;
                        }
                        ShellDiagLog(L"XamlCtrl", L"Round %u: VisualDiagConnection%u connected (hr=0x%08X), waiting for Taskbar tree...",
                            static_cast<unsigned>(round + 1), ep, static_cast<unsigned>(attempt.hr));

                        // Wait up to 2000ms for SetSite -> AdviseVisualTreeChange -> TaskbarFrame/SystemTrayFrame.
                        // If this endpoint belongs to a non-taskbar XAML thread (e.g. File Explorer),
                        // g_xaml.attached stays false and we keep scanning the remaining endpoints!
                        for (int w = 0; w < 40 && !g_xaml.attached.load(std::memory_order_acquire); ++w)
                            Sleep(50);

                        if (g_xaml.attached.load(std::memory_order_acquire))
                            break;
                    }
                    else
                    {
                        lastFailHr = attempt.hr;
                        Sleep(100);
                    }
                }

                if (!g_xaml.attached.load(std::memory_order_acquire))
                {
                    ShellDiagLog(L"XamlCtrl", L"Round %u finished (maxEp=%u tried=%u lastFailHr=0x%08X attached=0)",
                        static_cast<unsigned>(round + 1), maxEp, triedCount, static_cast<unsigned>(lastFailHr));
                    Sleep(500);
                }
            }

            if (g_xaml.attached.load(std::memory_order_acquire))
            {
                ShellDiagLog(L"XamlCtrl", L"AttachWorker succeeded (uiHwnd=0x%p)",
                    g_xaml.uiHwnd.load(std::memory_order_acquire));
            }
            else
            {
                ShellDiagLog(L"XamlCtrl", L"AttachWorker exhausted rounds without matching Taskbar elements");
            }

            FreeLibrary(xamlMod);
            return 0;
        }

        bool LaunchAttachWorkerIfNeeded()
        {
            if (g_xaml.attached.load(std::memory_order_acquire))
                return true;
            if (g_xaml.attachWorkerRunning.exchange(true, std::memory_order_acq_rel))
                return true;

            HANDLE th = CreateThread(nullptr, 0, AttachWorkerThread, nullptr, 0, nullptr);
            if (!th)
            {
                g_xaml.attachWorkerRunning.store(false, std::memory_order_release);
                return false;
            }
            CloseHandle(th);
            return true;
        }
    } // namespace

    HRESULT XamlTaskbarCtrl::GetClassObject(REFCLSID clsid, REFIID iid, void** result)
    {
        if (!result)
            return E_POINTER;
        *result = nullptr;
        if (!IsEqualCLSID(clsid, WinRingsXamlDiagClass))
            return CLASS_E_CLASSNOTAVAILABLE;
        try
        {
            return winrt::make_self<WinRingsXamlFactory>()->QueryInterface(iid, result);
        }
        catch (...) { return winrt::to_hresult(); }
    }

    bool XamlTaskbarCtrl::StartAttachment(HMODULE shellModule, const SwitcherConfig& initialConfig,
        std::function<void()> onFirstAttached)
    {
        {
            std::lock_guard<std::mutex> lock(g_xaml.cfgMutex);
            g_xaml.config = initialConfig;
            g_xaml.shellModule = shellModule;
            if (onFirstAttached)
                g_xaml.onFirstAttached = std::move(onFirstAttached);
        }

        if (g_xaml.attached.load(std::memory_order_acquire))
        {
            ApplyConfig(initialConfig);
            return true;
        }

        return LaunchAttachWorkerIfNeeded();
    }

    void XamlTaskbarCtrl::EnsureAttachRetry(HMODULE shellModule, const SwitcherConfig& config)
    {
        {
            std::lock_guard<std::mutex> lock(g_xaml.cfgMutex);
            g_xaml.config = config;
            if (shellModule)
                g_xaml.shellModule = shellModule;
        }
        if (g_xaml.attached.load(std::memory_order_acquire) ||
            g_xaml.attachWorkerRunning.load(std::memory_order_acquire))
        {
            return;
        }
        const ULONGLONG lastDone = g_xaml.lastAttachFinishTick.load(std::memory_order_acquire);
        const ULONGLONG now = GetTickCount64();
        if (lastDone != 0 && (now - lastDone) < 5000ull)
            return;

        // Reset connectedEndpoints on explicit retry in case endpoints changed.
        {
            std::lock_guard<std::mutex> lock(g_xaml.watchersMutex);
            g_xaml.connectedEndpoints.fill(false);
        }
        ShellDiagLog(L"XamlCtrl", L"EnsureAttachRetry launching fresh attach worker");
        LaunchAttachWorkerIfNeeded();
    }

    void XamlTaskbarCtrl::ApplyConfig(const SwitcherConfig& config)
    {
        {
            std::lock_guard<std::mutex> lock(g_xaml.cfgMutex);
            g_xaml.config = config;
        }
        std::vector<com_ptr<TaskbarWatcher>> snapshot;
        {
            std::lock_guard<std::mutex> lock(g_xaml.watchersMutex);
            snapshot = g_xaml.watchers;
        }
        for (const auto& w : snapshot)
        {
            if (w)
                w->RequestApply();
        }
    }

    void XamlTaskbarCtrl::RestoreAll()
    {
        g_xaml.active.store(false, std::memory_order_release);
        std::vector<com_ptr<TaskbarWatcher>> snapshot;
        {
            std::lock_guard<std::mutex> lock(g_xaml.watchersMutex);
            snapshot = g_xaml.watchers;
        }
        for (const auto& w : snapshot)
        {
            if (w)
                w->RequestRestoreSync(1500);
        }
    }

    bool XamlTaskbarCtrl::IsAttached() noexcept
    {
        return g_xaml.attached.load(std::memory_order_acquire);
    }

    UINT32 XamlTaskbarCtrl::GetStateBits() noexcept
    {
        return g_xaml.stateBits.load(std::memory_order_acquire);
    }

    int XamlTaskbarCtrl::GetSystemTrayWidthDip() noexcept
    {
        return g_xaml.systemTrayWidthDip.load(std::memory_order_acquire);
    }

    void XamlTaskbarCtrl::SetTrayMetricsCallback(std::function<void()> onTrayMetricsChanged)
    {
        std::lock_guard<std::mutex> lock(g_xaml.cfgMutex);
        g_xaml.onTrayMetricsChanged = std::move(onTrayMetricsChanged);
    }

    bool XamlTaskbarCtrl::RunOnUiThread(std::function<void()> fn, DWORD timeoutMs)
    {
        if (!fn)
            return false;
        HWND hwnd = g_xaml.uiHwnd.load(std::memory_order_acquire);
        if (!hwnd || !IsWindow(hwnd))
            return false;
        if (g_xaml.uiThreadId.load(std::memory_order_acquire) == GetCurrentThreadId())
        {
            try { fn(); }
            catch (...) {}
            return true;
        }

        auto task = std::make_shared<UiRunTask>();
        task->fn = std::move(fn);
        ULONG_PTR cookie = g_xaml.nextRunCookie.fetch_add(1, std::memory_order_relaxed);
        if (cookie == 0)
            cookie = g_xaml.nextRunCookie.fetch_add(1, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lock(g_xaml.runTasksMutex);
            g_xaml.runTasks.emplace(cookie, task);
        }

        DWORD_PTR res = 0;
        const LRESULT sendOk = SendMessageTimeoutW(hwnd, kMsgRunFn, static_cast<WPARAM>(cookie), 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, timeoutMs, &res);

        task->cancelled.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(g_xaml.runTasksMutex);
            g_xaml.runTasks.erase(cookie);
        }
        return sendOk != 0 && res != 0;
    }
} // namespace winrings
