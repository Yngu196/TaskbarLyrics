@echo off
REM Build script for MoeKoeTaskbarLyrics
REM Dependency resolution order is kept in sync with CMakeLists.txt,
REM scripts/bootstrap.ps1 and scripts/check_build_env.ps1:
REM   explicit override > .vcpkgroot > VCPKG_INSTALLATION_ROOT/VCPKG_ROOT
REM   > D:\vcpkg / C:\vcpkg
REM Run bootstrap once on a fresh clone:
REM   powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1 -Arch x64
REM
REM Usage:
REM   build [debug|release|clean] [x64|arm64|all]
REM     debug   - Build Debug configuration
REM     release - Build Release configuration
REM     clean   - Remove all build directories
REM     x64/arm64 - Target architecture (default: x64)
REM     all     - Release only: build x64 -> arm64 -> verify -> package
REM
REM Optional: set MKK_TOOLSET_VERSION=14.44.35207 to force one MSVC toolset
REM via /p:PlatformToolsetVersion. Unset by default, so VS picks its own.

setlocal

set "PROJECT_DIR=%~dp0"
set "BUILD_DIR=%PROJECT_DIR%out\build"
set "TOOLSET_VERSION=%MKK_TOOLSET_VERSION%"

REM ---- Resolve target architecture (default x64) ----
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

REM ---- Resolve VCPKG_INSTALLED_DIR for this VCPKG_TRIPLET ----
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
if defined TOOLSET_VERSION (
    echo Building %CONFIG% ^(%CONFIG_TYPE%^) with forced MSVC %TOOLSET_VERSION%...
    cmake --build "%BUILD_DIR%\%CONFIG%" --config %CONFIG_TYPE% -- /p:PlatformToolsetVersion=%TOOLSET_VERSION%
) else (
    echo Building %CONFIG% ^(%CONFIG_TYPE%^)...
    cmake --build "%BUILD_DIR%\%CONFIG%" --config %CONFIG_TYPE%
)
if errorlevel 1 (
    echo Build failed!
    exit /b 1
)

echo.
echo Build successful! Output: moeKoe-taskbar-lyrics\%ARCH%\MoeKoeTaskbarLyrics.exe
if /i "%PRESET_ARCH%"=="x64" echo                   Launcher: moeKoe-taskbar-lyrics\MoeKoeTaskbarLyrics.exe
goto :eof

REM ============================================================
REM  all mode: x64 -> arm64 -> verify -> package.
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
REM  Resolve VCPKG_INSTALLED_DIR (single source of truth).
REM  Priority, identical to CMakeLists.txt / bootstrap.ps1 /
REM  check_build_env.ps1:
REM    VCPKG_INSTALLED_DIR env -> .vcpkgroot (bootstrap pin)
REM    -> VCPKG_INSTALLATION_ROOT -> VCPKG_ROOT
REM    -> D:\vcpkg -> C:\vcpkg
REM  The pin outranks machine-level env vars so the tree bootstrap just
REM  populated is the tree we actually link against.
REM
REM  IMPORTANT: single-line IFs only. Inside a parenthesised block cmd.exe
REM  expands %VAR% when the block is parsed, BEFORE any SET in that block
REM  runs -> the old script printed an empty "[INFO] Resolved ..." line.
REM ============================================================
:resolve_vcpkg_installed_dir
if defined VCPKG_INSTALLED_DIR if exist "%VCPKG_INSTALLED_DIR%\include" if exist "%VCPKG_INSTALLED_DIR%\lib" goto :resolved_ok

set "_VCPKG_PINNED="
if exist "%PROJECT_DIR%.vcpkgroot" (
    for /f "usebackq tokens=* delims=" %%R in ("%PROJECT_DIR%.vcpkgroot") do if not defined _VCPKG_PINNED set "_VCPKG_PINNED=%%R"
)
if defined _VCPKG_PINNED if exist "%_VCPKG_PINNED%\installed\%VCPKG_TRIPLET%\include" if exist "%_VCPKG_PINNED%\installed\%VCPKG_TRIPLET%\lib" goto :use_pinned

if defined VCPKG_INSTALLATION_ROOT if exist "%VCPKG_INSTALLATION_ROOT%\installed\%VCPKG_TRIPLET%\include" if exist "%VCPKG_INSTALLATION_ROOT%\installed\%VCPKG_TRIPLET%\lib" goto :use_installation_root
if defined VCPKG_ROOT if exist "%VCPKG_ROOT%\installed\%VCPKG_TRIPLET%\include" if exist "%VCPKG_ROOT%\installed\%VCPKG_TRIPLET%\lib" goto :use_vcpkg_root

if exist "D:\vcpkg\installed\%VCPKG_TRIPLET%\include" if exist "D:\vcpkg\installed\%VCPKG_TRIPLET%\lib" goto :use_d_drive
if exist "C:\vcpkg\installed\%VCPKG_TRIPLET%\include" if exist "C:\vcpkg\installed\%VCPKG_TRIPLET%\lib" goto :use_c_drive
goto :resolve_failed

:use_installation_root
set "VCPKG_INSTALLED_DIR=%VCPKG_INSTALLATION_ROOT%\installed\%VCPKG_TRIPLET%"
set "_VCPKG_RESOLVED_BY=VCPKG_INSTALLATION_ROOT env"
goto :resolved_ok
:use_vcpkg_root
set "VCPKG_INSTALLED_DIR=%VCPKG_ROOT%\installed\%VCPKG_TRIPLET%"
set "_VCPKG_RESOLVED_BY=VCPKG_ROOT env"
goto :resolved_ok
:use_pinned
set "VCPKG_INSTALLED_DIR=%_VCPKG_PINNED%\installed\%VCPKG_TRIPLET%"
set "_VCPKG_RESOLVED_BY=.vcpkgroot (bootstrap pin)"
goto :resolved_ok
:use_d_drive
set "VCPKG_INSTALLED_DIR=D:\vcpkg\installed\%VCPKG_TRIPLET%"
set "_VCPKG_RESOLVED_BY=D:\vcpkg fallback"
goto :resolved_ok
:use_c_drive
set "VCPKG_INSTALLED_DIR=C:\vcpkg\installed\%VCPKG_TRIPLET%"
set "_VCPKG_RESOLVED_BY=C:\vcpkg fallback"
goto :resolved_ok

:resolved_ok
if not defined _VCPKG_RESOLVED_BY set "_VCPKG_RESOLVED_BY=VCPKG_INSTALLED_DIR env"
echo [INFO] Resolved VCPKG_INSTALLED_DIR: %VCPKG_INSTALLED_DIR%
echo [INFO] Source: %_VCPKG_RESOLVED_BY%
exit /b 0

:resolve_failed
echo [ERROR] Cannot resolve VCPKG_INSTALLED_DIR for triplet %VCPKG_TRIPLET%.
echo         First run: powershell -ExecutionPolicy Bypass -File "%PROJECT_DIR%scripts\bootstrap.ps1" -Arch %ARCH%
echo         Or set VCPKG_ROOT / VCPKG_INSTALLATION_ROOT / VCPKG_INSTALLED_DIR.
exit /b 1
