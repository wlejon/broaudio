# Test coverage report for broaudio (Windows / MSVC only).
#
# Runs all ctest-registered test exes under OpenCppCoverage and emits
# an HTML report at build/coverage/index.html.
#
# Requirements:
#   - OpenCppCoverage (winget install OpenCppCoverage.OpenCppCoverage)
#   - Debug build present in build\ (PDBs needed)
#
# Usage:
#   pwsh scripts/coverage.ps1
#   pwsh scripts/coverage.ps1 -Output build/cov

[CmdletBinding()]
param(
    [string]$Output = 'build/coverage'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$occ = "C:\Program Files\OpenCppCoverage\OpenCppCoverage.exe"
if (-not (Test-Path $occ)) {
    throw "OpenCppCoverage not found at $occ. Install: winget install OpenCppCoverage.OpenCppCoverage"
}

$build = Join-Path $root 'build'
if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
    throw "$build not configured. Run: cmake -B build"
}

$outAbs = if ([System.IO.Path]::IsPathRooted($Output)) { $Output } else { Join-Path $root $Output }
if (Test-Path $outAbs) { Remove-Item -Recurse -Force $outAbs }

& $occ `
    --sources "$root\src" `
    --modules test_ `
    --cover_children `
    --export_type "html:$outAbs" `
    --working_dir $root `
    --quiet `
    -- ctest --test-dir $build -C Debug --output-on-failure

Write-Host ""
Write-Host "Coverage report: $outAbs\index.html"
