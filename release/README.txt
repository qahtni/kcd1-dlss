DLSS FOR KINGDOM COME: DELIVERANCE @VERSION@
Kingdom Come: Deliverance (Steam 1.9.8, Epic 1.9.8b)
Author: mrtaxerman
@OPTI@
@OPTI@THIS IS THE DOWNLOAD WITH OPTISCALER @OPTI_VERSION@ INCLUDED, for AMD and Intel
@OPTI@cards (and NVIDIA cards that prefer FSR or XeSS): it upscales with AMD FSR
@OPTI@2.2 / 3.1 / 4 or Intel XeSS instead of NVIDIA DLSS, and the installers set
@OPTI@it up. On a GeForce RTX card take the standard download, it has DLSS.

The game has no modern upscaler. Its only way to run cheaper is the
"native resolution" option, which renders the scene at 720p or 900p and
stretches it to the screen with a plain filter, which looks terrible. This
mod puts NVIDIA DLSS (Super Resolution) in that place: the scene is
rendered at a lower resolution and DLSS builds the full-resolution picture
from it, with the game's own camera motion vectors, its moving-object
motion vectors, a camera jitter added by the mod, and texture detail kept
sharp (mip bias). On AMD and Intel cards the same hook drives FSR 3.1,
FSR 4 or XeSS through OptiScaler, see OPTISCALER.txt.

The mod also contains the 30 FPS Grass Fix (grass animates at the real
frame rate) and removes the forced sharpening in conversations.

REQUIREMENTS
@STD@- NVIDIA GeForce RTX 20 series or newer with a current driver (the shipped
@STD@  DLSS is version @DLSS_VERSION@). Other cards: OPTISCALER.txt.
@OPTI@- Any DirectX 11 graphics card. FSR 4 needs a Radeon RX 7000 / 9000;
@OPTI@  XeSS runs best on Intel Arc. Details: OPTISCALER.txt.
- Windows, or Steam Deck / Linux with Proton (installer included).
- Game version 1.9.8 (Steam) or 1.9.8b (Epic Games Store). On any other
  version the plugin says so in KCDDLSS.log and applies only the grass fix
  and the sharpening removal (they find their code by pattern); DLSS and
  the menu rows are skipped.

FILES IN THIS ARCHIVE
- install\KCDDLSS.asi: the mod, a plugin. When the game starts it hooks the
  renderer in memory. No game file is changed.
- install\KCDDLSS.ini: the settings (DLSS mode, preset, sharpness, ...).
  Read at every game start. Kept on reinstall.
@STD@- install\nvngx_dlss.dll: NVIDIA DLSS @DLSS_VERSION@ (NVIDIA RTX SDKs
@STD@  licence, install\NVIDIA-DLSS-LICENSE.txt). Kept if one is already there.
@OPTI@- install\OptiScaler\: OptiScaler @OPTI_VERSION@ (OptiScaler.dll, OptiScaler.ini and the
@OPTI@  AMD FSR and Intel XeSS files it uses), their licences in
@OPTI@  install\OptiScaler\Licenses. OPTISCALER.txt says where the source code is.
- install\version.dll: Ultimate ASI Loader 10.0.2 by ThirteenAG (MIT
  licence, install\Ultimate-ASI-Loader-LICENSE.txt). It loads .asi plugins.
- debug\KCDDLSS.asi: the same mod with full diagnostics in KCDDLSS.log.
  Only for bug reports, see REPORTING A PROBLEM.
- Install-Windows.bat, Uninstall-Windows.bat
- Install-Linux-SteamDeck.sh, Uninstall-Linux-SteamDeck.sh
  Plain text: open them in a text editor to see exactly what they do.
- OPTISCALER.txt: FSR / XeSS on AMD, Intel (and NVIDIA) through OptiScaler.

