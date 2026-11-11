[CmdletBinding()]
param([string]$Output = '', [int]$TimeoutSeconds = 180)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (-not $Output) { $Output = Join-Path $Root 'build/ai-release' }
$Evidence = [ordered]@{ commit=(& git -C $Root rev-parse HEAD).Trim(); configuration='Release'; nativeLaunch=$false; nativeSceneTests=$false; install=$false; update=$false; rollback=$false; liveChatGPTLogin=$false; liveModelTurn=$false; errors=@() }
$TemporaryParent = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
$TestRoot = Join-Path $TemporaryParent ('ИИ SDK ' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
$Installation = Join-Path $TestRoot 'SDK с пробелами'
New-Item -ItemType Directory -Force $TestRoot | Out-Null
function Save-Evidence {
    [IO.File]::WriteAllText((Join-Path $Output 'CI_EVIDENCE.json'), ($Evidence | ConvertTo-Json -Depth 20), [Text.UTF8Encoding]::new($false))
}
function Run-Installer {
    $Installer = Join-Path $Output 'IXRay-AI-Assistant-0.1.0-Windows-x64-Setup.exe'
    $Log = Join-Path $TestRoot ('installer-' + [Guid]::NewGuid().ToString('N') + '.log')
    $Process = Start-Process -FilePath $Installer -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',"/DIR=`"$Installation`"","/LOG=`"$Log`"") -Wait -PassThru
    if ($Process.ExitCode -ne 0) { throw "Installer failed with exit code $($Process.ExitCode)." }
}
try {
    Run-Installer
    $Evidence.install = $true
    $Exe = Join-Path $Installation 'bin/LevelEditor.exe'
    if (-not (Test-Path $Exe)) { throw 'Installed LevelEditor.exe is missing.' }
    $BuildInfo = Get-Content (Join-Path $Installation 'build-info.json') -Raw | ConvertFrom-Json
    $Evidence.nativeCommit = $BuildInfo.commit
    $Evidence.integrationCommit = $BuildInfo.integrationCommit
    $Process = Start-Process -FilePath $Exe -ArgumentList @('-ai-selftest','-ai-warp','-nosound','-fsltx','fs.ltx') -WorkingDirectory $Installation -PassThru
    $Deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while (-not $Process.HasExited -and [DateTime]::UtcNow -lt $Deadline) {
        Start-Sleep -Milliseconds 500
        $Process.Refresh()
    }
    if (-not $Process.HasExited) {
        Stop-Process -Id $Process.Id -Force
        throw 'Level Editor self-test timed out; no native success is claimed.'
    }
    $ReportPath = Join-Path $Installation '_appdata_ixray_/ai-selftest-report.json'
    if (-not (Test-Path $ReportPath)) { throw "Level Editor exited ($($Process.ExitCode)) without a self-test report." }
    $Report = Get-Content $ReportPath -Raw | ConvertFrom-Json
    $Evidence.nativeLaunch = $true
    $Evidence.nativeReport = $Report
    Copy-Item $ReportPath (Join-Path $Output 'ai-selftest-report.json')
    if ($Report.ok -ne $true) { throw 'The native scene self-test reported failures.' }
    $Evidence.nativeSceneTests = $true
    # A complete update must preserve project files, program rollback and integration data.
    $UserResource = Join-Path $Installation 'rawdata/user-preservation-test.txt'
    $PreviousSentinel = Join-Path $Installation 'bin/previous-build-test.txt'
    $PreviousTool = Join-Path $Installation 'tools/ai-assistant/previous-tool-test.txt'
    $FileSystemConfig = Join-Path $Installation 'fs.ltx'
    [IO.File]::WriteAllText($UserResource, 'preserve resource')
    [IO.File]::WriteAllText($PreviousSentinel, 'previous build')
    [IO.File]::WriteAllText($PreviousTool, 'previous tool')
    Add-Content -LiteralPath $FileSystemConfig -Value '; preserve project configuration' -Encoding ascii
    $ConfigHash = (Get-FileHash $FileSystemConfig -Algorithm SHA256).Hash
    Run-Installer
    if ((Get-Content $UserResource -Raw) -ne 'preserve resource') { throw 'Update changed the project preservation sentinel.' }
    if (-not (Test-Path (Join-Path $Installation '_previous_build/bin/previous-build-test.txt'))) { throw 'Previous program files were not backed up.' }
    if (-not (Test-Path (Join-Path $Installation '_previous_build/tools/ai-assistant/previous-tool-test.txt'))) { throw 'Previous service files were not backed up.' }
    if ((Get-FileHash $FileSystemConfig -Algorithm SHA256).Hash -ne $ConfigHash) { throw 'Update changed the project filesystem configuration.' }
    $Evidence.update = $true
    $PowerShell = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $Rollback = Start-Process -FilePath $PowerShell -ArgumentList @('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',"`"$(Join-Path $Installation 'Rollback.ps1')`"") -Wait -PassThru
    if ($Rollback.ExitCode -ne 0) { throw 'Rollback script failed.' }
    if (-not (Test-Path $PreviousSentinel) -or (Get-Content $UserResource -Raw) -ne 'preserve resource') { throw 'Rollback did not restore the previous program files and preserve project data.' }
    if (-not (Test-Path $PreviousTool) -or (Get-FileHash $FileSystemConfig -Algorithm SHA256).Hash -ne $ConfigHash) { throw 'Rollback did not restore service files and project configuration.' }
    $Evidence.rollback = $true
} catch {
    $Evidence.errors += $_.Exception.Message
    throw
} finally {
    Save-Evidence
    $EvidenceDir = Join-Path $Output 'installed-test-evidence'
    New-Item -ItemType Directory -Force $EvidenceDir | Out-Null
    Get-ChildItem $TestRoot -Filter '*.log' -File | Copy-Item -Destination $EvidenceDir
    foreach ($Directory in @('logs','_appdata_ixray_')) {
        $Source = Join-Path $Installation $Directory
        if (Test-Path $Source) { Copy-Item $Source $EvidenceDir -Recurse -Force }
    }
    # This directory is a disposable CI installation created by this script only.
    if (Test-Path $TestRoot) { Remove-Item $TestRoot -Recurse -Force }
}
