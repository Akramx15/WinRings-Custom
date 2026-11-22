#pragma once

#include <Windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../shared/RadialMath.h"
#include "../shared/WinRingsConfig.h"
#include "AcrylicBackdrop.h"
#include "DropTarget.h"
#include "DwmThumbPool.h"
#include "SlotModel.h"
#include "WindowTracker.h"

namespace winrings
{
    class AppIndex;

    class RingOverlay
    {
    public:
        struct Callbacks
        {
            std::function<void(const std::wstring& exeArgs)> launchTools;
            std::function<void(const SwitcherConfig& updatedConfig)> saveAndReloadConfig;
            std::function<void()> onToggleTopBarRequested;
            std::function<void()> onToggleMediaIslandRequested;
            std::function<void()> onExitRequested;
            std::function<void()> onOverlayClosed;
        };

        RingOverlay() = default;
        ~RingOverlay();

        RingOverlay(const RingOverlay&) = delete;
        RingOverlay& operator=(const RingOverlay&) = delete;

        bool Initialize(HINSTANCE instance, HMODULE shellModule, WindowTracker* tracker, Callbacks callbacks,
            AppIndex* appIndex = nullptr);
        void Shutdown();

        void SetConfig(const SwitcherConfig& config);
        const SwitcherConfig& Config() const noexcept { return m_config; }

        HWND Hwnd() const noexcept { return m_hwnd; }
        bool IsOpen() const noexcept { return m_isOpen; }
        bool IsLockedOpen() const noexcept { return m_isLockedOpen; }
        bool IsDragOrReorderActive() const noexcept { return m_dragActive || m_draggingOutFromRing || m_reorderActive; }
        double DpiScale() const noexcept { return m_dpiScale; }
        bool IsPointInActiveBounds(POINT screenPt) const noexcept;

        void OpenAt(int physicalX, int physicalY, std::vector<RingSlot> slots, bool lockOpen);
        void LockOpen() noexcept { m_isLockedOpen = true; }
        void UpdatePointer(int physicalX, int physicalY);
        void HandleScrollWheel(int wheelDelta);
        void CommitSelectionAndClose();
        void HandleEscapeKey();
        void Dismiss();

        void AppendFilterChar(wchar_t ch);
        void BackspaceFilter();

    private:
        struct SubmenuCard
        {
            RectD boundsDip;
            const WindowEntry* window = nullptr;
            const RingSlot* subSlot = nullptr;
            double currentScale = 1.0;
            double targetScale = 1.0;
        };

        enum class PressKind
        {
            None,
            CenterDeadzone,
            RingSlot,
            SubmenuCard
        };

        struct RankedMatch
        {
            bool isRingSlot = true;
            int index = -1;
            int tier = 99;
            size_t nameLen = 0;
        };

        using ThemeColors = RingMaterialSpec;

        static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
        static LRESULT CALLBACK WcaBackdropWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
        LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

        bool CreateWcaBackdropWindow();
        void DestroyWcaBackdropWindow();
        void UpdateWcaMaterial(const ThemeColors& tc);
        void UpdateWcaRegion(float dpiScale, float openScale, float outerR, float holeR,
            bool hasCurvedArc, bool hasQuery, const D2D1_ROUNDED_RECT& searchPill,
            bool hasSummary, const D2D1_ROUNDED_RECT& centerPill);

        bool CreateGraphicsDevices();
        void ReleaseGraphicsDevices();
        bool EnsureSwapChainSize(int widthPx, int heightPx);
        void EnsureNoiseBrush();
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> GetOrCreateD2DBitmap(IWICBitmapSource* wicSource);

        RingGeometry ActiveGeometry() const noexcept;
        void UpdateDpiAndPosition(int physicalX, int physicalY);
        AcrylicMaterialParams BuildAcrylicMaterial() const noexcept;
        ThemeColors BuildThemeColors() const noexcept;
        void RebuildLayoutAfterFilter();
        bool SlotHasSubmenuItems(const RingSlot& slot) const noexcept;
        void OpenSubmenuForSlot(int slotIndex, PointD originDip);
        void ClearSubmenu();
        int HitTestSubmenu(PointD localDip) const noexcept;
        D2D1_RECT_F GetSubmenuCloseRectDip(const D2D1_RECT_F& cardRectDip, WindowViewMode mode) const noexcept;
        int HitTestSubmenuCloseButton(PointD localDip) const noexcept;
        void SyncDwmThumbnails();
        void StepAnimations();
        Microsoft::WRL::ComPtr<ID2D1Geometry> BuildAnnulusGeometry(float outerR, float holeR) const;
        Microsoft::WRL::ComPtr<ID2D1Geometry> BuildCurvedArcGeometry(const IconArcLayout& arc) const;
        void DrawCurvedArcTrack(ID2D1Geometry* arcGeo, const IconArcLayout& arc, const ThemeColors& tc, bool paintFill, bool paintNoise);
        void Render();
        void ActivateSlotOrWindow(const RingSlot* slot, const WindowEntry* win, bool shiftHeld);
        void ShowContextMenuAt(POINT screenPt);
        std::wstring BuildFilterSummary() const;

