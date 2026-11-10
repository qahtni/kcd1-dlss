@echo off
setlocal EnableExtensions DisableDelayedExpansion
rem DLSS for Kingdom Come: Deliverance @VERSION@ - installer for Windows.
rem Double-click it in the extracted archive, wherever that is (for example Downloads).
rem It finds the game itself: in every Steam library, else in the GOG Galaxy and Epic default
rem folders. To pick a folder yourself, drag the game folder onto this file.
rem What it does (nothing else):
rem   1. Checks the game is closed.
@STD@rem   2. Copies install\KCDDLSS.asi (the mod), install\KCDDLSS.ini (its settings, kept if you already
@STD@rem      have one), install\nvngx_dlss.dll (NVIDIA DLSS, kept if one is already there) and
@STD@rem      install\version.dll (Ultimate ASI Loader) to Bin\Win64, next to KingdomCome.exe.
@STD@rem      A different version.dll that is already there is left alone.
@OPTI@rem   2. Copies install\KCDDLSS.asi (the mod), install\KCDDLSS.ini (its settings, kept if you already
@OPTI@rem      have one), install\version.dll (Ultimate ASI Loader) and OptiScaler (install\OptiScaler:
@OPTI@rem      OptiScaler.dll and its FSR / XeSS files; OptiScaler.ini kept if you already have one) to
@OPTI@rem      Bin\Win64, next to KingdomCome.exe, and sets NgxDll = OptiScaler.dll in KCDDLSS.ini.
@OPTI@rem      A different version.dll or OptiScaler.dll that is already there is left alone.
rem   3. Renames a standalone KCDGrassFix.asi (30 FPS Grass Fix) to KCDGrassFix.asi.off: this mod
rem      contains the same fix. The uninstaller renames it back.
rem   4. Copies the uninstaller to the game folder as KCDDLSS-Uninstall.bat.
rem No game file is changed. The mod sets the game up itself at every start (native-resolution
rem output, TAA off, motion vectors on, full-resolution GI): no user.cfg edits needed.
title DLSS for Kingdom Come: Deliverance - install
set "ASI_SIZE=@ASI_SIZE@"
set "UAL_SIZE=@UAL_SIZE@"
set "DLSS_SIZE=@DLSS_SIZE@"
@OPTI@set "OPTI_SIZE=@OPTI_SIZE@"
set "SELF=%~dp0"
set "SELF=%SELF:~0,-1%"
set "ARG=%~1"
echo DLSS for Kingdom Come: Deliverance @VERSION@ - install
call :findgame
if not defined GAME goto :nogame
echo Game folder: "%GAME%"
set "B=%GAME%\Bin\Win64"
tasklist /fi "imagename eq KingdomCome.exe" 2>nul | find /i "KingdomCome.exe" >nul && goto :running
@STD@for %%F in (KCDDLSS.asi KCDDLSS.ini nvngx_dlss.dll version.dll) do if not exist "%SELF%\install\%%F" (set "ERR=install\%%F is missing from the extracted folder." & goto :fail)
@OPTI@for %%F in (KCDDLSS.asi KCDDLSS.ini version.dll OptiScaler\OptiScaler.dll OptiScaler\OptiScaler.ini) do if not exist "%SELF%\install\%%F" (set "ERR=install\%%F is missing from the extracted folder." & goto :fail)
call :size "%SELF%\install\KCDDLSS.asi" SZ
if not "%SZ%"=="%ASI_SIZE%" (set "ERR=install\KCDDLSS.asi is damaged. Download the mod again." & goto :fail)
call :size "%SELF%\install\version.dll" SZ
if not "%SZ%"=="%UAL_SIZE%" (set "ERR=install\version.dll is damaged. Download the mod again." & goto :fail)
@STD@call :size "%SELF%\install\nvngx_dlss.dll" SZ
@STD@if not "%SZ%"=="%DLSS_SIZE%" (set "ERR=install\nvngx_dlss.dll is damaged. Download the mod again." & goto :fail)
@OPTI@call :size "%SELF%\install\OptiScaler\OptiScaler.dll" SZ
@OPTI@if not "%SZ%"=="%OPTI_SIZE%" (set "ERR=install\OptiScaler\OptiScaler.dll is damaged. Download the mod again." & goto :fail)

