#pragma once

#include <Windows.h>
#include <functional>

#include "../shared/WinRingsConfig.h"

namespace winrings
{
    class InputEngine
    {
    public:
        struct Callbacks
        {
            std::function<bool()> isRingOpen;
            std::function<bool()> isRingLockedOpen;
            std::function<bool()> isRingDragActive;
            std::function<bool(POINT screenPt)> isPointInOverlay;
            std::function<double()> getOverlayDpiScale;
            std::function<void(int screenX, int screenY)> onTriggerPressed;
            std::function<void(int screenX, int screenY, bool isTap)> onTriggerReleased;
            std::function<void()> onDoubleTapLastApp;
            std::function<void(int screenX, int screenY)> onMouseMoved;
            std::function<void(int wheelDelta)> onMouseWheel;
            std::function<void()> onCancelRequested;
            std::function<void()> onToggleRequested;
            std::function<void()> onCommitRequested;
            std::function<void(int stepDelta)> onArrowStep;
            std::function<void(wchar_t ch)> onFilterChar;
            std::function<void()> onFilterBackspace;
            std::function<void()> onUserPointerDelta;
            std::function<void()> onDirectSwitchStep;
            std::function<void()> onDirectSwitchCommit;
            std::function<bool(UINT msg)> shouldSuppressTouchpadMouse;
            std::function<bool(POINT screenPt, short wheelDelta)> onMediaIslandWheel;
        };

        InputEngine() = default;
        ~InputEngine();

        InputEngine(const InputEngine&) = delete;
        InputEngine& operator=(const InputEngine&) = delete;

        bool Install(const SwitcherConfig& config, Callbacks callbacks);
        void Uninstall();
        void UpdateConfig(const SwitcherConfig& config);
        void SetSuspended(bool suspended) noexcept;
        void ResetTransientState() noexcept;

    private:
        static LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wp, LPARAM lp);
        static LRESULT CALLBACK MouseHookProc(int code, WPARAM wp, LPARAM lp);

        LRESULT HandleKeyboard(int code, WPARAM wp, LPARAM lp);
        LRESULT HandleMouse(int code, WPARAM wp, LPARAM lp);

        bool IsConfiguredMouseDown(UINT msg, DWORD mouseData) const noexcept;
        bool IsConfiguredMouseUp(UINT msg, DWORD mouseData) const noexcept;
        bool IsHotkeyMatch(DWORD vkCode) const noexcept;
        bool IsHotkeyModifierVk(DWORD vkCode) const noexcept;
        bool IsDirectSwitchMatch(DWORD vkCode) const noexcept;
        bool IsDirectSwitchModifierVk(DWORD vkCode) const noexcept;
        static void SendDummyMaskKey(bool includeLWinUp = false) noexcept;

        void StartTrigger(bool mouseTrigger);
        void EndTrigger();

        SwitcherConfig m_config;
        Callbacks m_callbacks;
        HHOOK m_keyboardHook = nullptr;
        HHOOK m_mouseHook = nullptr;

        bool m_suspended = false;
        bool m_triggerHeld = false;
        bool m_mouseTrigger = false;
        bool m_rightButtonDown = false;
        bool m_chordActive = false;
        bool m_outsideClickPending = false;
        bool m_winKeyDown = false;
        bool m_winComboUsed = false;
        bool m_directSwitchActive = false;
        LONGLONG m_triggerDownQpc = 0;
        LONGLONG m_lastTapQpc = 0;
        LONGLONG m_qpcFreq = 1;
        POINT m_triggerOrigin{};
        POINT m_outsideClickOrigin{};

        static InputEngine* s_instance;
    };
} // namespace winrings
