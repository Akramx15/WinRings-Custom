$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot

function Find-VcVars64 {
    $candidates = @(
        'C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat'
    )
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($vsPath) {
            $candidates = @((Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat')) + $candidates
        }
    }
    foreach ($p in $candidates) {
        if ($p -and (Test-Path $p)) { return $p }
    }
    throw "Could not locate VS 2022 vcvars64.bat"
}

$vcvars = Find-VcVars64

$dirs = @(
    "$root\build\obj\tests",
    "$root\build\obj\shell",
    "$root\build\obj\host",
    "$root\build\obj\tools",
    "$root\build\obj\setup",
    "$root\build\bin",
    "$root\build\gen",
    "$root\build\tests",
    "$root\build\stage\WinRings",
    "$root\build\stage\WinRingsSetup"
)
foreach ($d in $dirs) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}

function Invoke-VcBat([string]$ScriptBody, [string]$Tag) {
    $batPath = "$root\build\obj\run_$Tag.bat"
    try {
        [System.IO.File]::WriteAllText($batPath, $ScriptBody, [System.Text.Encoding]::ASCII)
        & cmd.exe /c $batPath
        if ($LASTEXITCODE -ne 0) {
            throw "Build step '$Tag' failed with exit code $LASTEXITCODE"
        }
    }
    finally {
        if (Test-Path $batPath) {
            Remove-Item -Force $batPath -ErrorAction SilentlyContinue
        }
    }
}

$cflags = '/nologo /std:c++17 /O2 /MT /EHsc /W4 /WX /guard:cf /Brepro /utf-8 /DUNICODE /D_UNICODE'
$lflags = '/CETCOMPAT /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /GUARD:CF /Brepro'

Write-Host '==> [1/5] Building WinRingsTests.exe'
$bat1 = @"
@echo off
call "$vcvars" >nul || exit /b 1
cl $cflags /Fo"$root\build\obj\tests\\" "$root\src\shared\WinRingsConfig.cpp" "$root\tests\WinRingsTests.cpp" /link /SUBSYSTEM:CONSOLE $lflags /OUT:"$root\build\tests\WinRingsTests.exe" user32.lib advapi32.lib shell32.lib ole32.lib || exit /b 1
"@
Invoke-VcBat $bat1 'tests'

Write-Host '==> [2/5] Building WinRingsShell.dll'
$bat2 = @"
@echo off
call "$vcvars" >nul || exit /b 1
rc /nologo /fo "$root\build\obj\shell\WinRingsShell.res" "$root\src\shell\WinRingsShell.rc" || exit /b 1
cl $cflags /DWINRINGS_SHELL_EXPORTS /Fo"$root\build\obj\shell\\" "$root\src\shared\WinRingsConfig.cpp" "$root\src\shell\AcrylicBackdrop.cpp" "$root\src\shell\AppIndex.cpp" "$root\src\shell\WindowTracker.cpp" "$root\src\shell\SlotModel.cpp" "$root\src\shell\DwmThumbPool.cpp" "$root\src\shell\InputEngine.cpp" "$root\src\shell\FullWorkAreaCtrl.cpp" "$root\src\shell\GestureEngine.cpp" "$root\src\shell\MediaIsland.cpp" "$root\src\shell\WindowSnapCtrl.cpp" "$root\src\shell\DropTarget.cpp" "$root\src\shell\RingOverlay.cpp" "$root\src\shell\TopBarHook.cpp" "$root\src\shell\XamlTaskbarCtrl.cpp" "$root\src\shell\ShellController.cpp" "$root\src\shell\DllMain.cpp" /link /DLL /DEF:"$root\src\shell\WinRingsShell.def" /SUBSYSTEM:WINDOWS $lflags /OUT:"$root\build\bin\WinRingsShell.dll" "$root\build\obj\shell\WinRingsShell.res" d3d11.lib d2d1.lib dwrite.lib dcomp.lib dxgi.lib dxguid.lib dwmapi.lib windowscodecs.lib winhttp.lib hid.lib ole32.lib oleaut32.lib shell32.lib shlwapi.lib uxtheme.lib propsys.lib version.lib comctl32.lib user32.lib gdi32.lib advapi32.lib windowsapp.lib runtimeobject.lib || exit /b 1
"@
Invoke-VcBat $bat2 'shell'

