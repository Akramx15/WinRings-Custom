# WinRings-Custom Features (`v3.0.7.0`)

`WinRings-Custom` combines **upstream `WinRings v3.0.0`** (`WCA` native acrylic + 3D beveled glass edges, `m_outsideClickPending` click-vs-drag guard, `isRingDragActive` reorder guard, `explorer.exe` folder icon fix, slim core binary) with Akram's unified Windows 11 desktop enhancements:

---

## 1. Unified Minimal Top Bar (`TopBarHook` + `XamlTaskbarCtrl` + `ClockFormat.h`)

- **Top Edge Docking & Zero-Flash Sleep/Wake Lock:** Moves the native Windows 11 taskbar to the top monitor edge (`28–40 DIP` configurable height), updates the desktop work area cleanly, and uses a synchronous `Shell_TrayWnd` `WM_WINDOWPOSCHANGING` clamp (`SyncTrayTopLock`) plus immediate `0 ms` wake/display/unlock resync (`OnWakeOrUnlockImmediate` / `TriggerImmediateTopBarWakeSync`) so resuming from Sleep never briefly drops the taskbar to the bottom edge.
- **Single-Line Compact Clock:** Custom date/time format (`topBarClockFormat`) and casing (`CompactLower`, `Default`, `Lowercase`, `Uppercase`) with live preview in Settings Page 2 (**Top Bar & Media Island**).
- **Selective Chrome Hiding:** Independently hide Taskbar App Buttons, Start Button, Search, Task View, and Widgets while keeping the native System Tray functional.
- **Dynamic Tray Telemetry:** Queries primary `SystemTrayFrame.ActualWidth()` via `XamlTaskbarCtrl::GetSystemTrayWidthDip()` and notifies `MediaIsland` immediately on layout shifts.
- **Quick Toggles:** Accessible via Ring right-click menu (`Toggle Top Minimal Bar`), tray menu, and CLI (`WinRingsHost.exe --toggle-topbar`).

---

## 2. FullWorkArea Integration (`FullWorkAreaCtrl`)

- **Extend Maximized Windows Behind Top Bar:** Dynamically removes artificial top reservation so maximized apps utilize full vertical screen space (`fullWorkAreaEnabled`) on Windows 11 26H2 (`26300+`).
- **Post-Sleep/Wake & Unlock Zero-Drop Refit:** Subscribes to Modern Standby `GUID_CONSOLE_DISPLAY_STATE` display-on power broadcasts in both `FullWorkAreaCtrl` and `WinRingsHost`, removes wake burst throttling, extends wake follow-ups through `+1400 ms`, and performs zero-flicker `DWMWA_EXTENDED_FRAME_BOUNDS`-compensated `SetWindowPos` refit (`WINRINGS_ADMIN_ACT_REFIT_MAXIMIZED` relay for elevated Admin windows such as Antigravity) whenever `rcWork` reset pushes maximized apps down below the top bar.
- **Pill Click-Through & Auto-Hide Sync:** Works with floating pills and notifies `MediaIsland` whenever the taskbar auto-hides (`OnTaskbarVisibilityChanged`), plus optional `Win+\`` hotkey toggle (`fullWorkAreaHotkeys`).

---

## 3. Dynamic Media Island (`MediaIsland`)

