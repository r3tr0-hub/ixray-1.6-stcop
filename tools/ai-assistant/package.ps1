[CmdletBinding()]
param(
    [string]$Output = '',
    [string]$BinaryDirectory = '',
    [switch]$SkipInstaller
)
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'Packaging requires Windows x64.' }
$Root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$Lock = Get-Content (Join-Path $PSScriptRoot 'runtime-lock.json') -Raw | ConvertFrom-Json
if (-not $Output) { $Output = Join-Path $Root 'build/ai-release' }
if (-not $BinaryDirectory) { $BinaryDirectory = Join-Path $Root 'build/x64/Editors-Windows/bin/Release' }
$Stage = Join-Path $Output 'IXRay-AI-Assistant'
$Downloads = Join-Path $Output 'downloads'
New-Item -ItemType Directory -Force -Path $Output,$Downloads | Out-Null
function Write-Utf8Json($Value, $File, [int]$Depth = 5) {
    [IO.File]::WriteAllText($File, ($Value | ConvertTo-Json -Depth $Depth), [Text.UTF8Encoding]::new($false))
}
if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }
New-Item -ItemType Directory -Path $Stage | Out-Null
function Get-VerifiedArchive($Spec, $Name) {
    $File = Join-Path $Downloads $Name
    if ((Test-Path $File) -and (Get-FileHash $File -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Spec.sha256) {
        Remove-Item $File -Force
    }
    if (-not (Test-Path $File)) { Invoke-WebRequest -Uri $Spec.url -OutFile $File }
    if ((Get-FileHash $File -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Spec.sha256) {
        throw "SHA-256 mismatch: $Name"
    }
    return $File
}
if (-not (Test-Path (Join-Path $BinaryDirectory 'LevelEditor.exe'))) { throw 'No built LevelEditor.exe; compile Release before packaging.' }
New-Item -ItemType Directory (Join-Path $Stage 'bin') | Out-Null
Get-ChildItem $BinaryDirectory -File | Where-Object { $_.Extension -in '.dll','.exe','.txt','.json' } | Copy-Item -Destination (Join-Path $Stage 'bin')
# SDK assets are from the same checkout. This is a separate complete test installation.
foreach ($Directory in @('gamedata','rawdata')) {
    Copy-Item (Join-Path $Root $Directory) (Join-Path $Stage $Directory) -Recurse
}
Get-ChildItem $Root -File | Where-Object { $_.Name -like 'fs*.ltx' -or $_.Name -in 'ActorEditorLevel.cform','LICENSE.md' } | Copy-Item -Destination $Stage
$Assistant = Join-Path $Stage 'tools/ai-assistant'
New-Item -ItemType Directory -Force $Assistant | Out-Null
foreach ($Name in @('src','package.json','package-lock.json','runtime-lock.json','codex-download.json','README_RU.txt','TEST_REPORT_RU.txt','CI_EVIDENCE.json','licenses')) {
    $Source = Join-Path $PSScriptRoot $Name
    if (Test-Path $Source) { Copy-Item $Source (Join-Path $Assistant $Name) -Recurse }
}
$FixtureInstructions = Join-Path $PSScriptRoot 'fixtures/README_RU.txt'
if (Test-Path $FixtureInstructions) {
    New-Item -ItemType Directory -Force (Join-Path $Assistant 'fixtures') | Out-Null
    Copy-Item $FixtureInstructions (Join-Path $Assistant 'fixtures/README_RU.txt')
}
$Runtime = Join-Path $Assistant 'runtime'
New-Item -ItemType Directory -Force $Runtime | Out-Null
$NodeZip = Get-VerifiedArchive $Lock.node 'node-win-x64.zip'
$NodeExtract = Join-Path $Downloads 'node'
if (Test-Path $NodeExtract) { Remove-Item $NodeExtract -Force -Recurse }
Expand-Archive $NodeZip $NodeExtract
$NodeDistribution = Get-ChildItem $NodeExtract -Directory | Select-Object -First 1
Copy-Item (Join-Path $NodeDistribution.FullName 'node.exe') $Runtime
Copy-Item (Join-Path $NodeDistribution.FullName 'LICENSE') (Join-Path $Runtime 'NODE-LICENSE.txt')
$NodeExe = Join-Path $Runtime 'node.exe'
$NpmCli = Join-Path $NodeDistribution.FullName 'node_modules/npm/bin/npm-cli.js'
Push-Location $PSScriptRoot
try {
    & $NodeExe $NpmCli ci --omit=dev --ignore-scripts
    if ($LASTEXITCODE) { throw 'npm ci failed.' }
    $TestFiles = @(Get-ChildItem (Join-Path $PSScriptRoot 'tests/*.test.mjs') -File | ForEach-Object FullName)
    if ($TestFiles.Count -eq 0) { throw 'No AI protocol tests were found.' }
    & $NodeExe --test @TestFiles
    if ($LASTEXITCODE) { throw 'AI protocol tests failed.' }
} finally { Pop-Location }
Copy-Item (Join-Path $PSScriptRoot 'node_modules') $Assistant -Recurse
$CodexZip = Get-VerifiedArchive $Lock.codex 'codex-win-x64.zip'
$CodexExtract = Join-Path $Downloads 'codex'
if (Test-Path $CodexExtract) { Remove-Item $CodexExtract -Force -Recurse }
Expand-Archive $CodexZip $CodexExtract
$CodexDir = Join-Path $Runtime 'codex'
New-Item -ItemType Directory -Force $CodexDir | Out-Null
# Keep the official archive layout; helper binaries and resources must remain adjacent.
Copy-Item (Join-Path $CodexExtract '*') $CodexDir -Recurse -Force
$CodexFile = Get-ChildItem $CodexDir -File -Filter 'codex-*-windows-msvc.exe' | Select-Object -First 1
if ($CodexFile) {
    Move-Item $CodexFile.FullName (Join-Path $CodexDir 'codex.exe') -Force
    $CodexPackagePath = Join-Path $CodexDir 'codex-package.json'
    if (Test-Path $CodexPackagePath) {
        $CodexPackage = Get-Content $CodexPackagePath -Raw | ConvertFrom-Json
        if ($CodexPackage.version -ne $Lock.codex.version -or $CodexPackage.target -ne 'x86_64-pc-windows-msvc') { throw 'Unexpected official Codex package metadata.' }
        $CodexPackage.entrypoint = 'codex.exe'
        Write-Utf8Json $CodexPackage $CodexPackagePath
    }
}
if (-not (Test-Path (Join-Path $CodexDir 'codex.exe'))) { throw 'Official archive did not contain expected codex executable.' }
$CodexVersion = & (Join-Path $CodexDir 'codex.exe') --version
if ($LASTEXITCODE) { throw 'Official Codex binary launch failed.' }
if (($CodexVersion -join ' ').Trim() -ne "codex-cli $($Lock.codex.version)") { throw "Unexpected Codex version: $CodexVersion" }
$Smoke = Join-Path $PSScriptRoot 'scripts/smoke-app-server.mjs'
if (Test-Path $Smoke) {
    & $NodeExe $Smoke (Join-Path $CodexDir 'codex.exe')
    if ($LASTEXITCODE) { throw 'Official app-server protocol smoke failed.' }
}
# App-local MSVC runtime removes the need for a developer toolchain or global install.
$CrtFound = $false
$VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (Test-Path $VsWhere) {
    $VS = & $VsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($VS) {
        $CrtVersion = Get-ChildItem (Join-Path $VS 'VC/Redist/MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
        if ($CrtVersion) {
            $CRT = Join-Path $CrtVersion.FullName 'x64/Microsoft.VC143.CRT'
            if (Test-Path $CRT) {
                Copy-Item (Join-Path $CRT '*.dll') (Join-Path $Stage 'bin')
                $CrtFound = (Test-Path (Join-Path $Stage 'bin/vcruntime140.dll')) -and (Test-Path (Join-Path $Stage 'bin/msvcp140.dll'))
            }
        }
    }
}
if (-not $CrtFound) { throw 'App-local Microsoft VC143 x64 runtime is missing; install the Visual Studio 2022 C++ redist build component before packaging.' }
# Each standalone child executable must resolve its own runtime on a clean PC.
$RuntimeDirectories = @(Get-ChildItem $Runtime -Recurse -File -Filter '*.exe' | ForEach-Object DirectoryName | Sort-Object -Unique)
foreach ($Directory in $RuntimeDirectories) {
    foreach ($RuntimeDll in Get-ChildItem $CRT -Filter '*.dll' -File) {
        $Destination = Join-Path $Directory $RuntimeDll.Name
        if (-not (Test-Path $Destination)) { Copy-Item $RuntimeDll.FullName $Destination }
    }
}
foreach ($Name in @('Launch-LevelEditor.cmd','Rollback.cmd','Rollback.ps1')) {
    Copy-Item (Join-Path $PSScriptRoot $Name) $Stage
}
$Commit = (& git -C $Root rev-parse HEAD).Trim()
Write-Utf8Json @{ product='IXRay-AI-Assistant'; compatibilityLine='develop'; version=$Lock.version; upstream=$Lock.upstreamCommit; commit=$Commit; configuration='Release'; platform='Windows x64'; builtAt=[DateTime]::UtcNow.ToString('o'); node=$Lock.node.version; codex=$Lock.codex.version } (Join-Path $Stage 'build-info.json')
$Manifest = foreach ($File in Get-ChildItem $Stage -Recurse -File) {
    @{ path=$File.FullName.Substring($Stage.TrimEnd([IO.Path]::DirectorySeparatorChar).Length + 1).Replace('\','/'); sha256=(Get-FileHash $File.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
}
Write-Utf8Json @($Manifest) (Join-Path $Stage 'SHA256SUMS.json')
Compress-Archive -Path (Join-Path $Stage '*') -DestinationPath (Join-Path $Output 'IXRay-AI-Assistant-0.1.0-Windows-x64-Portable.zip') -Force
if (-not $SkipInstaller) {
    $ISCC = Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6/ISCC.exe'
    if (-not (Test-Path $ISCC)) { throw 'Inno Setup 6 is required on the build machine only.' }
    & $ISCC "/DStage=$Stage" "/DOutput=$Output" (Join-Path $PSScriptRoot 'installer.iss')
    if ($LASTEXITCODE) { throw 'Installer compilation failed.' }
}
Get-ChildItem $Output -File | ForEach-Object { Get-FileHash $_.FullName -Algorithm SHA256 } | Format-Table -AutoSize
