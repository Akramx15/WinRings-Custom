#pragma once

#include <Windows.h>
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
            std::function<bool(POINT screenPt)> isPointOverMediaIsland;
            std::function<void(short wheelDelta)> onMediaIslandTouchpadWheel;
        };

        GestureEngine() = default;
        ~GestureEngine();

        GestureEngine(const GestureEngine&) = delete;
        GestureEngine& operator=(const GestureEngine&) = delete;

        bool Initialize(HWND hwnd, const SwitcherConfig& cfg, Callbacks callbacks);
        void UpdateConfig(const SwitcherConfig& cfg);
        void Shutdown();
        void EnsureDeviceRegistration(bool forceReRegister = false);

        void OnRawInput(LPARAM lParam);
        bool ShouldSuppressMouseButton(UINT msg);

    private:
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
            bool hasContactId = false;
        };

        struct TouchpadRuntimeState
        {
            bool prevPhysBtn = false;
            bool prevTipDown = false;
            bool cornerTapTracking = false;
            ULONGLONG lastHidTick = 0;
            ULONGLONG cornerStartTick = 0;
            double cornerStartX = 0.0;
            double cornerStartY = 0.0;
            ULONGLONG lastTriggerTick = 0;
            ULONG activeFrameContacts = 0;
            ULONG remainingContinuationReports = 0;

            bool twoFingerScrollActive = false;
            bool twoFingerOverIsland = false;
            double twoFingerLastX = 0.0;
            double twoFingerLastY = 0.0;
            double twoFingerAccumY = 0.0;
            ULONGLONG twoFingerLastTick = 0;
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
