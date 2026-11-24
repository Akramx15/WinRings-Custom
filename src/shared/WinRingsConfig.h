#pragma once

#include <Windows.h>
#include <string>
#include <vector>

#include "ClockFormat.h"

namespace winrings
{
    enum class MouseTriggerButton
    {
        SideBack,
        SideForward,
        Middle,
        RightLeftChord,
        Disabled
    };

    enum class ScrollWheelBehavior
    {
        CycleSlotsOrWindows,
        CycleSubmenuOnly,
        Disabled
    };

    enum class WindowViewMode
    {
        OuterArc,
        TitleColumn
    };

    enum class RingTheme
    {
        A4S,       // "A4S Menu" (Default — live environment-reactive frosted acrylic)
        DarkWood,  // "Dark Wood" (previous matte #202020 look, renamed)
        Blue       // "Blue" (re-added classic #3186FF glass look)
    };

    enum class TopBarBackdrop
    {
        Clear,
        Frosted,
        Native
    };

    enum class SlotKind
    {
        App,
        Website,
        File,
        Folder,
        Script,
        Collection,
        RecycleBin
    };

    enum class SelectionIndicatorStyle
    {
        GlowOnly,
        SectorWedge,
        PointerLine
    };

    enum class HotCornerPos
    {
        TopLeft,
        TopRight
    };

    enum class HotCornerAction
    {
        TaskView,
        OpenRing
    };

    enum class TouchpadCornerPos
    {
        TopLeft,
        TopRight
    };

    enum class MediaIslandPos
    {
        Center,
        Left,
        NearTray
    };

    enum class MediaIslandFullscreenMode
    {
        SyncWithTaskbar,
        GamesOnly,
        NeverHide
    };

    struct PinnedSlotConfig
    {
        std::wstring id;
        SlotKind kind = SlotKind::App;
        std::wstring label;
        std::wstring pathOrUrl;
        std::wstring iconCacheFile;
        int maxFolderItems = 8;
        std::vector<PinnedSlotConfig> items;
    };

    struct SwitcherConfig
    {
        int version = 3;

        // Triggers & Input
        MouseTriggerButton mouseTrigger = MouseTriggerButton::SideBack;
        ScrollWheelBehavior scrollWheelMode = ScrollWheelBehavior::CycleSlotsOrWindows;
        bool doubleTapLastApp = true;
        int tapThresholdMs = 180;
        int doubleTapWindowMs = 280;
        bool hotkeyEnabled = true;
        UINT hotkeyModifiers = MOD_ALT;
        UINT hotkeyVk = VK_SPACE;
        bool menuKeyEnabled = false;
        bool interceptWinTab = false;
        bool interceptAltTab = false;

        // Ring Geometry, Look & Behavior
        double deadzoneDip = 18.0;
        double ringRadiusDip = 115.0;
        double iconSizeDip = 44.0;
        double thumbnailScale = 1.0;
        bool disableOpeningAnimation = true;
        WindowViewMode windowViewMode = WindowViewMode::OuterArc;
        bool fanOutSingleWindow = true;
        RingTheme theme = RingTheme::A4S;
        int glassLevel = 50;
        int lightDarkMode = 0;
        std::wstring accentColor = L"#0078D4";
        bool pointerFollow = true;
        bool shiftCenterOnActiveMonitor = true;
        bool includeUnpinnedRunningApps = true;
        int maxSlots = 14;

        // Unified Minimal Top Bar
        bool topBarEnabled = true;
        bool topBarDockTop = true;
        int topBarHeightDip = 30;
        bool topBarPreserveFontSize = true;
        bool topBarClockSingleLine = true;
        std::wstring topBarClockFormat = L"ddd d MMM  h:mm tt";
        ClockCasing topBarClockCasing = ClockCasing::CompactLower;
        bool topBarHideAppButtons = true;
        bool topBarHideStart = true;
        bool topBarHideSearch = true;
        bool topBarHideTaskView = true;
        bool topBarHideWidgets = true;
        TopBarBackdrop topBarBackdrop = TopBarBackdrop::Clear;
        bool topBarAllMonitors = true;

