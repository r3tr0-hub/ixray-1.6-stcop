[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Installer)
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'Installer smoke requires Windows.' }
$Installer = (Resolve-Path -LiteralPath $Installer).Path
$TestRoot = Join-Path ([IO.Path]::GetTempPath()) ('IXRay установщик тест ' + [guid]::NewGuid().ToString('N'))
$InstallRoot = Join-Path $TestRoot 'SDK с пробелами'
New-Item -ItemType Directory -Force $TestRoot | Out-Null
function Invoke-Installer([string]$LogName) {
    $Arguments = @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/SP-',('/DIR="' + $InstallRoot + '"'),('/LOG="' + (Join-Path $TestRoot $LogName) + '"'))
    $Run = Start-Process -FilePath $Installer -ArgumentList $Arguments -PassThru -Wait
    if ($Run.ExitCode -notin 0,3010) { throw "Installer failed ($($Run.ExitCode)): $LogName" }
}
function Assert-Text([string]$File, [string]$Expected) {
    if ((Get-Content -LiteralPath $File -Raw).Trim() -ne $Expected) { throw "Unexpected file content: $File" }
}
try {
    Invoke-Installer 'first-install.log'
    $Executable = Join-Path $InstallRoot 'bin/LevelEditor.exe'
    if (-not (Test-Path -LiteralPath $Executable)) { throw 'Installed LevelEditor.exe missing.' }
    $InitialHash = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash
    $UserResource = Join-Path $InstallRoot 'rawdata/objects/user-test-preserve.object'
    New-Item -ItemType Directory -Force (Split-Path $UserResource -Parent) | Out-Null
    Set-Content -LiteralPath $UserResource -Value 'user-resource-preserved' -Encoding ascii
    Set-Content -LiteralPath (Join-Path $InstallRoot 'bin/previous-program-marker.txt') -Value 'previous-bin' -Encoding ascii
    Set-Content -LiteralPath (Join-Path $InstallRoot 'tools/ai-assistant/previous-tool-marker.txt') -Value 'previous-tools' -Encoding ascii
    Add-Content -LiteralPath (Join-Path $InstallRoot 'fs.ltx') -Value '; installer smoke: preserve existing config' -Encoding ascii
    $FsHash = (Get-FileHash -LiteralPath (Join-Path $InstallRoot 'fs.ltx') -Algorithm SHA256).Hash
    Invoke-Installer 'update-install.log'
    Assert-Text $UserResource 'user-resource-preserved'
    if ((Get-FileHash -LiteralPath (Join-Path $InstallRoot 'fs.ltx') -Algorithm SHA256).Hash -ne $FsHash) { throw 'Update replaced fs.ltx.' }
    Assert-Text (Join-Path $InstallRoot '_previous_build/bin/previous-program-marker.txt') 'previous-bin'
    Assert-Text (Join-Path $InstallRoot '_previous_build/tools/ai-assistant/previous-tool-marker.txt') 'previous-tools'
    if (Test-Path -LiteralPath (Join-Path $InstallRoot 'bin/previous-program-marker.txt')) { throw 'Update mixed old/new bin contents.' }
    $PowerShell = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
    & $PowerShell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $InstallRoot 'Rollback.ps1') -InstallationRoot $InstallRoot
    if ($LASTEXITCODE) { throw 'Rollback.ps1 failed.' }
    Assert-Text (Join-Path $InstallRoot 'bin/previous-program-marker.txt') 'previous-bin'
    Assert-Text (Join-Path $InstallRoot 'tools/ai-assistant/previous-tool-marker.txt') 'previous-tools'
    Assert-Text $UserResource 'user-resource-preserved'
    if ((Get-FileHash -LiteralPath (Join-Path $InstallRoot 'fs.ltx') -Algorithm SHA256).Hash -ne $FsHash) { throw 'Rollback failed to restore fs.ltx.' }
    if ((Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash -ne $InitialHash) { throw 'Rollback restored unexpected LevelEditor.exe.' }
    if (Test-Path -LiteralPath (Join-Path $InstallRoot '_previous_build')) { throw 'Rollback did not consume the complete backup.' }
    Write-Host '{"passed":true,"freshInstall":true,"update":true,"rollback":true,"resourcesPreserved":true,"configurationPreserved":true,"unicodeAndSpaces":true}'
} finally {
    $Uninstaller = Join-Path $InstallRoot 'unins000.exe'
    if (Test-Path -LiteralPath $Uninstaller) {
        Start-Process -FilePath $Uninstaller -ArgumentList '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART' -Wait | Out-Null
    }
    if (Test-Path -LiteralPath $TestRoot) { Remove-Item -LiteralPath $TestRoot -Recurse -Force }
}
