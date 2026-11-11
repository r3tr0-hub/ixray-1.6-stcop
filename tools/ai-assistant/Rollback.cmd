@echo off
setlocal
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0Rollback.ps1" -InstallationRoot "%~dp0."
if errorlevel 1 (
  echo Rollback failed. See the message above.
) else (
  echo Rollback completed.
)
pause
