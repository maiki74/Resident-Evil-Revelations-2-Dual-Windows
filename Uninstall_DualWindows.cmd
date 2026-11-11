@echo off
rem DualWindows uninstaller for Resident Evil Revelations 2.
rem Removes only files DualWindows installs or creates. Never touches game files, FusionFix, saves or other mods.
rem Usage: double-click, or: Uninstall_DualWindows.cmd "C:\path\to\RESIDENT EVIL REVELATIONS 2"
setlocal EnableExtensions DisableDelayedExpansion
title DualWindows uninstaller
set "MOD=ResidentEvilRevelations2.DualWindows"
set "GAME="
set "FAILED=0"
set "DONE=0"
set "SCRIPTS="

echo ============================================
echo   DualWindows uninstaller
echo ============================================
echo.

rem ---- 1. Find the game folder (the one with rerev2.exe) ----
if not "%~1"=="" if exist "%~1\rerev2.exe" set "GAME=%~1"
if not defined GAME if exist "%~dp0rerev2.exe" set "GAME=%~dp0."
if not defined GAME if exist "%~dp0..\rerev2.exe" set "GAME=%~dp0.."
if not defined GAME call :FindSteam
if not defined GAME (
    echo Could not find the game folder automatically.
    set /p "GAME=Paste the game folder that contains rerev2.exe, then press Enter: "
)
if not defined GAME goto :NoGame
set "GAME=%GAME:"=%"
if not exist "%GAME%\rerev2.exe" goto :NoGame
for %%I in ("%GAME%") do set "GAME=%%~fI"
set "SCRIPTS=%GAME%\scripts"
echo Game folder: "%GAME%"
echo.

rem ---- 2. The game must be closed (its files are in use while it runs) ----
tasklist /FI "IMAGENAME eq rerev2.exe" /NH 2>nul | find /i "rerev2.exe" >nul
if not errorlevel 1 (
    echo ERROR: Resident Evil Revelations 2 is running.
    echo Close the game normally, then run this uninstaller again. Nothing was removed.
    goto :End
)

rem ---- 3. Is anything installed? ----
set "FOUND=0"
for %%F in ("%SCRIPTS%\%MOD%.asi" "%SCRIPTS%\%MOD%.asi.disabled" "%SCRIPTS%\%MOD%.ini" "%SCRIPTS%\%MOD%.log" "%SCRIPTS%\DualWindowsV2Backup") do if exist "%%~F" set "FOUND=1"
if "%FOUND%"=="0" (
    echo DualWindows is not installed in this game folder. Nothing to remove.
    goto :End
)

echo This removes the DualWindows mod files:
echo   scripts\%MOD%.asi  (and .asi.disabled if present)
echo   %%TEMP%%\DualWindows_xinput1_4.dll  (temporary copy the mod makes)
echo The game, FusionFix, dinput8.dll, your saves and other mods are NOT touched.
echo.
choice /c YN /n /m "Uninstall DualWindows now? [Y/N] "
if errorlevel 2 (
    echo Cancelled. Nothing was removed.
    goto :End
)
echo.

rem ---- 4. Mod files ----
call :Remove "%SCRIPTS%\%MOD%.asi"
call :Remove "%SCRIPTS%\%MOD%.asi.disabled"
call :Remove "%TEMP%\DualWindows_xinput1_4.dll"

rem ---- 5. Optional: restore the fullscreen setting the installer changed ----
set "DW_BACKUP=%SCRIPTS%\DualWindowsV2Backup\config.ini"
set "DW_CONFIG=%LOCALAPPDATA%\CAPCOM\RESIDENT EVIL REVELATIONS2\config.ini"
if exist "%DW_BACKUP%" if exist "%DW_CONFIG%" call :RestoreFullscreen