- **GSMTC Now-Playing Pill:** Displays album art thumbnail, title, and artist inside the Top Bar height using Windows `GlobalSystemMediaTransportControlsSessionManager`.
- **Hover Card Expansion:** Expanding on cursor hover exposes compact Previous / Play-Pause / Next controls (`mediaIslandHoverExpand`), optional scrolling marquee for long titles (`mediaIslandMarquee`), and hide-when-paused (`mediaIslandHideWhenPaused`).
- **Smart Dynamic Tray-Avoidance & Auto-Shrink:** Uses real `SystemTrayFrame.ActualWidth()` so the pill never overlaps crowded notification tray icons on `1280x800` displays, shifting left and compacting width proportionally when space is tight.
- **`Alt + Drag` Repositioning + Reset:** Hold `Alt` and drag the pill horizontally to save a custom percentage (`mediaIslandOffsetXPct`), or reset via right-click menu / **Reset Island Position** in Settings.
- **Right-Click Context Menu:** Right-clicking Media Island opens `Hide Media Island`, `Reset Position`, and `Settings...`; also toggleable anytime from Tray (`Show Media Island`), Ring menu, or `--toggle-media-island`.
- **Scroll-Wheel Master Volume:** Scrolling vertically over the Media Island pill—or anywhere along the full top bar height directly above it (`y = 0 .. barPx`)—adjusts Windows master volume by `±1%` per fine step (`mediaIslandScrollVolume`), supporting standard mice (`WH_MOUSE_LL`), `WM_POINTERWHEEL`, and `ELAN06FA` Precision Touchpad two-finger HID scroll (`RIM_TYPEHID`) with duplicate-event filtering.
- **Fullscreen Modes (`mediaIslandFullscreenMode`):**
  - `SyncWithTaskbar` (default): Hides automatically when a fullscreen window (e.g. YouTube/browser video or borderless fullscreen app) covers the primary monitor above `Shell_TrayWnd` in Z-order, or when the taskbar hides.
  - `GamesOnly`: Hides only for fullscreen fullscreen apps/games outside `SyncWithTaskbar` triggers.
  - `NeverHide`: Stays visible above fullscreen apps.

---

## 4. Gesture & Trigger Extensions (`GestureEngine` + `InputEngine`)

- **Win Key Start Replacement (`winKeyTrigger`):** Pressing Windows key opens WinRings instead of Start Menu (deployed alongside HKCU Open-Shell neutralization).
- **Direct Window Switch (`directSwitchEnabled`):** `Alt + F` (configurable to `Win + F` or `Alt + Q`) cycles windows immediately without rendering a switcher popup, freezing MRU order until modifier release.
- **Touchpad Physical Left-Click, Corner Tap & 2-Finger Volume (`ELAN06FA` HID):** Parses raw HID touchpad button & contact coordinates to distinguish physical left-click button presses from touchpad tap-to-click (`touchpadPhysLeftClickTrigger`), optional corner taps (`touchpadCornerTrigger` with serial multi-contact tracking and `350 ms` idle reset), and 2-finger scroll over Media Island, with automatic `GetRegisteredRawInputDevices` self-healing.
- **2D Mouse Shake (`shakeTriggerEnabled`):** Runs directly inside `InputEngine` (`WH_MOUSE_LL`, cached DPI scale, fixed circular buffer, zero Win32 API calls per pixel, `selfInjected`-only filter so PTP `LLMHF_INJECTED` cursor moves work reliably, live `GetAsyncKeyState` button check, and UIPI-aware cursor-heartbeat watchdog recovery that avoids false reinstalls while an elevated/Admin window is focused).
- **GNOME Hot Corner (`hotCornerEnabled`):** Triggers Task View (`Win+Tab`) or WinRings only when flicking with deliberate velocity/pressure into the configured top corner (`Top-Left` / `Top-Right`).

---

## 5. Radial Ring Customizations & Drag-to-Snap (`RingOverlay` + `WindowSnapCtrl` + `SettingsDialog`)

