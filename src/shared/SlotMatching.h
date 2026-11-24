#pragma once

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cwchar>
#include <string>
#include <vector>

#include "RadialMath.h"
#include "SearchMatch.h"
#include "WinRingsConfig.h"

namespace winrings
{
    inline bool IsExplorerExecutablePath(const std::wstring& path) noexcept
    {
        if (path.empty())
            return false;
        const size_t slash = path.find_last_of(L"\\/");
        const wchar_t* name = (slash != std::wstring::npos) ? (path.c_str() + slash + 1) : path.c_str();
        return _wcsicmp(name, L"explorer.exe") == 0;
    }

    inline bool ShouldShowSlotWindowPreview(SlotKind kind, size_t windowCount, bool fanOutSingleWindow) noexcept
    {
        if (kind != SlotKind::App)
            return false;
        const size_t minWin = fanOutSingleWindow ? 1 : 2;
        return windowCount >= minWin;
    }

    inline std::wstring StripExplorerTitleSuffix(const std::wstring& title)
    {
        std::wstring s = TrimCopyW(title);
        constexpr wchar_t kSuffix[] = L" - File Explorer";
        constexpr size_t kSuffixLen = _countof(kSuffix) - 1;
        if (s.size() > kSuffixLen &&
            _wcsicmp(s.c_str() + (s.size() - kSuffixLen), kSuffix) == 0)
        {
            s.resize(s.size() - kSuffixLen);
            s = TrimCopyW(s);
        }
        return s;
    }

    inline std::wstring NormalizeWindowsSlashes(std::wstring path)
    {
        for (auto& ch : path)
        {
            if (ch == L'/')
                ch = L'\\';
        }
        path = TrimCopyW(path);
        while (path.size() > 3 && path.back() == L'\\')
            path.pop_back();
        if (path.size() > 1 && path.back() == L'\\' && !(path.size() == 3 && path[1] == L':'))
            path.pop_back();
        return path;
    }

    inline bool ExplorerWindowMatchesFolder(const std::wstring& windowTitle, const std::wstring& expandedFolderPath)
    {
        const std::wstring stripped = StripExplorerTitleSuffix(windowTitle);
        if (stripped.empty() || expandedFolderPath.empty())
            return false;

        const std::wstring normPath = NormalizeWindowsSlashes(expandedFolderPath);
        if (normPath.empty())
            return false;

        const std::wstring normTitle = NormalizeWindowsSlashes(stripped);
        if (_wcsicmp(normTitle.c_str(), normPath.c_str()) == 0)
            return true;

        std::wstring trimmed = normPath;
        while (!trimmed.empty() && trimmed.back() == L'\\')
            trimmed.pop_back();
        if (trimmed.empty() || (trimmed.size() == 2 && trimmed[1] == L':'))
            return false;

        const size_t slash = trimmed.find_last_of(L'\\');
        const std::wstring leaf = (slash != std::wstring::npos) ? trimmed.substr(slash + 1) : trimmed;
        return !leaf.empty() && _wcsicmp(stripped.c_str(), leaf.c_str()) == 0;
    }

    inline bool ExplorerWindowMatchesRecycleBin(
        const std::wstring& windowTitle,
        const std::wstring& localizedRecycleBinName = L"Recycle Bin")
    {
        const std::wstring stripped = StripExplorerTitleSuffix(windowTitle);
        if (stripped.empty())
            return false;
        if (_wcsicmp(stripped.c_str(), L"Recycle Bin") == 0)
            return true;
        return !localizedRecycleBinName.empty() &&
            _wcsicmp(stripped.c_str(), localizedRecycleBinName.c_str()) == 0;
    }

    struct SpringHoverEval
    {
        double scaleMultiplier = 1.0;
        bool pulsing = false;
        bool shouldActivate = false;
    };

    inline SpringHoverEval EvaluateSpringHover(
        double elapsedMs,
        double dwellMs = 500.0,
        double pulseDurationMs = 420.0) noexcept
    {
        if (elapsedMs < dwellMs)
            return {};

        const double dur = (pulseDurationMs > 1.0) ? pulseDurationMs : 1.0;
        const double t = (elapsedMs - dwellMs) / dur;
        if (t >= 1.0)
            return { 1.0, false, true };

        const double beatPos = t * 2.0;
        const double u = beatPos - std::floor(beatPos);
        double scaleMultiplier = 1.0;
        if (u < 0.5)
            scaleMultiplier = 1.0 + 0.24 * std::sin(u * 2.0 * kPi);
        else
            scaleMultiplier = 1.0 - 0.14 * std::sin((u - 0.5) * 2.0 * kPi);
        return { scaleMultiplier, true, false };
    }

