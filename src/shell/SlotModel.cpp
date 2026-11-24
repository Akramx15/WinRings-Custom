#include "SlotModel.h"

#include <ShlObj.h>
#include <ShObjIdl.h>
#include <Shellapi.h>
#include <winhttp.h>
#include <algorithm>
#include <cwctype>
#include <thread>
#include <unordered_set>

#include "../shared/SearchMatch.h"
#include "AppIndex.h"

namespace winrings
{
    namespace
    {
        std::wstring FileStemOfPath(const std::wstring& path)
        {
            const size_t slash = path.find_last_of(L"\\/");
            std::wstring name = (slash != std::wstring::npos) ? path.substr(slash + 1) : path;
            const size_t dot = name.find_last_of(L'.');
            if (dot != std::wstring::npos && dot > 0)
                name.resize(dot);
            return name;
        }

        std::wstring ParentFolderNameOf(const std::wstring& path)
        {
            const size_t slash = path.find_last_of(L"\\/");
            if (slash == std::wstring::npos || slash == 0)
                return {};
            const size_t prevSlash = path.find_last_of(L"\\/", slash - 1);
            if (prevSlash == std::wstring::npos)
                return {};
            std::wstring folder = path.substr(prevSlash + 1, slash - prevSlash - 1);
            if (_wcsicmp(folder.c_str(), L"Application") == 0 ||
                _wcsicmp(folder.c_str(), L"bin") == 0 ||
                _wcsicmp(folder.c_str(), L"x64") == 0 ||
                _wcsicmp(folder.c_str(), L"win64") == 0 ||
                _wcsicmp(folder.c_str(), L"Program Files") == 0 ||
                _wcsicmp(folder.c_str(), L"Program Files (x86)") == 0 ||
                _wcsicmp(folder.c_str(), L"System32") == 0 ||
                _wcsicmp(folder.c_str(), L"Windows") == 0)
            {
                return ParentFolderNameOf(path.substr(0, slash));
            }
            return folder;
        }

        void AddAliasUnique(std::vector<std::wstring>& aliases, const std::wstring& value)
        {
            const std::wstring t = TrimCopyW(value);
            if (t.empty())
                return;
            const std::wstring low = ToLowerFold(t);
            for (const auto& existing : aliases)
            {
                if (ToLowerFold(existing) == low)
                    return;
            }
            aliases.push_back(t);
        }

        bool EndsWithInsensitiveW(const std::wstring& s, const wchar_t* suffix) noexcept
        {
            const size_t slen = wcslen(suffix);
            if (s.size() < slen)
                return false;
            return _wcsicmp(s.c_str() + (s.size() - slen), suffix) == 0;
        }

