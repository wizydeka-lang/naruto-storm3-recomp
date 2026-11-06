@echo off
rem Double-click to install: builds the ReXGlue SDK and the game from source
rem on this PC from your own game files, then adds shortcuts.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\install-windows.ps1" %*
echo.
pause
