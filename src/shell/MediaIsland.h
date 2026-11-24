#pragma once

#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "../shared/WinRingsConfig.h"

namespace winrings
{
    class MediaIsland
    {
    public:
        struct Callbacks
        {
            std::function<int()> getSystemTrayWidthDip;
            std::function<void(int offsetXPct)> onSaveOffsetXPct;
            std::function<void()> onToggleEnabled;
            std::function<void()> onOpenSettings;
        };

        MediaIsland() = default;
        ~MediaIsland();

        MediaIsland(const MediaIsland&) = delete;
        MediaIsland& operator=(const MediaIsland&) = delete;

        bool Initialize(HMODULE shellModule, const SwitcherConfig& cfg, Callbacks callbacks = {});
        void UpdateConfig(const SwitcherConfig& cfg);
        void Shutdown();

        void OnTaskbarVisibilityChanged(bool taskbarHidden);
        void OnForegroundChanged();
        void RefreshLayoutAndVisibility();
        void NotifyLayoutRefresh() noexcept;
        bool IsActiveVisible() const noexcept;
        bool HitTestScreenPoint(POINT screenPt) const noexcept;
        void PostExternalWheel(short wheelDelta) noexcept;
        void PostTouchpadWheel(short wheelDelta) noexcept;

    private:
        enum class TransportCmd : int
        {
            None = 0,
            Previous = 1,
            TogglePlayPause = 2,
            Next = 3
        };

        struct MediaSnapshot
        {
            bool hasSession = false;
            bool isPlaying = false;
            std::wstring title;
            std::wstring artist;
            uint64_t thumbGen = 0;
            int thumbW = 0;
            int thumbH = 0;
            std::vector<BYTE> thumbBgra;
            D2D1_COLOR_F accent{ 0.35f, 0.56f, 0.96f, 1.0f };
        };

        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept;
        LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

        static DWORD WINAPI WorkerThreadProc(void* param);
        void WorkerLoop();

        bool CreateRenderFactories();
        void ReleaseRenderFactories();

        bool ShouldBeVisible(const MediaSnapshot& snap) const noexcept;
        bool ShouldHideForFullscreen() const noexcept;
        void ShowIslandContextMenu(POINT screenPt);
        void UpdateWindowBoundsAndRender();
        void Render(const MediaSnapshot& snap, int widthPx, int heightPx, int dstX, int dstY);
        void EnsureThumbBitmap(const MediaSnapshot& snap);

        int HitTestControl(POINT clientPt) const noexcept;
        void SendTransportCommand(TransportCmd cmd);
        void AdjustMasterVolume(int wheelDelta, const wchar_t* source, bool fromTouchpadHid);

        HMODULE m_module = nullptr;
        HWND m_hwnd = nullptr;
        bool m_classRegistered = false;
        SwitcherConfig m_config;
        Callbacks m_callbacks;

        bool m_taskbarHidden = false;
        bool m_expanded = false;
        bool m_mouseTracked = false;
        int m_hoveredBtn = -1; // -1=none, 0=prev, 1=play, 2=next

        bool m_showVolumeOverlay = false;
        int m_lastVolumePct = 0;
        int m_wheelAccum = 0;
        ULONGLONG m_wheelLastTick = 0;
        ULONGLONG m_lastRealWheelTick = 0;
        ULONGLONG m_lastWheelLogTick = 0;
        LONG m_cachedBarTopPx = 0;
        LONG m_cachedBarBottomPx = 0;
        bool m_draggingPos = false;
        int m_dragStartCursorX = 0;
        int m_dragStartWinX = 0;
        int m_dragCurrentX = 0;
        bool m_fullscreenTimerRunning = false;
        bool m_lastFullscreenHide = false;
        double m_dpiScale = 1.0;

        float m_marqueeOffset = 0.0f;
        float m_marqueeMaxOffset = 0.0f;
        int m_marqueePauseTicks = 0;
        int m_marqueePhase = 0; // 0=pause start, 1=scrolling, 2=pause end
        bool m_marqueeTimerRunning = false;
        std::wstring m_marqueeLastTitle;

        D2D1_RECT_F m_btnPrevDip{};
        D2D1_RECT_F m_btnPlayDip{};
        D2D1_RECT_F m_btnNextDip{};

        Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
        Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> m_dcTarget;
        Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_titleFmt;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_artistFmt;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_pillFmt;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_smallFmt;
        Microsoft::WRL::ComPtr<ID2D1Bitmap> m_thumbBitmap;
        uint64_t m_cachedThumbGen = 0;

        HANDLE m_workerThread = nullptr;
        HANDLE m_stopEvent = nullptr;
        HANDLE m_wakeEvent = nullptr;
        std::atomic<bool> m_stopping{ false };
        std::atomic<int> m_pendingCmd{ 0 };

        mutable std::mutex m_stateMutex;
        MediaSnapshot m_snapshot;
    };
} // namespace winrings
