# WinRings-Custom Features (`v3.0.2.0`)

`WinRings-Custom` combines **upstream `WinRings v3.0.0`** (`WCA` native acrylic + 3D beveled glass edges, `m_outsideClickPending` click-vs-drag guard, `isRingDragActive` reorder guard, `explorer.exe` folder icon fix, slim core binary) with Akram's unified Windows 11 desktop enhancements:

---

## 1. Unified Minimal Top Bar (`TopBarHook` + `XamlTaskbarCtrl` + `ClockFormat.h`)

- **Top Edge Docking:** Moves the native Windows 11 taskbar to the top monitor edge (`28–40 DIP` configurable height) and updates the desktop work area cleanly.
- **Single-Line Compact Clock:** Custom date/time format (`topBarClockFormat`) and casing (`CompactLower`, `Default`, `Lowercase`, `Uppercase`) with live preview in Settings Page 2 (**Top Bar & Media Island**).
- **Selective Chrome Hiding:** Independently hide Taskbar App Buttons, Start Button, Search, Task View, and Widgets while keeping the native System Tray functional.
- **Dynamic Tray Telemetry:** Queries primary `SystemTrayFrame.ActualWidth()` via `XamlTaskbarCtrl::GetSystemTrayWidthDip()` and notifies `MediaIsland` immediately on layout shifts.
- **Quick Toggles:** Accessible via Ring right-click menu (`Toggle Top Minimal Bar`), tray menu, and CLI (`WinRingsHost.exe --toggle-topbar`).

---

## 2. FullWorkArea Integration (`FullWorkAreaCtrl`)

- **Extend Maximized Windows Behind Top Bar:** Dynamically removes artificial top reservation so maximized apps utilize full vertical screen space (`fullWorkAreaEnabled`) on Windows 11 26H2 (`26300+`).
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
- **Touchpad Physical Left-Click, Corner Tap & 2-Finger Volume (`ELAN06FA` HID):** Parses raw HID touchpad button & contact coordinates to distinguish physical left-click button presses from touchpad tap-to-click (`touchpadPhysLeftClickTrigger`), optional corner taps (`touchpadCornerTrigger`), and 2-finger scroll over Media Island, with automatic `GetRegisteredRawInputDevices` self-healing.
- **2D Mouse Shake (`shakeTriggerEnabled`):** Runs directly inside `InputEngine` (`WH_MOUSE_LL`, cached DPI scale, fixed circular buffer, zero Win32 API calls per pixel) so Volume/Quick Settings flyouts can never steal it; recognizes horizontal, diagonal (thumb shake), and circular shake motions without accidental false triggers.
- **GNOME Hot Corner (`hotCornerEnabled`):** Triggers Task View (`Win+Tab`) or WinRings only when flicking with deliberate velocity/pressure into the configured top corner (`Top-Left` / `Top-Right`).

---

## 5. Radial Ring Customizations & Drag-to-Snap (`RingOverlay` + `WindowSnapCtrl` + `SettingsDialog`)

- **Drag-to-Snap / Split-Screen (`dragToSnapEnabled`):**
  - Dragging an **outer-arc Window Card** or **unpinned running Ring slot** immediately dismisses the ring, restores & attaches the window under the cursor, and triggers Windows 11 native live drag (`SC_MOVE`, engaging Aero Snap & **Snap Assist**).
  - Dragging a **pinned Ring slot** inside the ring reorders pins normally; pulling it past `outerRadius + dragToSnapTearOffDip` after `dragToSnapHoldMs` cancels temporary reordering cleanly and snaps the open app—or optionally launches & auto-snaps a closed pinned app/folder/Recycle Bin/website (`dragToSnapLaunchClosed`) via `HSHELL_*` events (zero polling).
  - If a custom-chrome/elevated window declines cross-process `SC_MOVE`, WinRings automatically switches within `65 ms` to a translucent Fluent edge-snap preview (`Left`, `Right`, `Maximize`) and triggers native `Win + Arrow` on release.
- **Window Card `X` Close Button:** Clicking the `X` close badge on hovered outer-arc window cards closes that window immediately without activating it.
- **Ring & Card Context Actions:** Right-click slots/windows for **Run as Administrator**, **Close Window**, **End Task** (`TerminateProcessByPid`), **Toggle Top Minimal Bar**, and **Show Media Island**.
- **Selection Indicator Styles (`selectionIndicator`):** `GlowOnly`, `SectorWedge` (restored classic v1 blue wedge accent), or `PointerLine`.
- **Ultra-Fast No-Blur Mode (`disableRingBlur`):** Fully disables native `WCA` backdrop (`SW_HIDE`), `Windows::UI::Composition` blur, and synthetic grain noise (`useNoise = false`) for solid high-contrast rendering and minimal GPU work.