        // Appearance / Fast mode & Selection Indicator
        bool disableRingBlur = false;
        SelectionIndicatorStyle selectionIndicator = SelectionIndicatorStyle::SectorWedge;

        // Win Key & GNOME Direct Window Switch
        bool winKeyTrigger = true;
        bool directSwitchEnabled = true;
        UINT directSwitchModifiers = MOD_ALT;
        UINT directSwitchVk = 'F';

        // Touchpad Raw Input Triggers
        bool touchpadPhysLeftClickTrigger = true;
        bool touchpadCornerTrigger = false;
        TouchpadCornerPos touchpadCornerPos = TouchpadCornerPos::TopRight;
        int touchpadCornerSizePct = 18;

        // Mouse Shake Trigger (Aluminium OS / Magic Pointer)
        bool shakeTriggerEnabled = true;
        int shakeMinReversals = 4;
        int shakeWindowMs = 400;
        int shakeMinAmplitudeDip = 32;
        bool shakeSuppressFullscreen = true;

        // GNOME Pressure Hot Corner
        bool hotCornerEnabled = true;
        HotCornerPos hotCornerPos = HotCornerPos::TopLeft;
        HotCornerAction hotCornerAction = HotCornerAction::TaskView;
        int hotCornerPressureThreshold = 140;

        // Integrated FullWorkArea (Windows 11 26H2+)
        bool fullWorkAreaEnabled = true;
        bool fullWorkAreaPillClip = true;
        bool fullWorkAreaHotkeys = true;

        // Dynamic Island Media Pill
        bool mediaIslandEnabled = true;
        MediaIslandPos mediaIslandPos = MediaIslandPos::Left;
        bool mediaIslandHoverExpand = false;
        bool mediaIslandHideWhenPaused = false;
        bool mediaIslandScrollVolume = true;
        bool mediaIslandMarquee = true;
        MediaIslandFullscreenMode mediaIslandFullscreenMode = MediaIslandFullscreenMode::SyncWithTaskbar;
        int mediaIslandOffsetXPct = -1;

        // Drag-to-Snap / Split-Screen (from Ring & Window Cards)
        bool dragToSnapEnabled = true;
        bool dragToSnapLaunchClosed = true;
        int dragToSnapHoldMs = 150;
        int dragToSnapTearOffDip = 36;

        std::vector<PinnedSlotConfig> pinnedSlots;

        static std::wstring AppDataDirectory();
        static std::wstring ConfigPath();
        static SwitcherConfig CreateDefault();
        static SwitcherConfig LoadOrCreate();
        static bool LoadFromFile(const std::wstring& path, SwitcherConfig& outConfig);
        static bool ParseJsonUtf8(const std::string& jsonUtf8, SwitcherConfig& outConfig);

        std::string ToJsonUtf8() const;
        bool Save() const;
        bool SaveToFile(const std::wstring& path) const;
    };

    std::wstring Utf8ToWide(const std::string& utf8);
    std::string WideToUtf8(const std::wstring& wide);
    const wchar_t* ToString(MouseTriggerButton v) noexcept;
    const wchar_t* ToString(ScrollWheelBehavior v) noexcept;
    const wchar_t* ToString(WindowViewMode v) noexcept;
    const wchar_t* ToString(RingTheme v) noexcept;
    const wchar_t* ToString(TopBarBackdrop v) noexcept;
    const wchar_t* ToString(ClockCasing v) noexcept;
    const wchar_t* ToString(SlotKind v) noexcept;
    const wchar_t* ToString(SelectionIndicatorStyle v) noexcept;
    const wchar_t* ToString(HotCornerPos v) noexcept;
    const wchar_t* ToString(HotCornerAction v) noexcept;
    const wchar_t* ToString(TouchpadCornerPos v) noexcept;
    const wchar_t* ToString(MediaIslandPos v) noexcept;
    const wchar_t* ToString(MediaIslandFullscreenMode v) noexcept;
} // namespace winrings
