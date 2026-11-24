#include "TopBarHook.h"

#include <CommCtrl.h>
#include <ShellScalingApi.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "../shared/SlotMatching.h"
#include "ShellController.h"

namespace winrings
{
    namespace
    {
        // Verified read-only against C:\Windows\System32\Taskbar.dll
        // Windows 11 25H2 build 26200.8875 (x64)
        constexpr UINT32 kExpectedTimeDateStamp = 0xA4EA993Au;
        constexpr UINT32 kExpectedSizeOfImage   = 0x002FC000u;

        // 4 zero-WinRT Hook RVAs + 10-byte exact prologue signatures (NEVER hook WIL feature flags at runtime!)
        constexpr UINT32 kRvaGetMinSizePrimary  = 0x045EA0u; // 5-byte prologue
        constexpr UINT32 kRvaGetMinSizeSecond   = 0x075430u; // 5-byte prologue
        constexpr UINT32 kRvaGetDockedRect      = 0x0161A0u; // 5-byte prologue
        constexpr UINT32 kRvaGetAutoHideFlags   = 0x016240u; // 6-byte prologue (40 53 48 83 EC 20)

        // Direct internal helper RVAs (all verified in Taskbar.dll GFIDS & guarded by exact PE check)
        constexpr UINT32 kRvaGetStuckPlace      = 0x06D260u;
        constexpr UINT32 kRvaUpdateTaskbarSize  = 0x093540u;
        constexpr UINT32 kRvaUpdateSecondSize   = 0x0E18D0u;
        constexpr size_t kVerifySizeVtableOff   = 0x068u; // ITrayUI::VerifySize (0x0E1AD0)
        constexpr size_t kSizeWindowsVtableOff  = 0x0E8u; // ITrayUI::SizeWindows (0x092A60)

        // Verified pITrayUI member offsets for build 26200.8875
        constexpr size_t kOffTrayState    = 0x03Cu; // [pITrayUI - 8 + 0x44]
        constexpr size_t kOffStuckRects   = 0x078u; // RECT m_rcStuck[4]
        constexpr size_t kOffStuckPlace   = 0x110u; // UINT m_uStuckPlace (0=L,1=T,2=R,3=B)
        constexpr size_t kOffPrimaryMon   = 0x130u; // HMONITOR m_hMonPrimary
        constexpr size_t kOffPrimaryDip   = 0x32Cu; // int m_primaryTaskbarHeightDip
        constexpr size_t kOffSecondaryDip = 0x330u; // int m_secondaryTaskbarHeightDip

