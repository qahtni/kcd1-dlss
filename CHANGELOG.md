# Changelog

## BETA 5 (2026-11-12)

Plugin build 0.8.2.

- Fix: hair and beards wobbled on characters that barely move. Alpha-blended materials write only a part of their motion into the game's velocity buffer (velocity times alpha), so strands reported 0-60 % of the head's motion. Each object-velocity pixel now takes the longest object vector within a few pixels (`VelocityDilate` in `KCDDLSS.ini`: -1 automatic, 0 off, 1-4 fixed radius).
- Fix: the game started at a low resolution for some seconds (for example 800x600) and the Resolution row in Graphics Settings showed that value. The game saved the DLSS render size as its resolution and snapped it to the nearest display mode at the next start. The plugin hooks the settings object's profile Save and Load: the saved value stays the output size, and a profile saved by an older version is repaired at load in fullscreen and borderless (`SaveOutputRes` in `KCDDLSS.ini`, 0 = off).
- Fix: `Mode = Auto` in an ini kept from BETA 3 never started DLSS. It now falls back to Quality and the log says so.
- Fix attempt for "no KCDDLSS.log at all, files correct": when another program has loaded the Windows `version.dll` before the game loads its own DLLs, the loader copy named `version.dll` is skipped. `Install-Windows.bat` now also installs the loader as `dinput8.dll` (a foreign `dinput8.dll` is left alone); the uninstaller removes it. Reproduced on the PC with `tools/windows/test/prever.c` (starts the game with the Windows `version.dll` preloaded): no log with `version.dll` only, plugin loads with the `dinput8.dll` copy.
- A "1280x720 > 1080p" entry of the Resolution row is no longer taken as a new output size.
- `Report-Windows.bat`: new sections for the running game (loaded modules, whether `KCDDLSS.asi` is loaded) and Windows security (Smart App Control, antivirus, Code Integrity blocks).
- Debug build: `res:` lines (one per change of the game's resolution settings, window size and display mode), console commands `vstat`, `vdump`, `vdil`.
- Verified on the PC (RTX 4070, 1920x1080: fullscreen and borderless, old and new profiles, menu Confirm) and on Bazzite (RX 9070 XT, 3840x2160, FSR 4 through OptiScaler: display mode stays 3840x2160 from the first frame; 2560x1440 for 7 s with the fix off). Not tested: plain windowed mode. The hair fix is proven on velocity-buffer dumps, not by eye.

## BETA 4 (2026-11-10)

Plugin build 0.8.1.

- Fix: "Unsupported" on a GeForce system that also has an integrated GPU (or any other non-NVIDIA adapter). The game creates a device on every adapter while it enumerates them, and the first non-NVIDIA one switched the upscaling off for good. The decision is now made on the device the renderer really draws with, before the startup settings are applied.
- `KCDDLSS.log` starts with an `env:` block for bug reports: Windows / Proton version, game build, the files in `Bin\Win64`, installed mods, shader cache size, OptiScaler settings, all graphics adapters and the loaded loader / proxy / driver dlls. A line is logged at 1000 and 20000 upscaled frames.
- New `Report-Windows.bat`: changes nothing, collects the file list, a loader check, the graphics cards and the log into `KCDDLSS-report.txt` and opens it. It also explains a missing `KCDDLSS.log` (mod not loaded).
- The log falls back to `%TEMP%\KCDDLSS.log` when `Bin\Win64` is not writable.
- `Uninstall-Windows.bat`: the "is another .asi plugin left" test uses `if exist`, which also works under Wine's cmd.
- The debug console commands `jtest`, `jfixx`/`jfixy`/`jfix`, `jsx`/`jsy` (jitter tests through `KCDDLSS.cmd`) are in the source.
- Verified on the PC (RTX 4070: NVIDIA DLSS and the OptiScaler download, installer and report script on the real game folder) and on Bazzite (RX 9070 XT, FSR 4 through OptiScaler). The integrated-GPU case itself could not be reproduced on our machines.

## BETA 3 (2026-11-09)

Plugin build 0.8.0.

- No more `Mods/KCDDLSS` pak: the two menu rows are inserted into the Advanced Graphics page in memory while the game loads it (`ISystem::LoadXmlFromFile` hooked; the page is read through the game's own pak system, so another mod's copy of the page gets the rows too). The installers remove the BETA 1/2 pak and its `mod_order.txt` line; by hand, delete `Mods/KCDDLSS`. `MenuRows = 0` in the ini turns the rows off.
- The "Unsupported" choice is only added when no upscaler exists.
- Verified on the PC (RTX 4070, 1920x1080 desktop, profile anti-aliasing 2, Performance: rows shown, a row change applied live, exclusive fullscreen and borderless window both upscale) and on Bazzite (Proton, OptiScaler FSR 4).
- Added later the same day: a second download, `KCDDLSS-BETA-3-OptiScaler.7z`, same plugin, with OptiScaler 0.9.4 included and set up by the installers (copies OptiScaler and its FSR / XeSS files, sets `NgxDll = OptiScaler.dll`; default `Dx11Upscaler=fsr31_12`, which OptiScaler upgrades to FSR 4 on RDNA 3 / 4). No NVIDIA DLSS file in it. Verified on the PC (RTX 4070, FSR 3.1 via DX12, 3D menu at 60 fps) and on Bazzite (GE-Proton 11-12 with its own OptiScaler switched off: RX 9070 XT, FSR 4 from the game-folder copy).
- Tried and dropped: the game's own alt-tab repair routine (`DeviceInfo::OnActivate`) as a replacement for the minimize/restore fallback kick. It rebuilds the swap chain but minimizes the window as well, so the kick stays; the routine is reachable through a `kick` line in `KCDDLSS.cmd` for experiments.

## BETA 2 (2026-11-09)

Plugin build 0.7.5. Two player reports fixed (details in BUGS.md):

- Black screen at start until an alt-tab, when the profile's anti-aliasing was SMAA 1TX/2TX (`r_AntialiasingMode` 2/3). The plugin's live switch to 0 on the render thread left the game's render-target stack unbalanced, so the native-res stretch drew nothing until a swap-chain rebuild. The AA mode is now forced to 0 before the first frame and pinned there (the cvar's Set/ForceSet entries are hooked), so the profile load and the settings menu can no longer switch it live.
- Crash at the first upscaled frame with ReShade installed as dxgi.dll: the sampler cache created its biased samplers through ReShade's proxy device function but with the real device as `this`. It now uses the device's own vtable entry.
- The render size is computed from the desktop resolution from the first frame; the game saves the plugin's render size as the profile resolution at exit, so a session whose upscale never ran would have shrunk the next one.
- Crashes are logged in KCDDLSS.log (module and stack modules); the minimize/restore fallback kick restores the window from its own thread and reports the result.

## BETA 1 (2026-11-08)

- First public release (beta). Plugin build 0.7.4: DLSS Super Resolution with the game's camera and object motion vectors, camera jitter and mip bias; modes Ultra Quality (0.83, the mod's own), Quality, Balanced, Performance, Ultra Performance, Native; DLSS 4 Transformer or CNN model; two rows in Options > Advanced Graphics; "Unsupported" rows and native rendering on a PC without an upscaler; FSR / XeSS through OptiScaler (not included); the 30 FPS Grass Fix and the conversation-sharpening removal built in; installers for Windows and Steam Deck / Linux.
