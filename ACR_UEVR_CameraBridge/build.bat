@echo off
setlocal

if "%~1"=="" (
  echo Usage: build.bat C:\path\to\UEVR
  echo Example: build.bat C:\src\UEVR
  exit /b 1
)

set "UEVR_ROOT=%~1"
if not exist "%UEVR_ROOT%\include\uevr\Plugin.hpp" (
  echo ERROR: %UEVR_ROOT% does not look like a UEVR source checkout.
  exit /b 1
)

where cmake >nul 2>nul
if errorlevel 1 (
  echo ERROR: cmake is not in PATH.
  exit /b 1
)

cmake -S "%~dp0" -B "%~dp0build" -A x64 -DUEVR_ROOT="%UEVR_ROOT%"
if errorlevel 1 exit /b %errorlevel%

cmake --build "%~dp0build" --config Release
if errorlevel 1 exit /b %errorlevel%

echo.
echo Built:
echo   %~dp0build\Release\ACR_CameraBridge.dll
