#pragma once

#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>
#include <unordered_set>

#include "../shared/RadialMath.h"
#include "../shared/WinRingsConfig.h"
#include "WindowTracker.h"

namespace winrings
{
    class WindowSnapCtrl
    {
    public:
        WindowSnapCtrl() = default;
        ~WindowSnapCtrl();

        WindowSnapCtrl(const WindowSnapCtrl&) = delete;
        WindowSnapCtrl& operator=(const WindowSnapCtrl&) = delete;

        bool Initialize(HMODULE shellModule);
        void Shutdown();
        void UpdateConfig(const SwitcherConfig& cfg) noexcept { m_useSnapAssist = cfg.dragToSnapUseSnapAssist; }

        void BeginOpenWindowDrag(HWND targetHwnd, POINT cursorPt);
        void BeginClosedAppDrag(SlotKind kind, std::wstring label, std::wstring pathOrUrl, POINT cursorPt);
        void OnWindowCreatedOrActivated(HWND hwnd);

        bool IsFallbackDragActive() const noexcept { return m_fallbackActive; }
        void OnFallbackDragMove(POINT screenPt);
        bool OnFallbackDragRelease(POINT screenPt);
        void CancelActiveDrag();

    private:
        struct PendingLaunchSnap
        {
            bool active = false;
            SlotKind kind = SlotKind::App;
            std::wstring expectedAppKey;
            bool matchExactExeKey = false;
            SnapZone zone = SnapZone::None;
            HMONITOR targetMon = nullptr;
            ULONGLONG startTick = 0;
            ULONGLONG deadlineTick = 0;
            std::unordered_set<HWND> preExistingHwnds;
        };

        struct PendingRelease
        {
            bool active = false;
            bool closedMode = false;
            HWND targetHwnd = nullptr;
            SlotKind kind = SlotKind::App;
            std::wstring label;
            std::wstring pathOrUrl;
            POINT screenPt{};
        };

        static LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept;
        LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

        bool EnsureRenderFactories();
        void ReleaseRenderFactories();

        SnapZone HitTestSnapZone(POINT screenPt, HMONITOR& outMon, double& outDpiScale) const noexcept;
        void UpdatePreviewForPoint(POINT screenPt);
        void ProcessLiveDragMove(POINT screenPt);
        void MoveWindowToCursor(HWND target, POINT screenPt) noexcept;
        void CompletePendingRelease();
        void RenderZonePreview(HMONITOR mon, double dpiScale, SnapZone zone, const std::wstring& badgeText);
        void HidePreviewWindow() noexcept;

        static void BringWindowForeground(HWND target) noexcept;
        void ApplySnap(HWND target, SnapZone zone, HMONITOR targetMon) noexcept;
        void ApplyDirectSnap(HWND target, SnapZone zone, HMONITOR targetMon) noexcept;
        void ApplyNativeSnap(HWND target, SnapZone zone, HMONITOR targetMon) noexcept;
        void ClearPendingLaunch() noexcept;

        HMODULE m_module = nullptr;
        HWND m_previewHwnd = nullptr;
        bool m_classRegistered = false;
        bool m_initialized = false;
        bool m_useSnapAssist = false;

        // Live drag + edge-snap ghost preview state
        bool m_fallbackActive = false;
        bool m_fallbackIsClosedApp = false;
        HWND m_fallbackTargetHwnd = nullptr;
        SlotKind m_fallbackKind = SlotKind::App;
        std::wstring m_fallbackLabel;
        std::wstring m_fallbackPathOrUrl;
        SnapZone m_previewZone = SnapZone::None;
        HMONITOR m_previewMon = nullptr;

        int m_dragGrabOffsetX = 0;
        int m_dragGrabOffsetY = 0;
        int m_dragWinW = 0;
        int m_dragWinH = 0;
        POINT m_pendingMovePt{};
        POINT m_lastAppliedMovePt{};
        bool m_hasAppliedMovePt = false;
        ULONGLONG m_lastWindowMoveTick = 0;
        bool m_movePosted = false;
        bool m_moveFlushPending = false;
        PendingRelease m_pendingRelease;

        // Pending post-launch snap state
        PendingLaunchSnap m_pendingLaunch;
        HWND m_settleHwnd = nullptr;
        SnapZone m_settleZone = SnapZone::None;
        HMONITOR m_settleMonitor = nullptr;

        Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
        Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> m_dcTarget;
        Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_badgeFmt;
    };
} // namespace winrings
