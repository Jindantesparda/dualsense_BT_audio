@echo off
setlocal
cd /d "%~dp0"

rem Drop an audio file on this file to open it in the player and start playing,
rem or double-click it to open the player without a file.

if "%~1"=="" (
    start "" "DualSenseMusic.exe"
) else (
    start "" "DualSenseMusic.exe" "%~1"
)
