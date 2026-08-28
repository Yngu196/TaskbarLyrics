@echo off
REM Build script for MoeKoeTaskbarLyrics
REM Automatically applies MSVC 14.44 toolset to match ixwebsocket.lib
REM
REM Usage:
REM   build [debug|release|clean] [x64|arm64|all]
REM     debug   - Build Debug configuration
REM     release - Build Release configuration
REM     clean   - Remove all build directories
REM     x64/arm64 - Target architecture (default: x64)
REM     all     - Release only: build x64 -> arm64 -> verify -> package
REM   Target architecture defaults to x64.

setlocal

set "PROJECT_DIR=%~dp0"
set "BUILD_DIR=%PROJECT_DIR%out\build"
set "TOOLSET_VERSION=14.44.35207"

REM ---- 解析目标架构（默认 x64）----
set "ARCH=x64"
if not "%2"=="" set "ARCH=%2"

if /i "%ARCH%"=="all" goto :all

if /i "%ARCH%"=="x64" (
    set "PRESET_ARCH=x64"
    set "VCPKG_TRIPLET=x64-windows"
) else if /i "%ARCH%"=="arm64" (
    set "PRESET_ARCH=ARM64"
    set "VCPKG_TRIPLET=arm64-windows"
) else (
    echo Unknown architecture: %ARCH% ^(supported: x64, arm64, all^)
    goto :eof
)

if "%1"=="" (
    echo Usage: build [debug^|release^|clean] [x64^|arm64^|all]
    echo.
    echo   debug   - Build Debug configuration
    echo   release - Build Release configuration
    echo   clean   - Remove all build directories
    echo   x64/arm64 - Target architecture ^(default: x64^)
    echo   all     - Build x64 + arm64 + verify + package ^(release only^)
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

REM ---- 推导 VCPKG_INSTALLED_DIR（x64->installed/x64-windows, arm64->installed/arm64-windows）----
call :resolve_vcpkg_installed_dir
if errorlevel 1 goto :eof

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
cmake -B "%BUILD_DIR%\%CONFIG%" -S "%PROJECT_DIR%." --preset %CONFIG% -DVCPKG_INSTALLED_DIR="%VCPKG_INSTALLED_DIR%"
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
goto :eof

REM ============================================================
REM  all 模式：x64 -> arm64 -> verify -> package。
REM ============================================================
:all
if /i not "%1"=="release" (
    echo [all] mode requires release: build release all
    goto :eof
)
echo.
echo ===== Step 1/4: Build x64 Release =====
call "%~f0" release x64
if errorlevel 1 (
    echo [all] x64 build failed!
    exit /b 1
)
echo.
echo ===== Step 2/4: Build arm64 Release =====
call "%~f0" release arm64
if errorlevel 1 (
    echo [all] arm64 build failed!
    exit /b 1
)
echo.
echo ===== Step 3/4: Verify package architecture =====
python "%PROJECT_DIR%scripts\verify_package.py" "%PROJECT_DIR%moeKoe-taskbar-lyrics"
if errorlevel 1 (
    echo [all] architecture verification failed!
    exit /b 1
)
echo.
echo ===== Step 4/4: Package dual-arch zip =====
python "%PROJECT_DIR%scripts\pack_zip.py" "%PROJECT_DIR%moeKoe-taskbar-lyrics" "%PROJECT_DIR%moeKoe-taskbar-lyrics.zip"
if errorlevel 1 (
    echo [all] packaging failed!
    exit /b 1
)
echo.
echo [all] Done: moeKoe-taskbar-lyrics.zip
exit /b 0

REM ============================================================
REM  resolve VCPKG_INSTALLED_DIR for current VCPKG_TRIPLET.
REM  优先级：环境变量 VCPKG_INSTALLED_DIR -> VCPKG_ROOT -> VCPKG_INSTALLATION_ROOT。
REM ============================================================
:resolve_vcpkg_installed_dir
if defined VCPKG_INSTALLED_DIR (
    if exist "%VCPKG_INSTALLED_DIR%\include" (
        echo [INFO] Using VCPKG_INSTALLED_DIR from environment: %VCPKG_INSTALLED_DIR%
        exit /b 0
    )
)
if defined VCPKG_ROOT (
    if exist "%VCPKG_ROOT%\installed\%VCPKG_TRIPLET%\include" (
        set "VCPKG_INSTALLED_DIR=%VCPKG_ROOT%\installed\%VCPKG_TRIPLET%"
        echo [INFO] Resolved VCPKG_INSTALLED_DIR: %VCPKG_INSTALLED_DIR%
        exit /b 0
    )
)
if defined VCPKG_INSTALLATION_ROOT (
    if exist "%VCPKG_INSTALLATION_ROOT%\installed\%VCPKG_TRIPLET%\include" (
        set "VCPKG_INSTALLED_DIR=%VCPKG_INSTALLATION_ROOT%\installed\%VCPKG_TRIPLET%"
        echo [INFO] Resolved VCPKG_INSTALLED_DIR: %VCPKG_INSTALLED_DIR%
        exit /b 0
    )
)
echo [ERROR] Cannot resolve VCPKG_INSTALLED_DIR for triplet %VCPKG_TRIPLET%.
echo         Please set VCPKG_INSTALLED_DIR or VCPKG_ROOT / VCPKG_INSTALLATION_ROOT.
exit /b 1
