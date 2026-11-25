[CmdletBinding()]
param(
    [switch]$NoLaunch,
    [switch]$SkipBuild,
    [string]$SourceDir,
    [switch]$NoAdmin
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$identityPath = Join-Path $repoRoot 'src\shared\WinRingsIdentity.h'
if (-not (Test-Path -LiteralPath $identityPath)) {
    throw "Missing identity header: $identityPath"
}

$identityText = Get-Content -LiteralPath $identityPath -Raw -Encoding UTF8
if ($identityText -match 'WINRINGS_PRODUCT_VERSION_STR_W\s+L"([^"]+)"') {
    $version = $Matches[1]
} else {
    $version = '3.0.7.0'
}

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$currentUser = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
$elevatedTaskName = 'WinRings_Host_Elevated'

if (-not $SourceDir) {
    if (-not $SkipBuild) {
        Write-Host "==> Building WinRings v$version..."
        & "$PSScriptRoot\Build-WinRings.ps1"
        if ($LASTEXITCODE -ne 0) {
            throw "Build-WinRings.ps1 failed with exit code $LASTEXITCODE"
        }
    }
    $SourceDir = Join-Path $repoRoot 'build\stage\WinRings'
}

$requiredBins = @('WinRingsHost.exe', 'WinRingsShell.dll', 'WinRingsTools.exe')
foreach ($bin in $requiredBins) {
    $srcFile = Join-Path $SourceDir $bin
    if (-not (Test-Path -LiteralPath $srcFile)) {
        throw "Required build artifact missing: $srcFile"
    }
}

$installRoot = Join-Path $env:LOCALAPPDATA 'WinRings'
$installDir = Join-Path $installRoot "v$version"

$openShellKey = 'HKCU:\Software\OpenShell\StartMenu\Settings'
if (Test-Path -LiteralPath $openShellKey) {
    Set-ItemProperty -Path $openShellKey -Name 'WinKey' -Value 'Nothing' -Type String -Force
    Set-ItemProperty -Path $openShellKey -Name 'ShiftWin' -Value 'Nothing' -Type String -Force
    Set-ItemProperty -Path $openShellKey -Name 'EnableStartButton' -Value 0 -Type DWord -Force
    Write-Host "==> Enforced Open-Shell WinKey neutralization in HKCU"
}

if (-not $NoLaunch) {
    Write-Host "==> Stopping running WinRingsHost instances..."
    $runningHosts = Get-Process -Name 'WinRingsHost' -ErrorAction SilentlyContinue
    if ($runningHosts) {
        foreach ($proc in $runningHosts) {
            try {
                if ($proc.Path -and (Test-Path -LiteralPath $proc.Path)) {
                    Start-Process -FilePath $proc.Path -ArgumentList '--exit' -WindowStyle Hidden -Wait -ErrorAction SilentlyContinue
                }
            } catch {
            }
        }
        Start-Sleep -Milliseconds 600
    }

    $leftoverHosts = Get-Process -Name 'WinRingsHost' -ErrorAction SilentlyContinue
    if ($leftoverHosts) {
        foreach ($proc in $leftoverHosts) {
            try {
                Stop-Process -Id $proc.Id -Force -ErrorAction Stop
            } catch {
                # Old host may be elevated; elevate only the process termination helper so Explorer/Host stay medium-IL.
                Start-Process -FilePath 'powershell.exe' -Verb RunAs -Wait -WindowStyle Hidden `
                    -ArgumentList '-NoProfile', '-Command', "Stop-Process -Name WinRingsHost -Force -ErrorAction SilentlyContinue"
                break
            }
        }
        Start-Sleep -Milliseconds 400
    }

    # Stop Open-Shell StartMenu.exe cleanly (never touch Windows StartMenuExperienceHost).
    $openShellProcs = Get-Process -Name 'StartMenu' -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -and $_.Path -like '*Open-Shell*'
    }
    if ($openShellProcs) {
        $openShellProcs | Stop-Process -Force -ErrorAction SilentlyContinue
    }

    $safetyKey = 'HKCU:\Software\WinRings\HostSafety'
    if (Test-Path -LiteralPath $safetyKey) {
        Set-ItemProperty -Path $safetyKey -Name 'InjectionDisabled' -Value 0 -Type DWord -Force
        Set-ItemProperty -Path $safetyKey -Name 'AbnormalExplorerExitCount' -Value 0 -Type DWord -Force
        Write-Host "==> Reset HostSafety circuit breaker"
    }

    Write-Host "==> Restarting explorer.exe to unload any pinned WinRingsShell.dll..."
    Stop-Process -Name 'explorer' -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800

    # Clean up any residual uninstalled Open-Shell autostart entries or leftover locked DLL folder.
    foreach ($runPath in @('HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Run', 'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run')) {
        if (Test-Path -LiteralPath $runPath) {
            try {
                Remove-ItemProperty -LiteralPath $runPath -Name 'Open-Shell Start Menu' -Force -ErrorAction SilentlyContinue
            } catch {
            }
        }
    }
    $openShellDir = 'C:\Program Files\Open-Shell'
    if ((Test-Path -LiteralPath $openShellDir) -and -not (Test-Path -LiteralPath (Join-Path $openShellDir 'StartMenu.exe'))) {
        try {
            Remove-Item -LiteralPath $openShellDir -Recurse -Force -ErrorAction SilentlyContinue
            Write-Host "==> Removed residual Open-Shell directory"
        } catch {
        }
    }
}

Write-Host "==> Staging binaries to $installDir..."
New-Item -ItemType Directory -Path $installDir -Force | Out-Null
foreach ($bin in $requiredBins) {
    Copy-Item -LiteralPath (Join-Path $SourceDir $bin) -Destination (Join-Path $installDir $bin) -Force
}

$hostExe = Join-Path $installDir 'WinRingsHost.exe'

$startMenuShortcut = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\WinRings.lnk'
$wshell = New-Object -ComObject WScript.Shell
$shortcut = $wshell.CreateShortcut($startMenuShortcut)
$shortcut.TargetPath = $hostExe
$shortcut.WorkingDirectory = $installDir
$shortcut.Description = "WinRings Custom v$version"
$shortcut.Save()

if ($NoLaunch) {
    Write-Host "Deploy-WinRings: STAGED ONLY (NoLaunch) -> $installDir"
    exit 0
}

# Wait cleanly for Windows (winlogon) to respawn Medium-IL explorer.exe; NEVER launch explorer.exe elevated.
$explorerWait = 0
while (-not (Get-Process -Name 'explorer' -ErrorAction SilentlyContinue) -and $explorerWait -lt 15) {
    Start-Sleep -Milliseconds 200
    $explorerWait++
}
if (-not (Get-Process -Name 'explorer' -ErrorAction SilentlyContinue)) {
    Write-Host "==> Starting unelevated explorer.exe..."
    $null = & schtasks.exe /run /tn '\Microsoft\Windows\Shell\CreateExplorerShellUnelevatedTask' 2>$null
    $explorerWait = 0
    while (-not (Get-Process -Name 'explorer' -ErrorAction SilentlyContinue) -and $explorerWait -lt 10) {
        Start-Sleep -Milliseconds 200
        $explorerWait++
    }
    if (-not (Get-Process -Name 'explorer' -ErrorAction SilentlyContinue) -and -not $isAdmin) {
        Start-Process -FilePath 'explorer.exe'
        Start-Sleep -Seconds 2
    }
}

$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
if (-not $NoAdmin) {
    if (Test-Path -LiteralPath $runKey) {
        Remove-ItemProperty -LiteralPath $runKey -Name 'WinRings' -Force -ErrorAction SilentlyContinue
    }

    $escapedHost = $hostExe.Replace("'", "''")
    $escapedDir = $installDir.Replace("'", "''")
    $escapedUser = $currentUser.Replace("'", "''")
    $escapedTask = $elevatedTaskName.Replace("'", "''")

    $taskScript = @"
`$ErrorActionPreference = 'Stop'
`$action = New-ScheduledTaskAction -Execute '$escapedHost' -WorkingDirectory '$escapedDir'
`$trigger = New-ScheduledTaskTrigger -AtLogOn -User '$escapedUser'
`$principal = New-ScheduledTaskPrincipal -UserId '$escapedUser' -LogonType Interactive -RunLevel Highest
`$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -Priority 5
Register-ScheduledTask -TaskName '$escapedTask' -Action `$action -Trigger `$trigger -Principal `$principal -Settings `$settings -Force | Out-Null
Start-ScheduledTask -TaskName '$escapedTask'
"@

    Write-Host "==> Registering & starting elevated Scheduled Task '$elevatedTaskName' (zero UAC at login)..."
    if ($isAdmin) {
        Invoke-Expression $taskScript
    } else {
        $bytes = [System.Text.Encoding]::Unicode.GetBytes($taskScript)
        $encoded = [Convert]::ToBase64String($bytes)
        Start-Process -FilePath 'powershell.exe' -Verb RunAs -Wait -WindowStyle Hidden `
            -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', $encoded
    }

    Write-Host "Deploy-WinRings: SUCCEEDED (Admin Bridge Enabled via '$elevatedTaskName') -> $installDir"
} else {
    $existingTask = Get-ScheduledTask -TaskName $elevatedTaskName -ErrorAction SilentlyContinue
    if ($existingTask) {
        if ($isAdmin) {
            Unregister-ScheduledTask -TaskName $elevatedTaskName -Confirm:$false -ErrorAction SilentlyContinue
        } else {
            $escapedTask = $elevatedTaskName.Replace("'", "''")
            Start-Process -FilePath 'powershell.exe' -Verb RunAs -Wait -WindowStyle Hidden `
                -ArgumentList '-NoProfile', '-Command', "Unregister-ScheduledTask -TaskName '$escapedTask' -Confirm:`$false -ErrorAction SilentlyContinue"
        }
    }

    New-Item -Path $runKey -Force | Out-Null
    Set-ItemProperty -Path $runKey -Name 'WinRings' -Value "`"$hostExe`"" -Type String -Force

    Write-Host "==> Launching WinRingsHost.exe v$version (standard user mode)..."
    Start-Process -FilePath $hostExe -WorkingDirectory $installDir
    Write-Host "Deploy-WinRings: SUCCEEDED -> $installDir"
}
