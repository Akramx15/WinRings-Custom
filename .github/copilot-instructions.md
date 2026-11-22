# GitHub Copilot Instructions — WinRings-Custom

Please read and follow [`AGENTS.md`](../AGENTS.md) in the repository root before suggesting or applying changes.

## Essentials

- Pure native C++17 (`x64`, static `/MT` CRT, zero `.NET`), compiled with `/W4 /WX` via [`tools/Build-WinRings.ps1`](../tools/Build-WinRings.ps1).
- Do **not** launch `WinRingsHost.exe` inside sandboxed shells; stage with `tools/Deploy-WinRings.ps1 -NoLaunch`.
- Always run `tools/Build-WinRings.ps1` and `build\tests\WinRingsTests.exe` after edits, and preserve `%APPDATA%\WinRings\config.json` `pinnedSlots`.
- Fork base is `AhmadotEng/WinRings` (`upstream` `v3.0.0`), extending `WinRingsShell.dll` with Unified Top Minimal Bar, `FullWorkAreaCtrl`, `MediaIsland`, `GestureEngine`, and custom Ring enhancements documented in [`docs/CUSTOM_FEATURES.md`](../docs/CUSTOM_FEATURES.md) and [`docs/KNOWN_ISSUES.md`](../docs/KNOWN_ISSUES.md).
