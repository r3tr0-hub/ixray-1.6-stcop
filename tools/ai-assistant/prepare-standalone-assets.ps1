[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Destination)
$ErrorActionPreference = 'Stop'
$Destination = [IO.Path]::GetFullPath($Destination)
$LightningRoot = Join-Path $PSScriptRoot 'fixtures/lightning'
$SilenceRoot = Join-Path $PSScriptRoot 'fixtures/silence'
$Lightning = Get-Content -LiteralPath (Join-Path $LightningRoot 'ai-lightning-manifest.json') -Raw | ConvertFrom-Json
$Silence = Get-Content -LiteralPath (Join-Path $SilenceRoot 'silence-manifest.json') -Raw | ConvertFrom-Json
$Files = @(
    foreach ($Model in $Lightning.models.PSObject.Properties) {
        if ($Model.Name -notmatch '^gamedata/meshes/dm/dm_lightning(?:-0[1-4]|_stancia_0[1-3])[.]dm$') { throw "Unexpected lightning model path: $($Model.Name)" }
        [pscustomobject]@{ path=$Model.Name; sourceRoot=$LightningRoot; sha256=$Model.Value.sha256; origin='Original generated lightning model' }
    }
)
if ($Files.Count -ne 7 -or $Silence.path -ne 'gamedata/sounds/$no_sound.ogg') { throw 'Standalone editor assets do not match the expected startup resources.' }
$Files += [pscustomobject]@{ path=$Silence.path; sourceRoot=$SilenceRoot; sha256=$Silence.sha256; origin='Generated silent Vorbis sound' }
$Evidence = foreach ($File in $Files) {
    $Source = Join-Path $File.sourceRoot $File.path
    if ((Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash.ToLowerInvariant() -ne $File.sha256) { throw "Generated startup asset failed SHA-256 verification: $($File.path)" }
    $Target = Join-Path $Destination $File.path
    $Copied = -not (Test-Path -LiteralPath $Target)
    if ($Copied) {
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Target)) | Out-Null
        Copy-Item -LiteralPath $Source -Destination $Target
    }
    $ActualHash = (Get-FileHash -LiteralPath $Target -Algorithm SHA256).Hash.ToLowerInvariant()
    [pscustomobject]@{
        path=$File.path
        sha256=$ActualHash
        origin=$(if ($ActualHash -eq $File.sha256) { $File.origin } else { 'Existing SDK resource' })
        copied=$Copied
    }
}
$Provenance = @{ schema=1; purpose='Editor startup resources for the standalone test installation'; files=@($Evidence) }
[IO.File]::WriteAllText((Join-Path $Destination 'SDK_RUNTIME_FALLBACKS.json'),($Provenance | ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
$Count = @($Evidence | Where-Object copied).Count
Write-Host "Prepared standalone editor resources. Copied $Count generated files and preserved $($Files.Count - $Count) existing files."
