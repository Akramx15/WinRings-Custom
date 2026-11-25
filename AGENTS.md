# WinRings-Custom — AI Agent Instructions & Engineering Playbook

This file is the **single source of truth** for any AI coding assistant (Codex, Claude Code, Antigravity, GitHub Copilot, Cursor, etc.) working on **Akramx15/WinRings-Custom**.

---

## 1. Repository Identity & Upstream Lineage

- **Fork (`origin`):** `https://github.com/Akramx15/WinRings-Custom` (`main` branch, currently **`v3.0.6.0`**)
- **Upstream (`upstream`):** `https://github.com/AhmadotEng/WinRings` (synced from release tag **`v3.0.0`**, commit `df4012a`)
- **Pre-Upgrade Backup Branch:** `v2.0.1-custom-backup` (`8cf2b11`) — historical reference for the v2 custom fork before the v3 upgrade.

`WinRings-Custom` takes the ultra-lightweight pure native **C++17** (`x64`, `/MT`, zero `.NET`) core of **WinRings v3.0.0** (improved `WCA` Acrylic + 3D beveled glass edges, `m_outsideClickPending` click-vs-drag guard, `isRingDragActive` reorder protection, fixed `explorer.exe` folder icons, trimmed drop bloat) and integrates Akram's unified top-bar & gesture customizations into a single low-overhead shell extension.

---

## 2. Target Machine & Environment

- **OS:** Windows 11 x64 26H2 (Build `26300`)
- **Primary Display:** `1280x800`
- **Touchpad Hardware:** Precision Touchpad (`ELAN06FA` HID collection)
- **Toolchain:** Visual Studio / Build Tools 2022 (`MSVC v143` x64 + Windows 11 SDK), auto-discovered by [`tools/Build-WinRings.ps1`](tools/Build-WinRings.ps1) via `vswhere` and standard `C:\BuildTools` paths.
- **Compiler Flags:** `/std:c++17 /MT /O2 /EHsc /W4 /WX` (warnings are build-breaking errors; zero CLR/managed code).
- **Live User Config:** `%APPDATA%\WinRings\config.json`
- **Live Diagnostic Log:** `%APPDATA%\WinRings\shell_diag.log`

---

## 3. Architecture & Custom Module Map

| Binary | Role |
| :--- | :--- |
| **`WinRingsHost.exe`** | Resident elevated Admin Input & Window Action Bridge (`WinRings_Host_Elevated` zero-UAC task, hosts `InputEngine` + `GestureEngine` when elevated windows block Medium-IL `explorer.exe` hooks and executes cross-IL `WM_COPYDATA` window actions), crash-loop circuit breaker, Explorer injector, CLI (`--status`, `--toggle`, `--toggle-topbar`, `--toggle-media-island`, `--settings`, `--reload`, `--preview`, `--reset-safety`, `--exit`). |
| **`WinRingsShell.dll`** | In-process `explorer.exe` DLL hosting the radial switcher and all unified shell features (`ShellController.cpp`). Exported entry points: `WinRings_GetApi`, `WinRings_RunPreview`, `DllGetClassObject`, `DllCanUnloadNow`. |
| **`WinRingsTools.exe`** | Dark-mode Win32 + Direct2D 1.1 5-page Settings GUI (`1040x760`) with live 1:1 GPU preview. |

### Custom Fork Modules (on top of Upstream `v3.0.0`)

1. **Unified Minimal Top Bar** (`src/shell/TopBarHook.*`, `src/shell/XamlTaskbarCtrl.*`, `src/shared/ClockFormat.h`):
   - Docks Windows 11 taskbar cleanly to the top edge (`28–40 DIP`), supports single-line compact clock formatting/casing, selective hiding of app buttons / Start / Search / Task View / Widgets, backdrop styles, live XAML `SystemTrayFrame.ActualWidth()` reporting, and synchronous `Shell_TrayWnd` `WM_WINDOWPOSCHANGING` top-lock + immediate wake/display/unlock sync so Sleep/Wake never flashes the bar to the bottom.
2. **FullWorkArea Integration** (`src/shell/FullWorkAreaCtrl.*`):
   - Extends maximized windows behind the minimal top bar without corrupting work area or Z-order, with pill click-through, `0 ms` wake/display recovery, and `Win+\`` hotkey support.
3. **Media Island** (`src/shell/MediaIsland.*`):
   - Compact Dynamic Island media pill in the top bar powered by WinRT GSMTC (`GlobalSystemMediaTransportControlsSessionManager`).
   - Features hover expansion card, dynamic tray-width avoidance + progressive shrink (`XamlTaskbarCtrl::GetSystemTrayWidthDip()`), `Alt+Drag` horizontal repositioning (`mediaIslandOffsetXPct`), right-click context menu, scroll-wheel master volume (`IAudioEndpointVolume` via `WM_MOUSEWHEEL`, `WM_POINTERWHEEL`, `WH_MOUSE_LL`, and `ELAN06FA` 2-finger HID reports), full top-bar-height hit-testing (`y=0..barPx`), and Z-order fullscreen detection (`SyncWithTaskbar`, `GamesOnly`, `NeverHide`).
