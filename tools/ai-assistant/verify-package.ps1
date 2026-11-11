[CmdletBinding()]
param([string]$Output = '', [int]$TimeoutSeconds = 180)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (-not $Output) { $Output = Join-Path $Root 'build/ai-release' }
$Evidence = [ordered]@{ commit=(& git -C $Root rev-parse HEAD).Trim(); configuration='Release'; nativeLaunch=$false; nativeSceneTests=$false; install=$false; update=$false; rollback=$false; liveChatGPTLogin=$false; liveModelTurn=$false; errors=@(); screenshots=@(); diagnosticErrors=@() }
$TemporaryParent = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
$TestRoot = Join-Path $TemporaryParent ('ИИ SDK ' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
$Installation = Join-Path $TestRoot 'SDK с пробелами'
New-Item -ItemType Directory -Force $TestRoot | Out-Null
function Save-Evidence {
    [IO.File]::WriteAllText((Join-Path $Output 'CI_EVIDENCE.json'), ($Evidence | ConvertTo-Json -Depth 20), [Text.UTF8Encoding]::new($false))
}
function Save-DesktopScreenshot([string]$Name) {
    $Bitmap = $null
    $Graphics = $null
    try {
        Add-Type -AssemblyName System.Windows.Forms
        Add-Type -AssemblyName System.Drawing
        $Bounds = [System.Windows.Forms.SystemInformation]::VirtualScreen
        if ($Bounds.Width -le 0 -or $Bounds.Height -le 0) { throw 'No desktop surface is available.' }
        $Bitmap = [System.Drawing.Bitmap]::new($Bounds.Width, $Bounds.Height)
        $Graphics = [System.Drawing.Graphics]::FromImage($Bitmap)
        $Graphics.CopyFromScreen($Bounds.X, $Bounds.Y, 0, 0, $Bounds.Size, [System.Drawing.CopyPixelOperation]::SourceCopy)
        $Bitmap.Save((Join-Path $TestRoot $Name), [System.Drawing.Imaging.ImageFormat]::Png)
        $Evidence.screenshots += $Name
    } catch {
        $Evidence.diagnosticErrors += ('Desktop capture failed: ' + $_.Exception.Message)
    } finally {
        if ($Graphics) { $Graphics.Dispose() }
        if ($Bitmap) { $Bitmap.Dispose() }
    }
}
function Run-Installer {
    $Installer = Join-Path $Output 'IXRay-AI-Assistant-0.2.0-Windows-x64-Setup.exe'
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
    $RollbackOut = Join-Path $TestRoot 'rollback.stdout.log'
    $RollbackErr = Join-Path $TestRoot 'rollback.stderr.log'
    $Rollback = Start-Process -FilePath $PowerShell -ArgumentList @('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',"`"$(Join-Path $Installation 'Rollback.ps1')`"") -RedirectStandardOutput $RollbackOut -RedirectStandardError $RollbackErr -Wait -PassThru
    $Evidence.rollbackExitCode = $Rollback.ExitCode
    if ($Rollback.ExitCode -ne 0) {
        foreach ($Log in @($RollbackOut,$RollbackErr)) {
            if (Test-Path -LiteralPath $Log) { Write-Host (Get-Content -LiteralPath $Log -Raw) }
        }
        throw "Rollback script failed with exit code $($Rollback.ExitCode); see rollback logs."
    }
    if (-not (Test-Path $PreviousSentinel) -or (Get-Content $UserResource -Raw) -ne 'preserve resource') { throw 'Rollback did not restore the previous program files and preserve project data.' }
    if (-not (Test-Path $PreviousTool) -or (Get-FileHash $FileSystemConfig -Algorithm SHA256).Hash -ne $ConfigHash) { throw 'Rollback did not restore service files and project configuration.' }
    $Evidence.rollback = $true
    # Exercise native startup only after independent install/update/rollback checks.
    $Process = Start-Process -FilePath $Exe -ArgumentList @('-ai-selftest','-ai-warp','-nosound','-fsltx','fs.ltx') -WorkingDirectory $Installation -PassThru
    $Deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $ReportPath = Join-Path $Installation '_appdata_ixray_/ai-selftest-report.json'
    $Report = $null
    $ReportError = $null
    # The terminal report describes the checks. The editor window can remain
    # open after the report is written, so process exit is a separate result.
    while ($true) {
        $Process.Refresh()
        $Exited = $Process.HasExited
        if (Test-Path -LiteralPath $ReportPath -PathType Leaf) {
            try {
                $Candidate = Get-Content -LiteralPath $ReportPath -Raw | ConvertFrom-Json -ErrorAction Stop
                if ($null -eq $Candidate -or $Candidate.status -notin 'running','passed','failed') { throw 'The report has an unknown status.' }
                if ($Candidate.status -ne 'running') {
                    if ($Candidate.ok -isnot [bool] -or $Candidate.ok -ne ($Candidate.status -eq 'passed')) {
                        throw 'The terminal report status and ok result disagree.'
                    }
                    if ($Candidate.checks -isnot [Array] -or $Candidate.checks.Count -eq 0) { throw 'The terminal report contains no checks.' }
                    $Report = $Candidate
                }
                $ReportError = $null
            } catch {
                # A report may still be getting written. Retry until it is
                # complete or the process exits or reaches its deadline.
                $ReportError = $_.Exception.Message
            }
        }
        if ($null -ne $Report -or $Exited -or [DateTime]::UtcNow -ge $Deadline) { break }
        Start-Sleep -Milliseconds 500
    }
    $NativeFailure = $null
    if ($null -ne $Report) {
        $Evidence.nativeLaunch = $true
        $Evidence.nativeReport = $Report
        $Evidence.nativeSceneTests = $Report.ok
        Copy-Item -LiteralPath $ReportPath -Destination (Join-Path $Output 'ai-selftest-report.json')
        if (-not $Report.ok) { $NativeFailure = 'The native scene self-test reported failures.' }
    } elseif ($ReportError) {
        Save-DesktopScreenshot 'native-invalid-report.png'
        $NativeFailure = "Level Editor self-test report is invalid. $ReportError"
    } elseif ($Process.HasExited) {
        Save-DesktopScreenshot 'native-no-report.png'
        $NativeFailure = "Level Editor exited ($($Process.ExitCode)) before producing a terminal self-test report."
    } else {
        Save-DesktopScreenshot 'native-timeout.png'
        $NativeFailure = 'Level Editor self-test reached its deadline without a terminal report.'
    }

    $Evidence.nativeTeardown = 'exited'
    if (-not $Process.HasExited) { [void]$Process.WaitForExit(1500) }
    if (-not $Process.HasExited) {
        try {
            if ($Process.CloseMainWindow() -and $Process.WaitForExit(5000)) { $Evidence.nativeTeardown = 'graceful' }
        } catch {
            $Evidence.diagnosticErrors += ('Window close failed: ' + $_.Exception.Message)
        }
        if (-not $Process.HasExited) {
            try {
                $Process.Kill()
                if ($Process.WaitForExit(5000)) { $Evidence.nativeTeardown = 'forced' }
                else { $Evidence.nativeTeardown = 'cleanup_timeout' }
            } catch {
                $Evidence.nativeTeardown = 'cleanup_failed'
                $Evidence.diagnosticErrors += ('Process cleanup failed: ' + $_.Exception.Message)
            }
        }
    }
    if ($Process.HasExited) { $Evidence.nativeExitCode = $Process.ExitCode }
    $Process.Dispose()

    $EditorLogDirectory = Join-Path $Installation 'logs'
    $EditorLogs = @()
    if (Test-Path -LiteralPath $EditorLogDirectory -PathType Container) {
        $EditorLogs = @(Get-ChildItem -LiteralPath $EditorLogDirectory -Filter 'leveleditor-*.log' -File)
    }
    $Evidence.rendererLogCount = $EditorLogs.Count
    $Evidence.rendererAssertions = @(
        foreach ($Log in $EditorLogs) {
            Select-String -LiteralPath $Log.FullName -Pattern '\[error\]Function\s*:\s*(CResourceManager::_CreateRT|CRT::create)\s*$' | ForEach-Object {
                @{ file=$Log.Name; line=$_.LineNumber; function=$_.Matches[0].Groups[1].Value }
            }
        }
    )
    $Evidence.rendererClean = $EditorLogs.Count -gt 0 -and $Evidence.rendererAssertions.Count -eq 0
    if (-not $NativeFailure -and -not $Evidence.rendererClean) {
        if ($EditorLogs.Count -eq 0) { $NativeFailure = 'No Level Editor log was available to verify renderer startup.' }
        else { $NativeFailure = 'Level Editor reported render-target assertions. See rendererAssertions and installed test logs.' }
    }
    if ($NativeFailure) { throw $NativeFailure }
} catch {
    $Evidence.errors += $_.Exception.Message
    throw
} finally {
    $EvidenceDir = Join-Path $Output 'installed-test-evidence'
    New-Item -ItemType Directory -Force $EvidenceDir | Out-Null
    try {
        Get-ChildItem $TestRoot -File | Where-Object { $_.Extension -in '.log','.png' } | Copy-Item -Destination $EvidenceDir
        foreach ($Directory in @('logs','_appdata_ixray_')) {
            $Source = Join-Path $Installation $Directory
            if (Test-Path $Source) { Copy-Item $Source $EvidenceDir -Recurse -Force }
        }
    } catch {
        $Evidence.diagnosticErrors += ('Copying test logs failed: ' + $_.Exception.Message)
    }
    # This directory is a disposable CI installation created by this script only.
    try {
        if (Test-Path $TestRoot) { Remove-Item $TestRoot -Recurse -Force }
    } catch {
        $Evidence.diagnosticErrors += ('Removing the test installation failed: ' + $_.Exception.Message)
    }
    Save-Evidence
}