    inline int ComputeReorderTargetSlotIndex(double dx, double dy, int pinnedCount, int totalSlotCount) noexcept
    {
        if (pinnedCount <= 1 || totalSlotCount <= 0)
            return 0;
        const int idx = CalculateSectorIndex(dx, dy, totalSlotCount, 0.0);
        if (idx < 0)
            return 0;
        return (std::clamp)(idx, 0, pinnedCount - 1);
    }

    template <typename T>
    inline void ReorderVectorElement(std::vector<T>& items, size_t fromIdx, size_t toIdx)
    {
        if (fromIdx >= items.size() || toIdx >= items.size() || fromIdx == toIdx)
            return;
        if (fromIdx < toIdx)
        {
            std::rotate(
                items.begin() + static_cast<std::ptrdiff_t>(fromIdx),
                items.begin() + static_cast<std::ptrdiff_t>(fromIdx + 1),
                items.begin() + static_cast<std::ptrdiff_t>(toIdx + 1));
        }
        else
        {
            std::rotate(
                items.begin() + static_cast<std::ptrdiff_t>(toIdx),
                items.begin() + static_cast<std::ptrdiff_t>(fromIdx),
                items.begin() + static_cast<std::ptrdiff_t>(fromIdx + 1));
        }
    }

    inline bool HasSuffixInsensitiveW(const std::wstring& s, const wchar_t* suffix) noexcept
    {
        if (!suffix)
            return false;
        const size_t slen = wcslen(suffix);
        if (s.size() < slen)
            return false;
        return _wcsicmp(s.c_str() + (s.size() - slen), suffix) == 0;
    }

    inline std::wstring ChooseSearchPinPath(
        const std::wstring& rawPath,
        const std::wstring& resolvedTarget,
        const std::wstring& resolvedArgs)
    {
        if (HasSuffixInsensitiveW(rawPath, L".lnk") &&
            !resolvedTarget.empty() &&
            resolvedArgs.empty() &&
            HasSuffixInsensitiveW(resolvedTarget, L".exe"))
        {
            return resolvedTarget;
        }
        return rawPath;
    }

    inline int FindPinnedSlotIndexByPath(
        const std::vector<PinnedSlotConfig>& pinnedSlots,
        const std::wstring& primaryPath,
        const std::wstring& altPath = L"")
    {
        const std::wstring normPrimary = NormalizeWindowsSlashes(primaryPath);
        const std::wstring normAlt = NormalizeWindowsSlashes(altPath);
        if (normPrimary.empty() && normAlt.empty())
            return -1;

        for (size_t i = 0; i < pinnedSlots.size(); ++i)
        {
            const auto& slot = pinnedSlots[i];
            if (slot.kind != SlotKind::App || slot.pathOrUrl.empty())
                continue;
            const std::wstring normSlot = NormalizeWindowsSlashes(slot.pathOrUrl);
            if ((!normPrimary.empty() && _wcsicmp(normSlot.c_str(), normPrimary.c_str()) == 0) ||
                (!normAlt.empty() && _wcsicmp(normSlot.c_str(), normAlt.c_str()) == 0))
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    struct TopBarPosClampResult
    {
        bool modifiedMove = false;
        bool modifiedSize = false;
        int y = 0;
        int cy = 0;
    };

    inline TopBarPosClampResult ClampTopDockedTaskbarWindowPos(
        bool lockTop,
        int monTop,
        int monBottom,
        int targetHeightPx,
        int maxClampHeightPx,
        bool hasMove,
        bool hasSize,
        int reqY,
        int reqCy,
        int curTop,
        int curCy) noexcept
    {
        TopBarPosClampResult res{};
        res.y = reqY;
        res.cy = reqCy;
        if (!lockTop)
            return res;

        const int effectiveY = hasMove ? reqY : curTop;
        const int effectiveCy = hasSize ? reqCy : curCy;

        // Clamp bottom-edge drops back to monTop while leaving top auto-hide (y < monTop) untouched.
        if (effectiveY > monTop + 4 && effectiveY < monBottom)
        {
            res.y = monTop;
            res.modifiedMove = true;
        }

        // Clamp transient full-height resets (e.g. 48 DIP on wake) without disturbing auto-hide slivers (<= 4 px).
        if (targetHeightPx >= 24 && effectiveCy > targetHeightPx + 2 && effectiveCy <= maxClampHeightPx)
        {
            res.cy = targetHeightPx;
            res.modifiedSize = true;
        }

        return res;
    }
} // namespace winrings
