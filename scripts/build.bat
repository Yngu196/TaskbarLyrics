@echo off
REM ============================================================
REM  MoeKoeMusic Taskbar Lyrics - One-click Build Script
REM ============================================================

setlocal EnableDelayedExpansion

echo.
echo ============================================================
echo   MoeKoeMusic Taskbar Lyrics - Build Script
echo ============================================================
echo.

REM ---- 1. 定位 vcpkg ----
if not defined VCPKG_ROOT (
    if exist "D:\vcpkg\scripts\buildsystems\vcpkg.cmake" (
        set "VCPKG_ROOT=D:\vcpkg"
    ) else if exist "C:\dev\vcpkg\scripts\buildsystems\vcpkg.cmake" (
        set "VCPKG_ROOT=C:\dev\vcpkg"
    ) else if exist "%USERPROFILE%\vcpkg\scripts\buildsystems\vcpkg.cmake" (
        set "VCPKG_ROOT=%USERPROFILE%\vcpkg"
    ) else (
        echo [ERROR] vcpkg not found. Please install vcpkg or set VCPKG_ROOT.
        echo         https://github.com/microsoft/vcpkg
        pause
        exit /b 1
    )
)
echo [INFO] vcpkg root: %VCPKG_ROOT%

REM ---- 1.5 解析目标架构（默认 x64）----
set "ARCH=x64"
if not "%2"=="" set "ARCH=%2"
if /i "!ARCH!"=="x64" (
    set "VCPKG_TRIPLET=x64-windows"
    set "GENERATOR_ARCH=x64"
) else if /i "!ARCH!"=="arm64" (
    set "VCPKG_TRIPLET=arm64-windows"
    set "GENERATOR_ARCH=ARM64"
) else (
    echo [ERROR] Unknown architecture: !ARCH! ^(supported: x64, arm64^)
    pause
    exit /b 1
)
echo [INFO] Target architecture: !ARCH! ^(triplet: !VCPKG_TRIPLET!^)

REM ---- 2. 检查/安装依赖 ----
echo [INFO] Checking dependencies...
"%VCPKG_ROOT%\vcpkg.exe" install ixwebsocket:!VCPKG_TRIPLET! nlohmann-json:!VCPKG_TRIPLET! || (
    echo [ERROR] Failed to install dependencies.
    pause
    exit /b 1
)

REM ---- 3. 选择构建类型 ----
if "%1"=="Debug" (
    set "BUILD_TYPE=Debug"
) else (
    set "BUILD_TYPE=Release"
)
echo [INFO] Build type: !BUILD_TYPE!

REM ---- 4. CMake Configure ----
echo [INFO] Configuring CMake...
cmake -B build-!ARCH! -S . ^
    -A !GENERATOR_ARCH! ^
    -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
    -DCMAKE_BUILD_TYPE=!BUILD_TYPE! || (
    echo [ERROR] CMake configuration failed.
    pause
    exit /b 1
)

REM ---- 5. CMake Build ----
echo [INFO] Building...
cmake --build build-!ARCH! --config !BUILD_TYPE! --parallel || (
    echo [ERROR] Build failed.
    pause
    exit /b 1
)

echo.
echo ============================================================
echo   Build complete.
echo   Output: build-!ARCH!\!BUILD_TYPE!\MoeKoeTaskbarLyrics.exe
echo ============================================================
echo.
pause