4. **Gesture, Switcher & Drag-to-Snap Enhancements** (`src/shell/GestureEngine.*`, `src/shell/InputEngine.*`, `src/shell/WindowTracker.*`, `src/shell/WindowSnapCtrl.*`, `src/shell/RingOverlay.*`):
   - `winKeyTrigger` (Start Menu replacement via Win key, paired with Open-Shell neutralization)
   - `directSwitch` (`Alt+F` / `Win+F` / `Alt+Q` instant cycling with MRU freeze)
   - Raw Input touchpad physical left-click, corner-tap, and 2-finger Media Island volume scroll (`ELAN06FA` HID parsing with serial multi-contact continuation tracking, `350 ms` idle state reset, and periodic + resume-triggered `EnsureDeviceRegistration` auto-recovery)
   - Ultra-lightweight 2D vector mouse shake trigger in `InputEngine` (`WH_MOUSE_LL`, only filtering `selfInjected` `kWinRingsInjectedTag` events so PTP `LLMHF_INJECTED` moves are preserved, live `GetAsyncKeyState` button check, and UIPI-aware cursor-heartbeat silent hook eviction recovery in `ShellController` that ignores elevated/admin windows and secure desktops)
   - GNOME-style hot-corner velocity/pressure flick
   - Drag-to-Snap / Split-Screen (`WindowSnapCtrl`) from Ring icons (`Tear-off` for pinned, immediate drag for unpinned) and Window Cards: lightweight decoupled ~60Hz `SWP_ASYNCWINDOWPOS` live window tracking + Fluent edge ghost preview, deferred zero-swallowed `WM_LBUTTONUP` release, DWM border-compensated **Direct Snap** (`ApplyDirectSnap`, default `dragToSnapUseSnapAssist = false` for immediate foreground focus without Snap Assist click theft) or optional `Win+Arrow` Snap Assist (`dragToSnapUseSnapAssist = true`), plus optional closed pinned app launch & event-driven snap.
   - Ring window card `X` close button, right-click `Run as Administrator` & `End Task`, **Search Arc (`m_searchArcActive`) right-click menu** (`Open`, `Run as Administrator`, `Open File Location` with `.lnk` target / UWP package resolution, and `Pin to Ring` / `Unpin from Ring`), `Toggle Top Minimal Bar` & `Show Media Island` toggles, `SelectionIndicatorStyle` (`GlowOnly`, `SectorWedge`, `PointerLine`), and `disableRingBlur` performance mode.
   - **Elevated / Admin Window Support (`src/shared/ElevationUtil.h` + `WinRingsHost.cpp`):** Keeps `explorer.exe` Medium-IL while registering `WinRings_Host_Elevated` (`RunLevel = Highest`) at logon with zero UAC prompts (`WinRingsHost.manifest` remains `asInvoker` for CLI calls). When an elevated window holds focus or lies under the cursor while the Ring is closed, elevated `WinRingsHost.exe` captures `WH_MOUSE_LL`/`WH_KEYBOARD_LL`/`WM_INPUT` via `ShouldAdminBridgeInterceptInput` and commands `ShellController` via `AllowSetForegroundWindow`; `RingOverlay::OpenAt` claims foreground focus when opened over an elevated window so typing/clicking inside the Ring works natively; `WindowTracker` & `WindowSnapCtrl` relay Activate/Minimize/Close/End Task/Drag-to-Snap operations on elevated windows via authenticated `WM_COPYDATA` (`WINRINGS_COPYDATA_WINDOW_ACTION`).

---

## 4. Non-Negotiable Operational Rules & Pitfalls

1. **Sandbox / `exebox` Isolation Rule (CRITICAL):**
   - **NEVER launch `WinRingsHost.exe` from inside an agent sandbox shell (`run_command`)**.
   - Inside an isolated/sandboxed desktop session, `Shell_TrayWnd` reports invisible `0,0,0,0`, Explorer injection/UI state does not attach to the user's real interactive desktop, and starting `WinRingsHost.exe` can hold the host single-instance mutex prematurely.
   - Always use `powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\Deploy-WinRings.ps1 -NoLaunch` during automated staging, then instruct the user to run `.\tools\Deploy-WinRings.ps1` directly in their interactive desktop terminal.
2. **Preserve User `pinnedSlots` in `config.json`:**
   - **NEVER** overwrite `%APPDATA%\WinRings\config.json` with defaults. When adding new config fields on upgrade, read existing JSON first, inject defaults only for missing keys, and keep `pinnedSlots` intact.
3. **Lightweight Performance Budget:**
   - No `.NET`, no managed runtimes, `/MT` static CRT only.
   - Avoid background polling timers (`MediaIsland` uses an event-driven GSMTC worker + lightweight `350ms` `GetForegroundWindow` Z-order check only while active).
   - Respect `disableRingBlur`: when enabled, explicitly hide `m_wcaBackdropHwnd`, skip `m_acrylicBackdrop`, and force `useNoise = false` in both `RingOverlay.cpp` and `SettingsDialog.cpp`.