INSTALL (WINDOWS)
1. Extract the archive anywhere, for example to Downloads.
2. Double-click Install-Windows.bat in the extracted folder.
It finds the game in your Steam libraries (else GOG / Epic default folders)
@STD@and copies the four files to Bin\Win64.
@OPTI@and copies the files to Bin\Win64 (the mod, its settings, the loader and
@OPTI@OptiScaler), then sets NgxDll = OptiScaler.dll in KCDDLSS.ini.
If it can't find the game, drag the game folder onto Install-Windows.bat.
Windows needs no launch options.
If you have the standalone 30 FPS Grass Fix, the installer sets it aside
(KCDGrassFix.asi.off) because this mod contains the same fix.

INSTALL ON STEAM DECK / LINUX
1. Switch to Desktop Mode and extract the archive anywhere, for example to
   Downloads.
2. Exit Steam: right-click the Steam icon in the taskbar > Exit Steam.
3. Right-click an empty spot in the extracted folder > Open Terminal Here,
   type
     bash Install-Linux-SteamDeck.sh
   and press Enter.
It finds the game in every Steam library (SD card included, also Flatpak
Steam) and in ~/Games (Heroic, Lutris), copies the files to Bin/Win64
and tells Proton to load version.dll from the game folder (Proton prefers its
own built-in version.dll otherwise, and the mod would not load). It does
that in two places: the game's launch options in Steam get
WINEDLLOVERRIDES="version=n,b" in front of what is already there (Steam
must be closed for this, it rewrites its settings when it exits), and the
game's Proton prefix gets the same setting (works with Steam running, but
only after the game has been started once). If neither was possible, the
installer shows the exact launch options to type in: right-click the game >
Properties > General > Launch Options. For a game somewhere else:
  bash Install-Linux-SteamDeck.sh "/path/to/KingdomComeDeliverance"
@STD@Proton 9 or newer. DLSS on Linux needs the NVIDIA driver; AMD and Intel
@STD@cards use OptiScaler, see OPTISCALER.txt (on Linux that is one Proton
@STD@switch, no files to copy).
@OPTI@Proton 9 or newer. OptiScaler is loaded from the game folder, so no
@OPTI@OptiScaler Proton build is needed; see OPTISCALER.txt.

INSTALL BY HAND (NO SCRIPT)
To open the game folder: in Steam, right-click Kingdom Come: Deliverance >
Manage > Browse local files.
@STD@1. Copy install\KCDDLSS.asi, install\KCDDLSS.ini, install\nvngx_dlss.dll
@STD@   and install\version.dll into Bin\Win64 (the folder with KingdomCome.exe).
@OPTI@1. Copy install\KCDDLSS.asi, install\KCDDLSS.ini, install\version.dll and
@OPTI@   the contents of install\OptiScaler (not its Licenses folder) into Bin\Win64
@OPTI@   (the folder with KingdomCome.exe). In Bin\Win64\KCDDLSS.ini set the line
@OPTI@   NgxDll = OptiScaler.dll.
   Coming from BETA 1 or 2: delete the folder Mods\KCDDLSS (and its line in
   Mods\mod_order.txt); the plugin draws the menu rows itself now.
2. Steam Deck / Linux only: set the game's launch options to
     WINEDLLOVERRIDES="version=n,b" %command%
   If there was something in the box already, keep it after %command%.

WHAT THE MOD DOES AT EVERY START
No user.cfg or profile edits are needed. At start, and again whenever the
game's settings menu puts its own values back, the plugin sets:
- r_FullscreenNativeRes 1: the scene renders at the DLSS render size, the
  UI at your screen size. Leave the game's "native resolution" option
  alone, the mod owns it.
- r_AntialiasingMode 0: the game's SMAA/TAA is off, DLSS does the
  anti-aliasing.
- r_MotionVectors 1 and r_MotionBlurGBufferVelocity 1: the motion data
  DLSS needs (motion blur itself stays whatever you set).
- e_svoTI_ResScaleBase 1: the global illumination (SVOGI) cone tracing runs
  at the render resolution instead of half of it. At DLSS Performance the
  game's half would be a quarter of your screen, upscaled twice, and the
  lighting looks blotchy. Costs a little GPU time; GIFullRes = 0 in the ini
  turns it off.
Your screen resolution and everything else in the options stay yours. The
values are set in memory only and are not written to your profile.

