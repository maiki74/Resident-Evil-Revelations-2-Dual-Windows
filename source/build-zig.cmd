@echo off
rem Builds without Visual Studio. Usage: build-zig.cmd "path\to\zig.exe"
setlocal
set ZIG=%~1
if "%ZIG%"=="" set ZIG=zig
cd /d "%~dp0"
"%ZIG%" c++ -target x86-windows-gnu -shared -O2 -Wno-macro-redefined -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Iminhook/include DualWindows.cpp -x c minhook/src/buffer.c minhook/src/hook.c minhook/src/trampoline.c minhook/src/hde/hde32.c -ld3d9 -ld3dcompiler_47 -luser32 -lgdi32 -lsetupapi -lhid -lcfgmgr32 -o ResidentEvilRevelations2.DualWindows.asi
if errorlevel 1 exit /b 1
"%ZIG%" c++ -target x86-windows-gnu -O2 -Wno-macro-redefined -D_WIN32_WINNT=0x0A00 DualWindowsTest.cpp -ld3d9 -luser32 -o DualWindowsTest.exe
exit /b %errorlevel%
