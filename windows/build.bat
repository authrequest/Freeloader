@echo off
REM SPDX-License-Identifier: AGPL-3.0-or-later
REM Build plex_patch.dll + plex_inject.exe for Windows x64.
REM Requires zig 0.13.0 (auto-fetched to toolchain/ by the Linux build).
setlocal

cd /d "%~dp0"
set ROOT=%~dp0..

REM Resolve zig: %ZIG% override, then toolchain dir, then PATH.
if defined ZIG if exist "%ZIG%" goto :found
set ZIG=%ROOT%\toolchain\zig-windows-x86_64-0.13.0\zig.exe
if exist "%ZIG%" goto :found
where zig >nul 2>&1 && set ZIG=zig && goto :found
echo [x] zig not found. Set ZIG= or run build.sh first (which downloads zig).
exit /b 1

:found
echo [*] Using zig: %ZIG%
"%ZIG%" version

set TARGET=x86_64-windows-gnu
set CFLAGS=-target %TARGET% -O2 -I "%ROOT%\third_party\zydis" -I src
set CXXFLAGS=-target %TARGET% -std=c++20 -O2 -I "%ROOT%\third_party\zydis" -I src

if not exist build mkdir build

echo [*] compiling Zydis.c (C)
"%ZIG%" cc %CFLAGS% -c "%ROOT%\third_party\zydis\Zydis.c" -o build\Zydis.o
if errorlevel 1 goto :fail

echo [*] compiling dllmain.cpp (C++)
"%ZIG%" c++ %CXXFLAGS% -c src\dllmain.cpp -o build\dllmain.o
if errorlevel 1 goto :fail

echo [*] linking plex_patch.dll
"%ZIG%" c++ -target %TARGET% -shared -o build\plex_patch.dll build\dllmain.o build\Zydis.o -lkernel32
if errorlevel 1 goto :fail

echo [*] compiling + linking plex_inject.exe
"%ZIG%" c++ %CXXFLAGS% -o build\plex_inject.exe src\injector.cpp -lkernel32
if errorlevel 1 goto :fail

del /q build\Zydis.o build\dllmain.o 2>nul

echo.
echo [+] BUILD SUCCESSFUL
echo     build\plex_patch.dll   - godmode DLL (inject into PMS)
echo     build\plex_inject.exe  - injector (finds or launches PMS)
echo.
echo Usage:
echo   1. Copy both files to any directory.
echo   2. Start Plex Media Server normally, then:
echo        build\plex_inject.exe
echo      Or launch PMS through the injector:
echo        build\plex_inject.exe --launch "C:\Program Files\Plex\Plex Media Server\Plex Media Server.exe"
echo   3. Verify with DebugView: look for [plex_patch INF] messages.
exit /b 0

:fail
echo [x] BUILD FAILED
exit /b 1