THE DLSS SETTINGS IN THE GAME'S MENU
Options > Advanced Graphics has two new rows:
- "DLSS / Upscaling": Off, Ultra Quality, Quality, Balanced, Performance,
  Ultra Perf (= Ultra Performance; the menu field is too narrow for the
  full word). Off = no DLSS, native resolution. Render scale 0.83 / 0.67 /
  0.58 / 0.5 / 0.33 of your screen size. Ultra Quality is for DLSS's
  anti-aliasing with the least upscaling. There is no DLAA mode (DLSS at native resolution);
  Quality is the sharpest the mod offers.
- "DLSS Model": Transformer (the DLSS 4 model: sharper, more stable, a
  little more GPU time) or CNN (the classic DLSS 3 model).
On a PC without an upscaler (no NVIDIA card and no OptiScaler) both rows
show "Unsupported" and cannot be changed; the game renders at native size.
Both switch live (no Apply needed; Apply is fine too) and are saved into
Bin\Win64\KCDDLSS.ini (Mode = and Preset =), so they stick across starts.
If the rows are missing, KCDDLSS.log says why (a "menu:" line); both
can always be set in the ini instead. A few of the mod's lines (version,
mode and model changes, failures) also appear in the game's console (tilde
key) and in kcd.log, prefixed [KCDDLSS]; the full log is
Bin\Win64\KCDDLSS.log.

