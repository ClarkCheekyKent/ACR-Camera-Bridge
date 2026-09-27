@echo off
setlocal

if "%~1"=="" (
  echo Usage: install_example.bat "C:\path\to\ACR's UEVR game config folder"
  echo Example: install_example.bat "%APPDATA%\UnrealVRMod\acr"
  exit /b 1
)

set "DLL=%~dp0build\Release\ACR_CameraBridge.dll"
if not exist "%DLL%" (
  echo ERROR: Build the plugin first; DLL not found at:
  echo   %DLL%
  exit /b 1
)

if not exist "%~1\plugins" mkdir "%~1\plugins"
copy /Y "%DLL%" "%~1\plugins\ACR_CameraBridge.dll"
if errorlevel 1 exit /b 1
if not exist "%~1\scripts" mkdir "%~1\scripts"
copy /Y "%~dp0scripts\acr_camera_bridge.lua" "%~1\scripts\acr_camera_bridge.lua"
if errorlevel 1 exit /b 1
echo Installed Camera Bridge DLL and Lua panel to %~1