rem ---- 6. Optional: settings, logs, backups (asked one by one) ----
if exist "%SCRIPTS%\%MOD%.ini" (
    echo.
    choice /c YN /n /m "Delete your saved DualWindows control layout, file %MOD%.ini? [Y/N] "
    if not errorlevel 2 call :Remove "%SCRIPTS%\%MOD%.ini"
)
set "LOGS=0"
for %%F in ("%MOD%.log" "capture-game.bmp" "capture-player1.bmp" "capture-player2.bmp" "capture-shared.bmp") do if exist "%SCRIPTS%\%%~F" set "LOGS=1"
if "%LOGS%"=="1" (
    echo.
    choice /c YN /n /m "Delete DualWindows log and capture files? [Y/N] "
    if not errorlevel 2 for %%F in ("%MOD%.log" "capture-game.bmp" "capture-player1.bmp" "capture-player2.bmp" "capture-shared.bmp") do call :Remove "%SCRIPTS%\%%~F"
)
if exist "%SCRIPTS%\DualWindowsV2Backup\" (
    echo.
    echo The folder scripts\DualWindowsV2Backup holds backups made by DualWindows,
    echo including a copy of your game settings from before the install.
    choice /c YN /n /m "Delete the DualWindowsV2Backup folder? [Y/N] "
    if not errorlevel 2 call :RemoveFolder "%SCRIPTS%\DualWindowsV2Backup"
)

echo.
if "%FAILED%"=="0" (
    echo SUCCESS: DualWindows was uninstalled.
    set "DONE=1"
) else (
    echo WARNING: some files could not be removed. See the messages above.
    echo Close anything that may use them and run this uninstaller again.
)
goto :End

rem ================= helpers =================
:Remove
if not exist "%~1" exit /b 0
del /f /q "%~1" >nul 2>&1
if exist "%~1" (
    echo   ERROR: could not remove "%~1"
    set "FAILED=1"
) else (
    echo   Removed "%~1"
)
exit /b 0

:RemoveFolder
rd /s /q "%~1" >nul 2>&1
if exist "%~1\" (
    echo   ERROR: could not remove "%~1"
    set "FAILED=1"
) else (
    echo   Removed "%~1"
)
exit /b 0

:RestoreFullscreen
echo.
echo The installer switched the game to windowed mode and kept your old setting.
choice /c YN /n /m "Restore your previous fullscreen setting? [Y/N] "
if errorlevel 2 exit /b 0
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$o=Get-Content -LiteralPath $env:DW_BACKUP -Raw; $m=[regex]::Match($o,'(?m)^FullScreen=(ON|OFF)'); if(!$m.Success){exit 2}; $t=Get-Content -LiteralPath $env:DW_CONFIG -Raw; $t=[regex]::Replace($t,'(?m)^FullScreen=[^\r\n]*','FullScreen='+$m.Groups[1].Value); [IO.File]::WriteAllText($env:DW_CONFIG,$t,[Text.Encoding]::ASCII)"
if errorlevel 1 (
    echo   ERROR: could not restore the fullscreen setting. You can change it in the game's options.
    set "FAILED=1"
) else (
    echo   Fullscreen setting restored.
)
exit /b 0

:FindSteam
rem Looks in every Steam library (no fixed path).
set "STEAM="
for /f "tokens=2,*" %%A in ('reg query "HKCU\Software\Valve\Steam" /v SteamPath 2^>nul ^| find /i "SteamPath"') do set "STEAM=%%B"
if not defined STEAM exit /b 0
set "STEAM=%STEAM:/=\%"
call :TryLibrary "%STEAM%"
if defined GAME exit /b 0
if not exist "%STEAM%\steamapps\libraryfolders.vdf" exit /b 0
for /f "usebackq tokens=1,*" %%A in (`findstr /c:"\"path\"" "%STEAM%\steamapps\libraryfolders.vdf"`) do if not defined GAME call :TryLibrary %%B
exit /b 0

:TryLibrary
set "LIB=%~1"
set "LIB=%LIB:\\=\%"
if exist "%LIB%\steamapps\common\RESIDENT EVIL REVELATIONS 2\rerev2.exe" set "GAME=%LIB%\steamapps\common\RESIDENT EVIL REVELATIONS 2"
exit /b 0

:NoGame
echo ERROR: That folder does not contain rerev2.exe. Nothing was removed.
echo Run this file again and paste the game folder (Steam: Manage, Browse local files).
goto :End

:End
echo.
pause
rem The installer puts a copy of this file in the game's scripts folder: remove that copy last.
if "%DONE%"=="1" if /i "%~dp0"=="%SCRIPTS%\" (goto) 2>nul & del /f /q "%~f0"
endlocal
exit /b 0