SETTINGS (Bin\Win64\KCDDLSS.ini, read at every start)
Mode = Performance (default), Balanced, Quality, UltraQuality (0.83 render
  scale, DLSS anti-aliasing with the least upscaling), UltraPerformance, Native
  (DLSS off, the game renders at your screen size) or Auto (keeps the
  game's own resolution, Quality = picks the DLSS quality value). The menu
  row above writes this line. Render sizes follow NVIDIA's values: Quality
  2560x1440, Balanced 2227x1253, Performance 1920x1080, Ultra Performance
  1280x720 at 4K; Ultra Quality is the mod's own 0.83 (3187x1793 at 4K).
Preset = Transformer (default, the DLSS 310 "Transformer" model), CNN (the
  older DLSS 3 look) or Default.
Sharpness = 0 .. 1 (DLSS sharpening, default 0).
MipBiasOffset = -1.0: texture detail. NVIDIA and AMD recommend -1.0, Intel 0.
BlackMs = 0: set to e.g. 2000 to black out the 3D view for that many ms on
  a mode switch (hides the resize and the eye-adaptation flash).
Prereq = 1 / GIFullRes = 1: the start-up settings above (0 = you manage them).
NgxDll = : a file in Bin\Win64 to use as the upscaler instead of NVIDIA's
  driver, e.g. OptiScaler.dll. See OPTISCALER.txt.
Comments in the file explain the rest (object motion vectors, Exec; Probe
is for the debug build).

CHECK THAT IT WORKS
After the game reaches the menu, Bin\Win64\KCDDLSS.log contains
  dlss: CreateFeature 1920x1080 -> 3840x2160 Performance ... result 0x1
(your sizes) and "frame 1:" lines. The menu scene is already upscaled: at
Performance the picture is sharper than the game's 1080p and the edges are
clean without the TAA blur. In the game, grass and foliage should not
smear; if a moving object leaves a trail, tell the author which one.

REPORTING A PROBLEM
The shipped mod logs little. For a report: close the game, replace
Bin\Win64\KCDDLSS.asi with debug\KCDDLSS.asi from this archive (same name,
same place), start the game, reproduce the problem, close the game and
send Bin\Win64\KCDDLSS.log (plus KCDDLSS.prev.log after a crash: the
previous run's log is kept under that name). Then put the normal file back
(reinstall). The debug build also writes NVIDIA's DLSS messages and the
blue-frame probe (Probe = 1 in the ini) into the log.

KNOWN BEHAVIOUR
- The first start after installing switches the game to native-resolution
  output. In exclusive fullscreen the picture can go black for a moment;
  the mod minimizes and restores the window itself to recover. Tabbing out
  and back in does the same.
- The game's AA option is forced off while the mod runs. Set it in the ini
  (Prereq = 0) if you really want to manage it yourself.
- The settings menu's Apply puts the game's own values back for a moment;
  the mod re-forces its render size and settings in the same frame. The
  resolution list and the quality presets work as before.
- The mod talks to one game: KingdomCome.exe, Steam 1.9.8 or Epic 1.9.8b,
  64-bit DX11.
  Other versions get a one-line note in the log; only the grass fix and
  the sharpening removal are applied there.

FSR / XeSS THROUGH OPTISCALER (AMD, INTEL)
@STD@Not included in this archive. OPTISCALER.txt has the download link and the
@STD@steps for Windows (copy OptiScaler's files, one ini line) and for Linux
@STD@(a Proton build with OptiScaler, nothing to copy).
@OPTI@Included and set up by the installers. In the game press Insert for
@OPTI@OptiScaler's overlay (upscaler, FSR 4, sharpening). OPTISCALER.txt has
@OPTI@the details, the licences and where the source code is.

UNINSTALL
- With the script: in the game folder, double-click KCDDLSS-Uninstall.bat
  (Windows), or run
    bash KCDDLSS-Uninstall.sh
  (Steam Deck / Linux, with Steam closed). Uninstall-Windows.bat /
  Uninstall-Linux-SteamDeck.sh in the extracted archive do the same. They
  remove KCDDLSS.asi, KCDDLSS.ini, KCDDLSS.log and the licence texts,
  remove nvngx_dlss.dll and version.dll only if they are the shipped files,
  remove an old Mods\KCDDLSS of BETA 1/2 (and its mod_order.txt line), and rename
  KCDGrassFix.asi.off back to KCDGrassFix.asi. On Linux they
  also remove the launch option and the prefix setting (backups are kept).
@OPTI@  The OptiScaler files go too (if OptiScaler.dll is the shipped one).
- By hand: delete those files from Bin\Win64 (keep version.dll if you use
  other .asi mods). On Linux also remove WINEDLLOVERRIDES="version=n,b"
  from the launch options.
Nothing else was changed: the game's own settings were only set in memory.

GOOD TO KNOW
- Works next to other mods. Everything is done in memory: the menu rows
  are added to the Advanced Graphics page (Libs/UI/UIActions/
  MM_AdvancedGraphics.xml) as the game loads it, whichever mod's copy of
  that page is active, so no pak and no mod_order.txt entry. MenuRows = 0
  in KCDDLSS.ini turns that off. If you already use another ASI loader as
  version.dll, the installer leaves it alone and the mod loads through it.
- Tested on Windows 11 (RTX 4070, DLSS and OptiScaler FSR/XeSS) and on
  Linux with Proton (Bazzite, RX 9070 XT with OptiScaler FSR 4).
- The log is plain text and contains no personal data beyond the game path.

CHANGES
@VERSION@
- No more Mods\KCDDLSS pak: the two menu rows are added to the Advanced
  Graphics page in memory while the game loads it (works with other mods'
  copies of that page; MenuRows = 0 in the ini turns it off). The
  installer removes the BETA 1/2 pak; by hand, delete Mods\KCDDLSS.
- The "Unsupported" choice is only shown when no upscaler exists.
- A failing first frame is now repaired with the game's own alt-tab
  routine (no window minimize/restore); the old kick stays as a second step.
- Debug: a "kick" line in KCDDLSS.cmd runs that repair on demand.
BETA 2
- Fix: black screen at start (the game's stretch drew nothing) until an
  alt-tab, seen when the profile's anti-aliasing was SMAA 1TX/2TX: the AA
  mode is now forced to 0 before the first frame and kept there (the
  profile load and the settings menu can no longer switch it live).
- Fix: crash at the first upscaled frame with ReShade (dxgi.dll) installed.
- The render size is derived from the desktop resolution from the first
  frame (the game used to save the plugin's render size as the profile
  resolution when the upscale never ran).
- A crash is logged with the module and stack in KCDDLSS.log.
BETA 1
- First release.
