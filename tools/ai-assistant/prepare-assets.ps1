[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Destination,
    [Parameter(Mandatory=$true)][string]$CacheDirectory
)
$ErrorActionPreference = 'Stop'
$Spec = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'assets-lock.json') -Raw | ConvertFrom-Json
if ($Spec.schema -ne 1 -or $Spec.variant -ne 'gamedata_cop') { throw 'Unsupported SDK asset lock format or game variant.' }
New-Item -ItemType Directory -Force -Path $Destination,$CacheDirectory | Out-Null
$Destination = [IO.Path]::GetFullPath($Destination)
$Archive = Join-Path $CacheDirectory ('ixray-assets-' + $Spec.commit + '.zip')
function Get-AssetSha256([string]$File) {
    return (Get-FileHash -LiteralPath $File -Algorithm SHA256).Hash.ToLowerInvariant()
}
if ((Test-Path -LiteralPath $Archive) -and (Get-AssetSha256 $Archive) -ne $Spec.archive.sha256) {
    Remove-Item -LiteralPath $Archive -Force
}
if (-not (Test-Path -LiteralPath $Archive)) {
    $Temporary = $Archive + '.' + [Guid]::NewGuid().ToString('N') + '.part'
    try {
        Invoke-WebRequest -Uri $Spec.archive.url -OutFile $Temporary
        if ((Get-AssetSha256 $Temporary) -ne $Spec.archive.sha256) { throw 'The official IX-Ray asset archive failed SHA-256 verification.' }
        Move-Item -LiteralPath $Temporary -Destination $Archive -Force
    } finally {
        if (Test-Path -LiteralPath $Temporary) { Remove-Item -LiteralPath $Temporary -Force }
    }
}
if ((Get-AssetSha256 $Archive) -ne $Spec.archive.sha256) { throw 'The official IX-Ray asset archive does not match its pinned SHA-256.' }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$Gamedata = Join-Path $Destination 'gamedata'
New-Item -ItemType Directory -Force -Path $Gamedata | Out-Null
$Gamedata = [IO.Path]::GetFullPath($Gamedata)
$AllowedPrefix = $Gamedata.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$ZipPrefix = $Spec.archivePrefix + $Spec.variant + '/'
$Imported = 0
$Retained = 0
$ImportedLibraries = @{}
$Zip = [IO.Compression.ZipFile]::OpenRead($Archive)
try {
    # This matches upstream's baseline-plus-checkout overlay recipe. Existing
    # files copied from the selected IX-Ray revision always take precedence.
    foreach ($Entry in $Zip.Entries) {
        if (-not $Entry.FullName.StartsWith($ZipPrefix,[StringComparison]::Ordinal)) { continue }
        $Relative = $Entry.FullName.Substring($ZipPrefix.Length)
        if (-not $Relative -or $Relative.EndsWith('/')) { continue }
        $Target = [IO.Path]::GetFullPath((Join-Path $Gamedata $Relative))
        if (-not $Target.StartsWith($AllowedPrefix,[StringComparison]::OrdinalIgnoreCase)) { throw 'The pinned asset archive contains an unsafe path.' }
        if (Test-Path -LiteralPath $Target) { $Retained++; continue }
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Target)) | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($Entry,$Target,$false)
        $Imported++
        $ImportedLibraries[$Relative] = $true
    }
    $LicenseEntry = $Zip.GetEntry($Spec.archivePrefix + $Spec.license.path)
    if ($null -eq $LicenseEntry) { throw 'The official asset license is missing from the pinned archive.' }
    $LicenseDirectory = Join-Path $Destination 'tools/ai-assistant/licenses'
    New-Item -ItemType Directory -Force -Path $LicenseDirectory | Out-Null
    $LicenseFile = Join-Path $LicenseDirectory 'IXRay-Assets-CC-BY-NC-SA-4.0.txt'
    [IO.Compression.ZipFileExtensions]::ExtractToFile($LicenseEntry,$LicenseFile,$true)
    if ((Get-AssetSha256 $LicenseFile) -ne $Spec.license.sha256) { throw 'The asset license failed SHA-256 verification.' }
    $Attribution = @"
IX-Ray multiplatform assets, Call of Pripyat variant.
Source: $($Spec.repository)
Pinned source commit: $($Spec.commit)
License: $($Spec.license.name); full license in IXRay-Assets-CC-BY-NC-SA-4.0.txt.
The upstream asset files are preserved. Files present in the selected IX-Ray
SDK checkout take precedence as in upstream util/generate-patch.bat.
The separate ai_test scene, cube and textures are generated test resources.
"@
    [IO.File]::WriteAllText((Join-Path $LicenseDirectory 'IXRay-Assets-Attribution.txt'),$Attribution,[Text.UTF8Encoding]::new($false))
} finally { $Zip.Dispose() }
foreach ($Library in $Spec.requiredLibraries) {
    $File = Join-Path $Gamedata $Library.path
    if (-not (Test-Path -LiteralPath $File) -or (Get-Item -LiteralPath $File).Length -eq 0) { throw "Missing required SDK startup asset: $($Library.path)" }
    if ($ImportedLibraries.ContainsKey($Library.path) -and (Get-AssetSha256 $File) -ne $Library.sha256) {
        throw "The extracted SDK library differs from its pinned source: $($Library.path)"
    }
}
$Evidence = [ordered]@{
    repository=$Spec.repository
    commit=$Spec.commit
    variant=$Spec.variant
    archiveSha256=$Spec.archive.sha256
    license=$Spec.license.name
    importedMissingFiles=$Imported
    preservedCheckoutFiles=$Retained
    requiredLibraries=@($Spec.requiredLibraries | ForEach-Object { $_.path })
}
[IO.File]::WriteAllText((Join-Path $Destination 'SDK_ASSET_PROVENANCE.json'),($Evidence | ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
Write-Host "Prepared official CoP SDK asset baseline: $Imported missing files imported, $Retained checkout files preserved; commit $($Spec.commit)."
[pscustomobject]$Evidence
