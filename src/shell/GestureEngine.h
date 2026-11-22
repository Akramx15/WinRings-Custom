#pragma once

#include <Windows.h>
#include <deque>
#include <functional>
#include <unordered_map>
#include <vector>

#include "../shared/WinRingsConfig.h"

namespace winrings
{
    class GestureEngine
    {
    public:
        struct Callbacks
        {
            std::function<bool()> isRingOpen;
            std::function<void()> onToggleRing;
            std::function<void()> onOpenRing;
            std::function<double(POINT pt)> getDpiScaleAtPoint;
        };

        GestureEngine() = default;
        ~GestureEngine();

        GestureEngine(const GestureEngine&) = delete;
        GestureEngine& operator=(const GestureEngine&) = delete;

        bool Initialize(HWND hwnd, const SwitcherConfig& cfg, Callbacks callbacks);
        void UpdateConfig(const SwitcherConfig& cfg);
        void Shutdown();

        void OnRawInput(LPARAM lParam);
        bool ShouldSuppressMouseButton(UINT msg);

    private:
        struct ShakeReversal
        {
            ULONGLONG tickMs = 0;
            double ampPx = 0.0;
            POINT endPt{};
        };

        struct TouchpadCaps
        {
            bool valid = false;
            bool isTouchpad = false;
            std::vector<BYTE> preparsed;
            USHORT contactCountLC = 0;
            USHORT tipLC = 1;
            LONG xMin = 0;
            LONG xMax = 1;
            LONG yMin = 0;
            LONG yMax = 1;
            bool hasButton1 = false;
            bool hasTipSwitch = false;
            bool hasContactCount = false;
        };

        struct TouchpadRuntimeState
        {
            bool prevPhysBtn = false;
            bool prevTipDown = false;
            bool cornerTapTracking = false;
            ULONGLONG cornerStartTick = 0;
            double cornerStartX = 0.0;
            double cornerStartY = 0.0;
            ULONGLONG lastTriggerTick = 0;
        };

        void SyncDeviceRegistration();
        bool IsSuppressedContext() const noexcept;
        void ProcessMouseRaw(const RAWMOUSE& mouse);
        void ProcessHidRaw(HANDLE hDevice, const RAWHID& hid);
        const TouchpadCaps& GetOrInspectTouchpad(HANDLE hDevice);
        void TriggerHotCorner(const SwitcherConfig& cfg);
        void FlushPendingRawInput();

        HWND m_hwnd = nullptr;
        SwitcherConfig m_config;
        Callbacks m_callbacks;

        bool m_mouseRegistered = false;
        bool m_hidRegistered = false;
        bool m_inFlush = false;

        // Aluminium OS / Magic Pointer 2D mouse shake & swirl state
        double m_segDx = 0.0;
        double m_segDy = 0.0;
        double m_revBackAccum = 0.0;
        ULONGLONG m_segLastTick = 0;
        std::deque<ShakeReversal> m_reversals;
        ULONGLONG m_shakeCooldownUntil = 0;

        // GNOME pressure-barrier hot corner state
        int m_cornerPressure = 0;
        ULONGLONG m_cornerLastPushTick = 0;
        bool m_cornerArmed = true;
        ULONGLONG m_cornerCooldownUntil = 0;

        // Touchpad Raw Input state
        std::unordered_map<HANDLE, TouchpadCaps> m_hidCache;
        TouchpadRuntimeState m_tpState;
        ULONGLONG m_suppressClickUntilTick = 0;
        bool m_physBtnDownNow = false;
        bool m_eatingUpForSuppressedDown = false;
        ULONGLONG m_suppressedDownTick = 0;
    };
} // namespace winrings