copy /y /b "%SELF%\install\KCDDLSS.asi" "%B%\KCDDLSS.asi" >nul || goto :copyfail
del /q "%B%\KCDDLSS.log" 2>nul
echo Mod: Bin\Win64\KCDDLSS.asi copied.
if exist "%B%\KCDDLSS.ini" (
  echo Settings: Bin\Win64\KCDDLSS.ini is already there. Kept ^(delete it to get the defaults^).
) else (
  copy /y "%SELF%\install\KCDDLSS.ini" "%B%\KCDDLSS.ini" >nul || goto :copyfail
  echo Settings: Bin\Win64\KCDDLSS.ini copied.
)
@STD@if exist "%B%\nvngx_dlss.dll" (
@STD@  echo DLSS: Bin\Win64\nvngx_dlss.dll is already there. Kept.
@STD@) else (
@STD@  copy /y /b "%SELF%\install\nvngx_dlss.dll" "%B%\nvngx_dlss.dll" >nul || goto :copyfail
@STD@  copy /y "%SELF%\install\NVIDIA-DLSS-LICENSE.txt" "%B%\NVIDIA-DLSS-LICENSE.txt" >nul 2>nul
@STD@  echo DLSS: Bin\Win64\nvngx_dlss.dll copied ^(NVIDIA DLSS @DLSS_VERSION@^).
@STD@)
@OPTI@rem OptiScaler: a different OptiScaler.dll already there is the user's own setup and stays.
@OPTI@set "CUR="
@OPTI@if exist "%B%\OptiScaler.dll" call :size "%B%\OptiScaler.dll" CUR
@OPTI@if defined CUR if not "%CUR%"=="%OPTI_SIZE%" goto :optiforeign
@OPTI@for %%F in (OptiScaler.dll libxess.dll libxess_dx11.dll libxell.dll amd_fidelityfx_dx12.dll amd_fidelityfx_upscaler_dx12.dll amd_fidelityfx_vk.dll) do copy /y /b "%SELF%\install\OptiScaler\%%F" "%B%\%%F" >nul || goto :copyfail
@OPTI@if not exist "%B%\D3D12_Optiscaler" mkdir "%B%\D3D12_Optiscaler"
@OPTI@copy /y /b "%SELF%\install\OptiScaler\D3D12_Optiscaler\D3D12Core.dll" "%B%\D3D12_Optiscaler\D3D12Core.dll" >nul || goto :copyfail
@OPTI@if not exist "%B%\OptiScaler.ini" copy /y "%SELF%\install\OptiScaler\OptiScaler.ini" "%B%\OptiScaler.ini" >nul
@OPTI@if not exist "%B%\OptiScaler-Licenses" mkdir "%B%\OptiScaler-Licenses"
@OPTI@copy /y "%SELF%\install\OptiScaler\Licenses\*.*" "%B%\OptiScaler-Licenses" >nul 2>nul
@OPTI@echo OptiScaler: Bin\Win64\OptiScaler.dll and its FSR / XeSS files copied ^(OptiScaler @OPTI_VERSION@^).
@OPTI@goto :optiini
@OPTI@:optiforeign
@OPTI@echo OptiScaler: Bin\Win64\OptiScaler.dll is already there and is a different file. Left alone, the mod uses it.
@OPTI@:optiini
@OPTI@rem KCDDLSS.ini: NgxDll = OptiScaler.dll (the line is replaced, or added if an old ini has none)
@OPTI@set "KINI=%B%\KCDDLSS.ini"
@OPTI@powershell -NoProfile -ExecutionPolicy Bypass -Command "$f=$env:KINI; $n=0; $t=@([IO.File]::ReadAllLines($f) | ForEach-Object { if ($_ -match '^\s*NgxDll\s*=') { $n++; 'NgxDll = OptiScaler.dll' } else { $_ } }); if ($n -eq 0) { $t += 'NgxDll = OptiScaler.dll' }; [IO.File]::WriteAllLines($f, $t)" >nul 2>&1
@OPTI@findstr /r /i /c:"^NgxDll = OptiScaler\.dll" "%KINI%" >nul && echo Settings: KCDDLSS.ini set to NgxDll = OptiScaler.dll ^(the mod uses OptiScaler^). || echo Settings: could not edit KCDDLSS.ini. Set the line NgxDll = OptiScaler.dll in Bin\Win64\KCDDLSS.ini yourself.
set "CUR="
if exist "%B%\version.dll" call :size "%B%\version.dll" CUR
if defined CUR if not "%CUR%"=="%UAL_SIZE%" goto :foreign
copy /y /b "%SELF%\install\version.dll" "%B%\version.dll" >nul || goto :copyfail
copy /y "%SELF%\install\Ultimate-ASI-Loader-LICENSE.txt" "%B%\Ultimate-ASI-Loader-LICENSE.txt" >nul 2>nul
echo Loader: Bin\Win64\version.dll copied (Ultimate ASI Loader).
goto :grass
:foreign
echo Loader: Bin\Win64\version.dll is already there and is a different file. Left alone.
echo         If it is an ASI loader, the mod loads through it. Check Bin\Win64\KCDDLSS.log after a start.
echo         No KCDDLSS.log after a start = that version.dll is not an ASI loader ^(a renamed OptiScaler or
echo         another mod^): rename it to version.dll.old and run this installer again.

