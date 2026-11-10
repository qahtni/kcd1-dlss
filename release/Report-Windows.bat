@echo off
setlocal EnableExtensions DisableDelayedExpansion
rem DLSS for Kingdom Come: Deliverance @VERSION@ - support report for Windows.
rem Double-click it when the mod does not work. It changes nothing: it only looks at the game folder
rem and writes KCDDLSS-report.txt next to this file (file list, loader check, GPU, the mod's log),
rem then opens it. Paste that text into your comment or bug report (check it for anything private first).
rem It finds the game like the installer does; to pick a folder yourself, drag the game folder onto this file.
title DLSS for Kingdom Come: Deliverance - report
set "UAL_SIZE=@UAL_SIZE@"
set "SELF=%~dp0"
set "SELF=%SELF:~0,-1%"
set "ARG=%~1"
call :findgame
set "R=%SELF%\KCDDLSS-report.txt"
(echo.>"%R%") 2>nul || set "R=%TEMP%\KCDDLSS-report.txt"
if defined GAME goto :found
>"%R%" echo KCDDLSS report @VERSION@: Kingdom Come: Deliverance not found. Drag the game folder ^(the one with Bin, Data and Engine^) onto Report-Windows.bat.
goto :show
:found
set "B=%GAME%\Bin\Win64"
rem No ( ) block around lines that expand the game path: "Program Files (x86)" would end the block.
call :body > "%R%"
:show
echo Report written: "%R%"
start "" notepad "%R%"
exit /b 0

:body
echo KCDDLSS report @VERSION@, %DATE% %TIME%
ver
echo Game folder: %GAME%
echo.
echo --- Bin\Win64: dll, asi, ini, log (size, name, date)
for %%F in ("%B%\*.dll" "%B%\*.asi" "%B%\*.asi.off" "%B%\*.ini" "%B%\*.log") do echo %%~zF  %%~nxF  %%~tF
echo.
echo --- loader
call :loader
echo.
echo --- Mods
dir /b /ad "%GAME%\Mods" 2>nul
echo.
echo --- Engine shader cache
for %%F in ("%GAME%\Engine\shadercache*") do echo %%~zF  %%~nxF
echo.
echo --- graphics cards
powershell -NoProfile -Command "Get-CimInstance Win32_VideoController | ForEach-Object { $_.Name + ', driver ' + $_.DriverVersion }" 2>nul
echo.
echo --- OptiScaler.ini
if exist "%B%\OptiScaler.ini" findstr /b /i "Dx11Upscaler= OverlayMenu= ShortcutKey= Fsr4Update= LoadAsiPlugins=" "%B%\OptiScaler.ini"
if not exist "%B%\OptiScaler.ini" echo not there
echo.
echo --- KCDDLSS.log
call :log
exit /b

:loader
tasklist /fi "imagename eq KingdomCome.exe" 2>nul | find /i "KingdomCome.exe" >nul && echo The game is running right now.
if not exist "%B%\KCDDLSS.asi" echo KCDDLSS.asi is MISSING from Bin\Win64: the mod is not installed in this game folder.
if not exist "%B%\version.dll" echo version.dll is MISSING from Bin\Win64: nothing loads the mod. Run the installer again; if it disappears again, an antivirus is removing it.
set "CUR="
if exist "%B%\version.dll" call :size "%B%\version.dll" CUR
if defined CUR if "%CUR%"=="%UAL_SIZE%" echo version.dll is this mod's loader, Ultimate ASI Loader.
if defined CUR if not "%CUR%"=="%UAL_SIZE%" echo version.dll is a DIFFERENT file, %CUR% bytes, not this mod's loader. If it is not an ASI loader, the mod never loads.
for %%P in (dxgi winmm dinput8 dsound d3d11 dbghelp wininet winhttp) do if exist "%B%\%%P.dll" echo %%P.dll is in Bin\Win64: another mod's loader, ReShade or a renamed OptiScaler.
(echo.>"%B%\kcddlss-write-test.tmp") 2>nul
if not exist "%B%\kcddlss-write-test.tmp" echo Bin\Win64 is NOT writable for this user: the log goes to %TEMP%\KCDDLSS.log.
if exist "%B%\kcddlss-write-test.tmp" echo Bin\Win64 is writable.
del /q "%B%\kcddlss-write-test.tmp" 2>nul
exit /b

:log
set "L=%B%\KCDDLSS.log"
if not exist "%L%" if exist "%TEMP%\KCDDLSS.log" set "L=%TEMP%\KCDDLSS.log"
if not exist "%L%" echo There is no KCDDLSS.log: the mod was not loaded when the game last started (see the loader lines above).
if not exist "%L%" exit /b
echo "%L%", first 250 lines:
powershell -NoProfile -Command "Get-Content -LiteralPath $env:L -TotalCount 250" 2>nul
exit /b

:size
set "%~2=%~z1"
exit /b

rem Game folder search: around this file, the dragged folder, Steam libraries, GOG/Epic defaults.
:findgame
set "GAME="
for %%I in ("%SELF%\..\..") do if exist "%%~fI\Bin\Win64\KingdomCome.exe" set "GAME=%%~fI"
if defined GAME exit /b
if exist "%SELF%\Bin\Win64\KingdomCome.exe" (set "GAME=%SELF%" & exit /b)
if defined ARG call :trydir "%ARG%"
if defined GAME exit /b
set "STEAM="
for /f "tokens=2,*" %%A in ('reg query "HKCU\Software\Valve\Steam" /v SteamPath 2^>nul ^| find /i "SteamPath"') do set "STEAM=%%B"
if not defined STEAM set "STEAM=%ProgramFiles(x86)%\Steam"
set "STEAM=%STEAM:/=\%"
call :trylib "%STEAM%"
if exist "%STEAM%\steamapps\libraryfolders.vdf" for /f usebackq^ tokens^=2^,4^ delims^=^" %%A in ("%STEAM%\steamapps\libraryfolders.vdf") do if /i "%%A"=="path" call :trylib "%%B"
if defined GAME exit /b
call :trydir "%ProgramFiles(x86)%\GOG Galaxy\Games\Kingdom Come Deliverance"
call :trydir "%ProgramFiles%\Epic Games\KingdomComeDeliverance"
exit /b
:trylib
if defined GAME exit /b
set "L=%~1"
set "L=%L:\\=\%"
call :trydir "%L%\steamapps\common\KingdomComeDeliverance"
exit /b
:trydir
if not defined GAME if exist "%~1\Bin\Win64\KingdomCome.exe" set "GAME=%~f1"
exit /b