        bool ResolveKnownFolderAumidPath(const std::wstring& aumid, std::wstring& outPath)
        {
            outPath.clear();
            if (aumid.size() >= 38 && aumid[0] == L'{')
            {
                const size_t closeBrace = aumid.find(L'}');
                if (closeBrace != std::wstring::npos && closeBrace < 64)
                {
                    const std::wstring guidText = aumid.substr(0, closeBrace + 1);
                    KNOWNFOLDERID folderId{};
                    if (SUCCEEDED(CLSIDFromString(guidText.c_str(), &folderId)))
                    {
                        PWSTR folderPath = nullptr;
                        if (SUCCEEDED(SHGetKnownFolderPath(folderId, 0, nullptr, &folderPath)) && folderPath)
                        {
                            std::wstring rel = aumid.substr(closeBrace + 1);
                            if (!rel.empty() && (rel[0] == L'\\' || rel[0] == L'/'))
                                rel.erase(rel.begin());
                            std::wstring candidate = folderPath;
                            CoTaskMemFree(folderPath);
                            if (!rel.empty())
                            {
                                if (!candidate.empty() && candidate.back() != L'\\' && candidate.back() != L'/')
                                    candidate.push_back(L'\\');
                                candidate += rel;
                            }
                            if (!candidate.empty() && GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
                            {
                                outPath = std::move(candidate);
                                return true;
                            }
                        }
                    }
                }
            }
            if (aumid.size() >= 2 && aumid[1] == L':' && GetFileAttributesW(aumid.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                outPath = aumid;
                return true;
            }
            return false;
        }

        bool ResolveAppxPackageInstallDir(const std::wstring& aumid, std::wstring& outDir)
        {
            outDir.clear();
            const size_t bang = aumid.find(L'!');
            if (bang == std::wstring::npos || bang == 0)
                return false;
            const std::wstring familyName = aumid.substr(0, bang);

            using GetPackagesByPackageFamilyFn = LONG(WINAPI*)(PCWSTR, UINT32*, PWSTR*, UINT32*, WCHAR*);
            using GetPackagePathByFullNameFn = LONG(WINAPI*)(PCWSTR, UINT32*, PWSTR);

            auto findProc = [](const char* name) -> FARPROC {
                if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll"))
                {
                    if (FARPROC fn = GetProcAddress(k32, name))
                        return fn;
                }
                HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
                return kb ? GetProcAddress(kb, name) : nullptr;
            };

            auto getPkgs = reinterpret_cast<GetPackagesByPackageFamilyFn>(findProc("GetPackagesByPackageFamily"));
            auto getPkgPath = reinterpret_cast<GetPackagePathByFullNameFn>(findProc("GetPackagePathByFullName"));
            if (!getPkgs || !getPkgPath)
                return false;

            UINT32 count = 0;
            UINT32 bufLen = 0;
            if (getPkgs(familyName.c_str(), &count, nullptr, &bufLen, nullptr) != ERROR_INSUFFICIENT_BUFFER ||
                count == 0 || bufLen == 0)
            {
                return false;
            }

            std::vector<PWSTR> fullNames(count, nullptr);
            std::vector<wchar_t> buf(bufLen, L'\0');
            if (getPkgs(familyName.c_str(), &count, fullNames.data(), &bufLen, buf.data()) != ERROR_SUCCESS ||
                count == 0 || !fullNames[0])
            {
                return false;
            }

            UINT32 pathLen = 0;
            if (getPkgPath(fullNames[0], &pathLen, nullptr) != ERROR_INSUFFICIENT_BUFFER || pathLen == 0)
                return false;

            std::vector<wchar_t> pathBuf(pathLen + 1, L'\0');
            if (getPkgPath(fullNames[0], &pathLen, pathBuf.data()) != ERROR_SUCCESS || !pathBuf[0])
                return false;

            outDir = pathBuf.data();
            return !outDir.empty();
        }

        void PopulateSearchAliases(RingSlot& slot, const std::wstring& configuredLabel,
            const std::wstring& exePath, const AppIndex* appIndex)
        {
            AddAliasUnique(slot.searchAliases, slot.label);
            AddAliasUnique(slot.searchAliases, configuredLabel);
            if (!exePath.empty())
            {
                const std::wstring stem = FileStemOfPath(exePath);
                AddAliasUnique(slot.searchAliases, stem);
                AddAliasUnique(slot.searchAliases, AppIndex::ResolveExecutableFriendlyName(exePath));
                if (appIndex)
                    AddAliasUnique(slot.searchAliases, appIndex->FindFriendlyNameForExe(exePath));
                const std::wstring parent = ParentFolderNameOf(exePath);
                if (!parent.empty() && parent.size() <= 40)
                    AddAliasUnique(slot.searchAliases, parent);
            }
            for (const auto& w : slot.windows)
                AddAliasUnique(slot.searchAliases, w.title);
            for (const auto& sub : slot.subItems)
                AddAliasUnique(slot.searchAliases, sub.label);
        }

        std::wstring InferLabel(SlotKind kind, const std::wstring& pathOrUrl)
        {
            if (kind == SlotKind::RecycleBin)
                return L"Recycle Bin";
            if (kind == SlotKind::Website)
            {
                URL_COMPONENTS uc{ sizeof(uc) };
                wchar_t host[256]{};
                uc.lpszHostName = host;
                uc.dwHostNameLength = _countof(host);
                if (WinHttpCrackUrl(pathOrUrl.c_str(), static_cast<DWORD>(pathOrUrl.size()), 0, &uc) && host[0])
                    return host;
                return pathOrUrl;
            }
            const size_t slash = pathOrUrl.find_last_of(L"\\/");
            std::wstring name = (slash != std::wstring::npos) ? pathOrUrl.substr(slash + 1) : pathOrUrl;
            if (name.empty())
                return pathOrUrl;
            const size_t dot = name.find_last_of(L'.');
            if (dot != std::wstring::npos && dot > 0 && kind != SlotKind::Folder)
                name.resize(dot);
            return name;
        }

        bool IsRecycleBinFull()
        {
            SHQUERYRBINFO info{ sizeof(info) };
            if (SUCCEEDED(SHQueryRecycleBinW(nullptr, &info)))
                return info.i64NumItems > 0;
            return false;
        }

        Microsoft::WRL::ComPtr<IWICBitmapSource> LoadImageFromFileWic(
            IWICImagingFactory* wic, const std::wstring& path)
        {
            if (!wic || path.empty())
                return nullptr;
            Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
            if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf())) || !decoder)
                return nullptr;
            Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
            if (FAILED(decoder->GetFrame(0, frame.GetAddressOf())) || !frame)
                return nullptr;
            Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
            if (FAILED(wic->CreateFormatConverter(converter.GetAddressOf())) || !converter)
                return nullptr;
            if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
                return nullptr;
            return converter;
        }