4. **Strict `/W4 /WX` Discipline:**
   - Zero compiler warnings tolerated. Explicitly cast `LONG` / `size_t` operands in `std::clamp` / `std::min` / `std::max` calls and avoid shadowed locals (e.g. `slotCount` in `RingOverlay.cpp`).
5. **Mouse Wheel, Touchpad HID & Raw Input Reliability (`MediaIsland` + `InputEngine` + `GestureEngine`):**
   - `WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW` popups (`MediaIsland`) need both `WM_NCHITTEST -> HTCLIENT`, `WM_POINTERWHEEL`, top-edge full-bar hit-testing (`monTop..monTop+barPx`), `InputEngine` `onMediaIslandWheel` (`WH_MOUSE_LL`), and `ELAN06FA` 2-finger `RIM_TYPEHID` routing (with 80ms dedupe) so mouse wheels AND Precision Touchpads adjust volume reliably.
   - `RegisterRawInputDevices` inside `explorer.exe` is per-process and can be stolen by Volume / Quick Settings flyouts or File Explorer. Keep Mouse Shake inside `WH_MOUSE_LL` (`InputEngine`), and keep `GestureEngine::EnsureDeviceRegistration()` wired to the 1s watchdog so `hotCorner` & `touchpad HID` recover automatically.
6. **`SystemTrayFrame` & Z-Order Fullscreen Nuances:**
   - Primary taskbar tray icons on `1280x800` can expand past horizontal center; `MediaIsland` MUST query `XamlTaskbarCtrl::GetSystemTrayWidthDip()` dynamically rather than hardcoding static right bounds.
   - Browser/video fullscreen on Windows 11 leaves `Shell_TrayWnd` visible behind the fullscreen window; fullscreen hiding checks **both** taskbar visibility (`FullWorkAreaCtrl`) and whether an unrelated fullscreen window sits **above** `Shell_TrayWnd` in Z-order.
   - In `WinRingsHost.cpp`, `TaskbarCreated` MUST reset `g_runtime.retryIndex = 0` so Explorer restarts re-inject cleanly.
   - In `GestureEngine.cpp`, do **not** suppress shakes simply because `QUNS_BUSY` coincides with temporary cursor hiding (`CURSOR_SHOWING == 0`).
7. **Open-Shell Safe Stopping:**
   - In scripts, match Open-Shell strictly by `-Name 'StartMenu'` with path `*Open-Shell*`. Never use `StartMenu*` wildcard (it would match Windows `StartMenuExperienceHost`).

---

## 5. Mandatory Build & Verification Gate

After **any** C++ or build script modification, ALL THREE checks must pass before committing or claiming success:

1. **Full Release Build (`/W4 /WX`):**
   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\Build-WinRings.ps1
   ```
   Expected: `Build-WinRings: SUCCEEDED` (exit code `0`).

2. **Unit & Config Round-Trip Tests:**
   ```powershell
   & .\build\tests\WinRingsTests.exe
   ```
   Expected: `WinRingsTests: ALL PASSED` (exit code `0`).

3. **Shell DLL Load & Export Check:**
   Verify `build\stage\WinRings\WinRingsShell.dll` loads cleanly with `LoadLibraryExW` (`DONT_RESOLVE_DLL_REFERENCES` or standard load) and resolves `WinRings_GetApi` via `GetProcAddress` with `Win32Err=0` and non-zero pointer.

---

## 6. Syncing New Upstream `AhmadotEng/WinRings` Releases

When merging a future upstream tag (e.g., `v3.1.0`):

1. `git fetch upstream --tags`
2. Compare changed upstream files against our fork delta (`git log --oneline upstream/main..main` or diff against custom modules list above).
3. Keep custom standalone files (`ClockFormat.h`, `TopBarHook.*`, `XamlTaskbarCtrl.*`, `FullWorkAreaCtrl.*`, `GestureEngine.*`, `MediaIsland.*`) and merge upstream improvements into shared/shell/host files (`WinRingsConfig.*`, `WinRingsApi.h`, `WinRingsHost.cpp`, `WindowTracker.*`, `InputEngine.*`, `RingOverlay.*`, `ShellController.cpp`, `SettingsDialog.cpp`, `Build-WinRings.ps1`, `WinRingsTests.cpp`).
4. Run the **Mandatory Build & Verification Gate** (Section 5), bump `WINRINGS_PRODUCT_VERSION_*` in `src/shared/WinRingsIdentity.h`, update `docs/CUSTOM_FEATURES.md` & `docs/KNOWN_ISSUES.md`, commit cleanly, and push to `origin`.

---

## 7. Communication Preferences

- When communicating with **Akram**, write explanations, diagnostics, and summaries in **clear, natural Arabic**.
- Keep all C++ symbol names, JSON keys, CLI flags, UI element labels, and file paths in **English** with clickable markdown links.
