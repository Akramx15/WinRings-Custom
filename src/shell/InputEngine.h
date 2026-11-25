#pragma once

#include <Windows.h>
#include <array>
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
            std::function<bool(POINT screenPt, bool isKeyboard)> shouldProcessInput;
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
            std::function<void(POINT screenPt)> onShakeDetected;
            std::function<bool()> isSnapFallbackDragActive;
            std::function<void(POINT screenPt)> onSnapFallbackDragMove;
            std::function<bool(POINT screenPt)> onSnapFallbackDragRelease;
            std::function<void()> onSnapFallbackDragCancel;
        };

        InputEngine() = default;
        ~InputEngine();

        InputEngine(const InputEngine&) = delete;
        InputEngine& operator=(const InputEngine&) = delete;

        bool Install(const SwitcherConfig& config, Callbacks callbacks);
        void Uninstall();
        bool ReinstallHooks();
        void UpdateConfig(const SwitcherConfig& config);
        void SetSuspended(bool suspended) noexcept;
        bool IsSuspended() const noexcept { return m_suspended; }
        bool AreHooksInstalled() const noexcept { return m_keyboardHook != nullptr && m_mouseHook != nullptr; }
        ULONGLONG LastMouseHookTick() const noexcept { return m_lastMouseHookTick; }
        void UpdateCachedDpiScale(double dpiScale) noexcept;
        void ResetShakeState() noexcept;
        void ResetTransientState() noexcept;

    private:
        struct ShakeReversal
        {
            ULONGLONG tickMs = 0;
            double ampPx = 0.0;
            POINT endPt{};
        };
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
        double m_cachedDpiScale = 1.0;
        ULONGLONG m_lastMouseHookTick = 0;
        ULONGLONG m_lastKeyboardHookTick = 0;

        bool m_hasShakeLastPt = false;
        POINT m_shakeLastPt{};
        double m_shakeSegDx = 0.0;
        double m_shakeSegDy = 0.0;
        double m_shakeRevBackAccum = 0.0;
        ULONGLONG m_shakeSegLastTick = 0;
        ULONGLONG m_shakeCooldownUntil = 0;
        std::array<ShakeReversal, 16> m_shakeReversals{};
        size_t m_shakeRevHead = 0;
        size_t m_shakeRevCount = 0;

        LONGLONG m_triggerDownQpc = 0;
        LONGLONG m_lastTapQpc = 0;
        LONGLONG m_qpcFreq = 1;
        POINT m_triggerOrigin{};
        POINT m_outsideClickOrigin{};

        static InputEngine* s_instance;
    };
} // namespace winrings