        int PinnedVisibleSlotCount() const noexcept;
        void ResetDragSpringState() noexcept;
        void CancelPendingPressAndReorder(bool revertVisualOrder);
        void EnsureAnimTimerRunning();
        bool CanSlotSpringLoad(const RingSlot& slot) const noexcept;
        void TriggerSpringLoadForSlot(int slotIndex);

        void OnDragEnterOverlay();
        void OnDragOverOverlay(POINT screenPt);
        void OnDragLeaveOverlay();
        void OnDropOverlay(POINT screenPt, std::vector<std::wstring> files, std::wstring text);

        HINSTANCE m_instance = nullptr;
        HMODULE m_shellModule = nullptr;
        WindowTracker* m_tracker = nullptr;
        AppIndex* m_appIndex = nullptr;
        Callbacks m_callbacks;
        SwitcherConfig m_config;

        HWND m_hwnd = nullptr;
        HWND m_wcaBackdropHwnd = nullptr;
        bool m_classRegistered = false;
        bool m_wcaClassRegistered = false;
        bool m_bufferedPaintInit = false;
        bool m_wcaBackdropActive = false;
        int m_lastWcaState = -1;
        DWORD m_lastWcaGradColor = 0;
        int m_lastWcaDark = -1;
        uint64_t m_lastWcaRegionHash = 0;
        RingDropTarget* m_dropTarget = nullptr;
        DwmThumbPool m_thumbPool;

        // Direct3D / Direct2D / DirectComposition
        Microsoft::WRL::ComPtr<ID3D11Device> m_d3dDevice;
        Microsoft::WRL::ComPtr<IDXGIDevice> m_dxgiDevice;
        Microsoft::WRL::ComPtr<ID2D1Factory1> m_d2dFactory;
        Microsoft::WRL::ComPtr<ID2D1Device> m_d2dDevice;
        Microsoft::WRL::ComPtr<ID2D1DeviceContext> m_d2dContext;
        Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_titleFormat;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_smallFormat;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_searchFormat;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_glyphFormat;
        Microsoft::WRL::ComPtr<IDCompositionDevice> m_dcompDevice;
        Microsoft::WRL::ComPtr<IDCompositionTarget> m_dcompTarget;
        Microsoft::WRL::ComPtr<IDCompositionVisual> m_dcompVisual;
        Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swapChain;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_targetBitmap;
        Microsoft::WRL::ComPtr<IDXGISwapChain1> m_maskSwapChain;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_maskTargetBitmap;
        AcrylicBackdrop m_acrylicBackdrop;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_noiseBitmap;
        Microsoft::WRL::ComPtr<ID2D1BitmapBrush> m_noiseBrush;
        int m_swapWidthPx = 0;
        int m_swapHeightPx = 0;

        std::unordered_map<IWICBitmapSource*, Microsoft::WRL::ComPtr<ID2D1Bitmap1>> m_d2dBitmapCache;

        // Runtime ring state
        std::vector<RingSlot> m_allSlots;
        std::vector<RingSlot> m_visibleSlots;
        std::vector<bool> m_slotSearchMatched;
        std::vector<double> m_slotScales;
        std::vector<RingSlot> m_searchResultSlots;
        std::vector<RankedMatch> m_rankedMatches;
        int m_rankedMatchCursor = -1;
        bool m_searchArcActive = false;
        bool m_submenuHasArcTrack = false;
        IconArcLayout m_submenuArcLayout{};
        std::vector<SubmenuCard> m_submenuCards;
        std::vector<RectD> m_submenuCardRects;

        double m_dpiScale = 1.0;
        PointD m_centerScreenPhysical{};
        int m_selectedIndex = -1;
        int m_expandedSlotIndex = -1;
        int m_selectedSubIndex = -1;
        int m_hoveredCloseSubIndex = -1;
        RectD m_submenuBoundsDip{};
        PointD m_corridorOriginDip{};
        bool m_submenuIsTitleColumn = false;
        bool m_hasLastPointer = false;
        POINT m_lastPointerPhysical{};
        bool m_searchMouseLocked = false;
        PointD m_searchMouseAnchorDip{};
        std::wstring m_filterQuery;

        bool m_isOpen = false;
        bool m_isLockedOpen = false;
        bool m_contextMenuOpen = false;
        bool m_dragActive = false;
        bool m_dragOverCenter = false;
        bool m_draggingOutFromRing = false;
        PressKind m_pressKind = PressKind::None;
        int m_pressSlotIndex = -1;
        int m_pressSubIndex = -1;
        POINT m_pressScreenPt{};
        bool m_reorderActive = false;
        int m_reorderFromIndex = -1;
        int m_reorderCurrentIndex = -1;
        PointD m_reorderCursorDip{};
        int m_springSlotIndex = -1;
        LONGLONG m_springHoverStartQpc = 0;
        bool m_springTriggered = false;
        double m_springPulseScale = 1.0;
        double m_openProgress = 1.0;
        LONGLONG m_openStartQpc = 0;
        LONGLONG m_qpcFreq = 1;
        bool m_animTimerRunning = false;
    };
} // namespace winrings
