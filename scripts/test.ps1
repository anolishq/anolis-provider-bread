#!/usr/bin/env pwsh
# Test anolis-provider-bread via CTest presets.
#
# Usage:
#   .\scripts\test.ps1 [options] [-- <extra-ctest-args>]
#
# Options:
#   -Preset <name>      Test preset (default: dev-windows-foundation-debug on Windows,
#                       dev-foundation-debug otherwise)
#   -Suite <name>       all|unit|phase1 (default: all)
#   -VerboseOutput      Run ctest with -VV
#   -Help               Show help

[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Preset = "",
    [ValidateSet("all", "unit", "phase1")]
    [string]$Suite = "all",
    [switch]$VerboseOutput,
    [switch]$Help,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ExtraArgs
)

$ErrorActionPreference = "Stop"

for ($i = 0; $i -lt $ExtraArgs.Count; $i++) {
    $arg = $ExtraArgs[$i]
    switch -Regex ($arg) {
        '^--help$' { $Help = $true; continue }
        '^--preset$' {
            if ($i + 1 -ge $ExtraArgs.Count) { throw "--preset requires a value" }
            $i++
            $Preset = $ExtraArgs[$i]
            continue
        }
        '^--preset=(.+)$' { $Preset = $Matches[1]; continue }
        '^--suite$|^--test$' {
            if ($i + 1 -ge $ExtraArgs.Count) { throw "$arg requires a value" }
            $i++
            $Suite = $ExtraArgs[$i]
            continue
        }
        '^--suite=(.+)$|^--test=(.+)$' {
            $Suite = if ($Matches[1]) { $Matches[1] } else { $Matches[2] }
            continue
        }
        '^(-v|--verbose)$' { $VerboseOutput = $true; continue }
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

$ctestArgs = @("--preset", $Preset)
if ($Suite -eq "all") {
    $ctestArgs += @("-L", "unit|phase1")
} else {
    $ctestArgs += @("-L", $Suite)
}
if ($VerboseOutput) {
    $ctestArgs += "-VV"
}
if ($ExtraArgs) {
    $ctestArgs += $ExtraArgs
}

Write-Host "[INFO] Test preset: $Preset"
Write-Host "[INFO] Suite: $Suite"
& ctest @ctestArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
