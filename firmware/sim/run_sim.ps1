<#
.SYNOPSIS
  Build and launch the StackChan LVGL 9.5 + SDL2 Desktop Simulator (320x240).
.EXAMPLE
  .\run_sim.ps1
  .\run_sim.ps1 -Sandbox
  .\run_sim.ps1 -Zoom 3
  .\run_sim.ps1 -BuildOnly
#>
param(
    [switch]$Sandbox,
    [double]$Zoom = 2.0,
    [switch]$BuildOnly,
    [switch]$Clean,
    [int]$SmokeMs = 0
)

$ErrorActionPreference = "Stop"
$SimDir   = $PSScriptRoot
$BuildDir = Join-Path $SimDir "build"

if ($Clean -and (Test-Path $BuildDir)) {
    Remove-Item -Recurse -Force $BuildDir
}

# Locate CMake & Ninja (prefer IDF 6.1 tools, fallback to C:\Espressif or PATH)
$CmakeCandidates = @(
    "C:\Users\Nicolas\xs-dev\espressif-tools-6.1\tools\cmake\*\bin\cmake.exe",
    "C:\Espressif\tools\cmake\*\bin\cmake.exe"
)
$NinjaCandidates = @(
    "C:\Users\Nicolas\xs-dev\espressif-tools-6.1\tools\ninja\*\ninja.exe",
    "C:\Espressif\tools\ninja\*\ninja.exe"
)

$CmakeExe = (Get-Command cmake -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source)
if (-not $CmakeExe) {
    foreach ($pat in $CmakeCandidates) {
        $found = Get-Item $pat -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) { $CmakeExe = $found.FullName; break }
    }
}
if (-not $CmakeExe) { throw "CMake not found. Install CMake or ESP-IDF tools." }

$NinjaExe = (Get-Command ninja -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source)
if (-not $NinjaExe) {
    foreach ($pat in $NinjaCandidates) {
        $found = Get-Item $pat -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) { $NinjaExe = $found.FullName; break }
    }
}

# Locate Visual Studio vcvars64.bat from an installation that actually includes MSVC C++ tools
$VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$VcVars  = $null
if (Test-Path $VsWhere) {
    $vsPaths = & $VsWhere -all -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPaths) {
        $vsPaths = & $VsWhere -all -products * -property installationPath
    }
    foreach ($vp in $vsPaths) {
        $candidate = Join-Path $vp "VC\Auxiliary\Build\vcvars64.bat"
        $msvcDir   = Join-Path $vp "VC\Tools\MSVC"
        if ((Test-Path $candidate) -and (Test-Path $msvcDir)) { $VcVars = $candidate; break }
    }
}
if (-not $VcVars) {
    throw "Visual Studio C++ build tools (vcvars64.bat) not found."
}

$CmakeDir = Split-Path $CmakeExe -Parent
$ExtraPath = $CmakeDir
$GenArg = ""
if ($NinjaExe) {
    $ExtraPath = "$ExtraPath;$(Split-Path $NinjaExe -Parent)"
    $GenArg = "-G Ninja"
}

Write-Host "[StackChan Sim] Configuring & building..." -ForegroundColor Cyan
$Cmd = "set `"PATH=$ExtraPath;%PATH%`" && call `"$VcVars`" >nul && cmake -S `"$SimDir`" -B `"$BuildDir`" $GenArg -DCMAKE_BUILD_TYPE=Release && cmake --build `"$BuildDir`""
cmd /c $Cmd
if ($LASTEXITCODE -ne 0) {
    throw "Simulator build failed (exit code $LASTEXITCODE)."
}

$Exe = Join-Path $BuildDir "stackchan_sim.exe"
if (-not (Test-Path $Exe)) {
    $Exe = Join-Path $BuildDir "Release\stackchan_sim.exe"
}
if (-not (Test-Path $Exe)) {
    throw "stackchan_sim.exe not found in $BuildDir"
}

if ($BuildOnly) {
    Write-Host "[StackChan Sim] Build OK: $Exe" -ForegroundColor Green
    exit 0
}

$ArgsList = @("--zoom", "$Zoom")
if ($Sandbox) { $ArgsList += "--sandbox" }
if ($SmokeMs -gt 0) { $ArgsList += @("--smoke-ms", "$SmokeMs") }

Write-Host "[StackChan Sim] Launching $Exe $($ArgsList -join ' ')" -ForegroundColor Green
& $Exe @ArgsList
