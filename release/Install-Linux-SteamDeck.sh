#!/usr/bin/env bash
# DLSS for Kingdom Come: Deliverance @VERSION@ - installer for Steam Deck / Linux (Proton).
#   bash Install-Linux-SteamDeck.sh [game folder]
# Run it from the extracted archive, wherever that is (for example Downloads).
# It finds the game itself: in every Steam library (native and Flatpak Steam, SD cards and other
# drives Steam knows about), else in ~/Games (Heroic, Lutris). A game folder given as the first
# argument wins over all of that.
# What it does (nothing else):
#   1. Checks the game is closed.
@STD@#   2. Copies install/KCDDLSS.asi (the mod), install/KCDDLSS.ini (settings, kept if present),
@STD@#      install/nvngx_dlss.dll (NVIDIA DLSS, kept if present) and install/version.dll (Ultimate ASI
@STD@#      Loader) to Bin/Win64, next to KingdomCome.exe. A version.dll already there is left alone.
@OPTI@#   2. Copies install/KCDDLSS.asi (the mod), install/KCDDLSS.ini (settings, kept if present),
@OPTI@#      install/version.dll (Ultimate ASI Loader) and OptiScaler (install/OptiScaler: OptiScaler.dll
@OPTI@#      and its FSR / XeSS files; OptiScaler.ini kept if present) to Bin/Win64, next to
@OPTI@#      KingdomCome.exe, and sets NgxDll = OptiScaler.dll in KCDDLSS.ini. A different version.dll or
@OPTI@#      OptiScaler.dll already there is left alone.
#      A standalone KCDGrassFix.asi (30 FPS Grass Fix) is renamed to .off: this mod contains it.
#   3. Steam/Proton: adds the line "version"="native,builtin" to the game's Proton prefix
#      (DLL overrides in user.reg, backup kept), so Proton loads version.dll from the game folder.
#   4. Steam: puts WINEDLLOVERRIDES="version=n,b" in front of the game's launch options (the same
#      setting, kept by Steam even if the prefix is reset). Steam has to be closed for this: it
#      rewrites its settings file when it exits. If Steam is running, the installer asks you to close
#      it, or tells you what to type into the launch options yourself. Backup kept.
#   5. Copies the uninstaller to the game folder as KCDDLSS-Uninstall.sh.
# No game file is changed. The mod sets the game up itself at every start (no user.cfg edits).
@STD@# NVIDIA: DLSS runs through Proton's NVAPI bridge (on by default in Proton 9+). AMD / Intel: see
@STD@# OPTISCALER.txt (FSR / XeSS through OptiScaler, not included here).
@OPTI@# OptiScaler is loaded from the game folder: no OptiScaler Proton build needed. See OPTISCALER.txt.
set -u

