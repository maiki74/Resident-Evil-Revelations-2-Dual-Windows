@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl /nologo /c /O2 /MT /Iminhook\include minhook\src\buffer.c minhook\src\hook.c minhook\src\trampoline.c minhook\src\hde\hde32.c
if errorlevel 1 exit /b 1
cl /nologo /LD /O2 /MT /EHsc /utf-8 /std:c++17 DualWindows.cpp buffer.obj hook.obj trampoline.obj hde32.obj /link d3d9.lib d3dcompiler.lib user32.lib gdi32.lib setupapi.lib hid.lib cfgmgr32.lib /OUT:ResidentEvilRevelations2.DualWindows.asi
if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /EHsc /utf-8 /std:c++17 DualWindowsTest.cpp /link d3d9.lib user32.lib /OUT:DualWindowsTest.exe
exit /b %errorlevel%