        constexpr BYTE kSigMovRbxStackPushRdiSub20[10] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20
        };
        constexpr BYTE kSigMovRbxMovRsiStack[10] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10
        };
        constexpr BYTE kSigGetAutoHideFlags[10] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48
        };

        // Verified read-only against SystemTray.dll (Windows 11 25H2 build 26200.8875 x64)
        // OverflowXamlIslandManager::PositionWindow(void* pThis, uint64_t packedAnchorPoint)
        constexpr UINT32 kExpectedSysTrayTimeDateStamp = 0x6A1A2032u;
        constexpr UINT32 kExpectedSysTraySizeOfImage   = 0x001FB000u;
        constexpr UINT32 kRvaPositionOverflowWindow    = 0x0B9EB0u;
        constexpr BYTE kSigPositionOverflowWindow[10] = {
            0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20
        };

        using GetMinSizePrimaryFn      = void(__fastcall*)(void* pITrayUI, UINT dpi, SIZE* outSize);
        using GetMinSizeSecondFn       = void(__fastcall*)(void* pITrayUI, HMONITOR hMon, SIZE* outSize);
        using GetDockedRectFn          = UINT(__fastcall*)(void* pITrayUI, RECT* outRect, BOOL useOverride);
        using GetAutoHideFlagsFn       = UINT(__fastcall*)(void* pAutoHideThis);
        using PositionOverflowWindowFn = void(__fastcall*)(void* pThis, uint64_t packedAnchor);
        using GetStuckPlaceFn          = UINT(__fastcall*)(void* pITrayUI);
        using UpdateTaskbarSizeFn      = void(__fastcall*)(void* pITrayUI, int sizeDip);
        using UpdateSecondSizeFn       = void(__fastcall*)(void* pITrayUI, int sizeDip, HMONITOR hMon);
        using VerifySizeFn             = void(__fastcall*)(void* pITrayUI, bool a, bool b, HMONITOR hMon);
        using SizeWindowsFn            = void(__fastcall*)(void* pITrayUI);

        struct HookSite
        {
            BYTE* target = nullptr;
            BYTE* relay = nullptr;
            BYTE* trampoline = nullptr;
            BYTE saved8[8]{};
            size_t stealLen = 5;
            bool patched = false;
        };

        struct HookState
        {
            std::atomic<bool> installed{ false };
            std::atomic<bool> sysTrayInstalled{ false };
            std::atomic<bool> supported{ false };
            std::atomic<bool> active{ false };
            std::atomic<bool> topDockActive{ false };
            std::atomic<bool> allMonitors{ true };
            std::atomic<int> heightDip{ 30 };
            std::atomic<int> origSizeDip{ 48 };
            std::atomic<UINT> origStuckPlace{ 3 }; // ABE_BOTTOM
            std::atomic<UINT> origTrayState{ 0 };
            std::atomic<bool> capturedOriginal{ false };
            std::atomic<void*> pITrayUI{ nullptr };
            std::atomic<int> inFlight{ 0 };

            HMODULE taskbarModule = nullptr;
            BYTE* trampolinePage = nullptr; // intentionally kept allocated while DLL pinned
            BYTE* sysTrayTrampolinePage = nullptr;

            HookSite siteMinPrimary{};
            HookSite siteMinSecondary{};
            HookSite siteDockedRect{};
            HookSite siteAutoHideFlags{};
            HookSite sitePositionOverflow{};

            GetMinSizePrimaryFn origMinPrimary = nullptr;
            GetMinSizeSecondFn origMinSecondary = nullptr;
            GetDockedRectFn origDockedRect = nullptr;
            GetAutoHideFlagsFn origAutoHideFlags = nullptr;
            PositionOverflowWindowFn origPositionOverflow = nullptr;

            GetStuckPlaceFn fnGetStuckPlace = nullptr;
            UpdateTaskbarSizeFn fnUpdateTaskbarSize = nullptr;
            UpdateSecondSizeFn fnUpdateSecondSize = nullptr;
        };

        HookState g_hook;

        struct ScopedInFlight
        {
            ScopedInFlight() noexcept { g_hook.inFlight.fetch_add(1, std::memory_order_acq_rel); }
            ~ScopedInFlight() { g_hook.inFlight.fetch_sub(1, std::memory_order_acq_rel); }
        };

        // Thin CFG-exempt trampoline callers so indirect calls into our dynamic trampoline page
        // never fail under strict CFG even when compiled with /guard:cf.
        __declspec(guard(nocf)) void CallOrigMinPrimary(GetMinSizePrimaryFn fn, void* pITrayUI, UINT dpi, SIZE* outSize)
        {
            if (fn)
                fn(pITrayUI, dpi, outSize);
        }

        __declspec(guard(nocf)) void CallOrigMinSecondary(GetMinSizeSecondFn fn, void* pITrayUI, HMONITOR hMon, SIZE* outSize)
        {
            if (fn)
                fn(pITrayUI, hMon, outSize);
        }

        __declspec(guard(nocf)) UINT CallOrigDockedRect(GetDockedRectFn fn, void* pITrayUI, RECT* outRect, BOOL useOverride)
        {
            return fn ? fn(pITrayUI, outRect, useOverride) : 0;
        }

        __declspec(guard(nocf)) UINT CallOrigAutoHideFlags(GetAutoHideFlagsFn fn, void* pAutoHideThis)
        {
            return fn ? fn(pAutoHideThis) : 0;
        }

        __declspec(guard(nocf)) void CallOrigPositionOverflow(PositionOverflowWindowFn fn, void* pThis, uint64_t packedAnchor)
        {
            if (fn)
                fn(pThis, packedAnchor);
        }

        UINT QueryMonitorDpi(HMONITOR monitor) noexcept
        {
            using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);
            static GetDpiForMonitorFn pGetDpiForMonitor = []() -> GetDpiForMonitorFn {
                HMODULE shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                return shcore ? reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor")) : nullptr;
            }();

            if (pGetDpiForMonitor && monitor)
            {
                UINT dpiX = 96;
                UINT dpiY = 96;
                if (SUCCEEDED(pGetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX >= 72)
                    return dpiX;
            }
            HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
            if (tray)
            {
                const UINT d = GetDpiForWindow(tray);
                if (d >= 72)
                    return d;
            }
            return 96;
        }

        void RecordTrayUiInstance(void* pITrayUI) noexcept
        {
            if (!pITrayUI)
                return;

            void* expected = nullptr;
            const bool firstPtr = g_hook.pITrayUI.compare_exchange_strong(expected, pITrayUI, std::memory_order_acq_rel);

            if (!g_hook.capturedOriginal.load(std::memory_order_acquire))
            {
                const int rawDip = *reinterpret_cast<const int*>(
                    reinterpret_cast<const BYTE*>(pITrayUI) + kOffPrimaryDip);
                if (rawDip >= 24 && rawDip <= 96)
                    g_hook.origSizeDip.store(rawDip, std::memory_order_release);

                const UINT rawState = *reinterpret_cast<const UINT*>(
                    reinterpret_cast<const BYTE*>(pITrayUI) + kOffTrayState);
                g_hook.origTrayState.store(rawState, std::memory_order_release);

                UINT stuck = 3u;
                if (g_hook.fnGetStuckPlace)
                {
                    stuck = g_hook.fnGetStuckPlace(pITrayUI);
                    if (stuck <= 3)
                        g_hook.origStuckPlace.store(stuck, std::memory_order_release);
                }
                g_hook.capturedOriginal.store(true, std::memory_order_release);

                if (firstPtr)
                {
                    ShellDiagLog(L"TopBarHook",
                        L"Captured pITrayUI=0x%p origSizeDip=%d origStuckPlace=%u origTrayState=0x%X",
                        pITrayUI, g_hook.origSizeDip.load(), stuck, rawState);
                }
            }
        }

        void __fastcall Detour_GetMinSizePrimary(void* pITrayUI, UINT dpi, SIZE* outSize)
        {
            ScopedInFlight guard;
            RecordTrayUiInstance(pITrayUI);
            CallOrigMinPrimary(g_hook.origMinPrimary, pITrayUI, dpi, outSize);

            if (outSize && g_hook.active.load(std::memory_order_acquire))
            {
                const int targetDip = (std::clamp)(g_hook.heightDip.load(std::memory_order_acquire), 28, 48);
                const UINT effectiveDpi = dpi >= 72 ? dpi : 96;
                const int px = MulDiv(targetDip, static_cast<int>(effectiveDpi), 96);
                outSize->cx = px;
                outSize->cy = px;
            }
        }

        void __fastcall Detour_GetMinSizeSecondary(void* pITrayUI, HMONITOR hMon, SIZE* outSize)
        {
            ScopedInFlight guard;
            RecordTrayUiInstance(pITrayUI);
            CallOrigMinSecondary(g_hook.origMinSecondary, pITrayUI, hMon, outSize);

            if (outSize && g_hook.active.load(std::memory_order_acquire))
            {
                const int targetDip = (std::clamp)(g_hook.heightDip.load(std::memory_order_acquire), 28, 48);
                const UINT dpi = QueryMonitorDpi(hMon);
                const int px = MulDiv(targetDip, static_cast<int>(dpi), 96);
                outSize->cx = px;
                outSize->cy = px;
            }
        }

        UINT __fastcall Detour_GetDockedRect(void* pITrayUI, RECT* outRect, BOOL useOverride)
        {
            ScopedInFlight guard;
            RecordTrayUiInstance(pITrayUI);
            return CallOrigDockedRect(g_hook.origDockedRect, pITrayUI, outRect, useOverride);
        }

        UINT __fastcall Detour_GetAutoHideFlags(void* pAutoHideThis)
        {
            ScopedInFlight guard;
            if (g_hook.topDockActive.load(std::memory_order_acquire))
                return 0u;
            return CallOrigAutoHideFlags(g_hook.origAutoHideFlags, pAutoHideThis);
        }

        void __fastcall Detour_PositionOverflowWindow(void* pThis, uint64_t packedAnchor)
        {
            ScopedInFlight guard;
            CallOrigPositionOverflow(g_hook.origPositionOverflow, pThis, packedAnchor);

            if (g_hook.topDockActive.load(std::memory_order_acquire))
            {
                HWND hwndOverflow = nullptr;
                if (pThis)
                {
                    hwndOverflow = *reinterpret_cast<HWND*>(reinterpret_cast<BYTE*>(pThis) + 0x10);
                }
                if (!hwndOverflow || !IsWindow(hwndOverflow))
                {
                    hwndOverflow = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr);
                }
                const int anchorX = static_cast<int>(static_cast<int32_t>(packedAnchor & 0xFFFFFFFFull));
                TopBarHook::RepositionTrayPopupIfNeeded(hwndOverflow, anchorX);
            }
        }

        bool VerifyTaskbarPeAndSignatures(BYTE* base, bool verboseLog) noexcept
        {
            if (!base)
            {
                if (verboseLog) ShellDiagLog(L"TopBarHook", L"Verify failed: Taskbar.dll base is null");
                return false;
            }

            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000)
            {
                if (verboseLog) ShellDiagLog(L"TopBarHook", L"Verify failed: invalid DOS header");
                return false;
            }

            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
            {
                if (verboseLog) ShellDiagLog(L"TopBarHook", L"Verify failed: invalid NT/Machine header");
                return false;
            }
            if (nt->FileHeader.TimeDateStamp != kExpectedTimeDateStamp ||
                nt->OptionalHeader.SizeOfImage != kExpectedSizeOfImage)
            {
                static bool s_loggedMismatch = false;
                if (verboseLog && !s_loggedMismatch)
                {
                    s_loggedMismatch = true;
                    ShellDiagLog(L"TopBarHook",
                        L"Verify failed: PE mismatch TS=0x%08X (exp 0x%08X) SizeOfImage=0x%08X (exp 0x%08X)",
                        nt->FileHeader.TimeDateStamp, kExpectedTimeDateStamp,
                        nt->OptionalHeader.SizeOfImage, kExpectedSizeOfImage);
                }
                return false;
            }

            const bool s1 = std::memcmp(base + kRvaGetMinSizePrimary, kSigMovRbxStackPushRdiSub20, 10) == 0;
            const bool s2 = std::memcmp(base + kRvaGetMinSizeSecond, kSigMovRbxMovRsiStack, 10) == 0;
            const bool s3 = std::memcmp(base + kRvaGetDockedRect, kSigMovRbxMovRsiStack, 10) == 0;
            const bool s4 = std::memcmp(base + kRvaGetAutoHideFlags, kSigGetAutoHideFlags, 10) == 0;

            if (verboseLog)
            {
                ShellDiagLog(L"TopBarHook",
                    L"PE OK (base=0x%p TS=0x%08X SOI=0x%08X) sigs=[%d,%d,%d,%d]",
                    base, nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage,
                    s1 ? 1 : 0, s2 ? 1 : 0, s3 ? 1 : 0, s4 ? 1 : 0);
            }

            return s1 && s2 && s3 && s4;
        }

        BYTE* AllocateNearTrampolinePage(const BYTE* base) noexcept
        {
            SYSTEM_INFO si{};
            GetSystemInfo(&si);
            const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
            const uintptr_t origin = reinterpret_cast<uintptr_t>(base);
            const uintptr_t maxSpan = 0x60000000ull;
            const uintptr_t minAddr = (origin > maxSpan) ? (origin - maxSpan) : gran;
            const uintptr_t maxAddr = origin + maxSpan;

            auto tryDirection = [&](bool up) -> BYTE* {
                uintptr_t cur = origin;
                for (int step = 0; step < 8192; ++step)
                {
                    if (up)
                    {
                        if (cur > maxAddr - gran)
                            break;
                        cur += gran;
                    }
                    else
                    {
                        if (cur < minAddr + gran)
                            break;
                        cur -= gran;
                    }

                    MEMORY_BASIC_INFORMATION mbi{};
                    if (VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi)) == 0)
                        break;

                    const uintptr_t regStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                    const uintptr_t regEnd = regStart + mbi.RegionSize;

                    if (mbi.State == MEM_FREE && mbi.RegionSize >= 0x1000)
                    {
                        const uintptr_t lo = (std::max)(regStart, minAddr);
                        const uintptr_t hi = (std::min)(regEnd, maxAddr);
                        if (hi > lo && (hi - lo) >= 0x1000)
                        {
                            uintptr_t cand = 0;
                            if (!up)
                            {
                                const uintptr_t topFit = (std::min)(cur, hi - 0x1000);
                                cand = topFit & ~(gran - 1);
                                if (cand < lo)
                                    cand = (lo + gran - 1) & ~(gran - 1);
                            }
                            else
                            {
                                const uintptr_t botFit = (std::max)(cur, lo);
                                cand = (botFit + gran - 1) & ~(gran - 1);
                            }

                            if (cand >= lo && cand + 0x1000 <= hi)
                            {
                                void* mem = VirtualAlloc(reinterpret_cast<LPVOID>(cand), 0x1000,
                                    MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                                if (mem)
                                {
                                    const intptr_t delta = reinterpret_cast<const BYTE*>(mem) - base;
                                    if (delta > -0x70000000LL && delta < 0x70000000LL)
                                        return static_cast<BYTE*>(mem);
                                    VirtualFree(mem, 0, MEM_RELEASE);
                                }
                            }
                        }
                    }

                    if (!up)
                    {
                        if (regStart <= minAddr)
                            break;
                        cur = regStart;
                    }
                    else
                    {
                        if (regEnd >= maxAddr || regEnd <= cur)
                            break;
                        cur = (regEnd + gran - 1) & ~(gran - 1);
                    }
                }
                return nullptr;
            };

            if (BYTE* p = tryDirection(false))
                return p;
            return tryDirection(true);
        }

        void EmitAbsJump64(BYTE* dst, const void* targetAddr) noexcept
        {
            // ff 25 00 00 00 00 <8-byte addr>
            dst[0] = 0xFF;
            dst[1] = 0x25;
            dst[2] = 0x00;
            dst[3] = 0x00;
            dst[4] = 0x00;
            dst[5] = 0x00;
            const uint64_t addr = reinterpret_cast<uint64_t>(targetAddr);
            std::memcpy(dst + 6, &addr, sizeof(addr));
        }

        bool PrepareSite(HookSite& site, BYTE* target, size_t stealLen, void* detourFn, BYTE*& cursor) noexcept
        {
            if (!target || stealLen < 5 || stealLen > 8)
                return false;

            site.target = target;
            site.stealLen = stealLen;
            std::memcpy(site.saved8, target, 8);

            // 16-byte aligned trampoline (CFG requires 16-byte aligned targets)
            cursor = reinterpret_cast<BYTE*>((reinterpret_cast<uintptr_t>(cursor) + 15ull) & ~15ull);
            site.trampoline = cursor;
            std::memcpy(site.trampoline, target, stealLen);
            EmitAbsJump64(site.trampoline + stealLen, target + stealLen);
            cursor += 32;

            // 14-byte near relay: abs jump to detourFn
            site.relay = cursor;
            EmitAbsJump64(site.relay, detourFn);
            cursor += 16;

            const intptr_t diff = site.relay - (target + 5);
            return diff >= INT32_MIN && diff <= INT32_MAX;
        }

        bool PatchSite(HookSite& site) noexcept
        {
            const intptr_t diff = site.relay - (site.target + 5);
            if (diff < INT32_MIN || diff > INT32_MAX)
                return false;

            BYTE patched8[8]{};
            std::memcpy(patched8, site.saved8, 8);
            patched8[0] = 0xE9;
            const int32_t rel32 = static_cast<int32_t>(diff);
            std::memcpy(patched8 + 1, &rel32, sizeof(rel32));
            for (size_t i = 5; i < site.stealLen; ++i)
                patched8[i] = 0x90; // NOP padding through instruction boundary

            uint64_t patchedWord = 0;
            std::memcpy(&patchedWord, patched8, sizeof(patchedWord));

            DWORD oldProt = 0;
            if (!VirtualProtect(site.target, 8, PAGE_EXECUTE_READWRITE, &oldProt))
                return false;
            _InterlockedExchange64(reinterpret_cast<volatile LONG64*>(site.target), static_cast<LONG64>(patchedWord));
            DWORD dummy = 0;
            VirtualProtect(site.target, 8, oldProt, &dummy);
            FlushInstructionCache(GetCurrentProcess(), site.target, 8);
            site.patched = true;
            return true;
        }

        void UnpatchSite(HookSite& site) noexcept
        {
            if (!site.patched || !site.target)
                return;
            uint64_t origWord = 0;
            std::memcpy(&origWord, site.saved8, sizeof(origWord));
            DWORD oldProt = 0;
            if (VirtualProtect(site.target, 8, PAGE_EXECUTE_READWRITE, &oldProt))
            {
                _InterlockedExchange64(reinterpret_cast<volatile LONG64*>(site.target), static_cast<LONG64>(origWord));
                DWORD dummy = 0;
                VirtualProtect(site.target, 8, oldProt, &dummy);
                FlushInstructionCache(GetCurrentProcess(), site.target, 8);
            }
            site.patched = false;
        }

        struct SecondaryApplyCtx
        {
            void* pITrayUI = nullptr;
            int targetDip = 48;
            HMONITOR primaryMon = nullptr;
        };

        BOOL CALLBACK ApplySecondaryMonProc(HMONITOR hMon, HDC, LPRECT, LPARAM lp)
        {
            const auto* ctx = reinterpret_cast<const SecondaryApplyCtx*>(lp);
            if (!ctx || !ctx->pITrayUI || hMon == ctx->primaryMon)
                return TRUE;
            if (g_hook.fnUpdateSecondSize)
                g_hook.fnUpdateSecondSize(ctx->pITrayUI, ctx->targetDip, hMon);
            return TRUE;
        }

        void TriggerTrayUiCaptureOnCurrentThread() noexcept
        {
            HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
            if (tray && !g_hook.pITrayUI.load(std::memory_order_acquire))
            {
                WINDOWPOS wp{};
                wp.hwnd = tray;
                wp.flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE;
                SendMessageW(tray, WM_WINDOWPOSCHANGING, 0, reinterpret_cast<LPARAM>(&wp));
            }

            if (!g_hook.pITrayUI.load(std::memory_order_acquire))
            {
                APPBARDATA abd{ sizeof(abd) };
                SHAppBarMessage(ABM_GETTASKBARPOS, &abd);
            }

            if (tray && !g_hook.pITrayUI.load(std::memory_order_acquire))
            {
                SetWindowPos(tray, nullptr, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            }

            if (!g_hook.pITrayUI.load(std::memory_order_acquire))
            {
                ShellDiagLog(L"TopBarHook", L"TriggerTrayUiCapture failed to capture pITrayUI (tray=0x%p)", tray);
            }
        }

        bool ComputeExpectedEdgeRect(HMONITOR mon, UINT edge, int heightDip, RECT& outRect) noexcept
        {
            if (!mon)
                mon = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
            MONITORINFO mi{ sizeof(mi) };
            if (!GetMonitorInfoW(mon, &mi))
                return false;
            const UINT dpi = QueryMonitorDpi(mon);
            const int hPx = MulDiv(heightDip, static_cast<int>(dpi), 96);
            outRect = mi.rcMonitor;
            if (edge == 1u) // ABE_TOP
                outRect.bottom = outRect.top + hPx;
            else if (edge == 3u) // ABE_BOTTOM
                outRect.top = outRect.bottom - hPx;
            else if (edge == 0u) // ABE_LEFT
                outRect.right = outRect.left + hPx;
            else if (edge == 2u) // ABE_RIGHT
                outRect.left = outRect.right - hPx;
            return true;
        }

        // Scoped WH_CALLWNDPROC dispatch onto Shell_TrayWnd's UI thread
        struct TrayThreadDispatchCtx
        {
            DWORD targetTid = 0;
            UINT msg = 0;
            ULONG_PTR cookie = 0;
            std::function<void()> fn;
            std::atomic<bool> executed{ false };
            std::atomic<bool> completed{ false };
            std::atomic<bool> cancelled{ false };
        };

        std::mutex g_dispatchMutex;
        std::mutex g_activeCtxMutex;
        std::shared_ptr<TrayThreadDispatchCtx> g_activeDispatchCtx;
        std::atomic<ULONG_PTR> g_dispatchCookieSeq{ 0x57520001u };

        LRESULT CALLBACK TrayThreadHookProc(int code, WPARAM wp, LPARAM lp)
        {
            if (code == HC_ACTION && lp)
            {
                const auto* cwp = reinterpret_cast<const CWPSTRUCT*>(lp);
                std::shared_ptr<TrayThreadDispatchCtx> ctx;
                {
                    std::lock_guard<std::mutex> lock(g_activeCtxMutex);
                    ctx = g_activeDispatchCtx;
                }
                if (ctx && cwp->message == ctx->msg &&
                    static_cast<ULONG_PTR>(cwp->wParam) == ctx->cookie &&
                    GetCurrentThreadId() == ctx->targetTid &&
                    !ctx->cancelled.load(std::memory_order_acquire))
                {
                    if (!ctx->executed.exchange(true, std::memory_order_acq_rel))
                    {
                        if (ctx->fn)
                        {
                            try { ctx->fn(); }
                            catch (...) {}
                        }
                        ctx->completed.store(true, std::memory_order_release);
                    }
                }
            }
            return CallNextHookEx(nullptr, code, wp, lp);
        }
    } // namespace

    bool TopBarHook::Install()
    {
        if (g_hook.installed.load(std::memory_order_acquire))
            return true;

        HMODULE hTaskbar = GetModuleHandleW(L"Taskbar.dll");
        if (!hTaskbar)
        {
            ShellDiagLog(L"TopBarHook", L"Install: Taskbar.dll not loaded yet");
            return false;
        }

        BYTE* base = reinterpret_cast<BYTE*>(hTaskbar);
        if (!VerifyTaskbarPeAndSignatures(base, true))
        {
            g_hook.supported.store(false, std::memory_order_release);
            return false;
        }

        g_hook.supported.store(true, std::memory_order_release);
        g_hook.taskbarModule = hTaskbar;
        g_hook.fnGetStuckPlace = reinterpret_cast<GetStuckPlaceFn>(base + kRvaGetStuckPlace);
        g_hook.fnUpdateTaskbarSize = reinterpret_cast<UpdateTaskbarSizeFn>(base + kRvaUpdateTaskbarSize);
        g_hook.fnUpdateSecondSize = reinterpret_cast<UpdateSecondSizeFn>(base + kRvaUpdateSecondSize);

        if (!g_hook.trampolinePage)
            g_hook.trampolinePage = AllocateNearTrampolinePage(base);
        if (!g_hook.trampolinePage)
        {
            ShellDiagLog(L"TopBarHook", L"Install FAILED: AllocateNearTrampolinePage returned null (base=0x%p)", base);
            return false;
        }

        const intptr_t pageDelta = g_hook.trampolinePage - base;
        ShellDiagLog(L"TopBarHook", L"Trampoline page allocated at 0x%p (delta=%lld bytes)",
            g_hook.trampolinePage, static_cast<long long>(pageDelta));

        BYTE* cursor = g_hook.trampolinePage;
        const bool prepared =
            PrepareSite(g_hook.siteMinPrimary, base + kRvaGetMinSizePrimary, 5,
                reinterpret_cast<void*>(&Detour_GetMinSizePrimary), cursor) &&
            PrepareSite(g_hook.siteMinSecondary, base + kRvaGetMinSizeSecond, 5,
                reinterpret_cast<void*>(&Detour_GetMinSizeSecondary), cursor) &&
            PrepareSite(g_hook.siteDockedRect, base + kRvaGetDockedRect, 5,
                reinterpret_cast<void*>(&Detour_GetDockedRect), cursor) &&
            PrepareSite(g_hook.siteAutoHideFlags, base + kRvaGetAutoHideFlags, 6,
                reinterpret_cast<void*>(&Detour_GetAutoHideFlags), cursor);
        if (!prepared)
        {
            ShellDiagLog(L"TopBarHook", L"Install FAILED: PrepareSite out of rel32 range; freeing trampoline page");
            VirtualFree(g_hook.trampolinePage, 0, MEM_RELEASE);
            g_hook.trampolinePage = nullptr;
            return false;
        }

        // Assign trampoline pointers BEFORE writing detours so concurrent callers never see null orig*
        g_hook.origMinPrimary = reinterpret_cast<GetMinSizePrimaryFn>(g_hook.siteMinPrimary.trampoline);
        g_hook.origMinSecondary = reinterpret_cast<GetMinSizeSecondFn>(g_hook.siteMinSecondary.trampoline);
        g_hook.origDockedRect = reinterpret_cast<GetDockedRectFn>(g_hook.siteDockedRect.trampoline);
        g_hook.origAutoHideFlags = reinterpret_cast<GetAutoHideFlagsFn>(g_hook.siteAutoHideFlags.trampoline);
        FlushInstructionCache(GetCurrentProcess(), g_hook.trampolinePage, 0x1000);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        const bool p1 = PatchSite(g_hook.siteMinPrimary);
        const bool p2 = p1 && PatchSite(g_hook.siteMinSecondary);
        const bool p3 = p2 && PatchSite(g_hook.siteDockedRect);
        const bool p4 = p3 && PatchSite(g_hook.siteAutoHideFlags);

        if (!p4)
        {
            ShellDiagLog(L"TopBarHook", L"Install FAILED: PatchSite failed [%d,%d,%d,%d] err=%lu",
                p1 ? 1 : 0, p2 ? 1 : 0, p3 ? 1 : 0, p4 ? 1 : 0, GetLastError());
            UnpatchSite(g_hook.siteAutoHideFlags);
            UnpatchSite(g_hook.siteDockedRect);
            UnpatchSite(g_hook.siteMinSecondary);
            UnpatchSite(g_hook.siteMinPrimary);
            return false;
        }

        g_hook.installed.store(true, std::memory_order_release);
        ShellDiagLog(L"TopBarHook", L"Install SUCCEEDED (4/4 zero-WinRT hooks patched)");
        InstallSystemTrayHook();
        return true;
    }

    bool TopBarHook::InstallSystemTrayHook() noexcept
    {
        if (g_hook.sysTrayInstalled.load(std::memory_order_acquire))
            return true;

        HMODULE hSysTray = GetModuleHandleW(L"SystemTray.dll");
        if (!hSysTray)
            return false;

        BYTE* base = reinterpret_cast<BYTE*>(hSysTray);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000)
            return false;

        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt->FileHeader.TimeDateStamp != kExpectedSysTrayTimeDateStamp ||
            nt->OptionalHeader.SizeOfImage != kExpectedSysTraySizeOfImage)
        {
            return false;
        }

        if (std::memcmp(base + kRvaPositionOverflowWindow, kSigPositionOverflowWindow, 10) != 0)
            return false;

        if (!g_hook.sysTrayTrampolinePage)
            g_hook.sysTrayTrampolinePage = AllocateNearTrampolinePage(base);
        if (!g_hook.sysTrayTrampolinePage)
            return false;

        BYTE* cursor = g_hook.sysTrayTrampolinePage;
        if (!PrepareSite(g_hook.sitePositionOverflow, base + kRvaPositionOverflowWindow, 5,
                reinterpret_cast<void*>(&Detour_PositionOverflowWindow), cursor))
        {
            return false;
        }

        g_hook.origPositionOverflow = reinterpret_cast<PositionOverflowWindowFn>(g_hook.sitePositionOverflow.trampoline);
        FlushInstructionCache(GetCurrentProcess(), g_hook.sysTrayTrampolinePage, 0x1000);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (!PatchSite(g_hook.sitePositionOverflow))
            return false;

        g_hook.sysTrayInstalled.store(true, std::memory_order_release);
        ShellDiagLog(L"TopBarHook", L"SystemTray.dll PositionOverflowWindow hook installed (base=0x%p)", base);
        return true;
    }

    bool TopBarHook::RepositionTrayPopupIfNeeded(HWND hwndPopup, int preferredAnchorX) noexcept
    {
        if (!g_hook.topDockActive.load(std::memory_order_acquire) || !hwndPopup || !IsWindow(hwndPopup))
            return false;

        RECT rc{};
        if (!GetWindowRect(hwndPopup, &rc))
            return false;

        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0)
            return false;

        const bool hasExplicitAnchor = (preferredAnchorX != (-2147483647 - 1));
        const int centerX = hasExplicitAnchor ? preferredAnchorX : ((rc.left + rc.right) / 2);

        HMONITOR mon = MonitorFromPoint(POINT{ centerX, 0 }, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        if (!GetMonitorInfoW(mon, &mi))
            return false;

        const UINT dpi = QueryMonitorDpi(mon);
        const int heightDip = (std::clamp)(g_hook.heightDip.load(std::memory_order_acquire), 28, 48);
        int barBottomY = mi.rcMonitor.top + MulDiv(heightDip, static_cast<int>(dpi), 96);

        HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (tray && IsWindow(tray))
        {
            RECT rcTray{};
            if (GetWindowRect(tray, &rcTray) &&
                MonitorFromWindow(tray, MONITOR_DEFAULTTONEAREST) == mon &&
                rcTray.top <= mi.rcMonitor.top + 8 &&
                rcTray.bottom > mi.rcMonitor.top &&
                rcTray.bottom <= mi.rcMonitor.top + 120)
            {
                barBottomY = (std::max)(barBottomY, static_cast<int>(rcTray.bottom));
            }
        }

        const int gapPx = MulDiv(4, static_cast<int>(dpi), 96);
        const int targetY = barBottomY + gapPx;

        if (!hasExplicitAnchor && rc.top >= barBottomY && rc.top <= targetY + gapPx * 4)
            return false;

        int targetX = hasExplicitAnchor ? (preferredAnchorX - w / 2) : rc.left;
        const int minX = mi.rcMonitor.left + gapPx;
        const int maxX = (std::max)(minX, static_cast<int>(mi.rcMonitor.right) - w - gapPx);
        targetX = (std::clamp)(targetX, minX, maxX);

        if (rc.top < barBottomY || hasExplicitAnchor || rc.left != targetX)
        {
            SetWindowPos(hwndPopup, HWND_TOP, targetX, targetY, w, h,
                SWP_SHOWWINDOW | SWP_NOACTIVATE);
            ShellDiagLog(L"TopBarHook",
                L"Repositioned tray popup hwnd=0x%p from (%ld,%ld %dx%d) to (%d,%d) below top bar (barBottom=%d)",
                hwndPopup, rc.left, rc.top, w, h, targetX, targetY, barBottomY);
            return true;
        }
        return false;
    }

    bool TopBarHook::IsHooked() noexcept
    {
        return g_hook.installed.load(std::memory_order_acquire);
    }

    bool TopBarHook::IsSupportedBuild() noexcept
    {
        if (g_hook.supported.load(std::memory_order_acquire))
            return true;
        HMODULE hTaskbar = GetModuleHandleW(L"Taskbar.dll");
        return VerifyTaskbarPeAndSignatures(reinterpret_cast<BYTE*>(hTaskbar), false);
    }

    bool TopBarHook::IsTopDockActive() noexcept
    {
        return g_hook.topDockActive.load(std::memory_order_acquire);
    }

    bool TopBarHook::RunOnPrimaryTaskbarThread(HMODULE shellModule, std::function<void()> fn, DWORD timeoutMs)
    {
        if (!fn)
            return false;

        HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (!tray || !IsWindow(tray))
            return false;

        DWORD trayPid = 0;
        const DWORD trayTid = GetWindowThreadProcessId(tray, &trayPid);
        if (!trayTid || trayPid != GetCurrentProcessId())
            return false;

        if (trayTid == GetCurrentThreadId())
        {
            try { fn(); }
            catch (...) {}
            return true;
        }

        std::lock_guard<std::mutex> lock(g_dispatchMutex);
        static const UINT kRunMsg = RegisterWindowMessageW(L"WinRings.TopBar.RunOnTrayThread.v1");
        if (!kRunMsg)
            return false;

        auto ctx = std::make_shared<TrayThreadDispatchCtx>();
        ctx->targetTid = trayTid;
        ctx->msg = kRunMsg;
        ctx->cookie = g_dispatchCookieSeq.fetch_add(1, std::memory_order_relaxed);
        ctx->fn = std::move(fn);

        {
            std::lock_guard<std::mutex> activeLock(g_activeCtxMutex);
            g_activeDispatchCtx = ctx;
        }
        HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, TrayThreadHookProc, shellModule, trayTid);
        if (!hook)
        {
            ctx->cancelled.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> activeLock(g_activeCtxMutex);
                g_activeDispatchCtx.reset();
            }
            ShellDiagLog(L"TopBarHook", L"RunOnPrimaryTaskbarThread: SetWindowsHookExW failed err=%lu", GetLastError());
            return false;
        }

        DWORD_PTR res = 0;
        SendMessageTimeoutW(tray, kRunMsg, static_cast<WPARAM>(ctx->cookie), 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, timeoutMs, &res);

        UnhookWindowsHookEx(hook);
        ctx->cancelled.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> activeLock(g_activeCtxMutex);
            g_activeDispatchCtx.reset();
        }
        return ctx->completed.load(std::memory_order_acquire);
    }

    bool TopBarHook::ApplyOnTaskbarUiThread(bool enabled, bool dockTop, int heightDip, bool allMonitors)
    {
        if (!g_hook.installed.load(std::memory_order_acquire))
            return false;

        InstallSystemTrayHook();

        if (!g_hook.pITrayUI.load(std::memory_order_acquire))
            TriggerTrayUiCaptureOnCurrentThread();

        const int clampedDip = enabled ? (std::clamp)(heightDip, 28, 48)
                                       : g_hook.origSizeDip.load(std::memory_order_acquire);
        g_hook.active.store(enabled, std::memory_order_release);
        g_hook.topDockActive.store(enabled && dockTop, std::memory_order_release);
        g_hook.allMonitors.store(allMonitors, std::memory_order_release);
        g_hook.heightDip.store(clampedDip, std::memory_order_release);

        void* pITrayUI = g_hook.pITrayUI.load(std::memory_order_acquire);
        if (!pITrayUI)
        {
            ShellDiagLog(L"TopBarHook",
                L"Apply skipped: pITrayUI still null (enabled=%d dockTop=%d heightDip=%d)",
                enabled ? 1 : 0, dockTop ? 1 : 0, clampedDip);
            return false;
        }

        HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
        RECT rcBefore{};
        if (tray)
            GetWindowRect(tray, &rcBefore);

        auto* baseObj = reinterpret_cast<BYTE*>(pITrayUI);
        const UINT edgeBefore = g_hook.fnGetStuckPlace ? g_hook.fnGetStuckPlace(pITrayUI) : 99u;
        const int dipBefore = *reinterpret_cast<const int*>(baseObj + kOffPrimaryDip);

        HMONITOR primaryMon = *reinterpret_cast<HMONITOR*>(baseObj + kOffPrimaryMon);
        if (!primaryMon)
            primaryMon = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);

        MONITORINFO miBefore{ sizeof(miBefore) };
        GetMonitorInfoW(primaryMon, &miBefore);

        bool appliedOk = false;
        try
        {
            auto* vtable = *reinterpret_cast<void***>(pITrayUI);
            auto fnVerifySize = vtable ? reinterpret_cast<VerifySizeFn>(vtable[kVerifySizeVtableOff / sizeof(void*)]) : nullptr;
            auto fnSizeWindows = vtable ? reinterpret_cast<SizeWindowsFn>(vtable[kSizeWindowsVtableOff / sizeof(void*)]) : nullptr;

            // 1. Update primary & secondary height DIPs without entering WinRT SetStuckPlace.
            if (g_hook.fnUpdateTaskbarSize && dipBefore != clampedDip)
                g_hook.fnUpdateTaskbarSize(pITrayUI, clampedDip);
            if (*reinterpret_cast<const int*>(baseObj + kOffPrimaryDip) != clampedDip)
                *reinterpret_cast<int*>(baseObj + kOffPrimaryDip) = clampedDip;

            if (allMonitors && g_hook.fnUpdateSecondSize)
            {
                SecondaryApplyCtx ctx{};
                ctx.pITrayUI = pITrayUI;
                ctx.targetDip = clampedDip;
                ctx.primaryMon = primaryMon;
                EnumDisplayMonitors(nullptr, nullptr, ApplySecondaryMonProc, reinterpret_cast<LPARAM>(&ctx));
                if (*reinterpret_cast<const int*>(baseObj + kOffSecondaryDip) != clampedDip)
                    *reinterpret_cast<int*>(baseObj + kOffSecondaryDip) = clampedDip;
            }

            // 2. Set target edge cleanly on TrayUI's non-WinRT classic member [pITrayUI + 0x110],
            //    zero stuckRects[targetEdge] so VerifySize's EqualRect check cannot short-circuit,
            //    and invoke VerifySize (GFIDS-valid) to recompute stuckRects, SetWindowPos, and rcWork!
            const UINT targetEdge = (enabled && dockTop) ? 1u : g_hook.origStuckPlace.load(std::memory_order_acquire);
            if (targetEdge <= 3u)
            {
                *reinterpret_cast<UINT*>(baseObj + kOffStuckPlace) = targetEdge;
                auto* stuckRects = reinterpret_cast<RECT*>(baseObj + kOffStuckRects);
                SetRectEmpty(&stuckRects[targetEdge]);
            }

            if (fnVerifySize)
                fnVerifySize(pITrayUI, true, true, nullptr);

            // 3. Notify internal edge listeners safely on primary monitor & verify final rect.
            RECT expectedTargetRect{};
            const bool haveExpectedRect = ComputeExpectedEdgeRect(primaryMon, targetEdge, clampedDip, expectedTargetRect);
            if (tray && haveExpectedRect)
            {
                RECT rcNow{};
                GetWindowRect(tray, &rcNow);
                const bool visible = IsWindowVisible(tray) != FALSE;
                const bool rectMismatch =
                    std::abs(rcNow.left - expectedTargetRect.left) > 2 ||
                    std::abs(rcNow.top - expectedTargetRect.top) > 2 ||
                    std::abs(rcNow.right - expectedTargetRect.right) > 2 ||
                    std::abs(rcNow.bottom - expectedTargetRect.bottom) > 2;

                if (enabled && (rectMismatch || !visible))
                {
                    if (targetEdge <= 3u)
                        reinterpret_cast<RECT*>(baseObj + kOffStuckRects)[targetEdge] = expectedTargetRect;
                    const int w = expectedTargetRect.right - expectedTargetRect.left;
                    const int h = expectedTargetRect.bottom - expectedTargetRect.top;
                    SetWindowPos(tray, HWND_TOPMOST,
                        expectedTargetRect.left, expectedTargetRect.top, w, h,
                        SWP_NOACTIVATE | (dockTop ? SWP_SHOWWINDOW : 0u) | SWP_FRAMECHANGED);
                    if (fnSizeWindows)
                        fnSizeWindows(pITrayUI);
                }
                else if (!enabled)
                {
                    if (fnSizeWindows)
                        fnSizeWindows(pITrayUI);
                    if ((g_hook.origTrayState.load(std::memory_order_acquire) & 1u) != 0u)
                    {
                        // Let Explorer resume native auto-hide cleanly at the bottom edge.
                        PostMessageW(tray, WM_TIMER, 2, 0);
                    }
                }
            }

            RefreshSecondaryTaskbars();
            appliedOk = true;
        }
        catch (...)
        {
            ShellDiagLog(L"TopBarHook", L"ApplyOnTaskbarUiThread caught exception");
        }

        RECT rcAfter{};
        if (tray)
            GetWindowRect(tray, &rcAfter);
        MONITORINFO miAfter{ sizeof(miAfter) };
        GetMonitorInfoW(primaryMon, &miAfter);
        const UINT edgeAfter = g_hook.fnGetStuckPlace ? g_hook.fnGetStuckPlace(pITrayUI) : 99u;
        const int dipAfter = *reinterpret_cast<const int*>(baseObj + kOffPrimaryDip);
        const int visAfter = (tray && IsWindowVisible(tray)) ? 1 : 0;

        ShellDiagLog(L"TopBarHook",
            L"Apply(en=%d top=%d dip=%d allMon=%d ok=%d) edge:%u->%u dip:%d->%d rect:(%ld,%ld,%ld,%ld)->(%ld,%ld,%ld,%ld) work:(%ld,%ld,%ld,%ld)->(%ld,%ld,%ld,%ld) vis=%d",
            enabled ? 1 : 0, dockTop ? 1 : 0, clampedDip, allMonitors ? 1 : 0, appliedOk ? 1 : 0,
            edgeBefore, edgeAfter, dipBefore, dipAfter,
            rcBefore.left, rcBefore.top, rcBefore.right, rcBefore.bottom,
            rcAfter.left, rcAfter.top, rcAfter.right, rcAfter.bottom,
            miBefore.rcWork.left, miBefore.rcWork.top, miBefore.rcWork.right, miBefore.rcWork.bottom,
            miAfter.rcWork.left, miAfter.rcWork.top, miAfter.rcWork.right, miAfter.rcWork.bottom,
            visAfter);
        return appliedOk;
    }

    void TopBarHook::RefreshSecondaryTaskbars() noexcept
    {
        if (!g_hook.allMonitors.load(std::memory_order_acquire))
            return;
        HWND sec = nullptr;
        while ((sec = FindWindowExW(nullptr, sec, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr)
        {
            PostMessageW(sec, WM_SETTINGCHANGE, 0, 0);
        }
    }

    void TopBarHook::RestoreAndUnhook(const std::function<bool(std::function<void()>)>& runOnUiThread)
    {
        if (!g_hook.installed.load(std::memory_order_acquire))
            return;

        const int origDip = g_hook.origSizeDip.load(std::memory_order_acquire);
        bool ranOnUi = false;
        if (runOnUiThread)
        {
            ranOnUi = runOnUiThread([origDip]() {
                TopBarHook::ApplyOnTaskbarUiThread(false, false, origDip, true);
            });
        }
        if (!ranOnUi)
        {
            g_hook.active.store(false, std::memory_order_release);
            g_hook.topDockActive.store(false, std::memory_order_release);
        }

        g_hook.active.store(false, std::memory_order_release);
        g_hook.topDockActive.store(false, std::memory_order_release);

        for (int i = 0; i < 100 && g_hook.inFlight.load(std::memory_order_acquire) > 0; ++i)
            Sleep(1);

        UnpatchSite(g_hook.sitePositionOverflow);
        UnpatchSite(g_hook.siteAutoHideFlags);
        UnpatchSite(g_hook.siteDockedRect);
        UnpatchSite(g_hook.siteMinSecondary);
        UnpatchSite(g_hook.siteMinPrimary);

        g_hook.sysTrayInstalled.store(false, std::memory_order_release);
        g_hook.installed.store(false, std::memory_order_release);
        g_hook.pITrayUI.store(nullptr, std::memory_order_release);
        g_hook.capturedOriginal.store(false, std::memory_order_release);

        ShellDiagLog(L"TopBarHook", L"RestoreAndUnhook complete (ranOnUi=%d origDip=%d)",
            ranOnUi ? 1 : 0, origDip);
    }

    namespace
    {
        constexpr UINT_PTR kTrayTopLockSubclassId = 0x5752544Cull; // 'WRTL'
        std::atomic<bool> g_trayLockEnabled{ false };
        std::atomic<int> g_trayLockHeightDip{ 28 };
        std::atomic<HWND> g_trayLockedHwnd{ nullptr };
        std::atomic<bool> g_inTraySnap{ false };
        std::mutex g_trayWakeMutex;
        std::function<void()> g_trayWakeCallback;

        bool ComputeTrayTopMetrics(
            HWND tray,
            int clampedDip,
            MONITORINFO& outMi,
            int& outTargetPx,
            int& outMaxClampPx) noexcept
        {
            if (!tray || !IsWindow(tray))
                return false;
            HMONITOR hMon = MonitorFromWindow(tray, MONITOR_DEFAULTTOPRIMARY);
            outMi = MONITORINFO{ sizeof(outMi) };
            if (!GetMonitorInfoW(hMon, &outMi))
                return false;
            UINT dpi = GetDpiForWindow(tray);
            if (dpi < 72)
                dpi = QueryMonitorDpi(hMon);
            if (dpi < 72)
                dpi = 96;
            const int targetDip = (std::clamp)(clampedDip, 24, 48);
            outTargetPx = MulDiv(targetDip, static_cast<int>(dpi), 96);
            outMaxClampPx = MulDiv(72, static_cast<int>(dpi), 96);
            return true;
        }

        void SnapTrayWindowToTopIfNeeded(HWND tray) noexcept
        {
            if (!tray || !IsWindow(tray) ||
                !g_trayLockEnabled.load(std::memory_order_acquire) ||
                g_inTraySnap.load(std::memory_order_acquire))
            {
                return;
            }

            MONITORINFO mi{};
            int targetPx = 0;
            int maxClampPx = 0;
            RECT rc{};
            if (!GetWindowRect(tray, &rc) ||
                !ComputeTrayTopMetrics(tray, g_trayLockHeightDip.load(std::memory_order_acquire), mi, targetPx, maxClampPx))
            {
                return;
            }

            const int curH = rc.bottom - rc.top;
            const TopBarPosClampResult clamp = ClampTopDockedTaskbarWindowPos(
                true,
                mi.rcMonitor.top,
                mi.rcMonitor.bottom,
                targetPx,
                maxClampPx,
                true,
                true,
                rc.top,
                curH,
                rc.top,
                curH);

            if (clamp.modifiedMove || clamp.modifiedSize)
            {
                const int monW = mi.rcMonitor.right - mi.rcMonitor.left;
                g_inTraySnap.store(true, std::memory_order_release);
                SetWindowPos(
                    tray, nullptr,
                    mi.rcMonitor.left, clamp.y, monW, clamp.cy,
                    SWP_NOZORDER | SWP_NOACTIVATE);
                g_inTraySnap.store(false, std::memory_order_release);
            }
        }

        void InvokeTrayWakeCallback() noexcept
        {
            std::function<void()> cb;
            {
                std::lock_guard<std::mutex> lock(g_trayWakeMutex);
                cb = g_trayWakeCallback;
            }
            if (cb)
            {
                try { cb(); }
                catch (...) {}
            }
        }

        LRESULT CALLBACK TrayTopLockSubclassProc(
            HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
            UINT_PTR uIdSubclass, DWORD_PTR) noexcept
        {
            if (msg == WM_NCDESTROY)
            {
                RemoveWindowSubclass(hwnd, TrayTopLockSubclassProc, uIdSubclass);
                HWND expected = hwnd;
                g_trayLockedHwnd.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
                return DefSubclassProc(hwnd, msg, wp, lp);
            }

            if (msg == WM_WINDOWPOSCHANGING &&
                lp != 0 &&
                g_trayLockEnabled.load(std::memory_order_acquire) &&
                !g_inTraySnap.load(std::memory_order_acquire))
            {
                auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
                MONITORINFO mi{};
                int targetPx = 0;
                int maxClampPx = 0;
                RECT curRc{};
                if (GetWindowRect(hwnd, &curRc) &&
                    ComputeTrayTopMetrics(hwnd, g_trayLockHeightDip.load(std::memory_order_acquire), mi, targetPx, maxClampPx))
                {
                    const bool hasMove = (pos->flags & SWP_NOMOVE) == 0;
                    const bool hasSize = (pos->flags & SWP_NOSIZE) == 0;
                    const TopBarPosClampResult clamp = ClampTopDockedTaskbarWindowPos(
                        true,
                        mi.rcMonitor.top,
                        mi.rcMonitor.bottom,
                        targetPx,
                        maxClampPx,
                        hasMove,
                        hasSize,
                        pos->y,
                        pos->cy,
                        curRc.top,
                        curRc.bottom - curRc.top);

                    if (clamp.modifiedMove)
                    {
                        pos->y = clamp.y;
                        pos->flags &= ~SWP_NOMOVE;
                    }
                    if (clamp.modifiedSize)
                    {
                        pos->cy = clamp.cy;
                        pos->flags &= ~SWP_NOSIZE;
                    }
                }
            }

            if (msg == WM_DISPLAYCHANGE ||
                (msg == WM_POWERBROADCAST &&
                 (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND || wp == PBT_APMRESUMECRITICAL)))
            {
                const LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
                SnapTrayWindowToTopIfNeeded(hwnd);
                InvokeTrayWakeCallback();
                return r;
            }

            return DefSubclassProc(hwnd, msg, wp, lp);
        }
    } // namespace

    bool TopBarHook::SyncTrayTopLock(
        HMODULE shellModule,
        bool lockTop,
        int heightDip,
        std::function<void()> onTrayWakeOrDisplay)
    {
        g_trayLockHeightDip.store((std::clamp)(heightDip, 24, 48), std::memory_order_release);
        g_trayLockEnabled.store(lockTop, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(g_trayWakeMutex);
            g_trayWakeCallback = std::move(onTrayWakeOrDisplay);
        }

        if (!lockTop)
        {
            RemoveTrayTopLock(shellModule);
            return true;
        }

        auto ok = std::make_shared<std::atomic<bool>>(false);
        const bool dispatched = RunOnPrimaryTaskbarThread(shellModule, [ok]() {
            HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
            if (!tray || !IsWindow(tray))
                return;
            DWORD pid = 0;
            const DWORD tid = GetWindowThreadProcessId(tray, &pid);
            if (pid != GetCurrentProcessId() || tid != GetCurrentThreadId())
                return;

            const HWND prev = g_trayLockedHwnd.load(std::memory_order_acquire);
            if (prev && prev != tray && IsWindow(prev))
                RemoveWindowSubclass(prev, TrayTopLockSubclassProc, kTrayTopLockSubclassId);

            if (prev != tray)
            {
                if (SetWindowSubclass(tray, TrayTopLockSubclassProc, kTrayTopLockSubclassId, 0))
                {
                    g_trayLockedHwnd.store(tray, std::memory_order_release);
                    ShellDiagLog(L"TopBarHook", L"Installed Shell_TrayWnd top-lock subclass (hwnd=0x%p h=%d)",
                        tray, g_trayLockHeightDip.load(std::memory_order_acquire));
                    ok->store(true, std::memory_order_release);
                }
            }
            else
            {
                ok->store(true, std::memory_order_release);
            }

            SnapTrayWindowToTopIfNeeded(tray);
        }, 1500);

        return dispatched && ok->load(std::memory_order_acquire);
    }

    void TopBarHook::RemoveTrayTopLock(HMODULE shellModule)
    {
        g_trayLockEnabled.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(g_trayWakeMutex);
            g_trayWakeCallback = nullptr;
        }

        const HWND prev = g_trayLockedHwnd.load(std::memory_order_acquire);
        if (!prev)
            return;

        RunOnPrimaryTaskbarThread(shellModule, []() {
            const HWND locked = g_trayLockedHwnd.exchange(nullptr, std::memory_order_acq_rel);
            if (locked && IsWindow(locked))
            {
                DWORD pid = 0;
                const DWORD tid = GetWindowThreadProcessId(locked, &pid);
                if (pid == GetCurrentProcessId() && tid == GetCurrentThreadId())
                    RemoveWindowSubclass(locked, TrayTopLockSubclassProc, kTrayTopLockSubclassId);
            }
            HWND curTray = FindWindowW(L"Shell_TrayWnd", nullptr);
            if (curTray && curTray != locked && IsWindow(curTray))
            {
                DWORD pid = 0;
                const DWORD tid = GetWindowThreadProcessId(curTray, &pid);
                if (pid == GetCurrentProcessId() && tid == GetCurrentThreadId())
                    RemoveWindowSubclass(curTray, TrayTopLockSubclassProc, kTrayTopLockSubclassId);
            }
            if (locked)
                ShellDiagLog(L"TopBarHook", L"Removed Shell_TrayWnd top-lock subclass (hwnd=0x%p)", locked);
        }, 1500);
    }
} // namespace winrings