        void PopulateFolderSubItems(RingSlot& folderSlot, const std::wstring& folderPath, int maxItems, WindowTracker& tracker)
        {
            struct DirEntry
            {
                std::wstring name;
                std::wstring fullPath;
                bool isDir = false;
                ULONGLONG writeTime = 0;
            };

            std::vector<DirEntry> entries;
            const std::wstring pattern = folderPath + L"\\*";
            WIN32_FIND_DATAW fd{};
            HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
            if (find == INVALID_HANDLE_VALUE)
                return;

            do
            {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                    continue;
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0 &&
                    (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) != 0)
                    continue;
                ULARGE_INTEGER ft{};
                ft.LowPart = fd.ftLastWriteTime.dwLowDateTime;
                ft.HighPart = fd.ftLastWriteTime.dwHighDateTime;
                DirEntry e;
                e.name = fd.cFileName;
                e.fullPath = folderPath + L"\\" + fd.cFileName;
                e.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                e.writeTime = ft.QuadPart;
                entries.push_back(std::move(e));
            } while (FindNextFileW(find, &fd));
            FindClose(find);

            std::sort(entries.begin(), entries.end(), [](const DirEntry& a, const DirEntry& b) {
                return a.writeTime > b.writeTime;
            });

            const int limit = std::clamp(maxItems, 1, 9);
            const int fileLimit = (limit >= 2) ? (std::min)(limit, 7) : limit;
            for (int i = 0; i < static_cast<int>(entries.size()) && i < fileLimit; ++i)
            {
                RingSlot sub;
                sub.id = L"file-" + std::to_wstring(i);
                sub.kind = entries[i].isDir ? SlotKind::Folder : SlotKind::File;
                sub.label = entries[i].name;
                sub.pathOrUrl = entries[i].fullPath;
                sub.iconBitmap = tracker.GetShellIconBitmap(entries[i].fullPath);
                sub.isPinned = false;
                folderSlot.subItems.push_back(std::move(sub));
            }
            if (limit >= 2)
            {
                RingSlot openDir;
                openDir.id = L"folder-open";
                openDir.kind = SlotKind::Folder;
                openDir.label = L"Open in Explorer";
                openDir.pathOrUrl = folderPath;
                openDir.iconBitmap = tracker.GetStockIconBitmap(SIID_FOLDER);
                openDir.isPinned = false;
                folderSlot.subItems.push_back(std::move(openDir));
            }
        }

        template <typename Pred>
        void CollectMatchingExplorerWindows(
            const std::vector<AppWindowGroup>& running,
            std::vector<WindowEntry>& outWindows,
            Pred&& matchTitle)
        {
            for (const auto& grp : running)
            {
                const bool isExplorerGroup = IsExplorerExecutablePath(grp.exePath) ||
                    _wcsicmp(grp.processName.c_str(), L"explorer") == 0;
                if (!isExplorerGroup)
                    continue;
                for (const auto& w : grp.windows)
                {
                    if (matchTitle(w.title))
                        outWindows.push_back(w);
                }
            }
        }

        std::wstring GetLocalizedRecycleBinName()
        {
            std::wstring name = L"Recycle Bin";
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder, 0, nullptr, &pidl)) && pidl)
            {
                PWSTR display = nullptr;
                if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_NORMALDISPLAY, &display)) && display)
                {
                    if (display[0] != L'\0')
                        name = display;
                    CoTaskMemFree(display);
                }
                CoTaskMemFree(pidl);
            }
            return name;
        }

        RingSlot CreateSlotFromConfig(
            const PinnedSlotConfig& cfg,
            const std::vector<AppWindowGroup>& running,
            std::unordered_set<std::wstring>& consumedKeys,
            WindowTracker& tracker,
            IWICImagingFactory* wicFactory,
            const AppIndex* appIndex)
        {
            const std::wstring expanded = SlotModel::ExpandEnv(cfg.pathOrUrl);
            RingSlot slot;
            slot.id = cfg.id;
            slot.kind = cfg.kind;
            slot.label = cfg.label.empty() ? InferLabel(cfg.kind, expanded) : cfg.label;
            slot.pathOrUrl = expanded;
            slot.isPinned = true;
            slot.maxFolderItems = std::clamp(cfg.maxFolderItems, 1, 9);

            std::wstring resolvedExePath = expanded;
            if (cfg.kind == SlotKind::App)
            {
                const std::wstring key = WindowTracker::NormalizeAppKey(expanded, cfg.label);
                bool matched = false;
                for (const auto& grp : running)
                {
                    if (WindowTracker::AppKeysMatch(grp.normalizedKey, key))
                    {
                        slot.windows = grp.windows;
                        if (!grp.exePath.empty())
                            resolvedExePath = grp.exePath;
                        slot.iconBitmap = grp.iconBitmap ? grp.iconBitmap : tracker.GetShellIconBitmap(expanded);
                        consumedKeys.insert(grp.normalizedKey);
                        matched = true;
                        break;
                    }
                }
                if (!matched)
                    slot.iconBitmap = tracker.GetShellIconBitmap(expanded);

                const std::wstring stem = FileStemOfPath(resolvedExePath);
                const bool isExplorer = IsExplorerExecutablePath(resolvedExePath) ||
                    _wcsicmp(stem.c_str(), L"explorer") == 0;
                if (isExplorer)
                {
                    auto explorerIcon = tracker.GetShellIconBitmap(!expanded.empty() ? expanded : resolvedExePath);
                    if (explorerIcon)
                        slot.iconBitmap = std::move(explorerIcon);
                }

                std::wstring friendly = isExplorer
                    ? std::wstring(L"File Explorer")
                    : (appIndex ? appIndex->FindFriendlyNameForExe(resolvedExePath) : L"");
                if (friendly.empty())
                    friendly = AppIndex::ResolveExecutableFriendlyName(resolvedExePath);
                if (!friendly.empty() &&
                    (cfg.label.empty() || _wcsicmp(cfg.label.c_str(), stem.c_str()) == 0))
                {
                    slot.label = friendly;
                }
            }
            else if (cfg.kind == SlotKind::File || cfg.kind == SlotKind::Folder || cfg.kind == SlotKind::Script)
            {
                slot.iconBitmap = tracker.GetShellIconBitmap(expanded);
                if (cfg.kind == SlotKind::Folder)
                {
                    PopulateFolderSubItems(slot, expanded, slot.maxFolderItems, tracker);
                    CollectMatchingExplorerWindows(running, slot.windows, [&](const std::wstring& title) {
                        return SlotModel::ExplorerWindowMatchesFolder(title, expanded);
                    });
                }
            }
            else if (cfg.kind == SlotKind::Website)
            {
                if (!cfg.iconCacheFile.empty())
                {
                    const std::wstring fullIcon = (cfg.iconCacheFile.find(L':') != std::wstring::npos)
                        ? cfg.iconCacheFile
                        : (SwitcherConfig::AppDataDirectory() + L"\\" + cfg.iconCacheFile);
                    slot.iconBitmap = LoadImageFromFileWic(wicFactory, fullIcon);
                }
            }
            else if (cfg.kind == SlotKind::RecycleBin)
            {
                slot.iconBitmap = tracker.GetStockIconBitmap(IsRecycleBinFull() ? SIID_RECYCLERFULL : SIID_RECYCLER);
                const std::wstring recycleBinName = GetLocalizedRecycleBinName();
                CollectMatchingExplorerWindows(running, slot.windows, [&](const std::wstring& title) {
                    return SlotModel::ExplorerWindowMatchesRecycleBin(title, recycleBinName);
                });
            }
            else if (cfg.kind == SlotKind::Collection)
            {
                slot.iconBitmap = tracker.GetStockIconBitmap(SIID_FOLDER);
                for (const auto& child : cfg.items)
                    slot.subItems.push_back(CreateSlotFromConfig(child, running, consumedKeys, tracker, wicFactory, appIndex));
            }

            const HWND frontmost = tracker.LastActiveWindow();
            for (const auto& w : slot.windows)
            {
                if (frontmost && w.hwnd == frontmost)
                {
                    slot.isFrontmost = true;
                    break;
                }
            }

            PopulateSearchAliases(slot, cfg.label,
                cfg.kind == SlotKind::App ? resolvedExePath : std::wstring(), appIndex);
            return slot;
        }
    } // namespace

    bool SlotModel::ExplorerWindowMatchesFolder(const std::wstring& windowTitle, const std::wstring& expandedFolderPath)
    {
        return ::winrings::ExplorerWindowMatchesFolder(windowTitle, expandedFolderPath);
    }

    bool SlotModel::ExplorerWindowMatchesRecycleBin(const std::wstring& windowTitle, const std::wstring& localizedRecycleBinName)
    {
        return ::winrings::ExplorerWindowMatchesRecycleBin(windowTitle, localizedRecycleBinName);
    }

    std::wstring SlotModel::ExpandEnv(const std::wstring& input)
    {
        if (input.empty())
            return {};
        const DWORD needed = ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);
        if (!needed)
            return input;
        std::wstring out(static_cast<size_t>(needed), L'\0');
        const DWORD actual = ExpandEnvironmentStringsW(input.c_str(), out.data(), needed);
        if (!actual)
            return input;
        if (!out.empty() && out.back() == L'\0')
            out.pop_back();
        return out;
    }

    std::vector<RingSlot> SlotModel::BuildRingSlots(
        HWND ignoreHwnd,
        const SwitcherConfig& config,
        WindowTracker& tracker,
        IWICImagingFactory* wicFactory,
        const AppIndex* appIndex)
    {
        const auto running = tracker.EnumerateGroupedWindows(ignoreHwnd);
        const HWND frontmost = tracker.LastActiveWindow();
        std::vector<RingSlot> slots;
        std::unordered_set<std::wstring> consumedKeys;

        for (const auto& pinned : config.pinnedSlots)
        {
            if (static_cast<int>(slots.size()) >= config.maxSlots)
                break;
            slots.push_back(CreateSlotFromConfig(pinned, running, consumedKeys, tracker, wicFactory, appIndex));
        }

        if (config.includeUnpinnedRunningApps)
        {
            for (const auto& grp : running)
            {
                if (static_cast<int>(slots.size()) >= config.maxSlots)
                    break;
                if (consumedKeys.count(grp.normalizedKey))
                    continue;
                RingSlot s;
                s.id = L"run-" + grp.normalizedKey;
                s.kind = SlotKind::App;
                const bool isExplorer = IsExplorerExecutablePath(grp.exePath) ||
                    _wcsicmp(grp.processName.c_str(), L"explorer") == 0;
                std::wstring friendly = isExplorer
                    ? std::wstring(L"File Explorer")
                    : (appIndex ? appIndex->FindFriendlyNameForExe(grp.exePath) : L"");
                if (friendly.empty() && !grp.exePath.empty())
                    friendly = AppIndex::ResolveExecutableFriendlyName(grp.exePath);
                s.label = !friendly.empty() ? friendly : grp.processName;
                s.pathOrUrl = grp.exePath;
                if (isExplorer && !grp.exePath.empty())
                    s.iconBitmap = tracker.GetShellIconBitmap(grp.exePath);
                if (!s.iconBitmap)
                    s.iconBitmap = grp.iconBitmap;
                s.isPinned = false;
                s.windows = grp.windows;
                for (const auto& w : s.windows)
                {
                    if (frontmost && w.hwnd == frontmost)
                    {
                        s.isFrontmost = true;
                        break;
                    }
                }
                PopulateSearchAliases(s, grp.processName, grp.exePath, appIndex);
                slots.push_back(std::move(s));
            }
        }

        return slots;
    }

    bool SlotModel::ResolveShortcutTarget(
        const std::wstring& lnkPath,
        std::wstring& outTargetPath,
        std::wstring& outArguments,
        std::wstring& outWorkingDir)
    {
        outTargetPath.clear();
        outArguments.clear();
        outWorkingDir.clear();

        const std::wstring expandedLnk = ExpandEnv(lnkPath);
        if (expandedLnk.empty() || !EndsWithInsensitiveW(expandedLnk, L".lnk"))
            return false;

        Microsoft::WRL::ComPtr<IShellLinkW> shellLink;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(shellLink.GetAddressOf()))) || !shellLink)
        {
            return false;
        }
        Microsoft::WRL::ComPtr<IPersistFile> pf;
        if (FAILED(shellLink.As(&pf)) || !pf || FAILED(pf->Load(expandedLnk.c_str(), STGM_READ)))
            return false;

        wchar_t targetBuf[1024]{};
        if (FAILED(shellLink->GetPath(targetBuf, _countof(targetBuf), nullptr, SLGP_RAWPATH)) || !targetBuf[0])
            return false;

        std::wstring target = ExpandEnv(targetBuf);
        if (target.empty() || GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES)
            return false;

        outTargetPath = std::move(target);

        wchar_t argsBuf[2048]{};
        if (SUCCEEDED(shellLink->GetArguments(argsBuf, _countof(argsBuf))) && argsBuf[0])
            outArguments = argsBuf;

        wchar_t workBuf[1024]{};
        if (SUCCEEDED(shellLink->GetWorkingDirectory(workBuf, _countof(workBuf))) && workBuf[0])
            outWorkingDir = ExpandEnv(workBuf);

        return true;
    }

    void SlotModel::LaunchPathOrUrl(const std::wstring& pathOrUrl, const std::wstring& arguments)
    {
        if (pathOrUrl.empty())
            return;

        constexpr const wchar_t* kAppsPrefix = L"shell:AppsFolder\\";
        if (arguments.empty() && pathOrUrl.rfind(kAppsPrefix, 0) == 0)
        {
            const std::wstring aumid = pathOrUrl.substr(wcslen(kAppsPrefix));
            Microsoft::WRL::ComPtr<IApplicationActivationManager> aam;
            if (SUCCEEDED(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr,
                CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(aam.GetAddressOf()))) && aam)
            {
                DWORD pid = 0;
                if (SUCCEEDED(aam->ActivateApplication(aumid.c_str(), nullptr, AO_NONE, &pid)))
                    return;
            }
            SHELLEXECUTEINFOW seiApp{ sizeof(seiApp) };
            seiApp.fMask = SEE_MASK_FLAG_NO_UI;
            seiApp.lpVerb = L"open";
            seiApp.lpFile = L"explorer.exe";
            seiApp.lpParameters = pathOrUrl.c_str();
            seiApp.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&seiApp);
            return;
        }

        SHELLEXECUTEINFOW sei{ sizeof(sei) };
        sei.fMask = SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = L"open";
        sei.lpFile = pathOrUrl.c_str();
        sei.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
        sei.nShow = SW_SHOWNORMAL;
        ShellExecuteExW(&sei);
    }

    void SlotModel::LaunchRunAsAdmin(const std::wstring& pathOrUrl)
    {
        const std::wstring expanded = ExpandEnv(pathOrUrl);
        if (expanded.empty())
            return;

        constexpr const wchar_t* kAppsPrefix = L"shell:AppsFolder\\";
        if (expanded.rfind(kAppsPrefix, 0) == 0)
        {
            const std::wstring aumid = expanded.substr(wcslen(kAppsPrefix));
            std::wstring resolved;
            if (ResolveKnownFolderAumidPath(aumid, resolved))
            {
                SHELLEXECUTEINFOW sei{ sizeof(sei) };
                sei.fMask = SEE_MASK_FLAG_NO_UI;
                sei.lpVerb = L"runas";
                sei.lpFile = resolved.c_str();
                sei.nShow = SW_SHOWNORMAL;
                ShellExecuteExW(&sei);
                return;
            }

            SHELLEXECUTEINFOW sei{ sizeof(sei) };
            sei.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_FLAG_NO_UI;
            sei.lpVerb = L"runas";
            sei.lpFile = expanded.c_str();
            sei.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&sei);
            return;
        }

        if (EndsWithInsensitiveW(expanded, L".lnk"))
        {
            std::wstring target;
            std::wstring args;
            std::wstring workDir;
            if (ResolveShortcutTarget(expanded, target, args, workDir))
            {
                SHELLEXECUTEINFOW sei{ sizeof(sei) };
                sei.fMask = SEE_MASK_FLAG_NO_UI;
                sei.lpVerb = L"runas";
                sei.lpFile = target.c_str();
                sei.lpParameters = args.empty() ? nullptr : args.c_str();
                sei.lpDirectory = workDir.empty() ? nullptr : workDir.c_str();
                sei.nShow = SW_SHOWNORMAL;
                ShellExecuteExW(&sei);
                return;
            }

            SHELLEXECUTEINFOW sei{ sizeof(sei) };
            sei.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_FLAG_NO_UI;
            sei.lpVerb = L"runas";
            sei.lpFile = expanded.c_str();
            sei.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&sei);
            return;
        }

        SHELLEXECUTEINFOW sei{ sizeof(sei) };
        sei.fMask = SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = L"runas";
        sei.lpFile = expanded.c_str();
        sei.nShow = SW_SHOWNORMAL;
        ShellExecuteExW(&sei);
    }

    void SlotModel::RevealInExplorer(const std::wstring& path)
    {
        const std::wstring expanded = ExpandEnv(path);
        if (expanded.empty())
            return;

        std::wstring revealTarget;
        constexpr const wchar_t* kAppsPrefix = L"shell:AppsFolder\\";
        if (expanded.rfind(kAppsPrefix, 0) == 0)
        {
            const std::wstring aumid = expanded.substr(wcslen(kAppsPrefix));
            std::wstring resolved;
            if (ResolveKnownFolderAumidPath(aumid, resolved))
            {
                revealTarget = std::move(resolved);
            }
            else
            {
                std::wstring pkgDir;
                if (ResolveAppxPackageInstallDir(aumid, pkgDir))
                {
                    const std::wstring manifest = pkgDir + L"\\AppxManifest.xml";
                    if (GetFileAttributesW(manifest.c_str()) != INVALID_FILE_ATTRIBUTES)
                    {
                        revealTarget = manifest;
                    }
                    else
                    {
                        ShellExecuteW(nullptr, L"open", L"explorer.exe", pkgDir.c_str(), nullptr, SW_SHOWNORMAL);
                        return;
                    }
                }
                else
                {
                    ShellExecuteW(nullptr, L"open", L"explorer.exe", L"shell:AppsFolder", nullptr, SW_SHOWNORMAL);
                    return;
                }
            }
        }
        else if (EndsWithInsensitiveW(expanded, L".lnk"))
        {
            std::wstring target;
            std::wstring args;
            std::wstring workDir;
            if (ResolveShortcutTarget(expanded, target, args, workDir))
                revealTarget = std::move(target);
            else
                revealTarget = expanded;
        }
        else
        {
            revealTarget = expanded;
        }

        const std::wstring params = L"/select,\"" + revealTarget + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
    }

    namespace
    {
        std::wstring BuildDoubleNullList(const std::vector<std::wstring>& paths)
        {
            std::wstring doubleNull;
            for (const auto& p : paths)
            {
                if (!p.empty())
                {
                    doubleNull += p;
                    doubleNull.push_back(L'\0');
                }
            }
            if (!doubleNull.empty())
                doubleNull.push_back(L'\0');
            return doubleNull;
        }
    }

    bool SlotModel::SendToRecycleBin(const std::vector<std::wstring>& paths)
    {
        const std::wstring doubleNull = BuildDoubleNullList(paths);
        if (doubleNull.empty())
            return false;

        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = doubleNull.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
        return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
    }

    bool SlotModel::CopyFilesToFolder(const std::vector<std::wstring>& paths, const std::wstring& destFolder)
    {
        if (destFolder.empty())
            return false;
        const std::wstring srcList = BuildDoubleNullList(paths);
        if (srcList.empty())
            return false;

        std::wstring dstList = destFolder;
        dstList.push_back(L'\0');
        dstList.push_back(L'\0');

        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_COPY;
        op.pFrom = srcList.c_str();
        op.pTo = dstList.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_RENAMEONCOLLISION | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI;
        return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
    }

    bool SlotModel::EmptyRecycleBin(HWND ownerHwnd)
    {
        return SUCCEEDED(SHEmptyRecycleBinW(ownerHwnd, nullptr,
            SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND));
    }

    void SlotModel::FetchFaviconAsync(
        const std::wstring& url,
        std::function<void(std::wstring relativeIconPath)> onCompleted)
    {
        std::thread([url, cb = std::move(onCompleted)]() {
            URL_COMPONENTS uc{ sizeof(uc) };
            wchar_t scheme[32]{};
            wchar_t host[256]{};
            uc.lpszScheme = scheme;
            uc.dwSchemeLength = _countof(scheme);
            uc.lpszHostName = host;
            uc.dwHostNameLength = _countof(host);
            if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc) || !host[0])
            {
                if (cb) cb(L"");
                return;
            }

            std::wstring safeHost;
            for (wchar_t c : std::wstring(host))
            {
                if (std::iswalnum(c) || c == L'-' || c == L'.')
                    safeHost.push_back(c);
            }
            if (safeHost.empty())
            {
                if (cb) cb(L"");
                return;
            }

            const std::wstring appDir = SwitcherConfig::AppDataDirectory();
            const std::wstring iconsDir = appDir + L"\\icons";
            SHCreateDirectoryExW(nullptr, iconsDir.c_str(), nullptr);
            const std::wstring relPath = L"icons\\" + safeHost + L".ico";
            const std::wstring fullPath = appDir + L"\\" + relPath;
            if (GetFileAttributesW(fullPath.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                if (cb) cb(relPath);
                return;
            }

            std::wstring resultPath;
            HINTERNET session = WinHttpOpen(L"WinRings/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (session)
            {
                WinHttpSetTimeouts(session, 4000, 4000, 4000, 4000);
                const INTERNET_PORT port = uc.nPort ? uc.nPort : (uc.nScheme == INTERNET_SCHEME_HTTPS ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT);
                HINTERNET conn = WinHttpConnect(session, host, port, 0);
                if (conn)
                {
                    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
                    HINTERNET req = WinHttpOpenRequest(conn, L"GET", L"/favicon.ico", nullptr,
                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
                    if (req)
                    {
                        DWORD disableCookies = WINHTTP_DISABLE_COOKIES;
                        WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &disableCookies, sizeof(disableCookies));
                        if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
                            WinHttpReceiveResponse(req, nullptr))
                        {
                            DWORD statusCode = 0;
                            DWORD statusSize = sizeof(statusCode);
                            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
                            if (statusCode == 200)
                            {
                                std::string data;
                                DWORD avail = 0;
                                while (WinHttpQueryDataAvailable(req, &avail) && avail > 0 && data.size() < 512 * 1024)
                                {
                                    const size_t offset = data.size();
                                    data.resize(offset + avail);
                                    DWORD read = 0;
                                    if (!WinHttpReadData(req, data.data() + offset, avail, &read) || read == 0)
                                    {
                                        data.resize(offset);
                                        break;
                                    }
                                    data.resize(offset + read);
                                }
                                if (!data.empty())
                                {
                                    HANDLE file = CreateFileW(fullPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                                    if (file != INVALID_HANDLE_VALUE)
                                    {
                                        DWORD written = 0;
                                        if (WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                                            written == data.size())
                                        {
                                            resultPath = relPath;
                                        }
                                        CloseHandle(file);
                                    }
                                }
                            }
                        }
                        WinHttpCloseHandle(req);
                    }
                    WinHttpCloseHandle(conn);
                }
                WinHttpCloseHandle(session);
            }

            if (cb)
                cb(std::move(resultPath));
        }).detach();
    }
} // namespace winrings