Write-Host '==> [3/5] Building WinRingsHost.exe'
$bat3 = @"
@echo off
call "$vcvars" >nul || exit /b 1
rc /nologo /fo "$root\build\obj\host\WinRingsHost.res" "$root\src\host\WinRingsHost.rc" || exit /b 1
cl $cflags /Fo"$root\build\obj\host\\" "$root\src\shared\WinRingsConfig.cpp" "$root\src\host\WinRingsHost.cpp" /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:"$root\src\host\WinRingsHost.manifest" $lflags /OUT:"$root\build\bin\WinRingsHost.exe" "$root\build\obj\host\WinRingsHost.res" user32.lib shell32.lib advapi32.lib wtsapi32.lib bcrypt.lib shlwapi.lib ole32.lib || exit /b 1
"@
Invoke-VcBat $bat3 'host'

Write-Host '==> [4/5] Building WinRingsTools.exe'
$bat4 = @"
@echo off
call "$vcvars" >nul || exit /b 1
rc /nologo /fo "$root\build\obj\tools\WinRingsTools.res" "$root\src\tools\WinRingsTools.rc" || exit /b 1
cl $cflags /Fo"$root\build\obj\tools\\" "$root\src\shared\WinRingsConfig.cpp" "$root\src\tools\SettingsDialog.cpp" "$root\src\tools\WinRingsTools.cpp" /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:"$root\src\tools\WinRingsTools.manifest" $lflags /OUT:"$root\build\bin\WinRingsTools.exe" "$root\build\obj\tools\WinRingsTools.res" d3d11.lib d2d1.lib dwrite.lib dxgi.lib dxguid.lib dwmapi.lib windowscodecs.lib comctl32.lib comdlg32.lib shell32.lib shlwapi.lib ole32.lib uxtheme.lib user32.lib gdi32.lib advapi32.lib || exit /b 1
"@
Invoke-VcBat $bat4 'tools'

Write-Host '==> [5/5] Generating SHA-256 payload manifest & building WinRingsSetup.exe'
$hashHost  = (Get-FileHash -Algorithm SHA256 "$root\build\bin\WinRingsHost.exe").Hash.ToLowerInvariant()
$hashShell = (Get-FileHash -Algorithm SHA256 "$root\build\bin\WinRingsShell.dll").Hash.ToLowerInvariant()
$hashTools = (Get-FileHash -Algorithm SHA256 "$root\build\bin\WinRingsTools.exe").Hash.ToLowerInvariant()

$header = @"
#pragma once

#define IDR_PAYLOAD_HOST  101
#define IDR_PAYLOAD_SHELL 102
#define IDR_PAYLOAD_TOOLS 103

inline constexpr wchar_t kHashWinRingsHost[]  = L"$hashHost";
inline constexpr wchar_t kHashWinRingsShell[] = L"$hashShell";
inline constexpr wchar_t kHashWinRingsTools[] = L"$hashTools";
"@
[System.IO.File]::WriteAllText("$root\build\gen\payload_hashes.h", $header, [System.Text.Encoding]::UTF8)

$bat5 = @"
@echo off
call "$vcvars" >nul || exit /b 1
rc /nologo /fo "$root\build\obj\setup\WinRingsSetup.res" "$root\src\setup\WinRingsSetup.rc" || exit /b 1
cl $cflags /Fo"$root\build\obj\setup\\" "$root\src\setup\WinRingsSetup.cpp" /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:"$root\src\setup\WinRingsSetup.manifest" $lflags /OUT:"$root\build\bin\WinRingsSetup.exe" "$root\build\obj\setup\WinRingsSetup.res" bcrypt.lib comctl32.lib shell32.lib shlwapi.lib ole32.lib dwmapi.lib uxtheme.lib user32.lib gdi32.lib advapi32.lib || exit /b 1
"@
Invoke-VcBat $bat5 'setup'

Write-Host '==> Staging deliverables'
Copy-Item -Force "$root\build\bin\WinRingsHost.exe"  "$root\build\stage\WinRings\WinRingsHost.exe"
Copy-Item -Force "$root\build\bin\WinRingsShell.dll" "$root\build\stage\WinRings\WinRingsShell.dll"
Copy-Item -Force "$root\build\bin\WinRingsTools.exe" "$root\build\stage\WinRings\WinRingsTools.exe"
Copy-Item -Force "$root\build\bin\WinRingsSetup.exe" "$root\build\stage\WinRingsSetup\WinRingsSetup.exe"

Write-Host 'Build-WinRings: SUCCEEDED'
Get-ChildItem "$root\build\stage" -Recurse -File | ForEach-Object {
    "{0,-60} {1,10:N0} bytes" -f $_.FullName.Substring($root.Length + 1), $_.Length
}
