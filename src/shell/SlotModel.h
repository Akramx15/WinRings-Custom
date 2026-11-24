#pragma once

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <functional>
#include <string>
#include <vector>

#include "../shared/SlotMatching.h"
#include "../shared/WinRingsConfig.h"
#include "WindowTracker.h"

namespace winrings
{
    class AppIndex;

    struct RingSlot
    {
        std::wstring id;
        SlotKind kind = SlotKind::App;
        std::wstring label;
        std::wstring pathOrUrl;
        Microsoft::WRL::ComPtr<IWICBitmapSource> iconBitmap;
        bool isPinned = false;
        bool isFrontmost = false;
        int maxFolderItems = 8;
        std::vector<WindowEntry> windows;
        std::vector<RingSlot> subItems;
        std::vector<std::wstring> searchAliases;

        bool IsRunning() const noexcept { return !windows.empty(); }
        bool ShowsWindowPreview(bool fanOutSingleWindow) const noexcept
        {
            return ShouldShowSlotWindowPreview(kind, windows.size(), fanOutSingleWindow);
        }
        bool HasSubmenu(bool fanOutSingleWindow) const noexcept
        {
            return ShowsWindowPreview(fanOutSingleWindow) || kind == SlotKind::Folder || kind == SlotKind::Collection;
        }
    };

    class SlotModel
    {
    public:
        static std::vector<RingSlot> BuildRingSlots(
            HWND ignoreHwnd,
            const SwitcherConfig& config,
            WindowTracker& tracker,
            IWICImagingFactory* wicFactory,
            const AppIndex* appIndex = nullptr);

        static bool ExplorerWindowMatchesFolder(const std::wstring& windowTitle, const std::wstring& expandedFolderPath);
        static bool ExplorerWindowMatchesRecycleBin(const std::wstring& windowTitle, const std::wstring& localizedRecycleBinName = L"Recycle Bin");

        static bool ResolveShortcutTarget(
            const std::wstring& lnkPath,
            std::wstring& outTargetPath,
            std::wstring& outArguments,
            std::wstring& outWorkingDir);
        static void LaunchPathOrUrl(const std::wstring& pathOrUrl, const std::wstring& arguments = L"");
        static void LaunchRunAsAdmin(const std::wstring& pathOrUrl);
        static void RevealInExplorer(const std::wstring& path);
        static bool SendToRecycleBin(const std::vector<std::wstring>& paths);
        static bool CopyFilesToFolder(const std::vector<std::wstring>& paths, const std::wstring& destFolder);
        static bool EmptyRecycleBin(HWND ownerHwnd);
        static std::wstring ExpandEnv(const std::wstring& input);

        // Cookie-free background favicon fetch -> writes under %APPDATA%\WinRings\icons\ and saves relative path.
        static void FetchFaviconAsync(
            const std::wstring& url,
            std::function<void(std::wstring relativeIconPath)> onCompleted);
    };
} // namespace winrings
