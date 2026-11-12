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
echo --- running game
call :running
echo.
echo --- Windows security
call :security
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
set "CUR2="
if exist "%B%\dinput8.dll" call :size "%B%\dinput8.dll" CUR2
if defined CUR2 if "%CUR2%"=="%UAL_SIZE%" echo dinput8.dll is this mod's second copy of the loader.
if defined CUR2 if not "%CUR2%"=="%UAL_SIZE%" echo dinput8.dll is a DIFFERENT file, %CUR2% bytes: another mod's loader.
if not defined CUR2 echo dinput8.dll is not there: older install. Run the installer of this version to add the second loader copy.
for %%P in (dxgi winmm dsound d3d11 dbghelp wininet winhttp) do if exist "%B%\%%P.dll" echo %%P.dll is in Bin\Win64: another mod's loader, ReShade or a renamed OptiScaler.
(echo.>"%B%\kcddlss-write-test.tmp") 2>nul
if not exist "%B%\kcddlss-write-test.tmp" echo Bin\Win64 is NOT writable for this user: the log goes to %TEMP%\KCDDLSS.log.
if exist "%B%\kcddlss-write-test.tmp" echo Bin\Win64 is writable.
del /q "%B%\kcddlss-write-test.tmp" 2>nul
exit /b

rem What the running game really loaded: tells a loader that is in the folder but not used from one that works.
:running
powershell -NoProfile -Command "$p = Get-Process KingdomCome -ErrorAction SilentlyContinue; if (-not $p) { 'The game is not running. Start it, wait for the main menu and run this file again: only then can it see whether the loader was loaded.' } else { foreach ($q in $p) { 'exe: ' + $q.Path; if ($q.Path -and $q.Path -notlike ($env:B + '\*')) { 'RESULT: the running game is in ANOTHER folder than the one checked here. Install the mod into that folder (drag it onto Install-Windows.bat).' }; try { $m = $q.Modules; $m | Where-Object { $_.ModuleName -match '^(version|dxgi|winmm|dinput8|d3d11|OptiScaler|nvngx.*|amd_fidelityfx.*)\.dll$|\.asi$' } | ForEach-Object { 'loaded: ' + $_.FileName }; $v = $m | Where-Object { $_.ModuleName -eq 'version.dll' }; if ($m | Where-Object { $_.ModuleName -eq 'KCDDLSS.asi' }) { 'RESULT: KCDDLSS.asi is loaded in the game.' } elseif (-not $v) { 'RESULT: no version.dll is loaded in the game at all.' } elseif (-not ($v | Where-Object { $_.FileName -like ($env:B + '\*') })) { 'RESULT: the game loaded the version.dll of Windows, not the loader in Bin\Win64.' } elseif (-not ($m | Where-Object { $_.ModuleName -eq 'KCDDLSS.asi' })) { 'RESULT: the loader is loaded but KCDDLSS.asi is not.' } else { 'RESULT: loader and KCDDLSS.asi are loaded.' } } catch { 'modules not readable (game started as administrator?): ' + $_.Exception.Message } } }" 2>nul
exit /b

:security
powershell -NoProfile -Command "$s = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy' -ErrorAction SilentlyContinue).VerifiedAndReputablePolicyState; 'Smart App Control: ' + $(if ($s -eq 1) { 'ON: it blocks unsigned dll files such as mod loaders' } elseif ($s -eq 2) { 'evaluation mode' } else { 'off' }); Get-CimInstance -Namespace root/SecurityCenter2 -ClassName AntiVirusProduct -ErrorAction SilentlyContinue | ForEach-Object { 'antivirus: ' + $_.displayName }; Get-WinEvent -LogName 'Microsoft-Windows-CodeIntegrity/Operational' -MaxEvents 400 -ErrorAction SilentlyContinue | Where-Object { $_.Message -match 'KingdomCome|KCDDLSS|OptiScaler' } | Select-Object -First 4 | ForEach-Object { 'blocked by Windows: ' + $_.TimeCreated + ' ' + ($_.Message -replace '\s+', ' ') }" 2>nul
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
