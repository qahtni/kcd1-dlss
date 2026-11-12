@echo off
setlocal EnableExtensions DisableDelayedExpansion
rem DLSS for Kingdom Come: Deliverance @VERSION@ - uninstaller for Windows.
rem Works from the game folder (the installer leaves a copy there as KCDDLSS-Uninstall.bat)
rem or from the extracted archive (it then finds the game the same way the installer does; or drag
rem the game folder onto this file).
rem What it does:
rem   1. Deletes Bin\Win64\KCDDLSS.asi, KCDDLSS.ini, KCDDLSS.log, KCDDLSS_*.ppm and nvngx_dlss.dll
rem      (only the copy this mod installed, recognised by its size).
rem   2. Renames KCDGrassFix.asi.off back to KCDGrassFix.asi (the standalone 30 FPS Grass Fix).
rem   3. Deletes Bin\Win64\version.dll if it is the loader this mod installed and no other .asi
rem      plugin is left in Bin\Win64 (another plugin would still need it).
rem   4. Deletes KCDDLSS-Uninstall.bat from the game folder.
@OPTI@rem   5. Deletes the OptiScaler files this download installed (OptiScaler.dll recognised by its size,
@OPTI@rem      OptiScaler.ini, OptiScaler.log, the FSR / XeSS files, D3D12_Optiscaler, OptiScaler-Licenses).
rem The game's own settings are untouched: the mod never wrote to user.cfg or your profile.
title DLSS for Kingdom Come: Deliverance - uninstall
set "UAL_SIZE=@UAL_SIZE@"
set "DLSS_SIZE=@DLSS_SIZE@"
@OPTI@set "OPTI_SIZE=@OPTI_SIZE@"
set "SELF=%~dp0"
set "SELF=%SELF:~0,-1%"
set "ARG=%~1"
echo DLSS for Kingdom Come: Deliverance @VERSION@ - uninstall
call :findgame
if not defined GAME goto :nogame
echo Game folder: "%GAME%"
set "B=%GAME%\Bin\Win64"
tasklist /fi "imagename eq KingdomCome.exe" 2>nul | find /i "KingdomCome.exe" >nul && goto :running

del /q "%B%\KCDDLSS.asi" "%B%\KCDDLSS.ini" "%B%\KCDDLSS.log" "%B%\KCDDLSS.cmd" "%B%\KCDDLSS_*.ppm" 2>nul
if exist "%B%\KCDDLSS.asi" (set "ERR=Could not delete Bin\Win64\KCDDLSS.asi. Try right-click, Run as administrator." & goto :fail)
echo Mod: Bin\Win64\KCDDLSS.asi and its settings removed.
rem no if-block here: %CUR% would be expanded before call :size sets it (delayed expansion is off)
if not exist "%B%\nvngx_dlss.dll" goto :grass
call :size "%B%\nvngx_dlss.dll" CUR
if "%CUR%"=="%DLSS_SIZE%" (del /q "%B%\nvngx_dlss.dll" "%B%\NVIDIA-DLSS-LICENSE.txt" 2>nul & echo DLSS: Bin\Win64\nvngx_dlss.dll removed.) else (echo DLSS: Bin\Win64\nvngx_dlss.dll is not the file this mod installs. Left alone.)
:grass
@OPTI@if not exist "%B%\OptiScaler.dll" goto :optidone
@OPTI@call :size "%B%\OptiScaler.dll" CUR
@OPTI@if not "%CUR%"=="%OPTI_SIZE%" (echo OptiScaler: Bin\Win64\OptiScaler.dll is not the file this mod installs. Left alone.& goto :optidone)
@OPTI@for %%F in (OptiScaler.dll OptiScaler.ini OptiScaler.log libxess.dll libxess_dx11.dll libxell.dll amd_fidelityfx_dx12.dll amd_fidelityfx_upscaler_dx12.dll amd_fidelityfx_vk.dll) do del /q "%B%\%%F" 2>nul
@OPTI@if exist "%B%\OptiScaler-Licenses" rmdir /s /q "%B%\OptiScaler-Licenses"
@OPTI@if exist "%B%\D3D12_Optiscaler" rmdir /s /q "%B%\D3D12_Optiscaler"
@OPTI@echo OptiScaler: its files removed from Bin\Win64.
@OPTI@:optidone
if exist "%GAME%\Mods\KCDDLSS" rmdir /s /q "%GAME%\Mods\KCDDLSS" && echo Menu: Mods\KCDDLSS removed.
if exist "%GAME%\Mods\mod_order.txt" (findstr /v /i /x "KCDDLSS" "%GAME%\Mods\mod_order.txt" > "%GAME%\Mods\mod_order.kcddlss-tmp" & move /y "%GAME%\Mods\mod_order.kcddlss-tmp" "%GAME%\Mods\mod_order.txt" >nul)
if exist "%B%\KCDGrassFix.asi.off" if not exist "%B%\KCDGrassFix.asi" ren "%B%\KCDGrassFix.asi.off" "KCDGrassFix.asi" && echo Grass: standalone 30 FPS Grass Fix restored (KCDGrassFix.asi).
set "CUR2="
if exist "%B%\dinput8.dll" call :size "%B%\dinput8.dll" CUR2
set "OTHER2="
if exist "%B%\*.asi" set "OTHER2=1"
if "%CUR2%"=="%UAL_SIZE%" if not defined OTHER2 del /q "%B%\dinput8.dll" 2>nul & echo Loader: Bin\Win64\dinput8.dll removed.
if not exist "%B%\version.dll" (echo Loader: no version.dll there.& goto :done)
call :size "%B%\version.dll" CUR
if not "%CUR%"=="%UAL_SIZE%" (echo Loader: Bin\Win64\version.dll is not the file this mod installs. Left alone.& goto :done)
set "OTHER="
rem "if exist" with a wildcard also works under Wine's cmd (the for form did not expand there)
if exist "%B%\*.asi" set "OTHER=1"
if exist "%B%\scripts\*.asi" set "OTHER=1"
if exist "%B%\plugins\*.asi" set "OTHER=1"
if defined OTHER (echo Loader: another .asi plugin is still in Bin\Win64. version.dll left in place for it.& goto :done)
del /q "%B%\version.dll" "%B%\Ultimate-ASI-Loader-LICENSE.txt" 2>nul
echo Loader: Bin\Win64\version.dll removed.

:done
echo.
echo Done. The game renders as before (its own TAA and upscaling settings apply again).
echo.
pause
if /i "%SELF%"=="%GAME%" goto :delself
del /q "%GAME%\KCDDLSS-Uninstall.bat" 2>nul
exit /b 0
:delself
(goto) 2>nul & del /q "%~f0"

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
