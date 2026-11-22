#include "InputEngine.h"

#include <algorithm>
#include <cmath>

namespace winrings
{
    namespace
    {
        constexpr ULONG_PTR kWinRingsInjectedTag = 0x57524E47ull;
    }

    InputEngine* InputEngine::s_instance = nullptr;

    InputEngine::~InputEngine()
    {
        Uninstall();
    }

    bool InputEngine::Install(const SwitcherConfig& config, Callbacks callbacks)
    {
        Uninstall();
        m_config = config;
        m_callbacks = std::move(callbacks);
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        m_qpcFreq = freq.QuadPart > 0 ? freq.QuadPart : 1;
        s_instance = this;

        HMODULE selfMod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&KeyboardHookProc), &selfMod);

        m_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHookProc, selfMod, 0);
        m_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, selfMod, 0);
        return m_keyboardHook != nullptr && m_mouseHook != nullptr;
    }

    void InputEngine::Uninstall()
    {
        ResetTransientState();
        if (m_keyboardHook)
        {
            UnhookWindowsHookEx(m_keyboardHook);
            m_keyboardHook = nullptr;
        }
        if (m_mouseHook)
        {
            UnhookWindowsHookEx(m_mouseHook);
            m_mouseHook = nullptr;
        }
        if (s_instance == this)
            s_instance = nullptr;
    }

    void InputEngine::UpdateConfig(const SwitcherConfig& config)
    {
        m_config = config;
    }

    void InputEngine::SetSuspended(bool suspended) noexcept
    {
        m_suspended = suspended;
        if (suspended)
            ResetTransientState();
    }

    void InputEngine::ResetTransientState() noexcept
    {
        m_triggerHeld = false;
        m_mouseTrigger = false;
        m_rightButtonDown = false;
        m_chordActive = false;
        m_outsideClickPending = false;
        m_outsideClickOrigin = {};
        m_winKeyDown = false;
        m_winComboUsed = false;
        if (m_directSwitchActive)
        {
            m_directSwitchActive = false;
            if (m_callbacks.onDirectSwitchCommit)
                m_callbacks.onDirectSwitchCommit();
        }
        m_lastTapQpc = 0;
    }

    LRESULT CALLBACK InputEngine::KeyboardHookProc(int code, WPARAM wp, LPARAM lp)
    {
        try
        {
            if (code >= 0 && s_instance)
                return s_instance->HandleKeyboard(code, wp, lp);
        }
        catch (...) {}
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    LRESULT CALLBACK InputEngine::MouseHookProc(int code, WPARAM wp, LPARAM lp)
    {
        try
        {
            if (code >= 0 && s_instance)
                return s_instance->HandleMouse(code, wp, lp);
        }
        catch (...) {}
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    bool InputEngine::IsHotkeyMatch(DWORD vkCode) const noexcept
    {
        if (!m_config.hotkeyEnabled || vkCode != m_config.hotkeyVk)
            return false;

        const bool wantAlt = (m_config.hotkeyModifiers & MOD_ALT) != 0;
        const bool wantCtrl = (m_config.hotkeyModifiers & MOD_CONTROL) != 0;
        const bool wantShift = (m_config.hotkeyModifiers & MOD_SHIFT) != 0;
        const bool wantWin = (m_config.hotkeyModifiers & MOD_WIN) != 0;

        const bool hasAlt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        const bool hasCtrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool hasShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool hasWin = ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) ||
            ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0);

        return wantAlt == hasAlt && wantCtrl == hasCtrl &&
            wantShift == hasShift && wantWin == hasWin;
    }

    bool InputEngine::IsHotkeyModifierVk(DWORD vkCode) const noexcept
    {
        if ((m_config.hotkeyModifiers & MOD_ALT) &&
            (vkCode == VK_MENU || vkCode == VK_LMENU || vkCode == VK_RMENU))
            return true;
        if ((m_config.hotkeyModifiers & MOD_CONTROL) &&
            (vkCode == VK_CONTROL || vkCode == VK_LCONTROL || vkCode == VK_RCONTROL))
            return true;
        if ((m_config.hotkeyModifiers & MOD_SHIFT) &&
            (vkCode == VK_SHIFT || vkCode == VK_LSHIFT || vkCode == VK_RSHIFT))
            return true;
        if ((m_config.hotkeyModifiers & MOD_WIN) &&
            (vkCode == VK_LWIN || vkCode == VK_RWIN))
            return true;
        return false;
    }

    bool InputEngine::IsDirectSwitchMatch(DWORD vkCode) const noexcept
    {
        if (!m_config.directSwitchEnabled || vkCode != m_config.directSwitchVk)
            return false;

        const bool wantAlt = (m_config.directSwitchModifiers & MOD_ALT) != 0;
        const bool wantCtrl = (m_config.directSwitchModifiers & MOD_CONTROL) != 0;
        const bool wantShift = (m_config.directSwitchModifiers & MOD_SHIFT) != 0;
        const bool wantWin = (m_config.directSwitchModifiers & MOD_WIN) != 0;

        const bool hasAlt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        const bool hasCtrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool hasShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool hasWin = ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) ||
            ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0);

        return wantAlt == hasAlt && wantCtrl == hasCtrl &&
            wantShift == hasShift && wantWin == hasWin;
    }

    bool InputEngine::IsDirectSwitchModifierVk(DWORD vkCode) const noexcept
    {
        if ((m_config.directSwitchModifiers & MOD_ALT) &&
            (vkCode == VK_MENU || vkCode == VK_LMENU || vkCode == VK_RMENU))
            return true;
        if ((m_config.directSwitchModifiers & MOD_CONTROL) &&
            (vkCode == VK_CONTROL || vkCode == VK_LCONTROL || vkCode == VK_RCONTROL))
            return true;
        if ((m_config.directSwitchModifiers & MOD_SHIFT) &&
            (vkCode == VK_SHIFT || vkCode == VK_LSHIFT || vkCode == VK_RSHIFT))
            return true;
        if ((m_config.directSwitchModifiers & MOD_WIN) &&
            (vkCode == VK_LWIN || vkCode == VK_RWIN))
            return true;
        return false;
    }

    void InputEngine::SendDummyMaskKey(bool includeLWinUp) noexcept
    {
        INPUT inputs[3]{};
        UINT count = 2;

        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wVk = VK_NONAME;
        inputs[0].ki.dwExtraInfo = kWinRingsInjectedTag;

        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wVk = VK_NONAME;
        inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
        inputs[1].ki.dwExtraInfo = kWinRingsInjectedTag;

        if (includeLWinUp)
        {
            inputs[2].type = INPUT_KEYBOARD;
            inputs[2].ki.wVk = VK_LWIN;
            inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
            inputs[2].ki.dwExtraInfo = kWinRingsInjectedTag;
            count = 3;
        }
        SendInput(count, inputs, sizeof(INPUT));
    }

    LRESULT InputEngine::HandleKeyboard(int code, WPARAM wp, LPARAM lp)
    {
        if (m_suspended)
            return CallNextHookEx(m_keyboardHook, code, wp, lp);

        const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if (!kb)
            return CallNextHookEx(m_keyboardHook, code, wp, lp);

        const bool injected = (kb->flags & LLKHF_INJECTED) != 0;
        if (injected && kb->dwExtraInfo == kWinRingsInjectedTag)
            return CallNextHookEx(m_keyboardHook, code, wp, lp);

        const bool isDown = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
        const bool isUp = (wp == WM_KEYUP || wp == WM_SYSKEYUP);
        const bool ringOpen = m_callbacks.isRingOpen && m_callbacks.isRingOpen();

        if (!injected)
        {
            if (isDown)
            {
                if (kb->vkCode == VK_LWIN)
                {
                    if (!m_winKeyDown)
                    {
                        m_winKeyDown = true;
                        m_winComboUsed = false;
                    }
                }
                else if (m_winKeyDown)
                {
                    m_winComboUsed = true;
                }
            }
            else if (isUp && kb->vkCode == VK_LWIN && m_winKeyDown)
            {
                const bool soloTap = !m_winComboUsed && m_config.winKeyTrigger && !m_directSwitchActive;
                m_winKeyDown = false;
                m_winComboUsed = false;
                if (soloTap)
                {
                    SendDummyMaskKey(true);
                    if (m_callbacks.onToggleRequested)
                        m_callbacks.onToggleRequested();
                    return 1;
                }
            }
        }

        if (isDown)
        {
            // Priority #1: Direct Window Switch (no popup, fully swallows key so target app receives nothing)
            if (IsDirectSwitchMatch(kb->vkCode))
            {
                if (ringOpen && m_callbacks.onCancelRequested)
                    m_callbacks.onCancelRequested();
                if (!m_directSwitchActive)
                {
                    m_directSwitchActive = true;
                    if (m_winKeyDown)
                        m_winComboUsed = true;
                    SendDummyMaskKey(false);
                }
                if (m_callbacks.onDirectSwitchStep)
                    m_callbacks.onDirectSwitchStep();
                return 1;
            }

            if (ringOpen && kb->vkCode == VK_ESCAPE)
            {
                m_triggerHeld = false;
                if (m_callbacks.onCancelRequested) m_callbacks.onCancelRequested();
                return 1;
            }

            if (IsHotkeyMatch(kb->vkCode) && !m_triggerHeld)
            {
                StartTrigger(false);
                return 1;
            }

            if (m_config.menuKeyEnabled && kb->vkCode == VK_APPS)
            {
                if (m_callbacks.onToggleRequested) m_callbacks.onToggleRequested();
                return 1;
            }

            if (m_config.interceptWinTab && kb->vkCode == VK_TAB &&
                (((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) || ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0)))
            {
                if (m_callbacks.onToggleRequested) m_callbacks.onToggleRequested();
                return 1;
            }

            if (m_config.interceptAltTab && kb->vkCode == VK_TAB &&
                ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0))
            {
                if (m_callbacks.onToggleRequested) m_callbacks.onToggleRequested();
                return 1;
            }

            // Type-to-find & keyboard navigation whenever the ring is open
            if (ringOpen)
            {
                const bool sticky = m_callbacks.isRingLockedOpen && m_callbacks.isRingLockedOpen();
                const bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

                if (kb->vkCode == VK_TAB)
                {
                    if (m_callbacks.onArrowStep) m_callbacks.onArrowStep(shiftHeld ? 120 : -120);
                    return 1;
                }
                if (sticky)
                {
                    if (kb->vkCode == VK_RETURN)
                    {
                        if (m_callbacks.onCommitRequested) m_callbacks.onCommitRequested();
                        return 1;
                    }
                    if (kb->vkCode == VK_RIGHT || kb->vkCode == VK_DOWN)
                    {
                        if (m_callbacks.onArrowStep) m_callbacks.onArrowStep(-120);
                        return 1;
                    }
                    if (kb->vkCode == VK_LEFT || kb->vkCode == VK_UP)
                    {
                        if (m_callbacks.onArrowStep) m_callbacks.onArrowStep(120);
                        return 1;
                    }
                }
                if (kb->vkCode == VK_BACK)
                {
                    if (m_callbacks.onFilterBackspace) m_callbacks.onFilterBackspace();
                    return 1;
                }
                const bool ctrlOrAlt = ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) ||
                    ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
                if (!ctrlOrAlt)
                {
                    wchar_t typedChar = 0;
                    if (kb->vkCode >= 'A' && kb->vkCode <= 'Z')
                        typedChar = static_cast<wchar_t>(L'a' + (kb->vkCode - 'A'));
                    else if (kb->vkCode >= '0' && kb->vkCode <= '9')
                        typedChar = static_cast<wchar_t>(L'0' + (kb->vkCode - '0'));
                    else if (kb->vkCode >= VK_NUMPAD0 && kb->vkCode <= VK_NUMPAD9)
                        typedChar = static_cast<wchar_t>(L'0' + (kb->vkCode - VK_NUMPAD0));
                    else if (kb->vkCode == VK_SPACE)
                        typedChar = L' ';
                    else if (kb->vkCode == VK_OEM_MINUS || kb->vkCode == VK_SUBTRACT)
                        typedChar = L'-';
                    else if (kb->vkCode == VK_OEM_PERIOD || kb->vkCode == VK_DECIMAL)
                        typedChar = L'.';

                    if (typedChar != 0)
                    {
                        if (m_callbacks.onFilterChar) m_callbacks.onFilterChar(typedChar);
                        return 1;
                    }
                }
            }
        }
        else if (isUp)
        {
            if (m_directSwitchActive)
            {
                if (kb->vkCode == m_config.directSwitchVk)
                    return 1;
                if (IsDirectSwitchModifierVk(kb->vkCode))
                {
                    m_directSwitchActive = false;
                    if (m_callbacks.onDirectSwitchCommit)
                        m_callbacks.onDirectSwitchCommit();
                }
            }

            if (m_config.menuKeyEnabled && kb->vkCode == VK_APPS)
                return 1;

            if (m_triggerHeld && !m_mouseTrigger &&
                (kb->vkCode == m_config.hotkeyVk || IsHotkeyModifierVk(kb->vkCode)))
            {
                EndTrigger();
            }
        }

        return CallNextHookEx(m_keyboardHook, code, wp, lp);
    }

    bool InputEngine::IsConfiguredMouseDown(UINT msg, DWORD mouseData) const noexcept
    {
        const WORD xButton = HIWORD(mouseData);
        switch (m_config.mouseTrigger)
        {
        case MouseTriggerButton::Middle: return msg == WM_MBUTTONDOWN;
        case MouseTriggerButton::SideBack: return msg == WM_XBUTTONDOWN && xButton == XBUTTON1;
        case MouseTriggerButton::SideForward: return msg == WM_XBUTTONDOWN && xButton == XBUTTON2;
        default: return false;
        }
    }

    bool InputEngine::IsConfiguredMouseUp(UINT msg, DWORD mouseData) const noexcept
    {
        const WORD xButton = HIWORD(mouseData);
        switch (m_config.mouseTrigger)
        {
        case MouseTriggerButton::Middle: return msg == WM_MBUTTONUP;
        case MouseTriggerButton::SideBack: return msg == WM_XBUTTONUP && xButton == XBUTTON1;
        case MouseTriggerButton::SideForward: return msg == WM_XBUTTONUP && xButton == XBUTTON2;
        default: return false;
        }
    }

    LRESULT InputEngine::HandleMouse(int code, WPARAM wp, LPARAM lp)
    {
        if (m_suspended)
            return CallNextHookEx(m_mouseHook, code, wp, lp);

        const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        if (!ms)
            return CallNextHookEx(m_mouseHook, code, wp, lp);

        const UINT msg = static_cast<UINT>(wp);
        const bool injectedMouse = (ms->flags & LLMHF_INJECTED) != 0;
        const bool ringOpen = m_callbacks.isRingOpen && m_callbacks.isRingOpen();

        if (!injectedMouse && (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) &&
            m_callbacks.shouldSuppressTouchpadMouse && m_callbacks.shouldSuppressTouchpadMouse(msg))
        {
            return 1;
        }

        if (!injectedMouse && m_winKeyDown &&
            (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN ||
             msg == WM_XBUTTONDOWN || msg == WM_MOUSEWHEEL))
        {
            m_winComboUsed = true;
        }

        if (msg == WM_MOUSEMOVE)
        {
            if (m_outsideClickPending)
            {
                const int dragThresh = (std::max)(5, (std::max)(GetSystemMetrics(SM_CXDRAG), GetSystemMetrics(SM_CYDRAG)));
                if (std::abs(ms->pt.x - m_outsideClickOrigin.x) >= dragThresh ||
                    std::abs(ms->pt.y - m_outsideClickOrigin.y) >= dragThresh)
                {
                    m_outsideClickPending = false;
                }
            }
            if ((ms->flags & LLMHF_INJECTED) == 0 && m_callbacks.onUserPointerDelta)
                m_callbacks.onUserPointerDelta();
            if (ringOpen || m_triggerHeld)
            {
                if (m_callbacks.onMouseMoved)
                    m_callbacks.onMouseMoved(ms->pt.x, ms->pt.y);
            }
            return CallNextHookEx(m_mouseHook, code, wp, lp);
        }

        if (msg == WM_MOUSEWHEEL)
        {
            const short delta = static_cast<short>(HIWORD(ms->mouseData));
            if (delta != 0)
            {
                if (ringOpen && m_config.scrollWheelMode != ScrollWheelBehavior::Disabled)
                {
                    if (m_callbacks.onMouseWheel)
                        m_callbacks.onMouseWheel(delta);
                    return 1;
                }
                if (!ringOpen && m_callbacks.onMediaIslandWheel &&
                    m_callbacks.onMediaIslandWheel(ms->pt, delta))
                {
                    return 1;
                }
            }
        }

        // Right+Left chord trigger support (non-blocking for normal right-click)
        if (m_config.mouseTrigger == MouseTriggerButton::RightLeftChord)
        {
            if (msg == WM_RBUTTONDOWN)
            {
                m_rightButtonDown = true;
            }
            else if (msg == WM_LBUTTONDOWN && m_rightButtonDown && !m_triggerHeld)
            {
                m_chordActive = true;
                StartTrigger(true);
                return 1;
            }
            else if (msg == WM_LBUTTONUP && m_chordActive && m_triggerHeld)
            {
                m_chordActive = false;
                EndTrigger();
                return 1;
            }
            else if (msg == WM_RBUTTONUP)
            {
                m_rightButtonDown = false;
                if (m_chordActive)
                {
                    m_chordActive = false;
                    if (m_triggerHeld)
                        EndTrigger();
                    return 1;
                }
            }
        }
        else
        {
            if (IsConfiguredMouseDown(msg, ms->mouseData) && !m_triggerHeld)
            {
                StartTrigger(true);
                return 1;
            }
            if (IsConfiguredMouseUp(msg, ms->mouseData) && m_triggerHeld && m_mouseTrigger)
            {
                EndTrigger();
                return 1;
            }
        }

        // Click outside the overlay in sticky mode cancels only on clean click-release (not when dragging)
        if (msg == WM_LBUTTONDOWN && ringOpen &&
            m_callbacks.isRingLockedOpen && m_callbacks.isRingLockedOpen())
        {
            const bool dragActive = m_callbacks.isRingDragActive && m_callbacks.isRingDragActive();
            if (!dragActive && (!m_callbacks.isPointInOverlay || !m_callbacks.isPointInOverlay(ms->pt)))
            {
                m_outsideClickPending = true;
                m_outsideClickOrigin = ms->pt;
            }
            else
            {
                m_outsideClickPending = false;
            }
        }
        else if (msg == WM_LBUTTONUP && m_outsideClickPending)
        {
            m_outsideClickPending = false;
            const bool dragActive = m_callbacks.isRingDragActive && m_callbacks.isRingDragActive();
            if (ringOpen && m_callbacks.isRingLockedOpen && m_callbacks.isRingLockedOpen() &&
                !dragActive && (!m_callbacks.isPointInOverlay || !m_callbacks.isPointInOverlay(ms->pt)))
            {
                if (m_callbacks.onCancelRequested)
                    m_callbacks.onCancelRequested();
            }
        }

        return CallNextHookEx(m_mouseHook, code, wp, lp);
    }

    void InputEngine::StartTrigger(bool mouseTrigger)
    {
        m_triggerHeld = true;
        m_mouseTrigger = mouseTrigger;
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        m_triggerDownQpc = now.QuadPart;
        GetCursorPos(&m_triggerOrigin);
        if (m_callbacks.onTriggerPressed)
            m_callbacks.onTriggerPressed(m_triggerOrigin.x, m_triggerOrigin.y);
    }

    void InputEngine::EndTrigger()
    {
        m_triggerHeld = false;
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        POINT releasePt{};
        GetCursorPos(&releasePt);

        const double elapsedMs = static_cast<double>(now.QuadPart - m_triggerDownQpc) * 1000.0 /
            static_cast<double>(m_qpcFreq);
        const double dpiScale = m_callbacks.getOverlayDpiScale ? m_callbacks.getOverlayDpiScale() : 1.0;
        const double movedDip = std::hypot(
            static_cast<double>(releasePt.x - m_triggerOrigin.x),
            static_cast<double>(releasePt.y - m_triggerOrigin.y)) / (dpiScale > 0.0 ? dpiScale : 1.0);
        const bool lbuttonHeld = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        const bool isTap = lbuttonHeld ||
            ((elapsedMs < static_cast<double>(m_config.tapThresholdMs)) &&
             (movedDip < m_config.deadzoneDip));

        if (!lbuttonHeld && isTap && m_config.doubleTapLastApp)
        {
            const double sinceLastMs = (m_lastTapQpc > 0)
                ? (static_cast<double>(now.QuadPart - m_lastTapQpc) * 1000.0 / static_cast<double>(m_qpcFreq))
                : 1e9;
            if (sinceLastMs <= static_cast<double>(m_config.doubleTapWindowMs))
            {
                m_lastTapQpc = 0;
                if (m_callbacks.onDoubleTapLastApp)
                    m_callbacks.onDoubleTapLastApp();
                return;
            }
            m_lastTapQpc = now.QuadPart;
        }
        else
        {
            m_lastTapQpc = 0;
        }

        if (m_callbacks.onTriggerReleased)
            m_callbacks.onTriggerReleased(releasePt.x, releasePt.y, isTap);
    }
} // namespace winrings