# Started by double-click (no terminal): reopen in a terminal so the messages stay readable.
if [ ! -t 1 ] && [ -z "${KDL_TERM:-}" ] && { [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; }; then
  command -v konsole >/dev/null && KDL_TERM=1 exec konsole --hold -e bash "$0" "$@"
  command -v xterm >/dev/null && KDL_TERM=1 exec xterm -hold -e bash "$0" "$@"
fi

ASI_SHA=@ASI_SHA@   # install/KCDDLSS.asi
UAL_SHA=@UAL_SHA@   # install/version.dll
@STD@DLSS_SHA=@DLSS_SHA@   # install/nvngx_dlss.dll
@OPTI@OPTI_SHA=@OPTI_SHA@   # install/OptiScaler/OptiScaler.dll
LINE='"version"="native,builtin"'
die() { echo; echo "ERROR: $*"; echo "Nothing more was changed."; exit 1; }
sha() { sha256sum "$1" | cut -d' ' -f1; }
is_game() { [ -f "$1/Bin/Win64/KingdomCome.exe" ]; }
steam_libs() {  # every Steam library folder, one per line
  local r
  for r in "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.steam/root" \
           "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam" "$HOME/.var/app/com.valvesoftware.Steam/data/Steam"; do
    [ -d "$r/steamapps" ] || continue
    echo "$r"; sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$r/steamapps/libraryfolders.vdf" 2>/dev/null
  done
}
find_game() {  # candidate game folders, one per line
  local d lib
  for d in "$SELF/../.." "$SELF"; do is_game "$d" && { (cd "$d" && pwd -P); return; }; done
  steam_libs | while IFS= read -r lib; do d=$lib/steamapps/common/KingdomComeDeliverance; is_game "$d" && (cd "$d" && pwd -P); done
  find "$HOME/Games" -maxdepth 6 -type f -path '*/Bin/Win64/KingdomCome.exe' 2>/dev/null | sed 's|/Bin/Win64/KingdomCome.exe$||'
}
steam_roots() {  # Steam installs (not libraries), each once
  local r
  for r in "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.steam/root" \
           "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam" "$HOME/.var/app/com.valvesoftware.Steam/data/Steam"; do
    [ -d "$r/userdata" ] && (cd "$r" && pwd -P)
  done | awk '!seen[$0]++'
}
user_configs() {  # every Steam user's localconfig.vdf
  local r f
  steam_roots | while IFS= read -r r; do
    for f in "$r"/userdata/*/config/localconfig.vdf; do
      [ -f "$f" ] && [ "$(basename "$(dirname "$(dirname "$f")")")" != 0 ] && echo "$(stat -c %Y "$f") $f"
    done
  done | sort -rn | cut -d' ' -f2-   # newest first: the account in use
}
steam_running() { pgrep -x steam >/dev/null; }
lo_py() {  # Steam launch option editor (source: tools/release/launch_option.py)
  python3 -I - "$@" <<'GAF_PY'
@LAUNCH_OPTION_PY@
GAF_PY
}
wait_steam_closed() {  # 0 = Steam is closed now, 1 = the user skipped or nobody can answer
  local a i
  while steam_running; do
    echo "Steam is running. Steam saves its settings when it closes, which would undo this change."
    [ -t 0 ] || return 1
    echo "Close Steam now: Steam menu > Exit (Desktop Mode: right-click the Steam icon in the taskbar > Exit Steam)."
    read -r -p "Press Enter when Steam is closed, or type S and press Enter to skip this step: " a || return 1
    case $a in s|S) return 1;; esac
    for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do steam_running || break; read -r -t 1 _ < /dev/null; done
  done
  return 0
}
prefix_busy() {  # is a wineserver of this prefix still alive? (it rewrites user.reg when it exits)
  local p
  for p in $(pgrep -x wineserver); do
    tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -qxF "WINEPREFIX=$1" && return 0
  done
  return 1
}

SELF=$(cd "$(dirname "$0")" && pwd -P)
echo "DLSS for Kingdom Come: Deliverance @VERSION@ - install"
if [ $# -ge 1 ]; then
  is_game "$1" || die "No Kingdom Come: Deliverance in $1 (it needs Bin/Win64/KingdomCome.exe)."
  GAME=$(cd "$1" && pwd -P)
else
  mapfile -t found < <(find_game | awk '!seen[$0]++')
  if [ ${#found[@]} -eq 0 ]; then
    die "Kingdom Come: Deliverance not found. Run it with the game folder:
  bash Install-Linux-SteamDeck.sh \"/path/to/KingdomComeDeliverance\""
  elif [ ${#found[@]} -eq 1 ]; then
    GAME=${found[0]}
  else
    echo "Found more than one copy of the game:"; for i in "${!found[@]}"; do echo "  $((i + 1))) ${found[$i]}"; done
    [ -t 0 ] || die "Run it again with the game folder as the first argument."
    read -r -p "Install into which one? Number: " n
    [[ "$n" =~ ^[0-9]+$ ]] && [ "$n" -ge 1 ] && [ "$n" -le ${#found[@]} ] || die "No such number."
    GAME=${found[$((n - 1))]}
  fi
fi
echo "Game folder: $GAME"
B=$GAME/Bin/Win64
pgrep -f '[K]ingdomCome\.exe' >/dev/null && die "The game is running. Close it first."
@STD@for f in KCDDLSS.asi KCDDLSS.ini nvngx_dlss.dll version.dll; do [ -f "$SELF/install/$f" ] || die "install/$f is missing from $SELF."; done
@OPTI@for f in KCDDLSS.asi KCDDLSS.ini version.dll OptiScaler/OptiScaler.dll OptiScaler/OptiScaler.ini; do [ -f "$SELF/install/$f" ] || die "install/$f is missing from $SELF."; done
[ "$(sha "$SELF/install/KCDDLSS.asi")" = "$ASI_SHA" ] || die "install/KCDDLSS.asi is damaged. Download the mod again."
[ "$(sha "$SELF/install/version.dll")" = "$UAL_SHA" ] || die "install/version.dll is damaged. Download the mod again."
@STD@[ "$(sha "$SELF/install/nvngx_dlss.dll")" = "$DLSS_SHA" ] || die "install/nvngx_dlss.dll is damaged. Download the mod again."
@OPTI@[ "$(sha "$SELF/install/OptiScaler/OptiScaler.dll")" = "$OPTI_SHA" ] || die "install/OptiScaler/OptiScaler.dll is damaged. Download the mod again."

# 1. The files next to KingdomCome.exe
cp -f "$SELF/install/KCDDLSS.asi" "$B/KCDDLSS.asi" || { rm -f "$B/KCDDLSS.asi"; die "Could not copy KCDDLSS.asi to $B."; }
rm -f "$B/KCDDLSS.log"
echo "Mod: Bin/Win64/KCDDLSS.asi copied."
if [ -e "$B/KCDDLSS.ini" ]; then echo "Settings: Bin/Win64/KCDDLSS.ini is already there. Kept (delete it to get the defaults)."
else cp -f "$SELF/install/KCDDLSS.ini" "$B/KCDDLSS.ini" || die "Could not copy KCDDLSS.ini to $B."; echo "Settings: Bin/Win64/KCDDLSS.ini copied."; fi
@STD@if [ -e "$B/nvngx_dlss.dll" ]; then echo "DLSS: Bin/Win64/nvngx_dlss.dll is already there. Kept."
@STD@else cp -f "$SELF/install/nvngx_dlss.dll" "$B/nvngx_dlss.dll" || { rm -f "$B/nvngx_dlss.dll"; die "Could not copy nvngx_dlss.dll to $B (disk full?). The half-copied file was removed."; }
@STD@     cp -f "$SELF/install/NVIDIA-DLSS-LICENSE.txt" "$B/NVIDIA-DLSS-LICENSE.txt" 2>/dev/null; echo "DLSS: Bin/Win64/nvngx_dlss.dll copied (NVIDIA DLSS @DLSS_VERSION@)."; fi
@OPTI@# OptiScaler: a different OptiScaler.dll already there is the user's own setup and stays.
@OPTI@if [ -e "$B/OptiScaler.dll" ] && [ "$(sha "$B/OptiScaler.dll")" != "$OPTI_SHA" ]; then
@OPTI@  echo "OptiScaler: Bin/Win64/OptiScaler.dll is already there and is a different file. Left alone, the mod uses it."
@OPTI@else
@OPTI@  for f in OptiScaler.dll libxess.dll libxess_dx11.dll libxell.dll amd_fidelityfx_dx12.dll amd_fidelityfx_upscaler_dx12.dll amd_fidelityfx_vk.dll; do
@OPTI@    cp -f "$SELF/install/OptiScaler/$f" "$B/$f" || { rm -f "$B/$f"; die "Could not copy $f to $B (disk full?). The half-copied file was removed."; }
@OPTI@  done
@OPTI@  mkdir -p "$B/D3D12_Optiscaler" && cp -f "$SELF/install/OptiScaler/D3D12_Optiscaler/D3D12Core.dll" "$B/D3D12_Optiscaler/D3D12Core.dll" ||
@OPTI@    { rm -f "$B/D3D12_Optiscaler/D3D12Core.dll"; die "Could not copy D3D12_Optiscaler/D3D12Core.dll to $B (disk full?). The half-copied file was removed."; }
@OPTI@  [ -e "$B/OptiScaler.ini" ] || cp -f "$SELF/install/OptiScaler/OptiScaler.ini" "$B/OptiScaler.ini" || die "Could not copy OptiScaler.ini to $B."
@OPTI@  # OptiScaler.ini (also a kept one): GPU spoofing off; with it on OptiScaler 0.9.4 can hang at start when another game in the library has DLSS
@OPTI@  sed -i -E 's/^[ \t]*(Dxgi|StreamlineSpoofing)[ \t]*=[^\r]*/\1=false/' "$B/OptiScaler.ini" && echo "Settings: OptiScaler.ini set to Dxgi=false and StreamlineSpoofing=false (GPU spoofing off)."
@OPTI@  mkdir -p "$B/OptiScaler-Licenses" && cp -f "$SELF/install/OptiScaler/Licenses/"* "$B/OptiScaler-Licenses/" 2>/dev/null
@OPTI@  echo "OptiScaler: Bin/Win64/OptiScaler.dll and its FSR / XeSS files copied (OptiScaler @OPTI_VERSION@)."
@OPTI@fi
@OPTI@# KCDDLSS.ini: NgxDll = OptiScaler.dll (the line is replaced, or added if an old ini has none; CRLF kept)
@OPTI@if awk '/^[ \t]*NgxDll[ \t]*=/ { cr = sub(/\r$/, ""); print "NgxDll = OptiScaler.dll" (cr ? "\r" : ""); n = 1; next } { print } END { if (!n) printf "NgxDll = OptiScaler.dll\r\n" }' "$B/KCDDLSS.ini" > "$B/KCDDLSS.ini.kcddlss-tmp" && mv -f "$B/KCDDLSS.ini.kcddlss-tmp" "$B/KCDDLSS.ini"; then
@OPTI@  echo "Settings: KCDDLSS.ini set to NgxDll = OptiScaler.dll (the mod uses OptiScaler)."
@OPTI@else rm -f "$B/KCDDLSS.ini.kcddlss-tmp"; echo "Settings: could not edit KCDDLSS.ini. Set the line NgxDll = OptiScaler.dll in Bin/Win64/KCDDLSS.ini yourself."; fi
if [ -e "$B/KCDGrassFix.asi" ]; then mv -f "$B/KCDGrassFix.asi" "$B/KCDGrassFix.asi.off" && echo "Grass: standalone 30 FPS Grass Fix set aside as KCDGrassFix.asi.off (this mod contains it)."; fi
# Options-menu rows: since 0.8.0 the plugin adds them to the page in memory. The Mods/KCDDLSS pak of BETA 1/2 must go (it would double the rows).
for MODS in "$GAME/Mods" "$GAME/mods"; do
  [ -d "$MODS/KCDDLSS" ] && rm -rf "$MODS/KCDDLSS" && echo "Menu: old Mods/KCDDLSS pak of BETA 1/2 removed (the rows now come from the plugin itself)."
  [ -f "$MODS/mod_order.txt" ] && grep -qix 'KCDDLSS' "$MODS/mod_order.txt" && { grep -vix 'KCDDLSS' "$MODS/mod_order.txt" > "$MODS/mod_order.kcddlss-tmp"; mv -f "$MODS/mod_order.kcddlss-tmp" "$MODS/mod_order.txt"; }
done
if [ -e "$B/version.dll" ] && [ "$(sha "$B/version.dll")" != "$UAL_SHA" ]; then
  echo "Loader: Bin/Win64/version.dll is already there and is a different file. Left alone."
  echo "        If it is an ASI loader, the mod loads through it. Check Bin/Win64/KCDDLSS.log after a start."
else
  cp -f "$SELF/install/version.dll" "$B/version.dll" || die "Could not copy version.dll to $B."
  cp -f "$SELF/install/Ultimate-ASI-Loader-LICENSE.txt" "$B/Ultimate-ASI-Loader-LICENSE.txt" 2>/dev/null
  echo "Loader: Bin/Win64/version.dll copied (Ultimate ASI Loader)."
fi

# 2. Proton: let it load version.dll from the game folder (Wine prefers its own copy otherwise)
PFX=0; PFXSEEN=0
while IFS= read -r r; do
  [ -n "$r" ] || continue
  PFXSEEN=1
  pfx=$(dirname "$r")
  for i in 1 2 3 4 5 6 7 8 9 10; do prefix_busy "$pfx" || break; read -r -t 1 _ < /dev/null; done
  prefix_busy "$pfx" && die "Proton is still running for this game. Wait a few seconds and run this again."
  if LC_ALL=C grep -qxF "$LINE" "$r"; then
    echo "Proton prefix: override already set."; PFX=1; continue
  fi
  if LC_ALL=C awk '/^\[Software\\\\Wine\\\\DllOverrides\]/ {f=1; next} f && /^$/ {f=0} f && /^"\*?version"=/ {found=1} END {exit !found}' "$r"; then
    echo "Proton prefix: a different setting for version.dll is already there. Left alone: $r"; continue
  fi
  LC_ALL=C grep -q '^\[Software\\\\Wine\\\\DllOverrides\]' "$r" || { echo "Proton prefix: no DLL override list in $r. Left alone."; continue; }
  cp -p "$r" "$r.kcddlss-backup" || die "Could not back up $r."
  LC_ALL=C awk -v line="$LINE" '
    /^\[Software\\\\Wine\\\\DllOverrides\]/ {f=1}
    f && /^$/ {print line; f=0}
    {print}
    END {if (f) print line}' "$r.kcddlss-backup" > "$r.kcddlss-tmp" && mv -f "$r.kcddlss-tmp" "$r" ||
    die "Could not edit $r (backup: $r.kcddlss-backup)."
  echo "Proton prefix: override added (backup: user.reg.kcddlss-backup)."; PFX=1
done < <(steam_libs | while IFS= read -r lib; do
           r=$lib/steamapps/compatdata/379430/pfx/user.reg; [ -f "$r" ] && echo "$(cd "$(dirname "$r")" && pwd -P)/user.reg"
         done | awk '!seen[$0]++')
[ $PFXSEEN = 1 ] || echo "Proton prefix: none found for this game yet (it appears after the first start)."

# 3. Steam launch option (Steam must be closed)
LO=0; LOSKIP=
mapfile -t cfgs < <(user_configs)
if [ ${#cfgs[@]} -eq 0 ]; then
  echo "Steam launch option: no Steam user settings found."; LOSKIP=nosteam
elif ! command -v python3 >/dev/null; then
  echo "Steam launch option: python3 is missing, so it can't be added automatically."; LOSKIP=nopython
elif lo_py check "${cfgs[@]}"; then
  echo "Steam launch option: already set."; LO=1
elif ! wait_steam_closed; then
  LOSKIP=running
else
  echo "Steam launch option:"
  lo_py add "${cfgs[@]}" && LO=1
fi
if [ $LO = 0 ]; then
  SUG=
  [ ${#cfgs[@]} -gt 0 ] && command -v python3 >/dev/null && SUG=$(lo_py suggest "${cfgs[0]}" 2>/dev/null)
  [ -n "$SUG" ] || SUG='WINEDLLOVERRIDES="version=n,b" %command%'
  echo
  if [ $PFX = 1 ]; then
    echo "Steam launch option: not added. The mod still loads through the Proton prefix setting."
    echo "To add the launch option as well (it survives a reset of the prefix), close Steam and run this"
    echo "again, or add it yourself:"
  else
    echo "IMPORTANT: the mod will not load yet. Close Steam and run this again, or add the launch option yourself:"
  fi
  echo "  Steam: right-click Kingdom Come: Deliverance > Properties > General > Launch Options, and set it to:"
  echo "    $SUG"
  echo "  Heroic/Lutris: add the environment variable WINEDLLOVERRIDES with the value version=n,b"
fi

cp -f "$SELF/Uninstall-Linux-SteamDeck.sh" "$GAME/KCDDLSS-Uninstall.sh" &&
  echo "Uninstaller copied to the game folder: KCDDLSS-Uninstall.sh"
echo
echo "Done. Start the game. Bin/Win64/KCDDLSS.log then shows \"dlss: CreateFeature ... result 0x1\" once the menu is up."
@STD@echo "If that file does not appear, the loader did not start: see README.txt. \"no usable NGX core\" in the log on an"
@STD@echo "NVIDIA card: add PROTON_ENABLE_NVAPI=1 to the launch options. AMD / Intel: see OPTISCALER.txt."
@OPTI@echo "If that file does not appear, the loader did not start: see README.txt."
@OPTI@echo "Upscaler: OptiScaler. In the game press Insert for its overlay (FSR or XeSS). More: OPTISCALER.txt."
echo "Settings (DLSS mode, preset, sharpness): Bin/Win64/KCDDLSS.ini, read at every start."
