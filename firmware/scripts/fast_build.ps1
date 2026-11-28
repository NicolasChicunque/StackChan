<#
.SYNOPSIS
  Fast incremental ESP-IDF 6.1 build & app-flash helper for StackChan with ccache and modular app presets.
.DESCRIPTION
  Accelerates real ESP32-S3 firmware iteration by:
    1. Enabling ccache (IDF_CCACHE_ENABLE=1) automatically so unchanged C/C++ objects compile in milliseconds.
    2. Allowing single-app (-App) or minimal (-Pack) builds via STACKCHAN_ENABLE_APP_* CMake flags.
    3. Using `idf.py app-flash` by default (skips bootloader/partitions/assets -> ~3-10s flash).
    4. Using high-speed flashing (-b 921600) on USB-Serial-JTAG.
.EXAMPLE
  .\fast_build.ps1 -App music -Flash -Port COM3
  .\fast_build.ps1 -Pack suite -Flash -Monitor
  .\fast_build.ps1 -Pack minimal
  .\fast_build.ps1 -App speaker -DryRun
#>
param(
    [ValidateSet("", "music", "alarm", "gemini", "mcp", "speaker", "fx", "health", "games", "deck", "nfc", "home", "calendar", "english", "trakt", "files", "companion", "sentinel")]
    [string]$App = "",

    [ValidateSet("suite", "labs", "minimal")]
    [string]$Pack = "suite",

    [switch]$Flash,
    [switch]$FullFlash,
    [switch]$Monitor,
    [string]$Port = "COM3",
    [int]$Baud = 921600,
    [string]$BuildDir = "",
    [switch]$NoCcache,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

$FirmwareDir  = Split-Path $PSScriptRoot -Parent
$IdfPath      = if ($env:IDF_PATH) { $env:IDF_PATH } else { "C:\Users\Nicolas\xs-dev\esp-idf-v6.1" }
$IdfToolsPath = if ($env:IDF_TOOLS_PATH) { $env:IDF_TOOLS_PATH } else { "C:\Users\Nicolas\xs-dev\espressif-tools-6.1" }

$AllApps = @(
    "MUSIC", "ALARM", "GEMINI", "MCP", "SPEAKER",
    "FX", "HEALTH", "GAMES", "DECK", "NFC", "HOME",
    "CALENDAR", "ENGLISH", "TRAKT", "FILES", "COMPANION", "SENTINEL"
)
$SuiteApps = @("MUSIC", "ALARM", "GEMINI", "MCP", "SPEAKER")
$LabsApps  = @("FX", "HEALTH", "GAMES", "DECK", "NFC", "HOME", "CALENDAR", "ENGLISH", "TRAKT", "FILES", "COMPANION", "SENTINEL")

$EnabledApps = @()
if ($App -ne "") {
    $EnabledApps = @($App.ToUpper())
    $PresetLabel = "single-$App"
} else {
    switch ($Pack) {
        "suite"   { $EnabledApps = $SuiteApps; $PresetLabel = "suite" }
        "labs"    { $EnabledApps = $LabsApps;  $PresetLabel = "labs" }
        "minimal" { $EnabledApps = @();        $PresetLabel = "minimal" }
    }
}

$CmakeFlags = @()
foreach ($a in $AllApps) {
    $val = if ($EnabledApps -contains $a) { "ON" } else { "OFF" }
    $CmakeFlags += "-DSTACKCHAN_ENABLE_APP_${a}=${val}"
}

# Separate build dir per preset avoids CMake reconfiguration thrashing when switching packs
if ($BuildDir -eq "") {
    if ($PresetLabel -eq "suite") {
        $BuildDir = "build"
    } else {
        $BuildDir = "build-$PresetLabel"
    }
}

$IdfArgs = @("-B", $BuildDir) + $CmakeFlags

if ($FullFlash) {
    $IdfArgs += @("-p", $Port, "-b", "$Baud", "flash")
} elseif ($Flash) {
    $IdfArgs += @("-p", $Port, "-b", "$Baud", "app-flash")
} else {
    $IdfArgs += "build"
}

if ($Monitor) {
    if (-not $Flash -and -not $FullFlash) {
        $IdfArgs += @("-p", $Port)
    }
    $IdfArgs += "monitor"
}

$CcacheEnv = if ($NoCcache) { "set IDF_CCACHE_ENABLE=0" } else { "set IDF_CCACHE_ENABLE=1" }
$IdfCmd    = "idf.py " + ($IdfArgs -join " ")
$FullCmd   = "set IDF_TOOLS_PATH=$IdfToolsPath&& $CcacheEnv&& call `"$IdfPath\export.bat`" && cd /d `"$FirmwareDir`" && $IdfCmd"

Write-Host "[StackChan FastBuild] Preset: $PresetLabel | BuildDir: $BuildDir | ccache: $(-not $NoCcache)" -ForegroundColor Cyan
Write-Host "[StackChan FastBuild] Command: $IdfCmd" -ForegroundColor DarkGray

if ($DryRun) {
    Write-Host "[StackChan FastBuild] DryRun mode — exiting without building." -ForegroundColor Yellow
    exit 0
}

cmd /c $FullCmd
exit $LASTEXITCODE
