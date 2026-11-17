# Native DLSS Support - True Upscaling (BETA)

A plugin for **Kingdom Come: Deliverance** (the first game) that replaces the game's plain "native resolution" stretch with NVIDIA DLSS Super Resolution. AMD and Intel cards get FSR 3.1, FSR 4 or XeSS through OptiScaler.

This is a **beta**. Problems: open an issue or reply on Nexus with the text from `Report-Windows.bat` (Linux: `Bin\Win64\KCDDLSS.log`).

How it works: [HOW-IT-WORKS.md](HOW-IT-WORKS.md). Changes: [CHANGELOG.md](CHANGELOG.md).

## What you get

- Two rows in Options > Advanced Graphics: **DLSS / Upscaling** (Off, Ultra Quality, Quality, Balanced, Performance, Ultra Perf) and **DLSS Model** (Transformer, CNN). Both switch live.
- Less blocky global illumination at DLSS render sizes, a smooth character outline in the inventory, the [30 FPS Grass Fix](https://github.com/qahtni/kcd1-30fps-grass-fix) built in.
- No game file is changed. Everything is set in memory at start.

## Download

From the [Releases page](https://github.com/qahtni/kcd1-dlss/releases/latest) or Nexus Mods:

- `KCDDLSS-BETA-<n>.7z`: NVIDIA GeForce RTX 20 series or newer. DLSS included.
- `KCDDLSS-BETA-<n>-OptiScaler.7z`: AMD and Intel cards. OptiScaler included and set up.

Needs game version 1.9.8 (Steam; GOG and Epic are experimental) and Windows, or Linux / Steam Deck with Proton 9 or newer.

## Install

- **Windows:** extract, double-click `Install-Windows.bat`.
- **Steam Deck / Linux:** Desktop Mode, exit Steam, run `bash Install-Linux-SteamDeck.sh` in the extracted folder.
- **By hand:** copy the files in `install\` into `Bin\Win64`. On Linux add the launch option `WINEDLLOVERRIDES="version=n,b" %command%`.

It works when `Bin\Win64\KCDDLSS.log` has a line like `dlss: CreateFeature 1920x1080 -> 3840x2160 Performance ... result 0x1`.

Uninstall: `KCDDLSS-Uninstall.bat` or `bash KCDDLSS-Uninstall.sh` in the game folder.

## Settings

`Bin\Win64\KCDDLSS.ini`: `Mode`, `Preset`, `Sharpness`, `MipBiasOffset`, `GIFullRes`, `InventoryFix`. The rest is in the archive's `README.txt` under ADVANCED SETTINGS.

## Building from source

Needs mingw-w64, python3 and 7z.

```sh
tools/asi/build.sh                                     # -> tools/asi/out/KCDDLSS.asi and KCDDLSS-debug.asi
WHGAME_DLL=/path/to/WHGame.dll tools/asi/test.sh       # offline test, needs wine and your own stock Steam 1.9.8 WHGame.dll
release/build_release.sh BETA-<n> dinput8.dll UAL-LICENSE nvngx_dlss.dll DLSS-LICENSE.txt   # -> dist/KCDDLSS-BETA-<n>.7z
```

The third-party files are not stored here: see [THIRD_PARTY.md](THIRD_PARTY.md) and the header of `release/build_release.sh`.

## Credits and licence

By mrtaxerman. NVIDIA DLSS (NVIDIA RTX SDKs licence), [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) by ThirteenAG (MIT), [OptiScaler](https://github.com/optiscaler/OptiScaler) (GPL-3.0, only in the OptiScaler download). The code here is public domain under [The Unlicense](LICENSE); game files are not included.
