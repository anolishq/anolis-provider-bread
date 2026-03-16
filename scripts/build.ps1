#!/usr/bin/env pwsh
# Build anolis-provider-bread via CMake presets.
#
# Usage:
#   .\scripts\build.ps1 [options] [-- <extra-cmake-configure-args>]
#
# Options:
#   -Preset <name>     Configure/build preset (default: dev-windows-foundation-debug on Windows,
#                      dev-foundation-debug otherwise)
#   -Clean             Remove preset build directory before configure
#   -Jobs <N>          Parallel build jobs
#   -Help              Show help

[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Preset = "",
    [switch]$Clean,
    [int]$Jobs,
    [switch]$Help,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ExtraArgs
)

$ErrorActionPreference = "Stop"

for ($i = 0; $i -lt $ExtraArgs.Count; $i++) {
    $arg = $ExtraArgs[$i]
    switch -Regex ($arg) {
        '^--help$' { $Help = $true; continue }
        '^--clean$' { $Clean = $true; continue }
        '^--preset$' {
            if ($i + 1 -ge $ExtraArgs.Count) { throw "--preset requires a value" }
            $i++
            $Preset = $ExtraArgs[$i]
            continue
        }
        '^--preset=(.+)$' { $Preset = $Matches[1]; continue }
        '^(-j|--jobs)$' {
            if ($i + 1 -ge $ExtraArgs.Count) { throw "$arg requires a value" }
            $i++
            $Jobs = [int]$ExtraArgs[$i]
            continue
        }
        '^--jobs=(.+)$' { $Jobs = [int]$Matches[1]; continue }
        '^--$' {
            if ($i + 1 -lt $ExtraArgs.Count) {
                $ExtraArgs = $ExtraArgs[($i + 1) .. ($ExtraArgs.Count - 1)]
            } else {
                $ExtraArgs = @()
            }
            break
        }
        default { throw "Unknown argument: $arg" }
    }
}

if ($Help) {
    Get-Content $MyInvocation.MyCommand.Path | Select-Object -First 16
    exit 0
}

if (-not $Preset) {
    if ($env:OS -eq "Windows_NT") {
        $Preset = "dev-windows-foundation-debug"
    } else {
        $Preset = "dev-foundation-debug"
    }
}

if (($env:OS -eq "Windows_NT") -and $Preset -in @("dev-foundation-debug", "dev-foundation-release", "ci-foundation-release")) {
    throw "Preset '$Preset' uses Ninja and may not work with MSVC on Windows. Use 'dev-windows-foundation-debug', 'dev-windows-foundation-release', or 'ci-windows-foundation-release'."
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptDir
$buildDir = Join-Path $repoRoot "build\$Preset"

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "[INFO] Cleaning build directory: $buildDir"
    Remove-Item -Recurse -Force $buildDir
}

Write-Host "[INFO] Configure preset: $Preset"
& cmake --preset $Preset @ExtraArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "[INFO] Build preset: $Preset"
if ($Jobs -gt 0) {
    & cmake --build --preset $Preset --parallel $Jobs
} else {
    & cmake --build --preset $Preset --parallel
}
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
