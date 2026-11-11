[CmdletBinding()]
param(
    [string]$InstallationRoot = '',
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
try {
    if ($env:OS -ne 'Windows_NT') { throw 'Откат выполняется на Windows.' }
    # Windows PowerShell 5.1 can bind a script parameter before PSScriptRoot is
    # available. Resolve it in the body so running Rollback.ps1 directly works.
    if ([string]::IsNullOrWhiteSpace($InstallationRoot)) {
        $InstallationRoot = $PSScriptRoot
        if ([string]::IsNullOrWhiteSpace($InstallationRoot) -and -not [string]::IsNullOrWhiteSpace($PSCommandPath)) {
            $InstallationRoot = [IO.Path]::GetDirectoryName($PSCommandPath)
        }
    }
    if ([string]::IsNullOrWhiteSpace($InstallationRoot)) { throw 'Не удалось определить папку установки SDK.' }
    $RootDirectory = Get-Item -LiteralPath $InstallationRoot -ErrorAction Stop
    if ($RootDirectory -isnot [IO.DirectoryInfo]) { throw 'Укажите существующую папку установки SDK.' }
    $InstallationRoot = $RootDirectory.FullName
    $ProgramPrefixes = @((Join-Path $InstallationRoot 'bin'), (Join-Path $InstallationRoot 'tools/ai-assistant'))
    $Running = @(Get-CimInstance Win32_Process | Where-Object {
        $Executable = $_.ExecutablePath
        if (-not $Executable) { return $false }
        foreach ($Prefix in $ProgramPrefixes) {
            if ($Executable.StartsWith($Prefix + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { return $true }
        }
        return $false
    })
    if ($Running.Count) {
        throw ('Закройте процессы этой установки: ' + (($Running | ForEach-Object { "$($_.Name) (PID $($_.ProcessId))" }) -join ', '))
    }
    if ($CheckOnly) { Write-Host 'Процессы SDK остановлены.'; exit 0 }
    $Previous = Join-Path $InstallationRoot '_previous_build'
    $BackupInfo = Get-Content (Join-Path $Previous 'backup-info.json') -Raw | ConvertFrom-Json
    $BuildInfo = Get-Content (Join-Path $Previous 'build-info.json') -Raw | ConvertFrom-Json
    if ($BackupInfo.product -ne 'IXRay-AI-Assistant' -or -not $BackupInfo.complete -or
        $BuildInfo.product -ne 'IXRay-AI-Assistant' -or $BuildInfo.compatibilityLine -ne 'develop' -or
        -not (Test-Path (Join-Path $Previous 'bin/LevelEditor.exe')) -or
        -not (Test-Path (Join-Path $Previous 'tools/ai-assistant/src/bridge.mjs'))) {
        throw 'Полная совместимая резервная копия не найдена. Файлы не изменены.'
    }
    $Recovery = Join-Path $InstallationRoot ('_replaced_build_' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'))
    New-Item -ItemType Directory -Path $Recovery | Out-Null
    $Directories = @('bin', 'tools/ai-assistant')
    $RootFiles = @('build-info.json','SHA256SUMS.json','Launch-LevelEditor.cmd','Rollback.cmd','Rollback.ps1')
    $RootFiles += @(Get-ChildItem $Previous -Filter 'fs*.ltx' -File | ForEach-Object Name)
    $RootFiles += @(Get-ChildItem $InstallationRoot -Filter 'fs*.ltx' -File | ForEach-Object Name)
    $RootFiles = @($RootFiles | Sort-Object -Unique)
    foreach ($Name in $RootFiles) {
        $Current = Join-Path $InstallationRoot $Name
        if (Test-Path $Current) { Copy-Item $Current (Join-Path $Recovery $Name) }
    }
    $MovedCurrent = [Collections.Generic.List[string]]::new()
    $RestoredPrevious = [Collections.Generic.List[string]]::new()
    try {
        foreach ($Directory in $Directories) {
            $Current = Join-Path $InstallationRoot $Directory
            $SavedCurrent = Join-Path $Recovery $Directory
            New-Item -ItemType Directory -Force -Path (Split-Path $SavedCurrent -Parent) | Out-Null
            Move-Item -LiteralPath $Current -Destination $SavedCurrent
            $MovedCurrent.Add($Directory)
            Move-Item -LiteralPath (Join-Path $Previous $Directory) -Destination $Current
            $RestoredPrevious.Add($Directory)
        }
        foreach ($Name in $RootFiles) {
            $Old = Join-Path $Previous $Name
            $Current = Join-Path $InstallationRoot $Name
            if (Test-Path $Old) { Copy-Item $Old $Current -Force }
            elseif (Test-Path $Current) { Remove-Item $Current -Force }
        }
    } catch {
        $RestoreError = $_
        for ($Index = $RestoredPrevious.Count - 1; $Index -ge 0; $Index--) {
            $Directory = $RestoredPrevious[$Index]
            Move-Item -LiteralPath (Join-Path $InstallationRoot $Directory) -Destination (Join-Path $Previous $Directory)
        }
        for ($Index = $MovedCurrent.Count - 1; $Index -ge 0; $Index--) {
            $Directory = $MovedCurrent[$Index]
            Move-Item -LiteralPath (Join-Path $Recovery $Directory) -Destination (Join-Path $InstallationRoot $Directory)
        }
        foreach ($Name in $RootFiles) {
            $Saved = Join-Path $Recovery $Name
            if (Test-Path $Saved) { Copy-Item $Saved (Join-Path $InstallationRoot $Name) -Force }
        }
        throw $RestoreError
    }
    Move-Item -LiteralPath $Previous -Destination (Join-Path $Recovery 'rollback-source')
    Write-Host ('Восстановлена версия ' + $BuildInfo.version + '. Заменённые файлы сохранены: ' + $Recovery)
    Write-Host 'Сцены, ресурсы, переписка и данные входа сохранены.'
    exit 0
} catch {
    Write-Error -ErrorRecord $_ -ErrorAction Continue
    exit 1
}
