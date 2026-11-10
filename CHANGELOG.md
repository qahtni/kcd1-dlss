# Changelog

## Unreleased

- Epic Games Store version 1.9.8b supported. Its `WHGame.dll` is a different build from Steam 1.9.8, so every fixed address the plugin used had moved: the log said "creation-site bytes differ", "jitter generator bytes differ" and "LoadXmlFromFile slot is ..." and no DLSS ran. The addresses are now one table with a row per build, and the plugin picks the row at start by checking which build's functions sit in the renderer and `CSystem` vtables (read only). The log names the build: `WHGame.dll build: Epic 1.9.8b (2/2 identity checks)`. Confirmed in game on Epic 1.9.8b. Steam 1.9.8 uses the same values and checks as before (unchanged by inspection, not re-run); see HOW-IT-WORKS.md, "Game builds".
- On an unknown game version the plugin now skips everything at fixed addresses (DLSS, motion-vector and jitter patches, menu rows, console cvars) instead of reading the Steam addresses; the grass fix and the sharpening removal, which find their code by pattern, still apply.

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
