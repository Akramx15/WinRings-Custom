#include "GestureEngine.h"

#include <hidsdi.h>
#include <hidpi.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace winrings
{
    void ShellDiagLog(const wchar_t* tag, const wchar_t* fmt, ...) noexcept;

    namespace
    {
        constexpr ULONG_PTR kWinRingsInjectedTag = 0x57524E47ull;
        constexpr USHORT kUsagePageGenericDesktop = 0x01;
        constexpr USHORT kUsageGenericMouse = 0x02;
        constexpr USHORT kUsageGenericX = 0x30;
        constexpr USHORT kUsageGenericY = 0x31;

        constexpr USHORT kUsagePageButton = 0x09;
        constexpr USHORT kUsageButton1 = 0x01;

        constexpr USHORT kUsagePageDigitizer = 0x0D;
        constexpr USHORT kUsageDigitizerTouchPad = 0x05;
        constexpr USHORT kUsageDigitizerFinger = 0x22;
        constexpr USHORT kUsageDigitizerTipSwitch = 0x42;
        constexpr USHORT kUsageDigitizerContactCount = 0x54;

        bool HasUsage(const USAGE* list, ULONG count, USAGE target) noexcept
        {
            for (ULONG i = 0; i < count; ++i)
            {
                if (list[i] == target)
                    return true;
            }
            return false;
        }
    } // namespace

    GestureEngine::~GestureEngine()
    {
        Shutdown();
    }

    bool GestureEngine::Initialize(HWND hwnd, const SwitcherConfig& cfg, Callbacks callbacks)
    {
        Shutdown();
        m_hwnd = hwnd;
        m_config = cfg;
        m_callbacks = std::move(callbacks);
        SyncDeviceRegistration();
        return true;
    }

    void GestureEngine::UpdateConfig(const SwitcherConfig& cfg)
    {
        m_config = cfg;
        if (m_hwnd)
            SyncDeviceRegistration();
    }

    void GestureEngine::Shutdown()
    {
        if (m_mouseRegistered)
        {
            RAWINPUTDEVICE rid{};
            rid.usUsagePage = kUsagePageGenericDesktop;
            rid.usUsage = kUsageGenericMouse;
            rid.dwFlags = RIDEV_REMOVE;
            RegisterRawInputDevices(&rid, 1, sizeof(rid));
            m_mouseRegistered = false;
        }
        if (m_hidRegistered)
        {
            RAWINPUTDEVICE rid{};
            rid.usUsagePage = kUsagePageDigitizer;
            rid.usUsage = kUsageDigitizerTouchPad;
            rid.dwFlags = RIDEV_REMOVE;
            RegisterRawInputDevices(&rid, 1, sizeof(rid));
            m_hidRegistered = false;
        }
        m_hidCache.clear();
        m_cornerPressure = 0;
        m_cornerArmed = true;
        m_physBtnDownNow = false;
        m_suppressClickUntilTick = 0;
        m_eatingUpForSuppressedDown = false;
        m_tpState = {};
        m_hwnd = nullptr;
    }

    void GestureEngine::SyncDeviceRegistration()
    {
        if (!m_hwnd)
            return;

        const bool wantMouse = m_config.hotCornerEnabled;
        if (!wantMouse && m_mouseRegistered)
        {
            RAWINPUTDEVICE rid{};
            rid.usUsagePage = kUsagePageGenericDesktop;
            rid.usUsage = kUsageGenericMouse;
            rid.dwFlags = RIDEV_REMOVE;
            RegisterRawInputDevices(&rid, 1, sizeof(rid));
            m_mouseRegistered = false;
            m_cornerPressure = 0;
        }

        const bool wantHid = m_config.touchpadPhysLeftClickTrigger || m_config.touchpadCornerTrigger ||
            (m_config.mediaIslandEnabled && m_config.mediaIslandScrollVolume);
        if (!wantHid && m_hidRegistered)
        {
            RAWINPUTDEVICE rid{};
            rid.usUsagePage = kUsagePageDigitizer;
            rid.usUsage = kUsageDigitizerTouchPad;
            rid.dwFlags = RIDEV_REMOVE;
            RegisterRawInputDevices(&rid, 1, sizeof(rid));
            m_hidRegistered = false;
            m_physBtnDownNow = false;
            m_suppressClickUntilTick = 0;
            m_eatingUpForSuppressedDown = false;
            m_tpState = {};
        }

        EnsureDeviceRegistration();
    }

    void GestureEngine::EnsureDeviceRegistration()
    {
        if (!m_hwnd || !IsWindow(m_hwnd))
            return;

        const bool wantMouse = m_config.hotCornerEnabled;
        const bool wantHid = m_config.touchpadPhysLeftClickTrigger || m_config.touchpadCornerTrigger ||
            (m_config.mediaIslandEnabled && m_config.mediaIslandScrollVolume);
        if (!wantMouse && !wantHid)
            return;

        UINT count = 0;
        if (GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) == static_cast<UINT>(-1) &&
            GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        {
            return;
        }

        std::vector<RAWINPUTDEVICE> devs(count);
        if (count > 0)
        {
            const UINT actual = GetRegisteredRawInputDevices(devs.data(), &count, sizeof(RAWINPUTDEVICE));
            if (actual == static_cast<UINT>(-1))
                devs.clear();
            else
                devs.resize(actual);
        }

        bool hasMouse = false;
        bool hasHid = false;
        for (const auto& d : devs)
        {
            if (d.hwndTarget != m_hwnd || (d.dwFlags & RIDEV_REMOVE) != 0)
                continue;
            if (d.usUsagePage == kUsagePageGenericDesktop && d.usUsage == kUsageGenericMouse)
                hasMouse = true;
            else if (d.usUsagePage == kUsagePageDigitizer && d.usUsage == kUsageDigitizerTouchPad)
                hasHid = true;
        }

        if (wantMouse && !hasMouse)
        {
            const bool wasRegistered = m_mouseRegistered;
            RAWINPUTDEVICE rid{};
            rid.usUsagePage = kUsagePageGenericDesktop;
            rid.usUsage = kUsageGenericMouse;
            rid.dwFlags = RIDEV_INPUTSINK;
            rid.hwndTarget = m_hwnd;
            m_mouseRegistered = (RegisterRawInputDevices(&rid, 1, sizeof(rid)) != FALSE);
            ShellDiagLog(L"Gesture", L"%s Mouse RawInput (hotCorner) ok=%d",
                wasRegistered ? L"Recovered" : L"Registered", m_mouseRegistered ? 1 : 0);
        }

        if (wantHid && !hasHid)
        {
            const bool wasRegistered = m_hidRegistered;
            RAWINPUTDEVICE rid{};
            rid.usUsagePage = kUsagePageDigitizer;
            rid.usUsage = kUsageDigitizerTouchPad;
            rid.dwFlags = RIDEV_INPUTSINK;
            rid.hwndTarget = m_hwnd;
            m_hidRegistered = (RegisterRawInputDevices(&rid, 1, sizeof(rid)) != FALSE);
            ShellDiagLog(L"Gesture", L"%s Touchpad HID RawInput ok=%d",
                wasRegistered ? L"Recovered" : L"Registered", m_hidRegistered ? 1 : 0);
        }
    }

    bool GestureEngine::IsSuppressedContext() const noexcept
    {
        if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
            (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0 ||
            (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0)
        {
            return true;
        }

        if (m_config.shakeSuppressFullscreen)
        {
            QUERY_USER_NOTIFICATION_STATE quns = QUNS_ACCEPTS_NOTIFICATIONS;
            if (SUCCEEDED(SHQueryUserNotificationState(&quns)))
            {
                if (quns == QUNS_RUNNING_D3D_FULL_SCREEN || quns == QUNS_PRESENTATION_MODE)
                    return true;
            }
        }
        return false;
    }

    void GestureEngine::OnRawInput(LPARAM lParam)
    {
        if (!lParam)
            return;

        UINT size = 0;
        HRAWINPUT hRaw = reinterpret_cast<HRAWINPUT>(lParam);
        if (GetRawInputData(hRaw, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || size == 0 || size > 65536)
            return;

        BYTE stackBuf[512]{};
        std::vector<BYTE> heapBuf;
        BYTE* buf = stackBuf;
        if (size > sizeof(stackBuf))
        {
            heapBuf.resize(size);
            buf = heapBuf.data();
        }

        if (GetRawInputData(hRaw, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) != size)
            return;

        const auto* raw = reinterpret_cast<const RAWINPUT*>(buf);
        if (raw->header.dwType == RIM_TYPEMOUSE)
            ProcessMouseRaw(raw->data.mouse);
        else if (raw->header.dwType == RIM_TYPEHID)
            ProcessHidRaw(raw->header.hDevice, raw->data.hid);
    }

    void GestureEngine::FlushPendingRawInput()
    {
        if (m_inFlush || !m_hwnd)
            return;
        if (HIWORD(GetQueueStatus(QS_RAWINPUT)) == 0)
            return;

        m_inFlush = true;
        MSG msg{};
        while (PeekMessageW(&msg, m_hwnd, WM_INPUT, WM_INPUT, PM_REMOVE | PM_NOYIELD | PM_QS_INPUT))
        {
            OnRawInput(msg.lParam);
            DefWindowProcW(msg.hwnd, msg.message, msg.wParam, msg.lParam);
        }
        m_inFlush = false;
    }

    bool GestureEngine::ShouldSuppressMouseButton(UINT msg)
    {
        if (!m_config.touchpadPhysLeftClickTrigger && !m_config.touchpadCornerTrigger)
            return false;

        FlushPendingRawInput();

        const ULONGLONG now = GetTickCount64();
        if (m_eatingUpForSuppressedDown && (now - m_suppressedDownTick) > 600)
            m_eatingUpForSuppressedDown = false;

        if (msg == WM_LBUTTONDOWN)
        {
            if (now <= m_suppressClickUntilTick || m_physBtnDownNow)
            {
                m_eatingUpForSuppressedDown = true;
                m_suppressedDownTick = now;
                return true;
            }
        }
        else if (msg == WM_LBUTTONUP)
        {
            if (m_eatingUpForSuppressedDown)
            {
                m_eatingUpForSuppressedDown = false;
                return true;
            }
        }
        return false;
    }

    void GestureEngine::TriggerHotCorner(const SwitcherConfig& cfg)
    {
        if (cfg.hotCornerAction == HotCornerAction::OpenRing)
        {
            if (m_callbacks.onOpenRing)
                m_callbacks.onOpenRing();
            return;
        }

        INPUT inputs[4]{};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wVk = VK_LWIN;
        inputs[0].ki.dwExtraInfo = kWinRingsInjectedTag;

        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wVk = VK_TAB;
        inputs[1].ki.dwExtraInfo = kWinRingsInjectedTag;

        inputs[2].type = INPUT_KEYBOARD;
        inputs[2].ki.wVk = VK_TAB;
        inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
        inputs[2].ki.dwExtraInfo = kWinRingsInjectedTag;

        inputs[3].type = INPUT_KEYBOARD;
        inputs[3].ki.wVk = VK_LWIN;
        inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
        inputs[3].ki.dwExtraInfo = kWinRingsInjectedTag;

        SendInput(4, inputs, sizeof(INPUT));
    }

    void GestureEngine::ProcessMouseRaw(const RAWMOUSE& mouse)
    {
        if (!m_config.hotCornerEnabled || (mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0)
            return;

        const LONG dx = mouse.lLastX;
        const LONG dy = mouse.lLastY;
        if (dx == 0 && dy == 0)
            return;
        if (std::abs(dx) > 800 || std::abs(dy) > 800)
            return;

        const ULONGLONG now = GetTickCount64();
        if (now < m_cornerCooldownUntil)
            return;

        POINT curPt{};
        if (!GetCursorPos(&curPt))
            return;

        // GNOME Pressure-Barrier Hot Corner
        HMONITOR hMon = MonitorFromPoint(curPt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        if (!GetMonitorInfoW(hMon, &mi))
            return;

        const bool inCorner = (m_config.hotCornerPos == HotCornerPos::TopLeft)
            ? (curPt.x <= mi.rcMonitor.left + 4 && curPt.y <= mi.rcMonitor.top + 4)
            : (curPt.x >= mi.rcMonitor.right - 5 && curPt.y <= mi.rcMonitor.top + 4);

        if (!inCorner)
        {
            m_cornerPressure = 0;
            m_cornerArmed = true;
            return;
        }
        if (!m_cornerArmed)
            return;

        if ((now - m_cornerLastPushTick) > 220)
            m_cornerPressure = 0;

        LONG push = 0;
        if (m_config.hotCornerPos == HotCornerPos::TopLeft)
            push = (std::max)(0L, -dx) + (std::max)(0L, -dy);
        else
            push = (std::max)(0L, dx) + (std::max)(0L, -dy);

        if (push > 0)
        {
            m_cornerPressure += static_cast<int>(push);
            m_cornerLastPushTick = now;
            if (m_cornerPressure >= m_config.hotCornerPressureThreshold)
            {
                m_cornerPressure = 0;
                m_cornerArmed = false;
                const bool ringOpen = m_callbacks.isRingOpen && m_callbacks.isRingOpen();
                if (!ringOpen && !IsSuppressedContext())
                {
                    m_cornerCooldownUntil = now + 850;
                    TriggerHotCorner(m_config);
                }
            }
        }
    }

    const GestureEngine::TouchpadCaps& GestureEngine::GetOrInspectTouchpad(HANDLE hDevice)
    {
        if (auto it = m_hidCache.find(hDevice); it != m_hidCache.end())
            return it->second;

        TouchpadCaps info{};
        RID_DEVICE_INFO devInfo{};
        devInfo.cbSize = sizeof(devInfo);
        UINT devInfoSize = sizeof(devInfo);
        if (GetRawInputDeviceInfoW(hDevice, RIDI_DEVICEINFO, &devInfo, &devInfoSize) == static_cast<UINT>(-1) ||
            devInfo.dwType != RIM_TYPEHID ||
            devInfo.hid.usUsagePage != kUsagePageDigitizer ||
            devInfo.hid.usUsage != kUsageDigitizerTouchPad)
        {
            return m_hidCache.emplace(hDevice, std::move(info)).first->second;
        }

        UINT ppSize = 0;
        if (GetRawInputDeviceInfoW(hDevice, RIDI_PREPARSEDDATA, nullptr, &ppSize) != 0 || ppSize == 0 || ppSize > 65536)
            return m_hidCache.emplace(hDevice, std::move(info)).first->second;

        info.preparsed.resize(ppSize);
        if (GetRawInputDeviceInfoW(hDevice, RIDI_PREPARSEDDATA, info.preparsed.data(), &ppSize) == static_cast<UINT>(-1))
        {
            info.preparsed.clear();
            return m_hidCache.emplace(hDevice, std::move(info)).first->second;
        }

        PHIDP_PREPARSED_DATA pp = reinterpret_cast<PHIDP_PREPARSED_DATA>(info.preparsed.data());
        HIDP_CAPS caps{};
        if (HidP_GetCaps(pp, &caps) != HIDP_STATUS_SUCCESS)
            return m_hidCache.emplace(hDevice, std::move(info)).first->second;

        info.isTouchpad = true;

        if (caps.NumberInputButtonCaps > 0)
        {
            USHORT btnCount = caps.NumberInputButtonCaps;
            std::vector<HIDP_BUTTON_CAPS> btnCaps(btnCount);
            if (HidP_GetButtonCaps(HidP_Input, btnCaps.data(), &btnCount, pp) == HIDP_STATUS_SUCCESS)
            {
                for (USHORT i = 0; i < btnCount; ++i)
                {
                    const auto& b = btnCaps[i];
                    const USAGE uMin = b.IsRange ? b.Range.UsageMin : b.NotRange.Usage;
                    const USAGE uMax = b.IsRange ? b.Range.UsageMax : b.NotRange.Usage;
                    if (b.UsagePage == kUsagePageButton && b.LinkCollection == 0 &&
                        uMin <= kUsageButton1 && uMax >= kUsageButton1)
                    {
                        info.hasButton1 = true;
                    }
                    else if (b.UsagePage == kUsagePageDigitizer &&
                        b.LinkUsagePage == kUsagePageDigitizer && b.LinkUsage == kUsageDigitizerFinger &&
                        uMin <= kUsageDigitizerTipSwitch && uMax >= kUsageDigitizerTipSwitch)
                    {
                        if (!info.hasTipSwitch)
                        {
                            info.hasTipSwitch = true;
                            info.tipLC = b.LinkCollection;
                        }
                    }
                }
            }
        }

        if (caps.NumberInputValueCaps > 0)
        {
            USHORT valCount = caps.NumberInputValueCaps;
            std::vector<HIDP_VALUE_CAPS> valCaps(valCount);
            if (HidP_GetValueCaps(HidP_Input, valCaps.data(), &valCount, pp) == HIDP_STATUS_SUCCESS)
            {
                for (USHORT i = 0; i < valCount; ++i)
                {
                    const auto& v = valCaps[i];
                    const USAGE vUsage = v.IsRange ? v.Range.UsageMin : v.NotRange.Usage;
                    if (v.UsagePage == kUsagePageDigitizer && vUsage == kUsageDigitizerContactCount)
                    {
                        info.hasContactCount = true;
                        info.contactCountLC = v.LinkCollection;
                    }
                    else if (v.UsagePage == kUsagePageGenericDesktop && v.LinkCollection == info.tipLC)
                    {
                        if (vUsage == kUsageGenericX)
                        {
                            info.xMin = v.LogicalMin;
                            info.xMax = (v.LogicalMax > v.LogicalMin) ? v.LogicalMax : 1;
                        }
                        else if (vUsage == kUsageGenericY)
                        {
                            info.yMin = v.LogicalMin;
                            info.yMax = (v.LogicalMax > v.LogicalMin) ? v.LogicalMax : 1;
                        }
                    }
                }
            }
        }

        info.valid = info.hasButton1 || info.hasTipSwitch;
        return m_hidCache.emplace(hDevice, std::move(info)).first->second;
    }

    void GestureEngine::ProcessHidRaw(HANDLE hDevice, const RAWHID& hid)
    {
        const bool wantMediaScroll = m_config.mediaIslandEnabled && m_config.mediaIslandScrollVolume;
        if (!m_config.touchpadPhysLeftClickTrigger && !m_config.touchpadCornerTrigger && !wantMediaScroll)
            return;

        const TouchpadCaps& caps = GetOrInspectTouchpad(hDevice);
        if (!caps.valid || caps.preparsed.empty() || hid.dwSizeHid == 0 || hid.dwCount == 0)
            return;

        PHIDP_PREPARSED_DATA pp = reinterpret_cast<PHIDP_PREPARSED_DATA>(const_cast<BYTE*>(caps.preparsed.data()));
        const ULONGLONG now = GetTickCount64();

        for (DWORD idx = 0; idx < hid.dwCount; ++idx)
        {
            PCHAR report = reinterpret_cast<PCHAR>(const_cast<BYTE*>(hid.bRawData + idx * hid.dwSizeHid));
            ULONG reportLen = hid.dwSizeHid;

            bool physBtn = false;
            if (caps.hasButton1)
            {
                USAGE usages[16]{};
                ULONG uCount = 16;
                if (HidP_GetUsages(HidP_Input, kUsagePageButton, 0, usages, &uCount, pp, report, reportLen) == HIDP_STATUS_SUCCESS)
                    physBtn = HasUsage(usages, uCount, kUsageButton1);
            }

            bool tipDown = false;
            if (caps.hasTipSwitch)
            {
                USAGE usages[16]{};
                ULONG uCount = 16;
                if (HidP_GetUsages(HidP_Input, kUsagePageDigitizer, caps.tipLC, usages, &uCount, pp, report, reportLen) == HIDP_STATUS_SUCCESS)
                    tipDown = HasUsage(usages, uCount, kUsageDigitizerTipSwitch);
            }

            ULONG contactCount = tipDown ? 1u : 0u;
            if (caps.hasContactCount)
            {
                ULONG ccVal = 0;
                if (HidP_GetUsageValue(HidP_Input, kUsagePageDigitizer, caps.contactCountLC,
                    kUsageDigitizerContactCount, &ccVal, pp, report, reportLen) == HIDP_STATUS_SUCCESS)
                {
                    contactCount = ccVal;
                }
            }

            ULONG rawX = 0;
            ULONG rawY = 0;
            HidP_GetUsageValue(HidP_Input, kUsagePageGenericDesktop, caps.tipLC,
                kUsageGenericX, &rawX, pp, report, reportLen);
            HidP_GetUsageValue(HidP_Input, kUsagePageGenericDesktop, caps.tipLC,
                kUsageGenericY, &rawY, pp, report, reportLen);

            const double xSpan = static_cast<double>((std::max)(1L, caps.xMax - caps.xMin));
            const double ySpan = static_cast<double>((std::max)(1L, caps.yMax - caps.yMin));
            const double nx = (std::clamp)(static_cast<double>(static_cast<LONG>(rawX) - caps.xMin) / xSpan, 0.0, 1.0);
            const double ny = (std::clamp)(static_cast<double>(static_cast<LONG>(rawY) - caps.yMin) / ySpan, 0.0, 1.0);

            // Physical Left-Click Trigger (preserves bottom-right physical right-click zone)
            if (m_config.touchpadPhysLeftClickTrigger)
            {
                const bool inRightClickZone = (nx >= 0.50 && ny >= 0.75);
                if (physBtn && !m_tpState.prevPhysBtn && !inRightClickZone)
                {
                    m_physBtnDownNow = true;
                    m_suppressClickUntilTick = now + 140;
                    if ((now - m_tpState.lastTriggerTick) >= 220)
                    {
                        m_tpState.lastTriggerTick = now;
                        if (m_callbacks.onToggleRing)
                            m_callbacks.onToggleRing();
                    }
                }
                else if (physBtn && m_physBtnDownNow)
                {
                    m_suppressClickUntilTick = now + 90;
                }
                else if (!physBtn && m_tpState.prevPhysBtn && m_physBtnDownNow)
                {
                    m_physBtnDownNow = false;
                    m_suppressClickUntilTick = now + 90;
                }
            }
            else
            {
                m_physBtnDownNow = false;
            }

            // Touchpad Corner Tap Trigger
            if (m_config.touchpadCornerTrigger)
            {
                const double frac = static_cast<double>(m_config.touchpadCornerSizePct) / 100.0;
                const bool inCorner = (m_config.touchpadCornerPos == TouchpadCornerPos::TopLeft)
                    ? (nx <= frac && ny <= frac)
                    : (nx >= (1.0 - frac) && ny <= frac);

                if (tipDown && !m_tpState.prevTipDown && contactCount == 1 && !physBtn && inCorner)
                {
                    m_tpState.cornerTapTracking = true;
                    m_tpState.cornerStartTick = now;
                    m_tpState.cornerStartX = nx;
                    m_tpState.cornerStartY = ny;
                }
                else if (m_tpState.cornerTapTracking && tipDown)
                {
                    if (contactCount > 1 || physBtn ||
                        std::abs(nx - m_tpState.cornerStartX) > 0.05 ||
                        std::abs(ny - m_tpState.cornerStartY) > 0.05 ||
                        (now - m_tpState.cornerStartTick) > 260)
                    {
                        m_tpState.cornerTapTracking = false;
                    }
                }
                else if (!tipDown && m_tpState.prevTipDown && m_tpState.cornerTapTracking)
                {
                    m_tpState.cornerTapTracking = false;
                    if ((now - m_tpState.cornerStartTick) <= 260 && (now - m_tpState.lastTriggerTick) >= 220)
                    {
                        m_tpState.lastTriggerTick = now;
                        m_suppressClickUntilTick = now + 140;
                        if (m_callbacks.onToggleRing)
                            m_callbacks.onToggleRing();
                    }
                }
            }
            else
            {
                m_tpState.cornerTapTracking = false;
            }

            // Two-finger touchpad vertical scroll over Media Island -> Master Volume control
            if (wantMediaScroll)
            {
                if (contactCount == 2 && tipDown && !physBtn)
                {
                    if (!m_tpState.twoFingerScrollActive || (now - m_tpState.twoFingerLastTick) > 250)
                    {
                        POINT curPt{};
                        const bool ringOpen = m_callbacks.isRingOpen && m_callbacks.isRingOpen();
                        m_tpState.twoFingerScrollActive = true;
                        m_tpState.twoFingerAccumY = 0.0;
                        m_tpState.twoFingerLastX = nx;
                        m_tpState.twoFingerLastY = ny;
                        m_tpState.twoFingerOverIsland = !ringOpen && GetCursorPos(&curPt) &&
                            m_callbacks.isPointOverMediaIsland && m_callbacks.isPointOverMediaIsland(curPt);
                    }
                    else
                    {
                        const double dnx = nx - m_tpState.twoFingerLastX;
                        const double dny = ny - m_tpState.twoFingerLastY;
                        m_tpState.twoFingerLastX = nx;
                        m_tpState.twoFingerLastY = ny;
                        if (m_tpState.twoFingerOverIsland &&
                            std::abs(dnx) < 0.25 && std::abs(dny) < 0.25 &&
                            std::abs(dny) >= std::abs(dnx) * 0.7)
                        {
                            // Moving fingers upward on the pad (negative HID Y delta) raises volume (+).
                            m_tpState.twoFingerAccumY += -dny;
                            constexpr double kStepFrac = 0.022;
                            while (std::abs(m_tpState.twoFingerAccumY) >= kStepFrac)
                            {
                                const short delta = (m_tpState.twoFingerAccumY > 0.0)
                                    ? static_cast<short>(30)
                                    : static_cast<short>(-30);
                                m_tpState.twoFingerAccumY -= (delta > 0) ? kStepFrac : -kStepFrac;
                                if (m_callbacks.onMediaIslandTouchpadWheel)
                                    m_callbacks.onMediaIslandTouchpadWheel(delta);
                            }
                        }
                    }
                    m_tpState.twoFingerLastTick = now;
                }
                else if (contactCount != 0 || !tipDown || physBtn)
                {
                    m_tpState.twoFingerScrollActive = false;
                    m_tpState.twoFingerOverIsland = false;
                    m_tpState.twoFingerAccumY = 0.0;
                }
            }
            else
            {
                m_tpState.twoFingerScrollActive = false;
                m_tpState.twoFingerOverIsland = false;
                m_tpState.twoFingerAccumY = 0.0;
            }

            m_tpState.prevPhysBtn = physBtn;
            m_tpState.prevTipDown = tipDown;
        }
    }
} // namespace winrings
