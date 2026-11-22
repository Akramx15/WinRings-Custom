# CLAUDE.md

Read and follow [`AGENTS.md`](AGENTS.md) as the authoritative engineering and operational guide for **Akramx15/WinRings-Custom**.

## Critical Quick Rules

- **Never launch `WinRingsHost.exe` from an automated shell / sandbox.** Use `.\tools\Deploy-WinRings.ps1 -NoLaunch` for automated staging, and let the user run `.\tools\Deploy-WinRings.ps1` interactively on the desktop.
- **Compiler strictness:** MSVC `/std:c++17 /MT /O2 /EHsc /W4 /WX` — zero warnings, zero `.NET` / CLR dependencies.
- **Preserve user config:** Never overwrite `%APPDATA%\WinRings\config.json` `pinnedSlots`.
- **Mandatory verification after edits:**
  1. `powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\Build-WinRings.ps1`
  2. `& .\build\tests\WinRingsTests.exe`
  3. Verify `WinRingsShell.dll` export `WinRings_GetApi` via `LoadLibraryExW` + `GetProcAddress`.
- **Upstream tracking:** `origin` is `Akramx15/WinRings-Custom`; `upstream` is `AhmadotEng/WinRings`. Keep custom modules (`TopBarHook`, `XamlTaskbarCtrl`, `FullWorkAreaCtrl`, `GestureEngine`, `MediaIsland`, `ClockFormat.h`) intact when syncing upstream updates.
- **Language:** Explain changes and summaries to Akram in clear, natural Arabic while keeping code symbols, paths, CLI flags, and UI labels in English.