:grass
if exist "%B%\KCDGrassFix.asi" (
  ren "%B%\KCDGrassFix.asi" "KCDGrassFix.asi.off" && echo Grass: standalone 30 FPS Grass Fix set aside as KCDGrassFix.asi.off ^(this mod contains it^).
)

rem Options-menu rows: since 0.8.0 the plugin adds them to the page in memory. The Mods\KCDDLSS pak of BETA 1/2 must go (it would double the rows).
if exist "%GAME%\Mods\KCDDLSS" (rmdir /s /q "%GAME%\Mods\KCDDLSS" && echo Menu: old Mods\KCDDLSS pak of BETA 1/2 removed ^(the rows now come from the plugin itself^).)
if exist "%GAME%\Mods\mod_order.txt" (findstr /i /x "KCDDLSS" "%GAME%\Mods\mod_order.txt" >nul && (findstr /v /i /x "KCDDLSS" "%GAME%\Mods\mod_order.txt" > "%GAME%\Mods\mod_order.kcddlss-tmp" & move /y "%GAME%\Mods\mod_order.kcddlss-tmp" "%GAME%\Mods\mod_order.txt" >nul))

copy /y "%SELF%\Uninstall-Windows.bat" "%GAME%\KCDDLSS-Uninstall.bat" >nul && echo Uninstaller copied to the game folder: KCDDLSS-Uninstall.bat
echo.
echo Done. No launch options are needed on Windows. Start the game.
echo Bin\Win64\KCDDLSS.log then shows "dlss: CreateFeature ... result 0x1" once the menu is up.
echo DLSS mode: Options ^> Advanced Graphics ^> "DLSS / Upscaling" in the game, or Bin\Win64\KCDDLSS.ini.
echo Not working? Double-click Report-Windows.bat in this folder and post the text it shows.
@STD@echo AMD / Intel GPU: see README.txt, section "FSR / XeSS through OptiScaler".
@OPTI@echo Upscaler: OptiScaler. In the game press Insert for its overlay ^(FSR or XeSS^). More: OPTISCALER.txt.
echo.
pause
exit /b 0

:copyfail
set "ERR=Could not copy into Bin\Win64 (no permission? disk full?). Try right-click, Run as administrator."
set "CUR="
if exist "%B%\nvngx_dlss.dll" call :size "%B%\nvngx_dlss.dll" CUR
if defined CUR if not "%CUR%"=="%DLSS_SIZE%" del /q "%B%\nvngx_dlss.dll" 2>nul
goto :fail
:nogame
set "ERR=Kingdom Come: Deliverance not found. Drag the game folder (the one with Bin, Data and Engine) onto this file."
goto :fail
:running
set "ERR=The game is running. Close it first."
:fail
echo.
setlocal EnableDelayedExpansion
echo ERROR: !ERR!
endlocal
echo Nothing more was changed.
echo.
pause
exit /b 1

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
