@echo off
REM ============================================================
REM  MoeKoeMusic Taskbar Lyrics - One-click Build Script.
REM ============================================================
REM  已废弃独立 CMake/vcpkg 构建逻辑：本脚本仅作为 build.cmd 的兼容代理入口，
REM  将全部参数原样转发给 build.cmd。
REM  用法与 build.cmd 完全一致，例如。
REM    scripts\build.bat release x64、
REM    scripts\build.bat release arm64、
REM    scripts\build.bat release all。
REM ============================================================

call "%~dp0..\build.cmd" %*
exit /b %ERRORLEVEL%