- **Drag-to-Snap / Split-Screen (`dragToSnapEnabled`):**
  - Dragging an **outer-arc Window Card** or **unpinned running Ring slot** immediately dismisses the ring, restores the window cleanly (`WINDOWPLACEMENT` size-aware), and moves it **live with the cursor** at up to ~60Hz via decoupled async `SetWindowPos(..., SWP_ASYNCWINDOWPOS)` (virtually 0% extra CPU/RAM, zero new hooks, zero blocked `WH_MOUSE_LL` callbacks) while rendering a translucent Fluent edge preview (`Left`, `Right`, `Maximize`).
  - Dragging a **pinned Ring slot** inside the ring reorders pins normally; pulling it past `outerRadius + dragToSnapTearOffDip` after `dragToSnapHoldMs` cancels temporary reordering cleanly and live-drags the open app—or optionally launches & auto-snaps a closed pinned app/folder/Recycle Bin/website (`dragToSnapLaunchClosed`) via `HSHELL_*` events (zero polling).
  - Releasing on a screen edge commits immediately with `WM_LBUTTONUP` preserved (never swallowed) and deferred snap execution: by default (`dragToSnapUseSnapAssist = false`), **Direct Pixel-Perfect Snap** compensates DWM extended frame borders (`DWMWA_EXTENDED_FRAME_BOUNDS`) via `ComputeDirectSnapRect` and sets immediate foreground focus with **zero stolen clicks**; toggling `dragToSnapUseSnapAssist = true` uses native `Win + Arrow` + Windows 11 Snap Assist. Releasing outside any edge drops the window cleanly at its live position.
- **Window Card `X` Close Button:** Clicking the `X` close badge on hovered outer-arc window cards closes that window immediately without activating it.
- **Ring, Window Card & Search Arc Context Actions:** Right-click main slots/windows for **Run as Administrator**, **Close Window**, **End Task** (`TerminateProcessByPid`), **Toggle Top Minimal Bar**, and **Show Media Island**; right-clicking any result in the top **Search Arc** (`m_searchArcActive`) preserves the arc while offering **`Open`**, **`Run as Administrator`**, **`Open File Location`** (resolving `.lnk` shortcuts and UWP `shell:AppsFolder` targets), and **`Pin to Ring` / `Unpin from Ring`** (smart `.lnk`-to-`.exe` resolution + duplicate pin detection).
- **Selection Indicator Styles (`selectionIndicator`):** `GlowOnly`, `SectorWedge` (restored classic v1 blue wedge accent), or `PointerLine`.
- **Ultra-Fast No-Blur Mode (`disableRingBlur`):** Fully disables native `WCA` backdrop (`SW_HIDE`), `Windows::UI::Composition` blur, and synthetic grain noise (`useNoise = false`) for solid high-contrast rendering and minimal GPU work.

---

## 6. Full Administrator Window Support (`ElevationUtil.h` + `WinRingsHost` Admin Bridge)

- **Zero-UAC Elevated Host Task (`WinRings_Host_Elevated`):** `Deploy-WinRings.ps1` registers an interactive Scheduled Task (`RunLevel = Highest`) so resident `WinRingsHost.exe` starts elevated automatically on logon without UAC prompts, while `WinRingsHost.manifest` stays `asInvoker` for fast CLI commands and `explorer.exe` remains strictly Medium-IL.
- **Selective Admin Input Bridge:** While an elevated window holds focus (or lies under the cursor for mouse input) and the Ring overlay is closed, elevated `WinRingsHost.exe` captures global `WH_MOUSE_LL`, `WH_KEYBOARD_LL`, `RAWMOUSE` hot-corner, and `ELAN06FA` touchpad HID events via `InputEngine` + `GestureEngine` and signals `explorer.exe` (`AllowSetForegroundWindow`), letting `RingOverlay::OpenAt` claim focus cleanly so typing/clicking inside the Ring works seamlessly.
- **Cross-IL Window Action Relay (`WINRINGS_COPYDATA_WINDOW_ACTION`):** When activating, minimizing, closing (`X` button / menu), terminating (`End Task`), or live-dragging/snapping (`WindowSnapCtrl`) an elevated window from Medium-IL `explorer.exe`, `ShellController::RequestHostWindowAction` relays an authenticated `WM_COPYDATA` request (verified by session `attemptCookie` and Explorer PID) to elevated `WinRingsHost.exe`.
