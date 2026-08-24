@echo off
REM Build script for MoeKoeTaskbarLyrics
REM Automatically applies MSVC 14.44 toolset to match ixwebsocket.lib
REM
REM Usage:
REM   build [debug|release] [x64|arm64]
REM     debug   - Build Debug configuration
REM     release - Build Release configuration
REM     clean   - Remove all build directories
REM   Target architecture defaults to x64.

setlocal

set "PROJECT_DIR=%~dp0"
set "BUILD_DIR=%PROJECT_DIR%out\build"
set "TOOLSET_VERSION=14.44.35207"

REM ---- 解析目标架构（默认 x64）----
set "ARCH=x64"
if not "%2"=="" set "ARCH=%2"

if /i "%ARCH%"=="x64" (
    set "PRESET_ARCH=x64"
) else if /i "%ARCH%"=="arm64" (
    set "PRESET_ARCH=ARM64"
) else (
    echo Unknown architecture: %ARCH% ^(supported: x64, arm64^)
    goto :eof
)

if "%1"=="" (
    echo Usage: build [debug^|release^|clean] [x64^|arm64]
    echo.
    echo   debug   - Build Debug configuration
    echo   release - Build Release configuration
    echo   clean   - Remove all build directories
    echo   x64/arm64 - Target architecture ^(default: x64^)
    goto :eof
)

if /i "%1"=="clean" (
    echo Cleaning build directories...
    if exist "%BUILD_DIR%\%PRESET_ARCH%-Debug" rmdir /s /q "%BUILD_DIR%\%PRESET_ARCH%-Debug"
    if exist "%BUILD_DIR%\%PRESET_ARCH%-Release" rmdir /s /q "%BUILD_DIR%\%PRESET_ARCH%-Release"
    if exist "%BUILD_DIR%\%PRESET_ARCH%-Debug-ninja" rmdir /s /q "%BUILD_DIR%\%PRESET_ARCH%-Debug-ninja"
    echo Done.
    goto :eof
)

if /i "%1"=="debug" (
    set "CONFIG=%PRESET_ARCH%-Debug"
    set "CONFIG_TYPE=Debug"
) else if /i "%1"=="release" (
    set "CONFIG=%PRESET_ARCH%-Release"
    set "CONFIG_TYPE=Release"
) else (
    echo Unknown configuration: %1
    goto :eof
)

echo Configuring %CONFIG%...
cmake -B "%BUILD_DIR%\%CONFIG%" -S "%PROJECT_DIR%." --preset %CONFIG%
if errorlevel 1 (
    echo Configure failed!
    exit /b 1
)

echo.
echo Building %CONFIG% (%CONFIG_TYPE%) with MSVC %TOOLSET_VERSION%...
cmake --build "%BUILD_DIR%\%CONFIG%" --config %CONFIG_TYPE% -- /p:PlatformToolsetVersion=%TOOLSET_VERSION%
if errorlevel 1 (
    echo Build failed!
    exit /b 1
)

echo.
echo Build successful! Output: moeKoe-taskbar-lyrics\%PRESET_ARCH%\%CONFIG_TYPE%\MoeKoeTaskbarLyrics.exe (root MoeKoeTaskbarLyrics.exe is the launcher)
