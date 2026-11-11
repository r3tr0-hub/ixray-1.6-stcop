[CmdletBinding()]
param([int]$Parallel = 4, [switch]$SkipConfigure)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
Push-Location $Root
try {
    if ($env:OS -ne 'Windows_NT') { throw 'Windows x64 / Visual Studio 2022 is required to build LevelEditor.' }
    if (-not $SkipConfigure) {
        & cmake --preset Editors-x64-Windows -DIXRAY_CI=ON -DIXRAY_PROFILER=OFF -DIXRAY_USE_COMPRESSOR=ON
        if ($LASTEXITCODE) { throw 'CMake configure failed.' }
    }
    & cmake --build build/x64/Editors-Windows --config Release --target LevelEditor xrCompress --parallel $Parallel
    if ($LASTEXITCODE) { throw 'SDK editor or compressor Release build failed.' }
} finally { Pop-Location }
