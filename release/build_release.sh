#!/usr/bin/env bash
# Builds dist/KCDDLSS-<version>.7z (the archive for Nexus / GitHub Releases).
# usage: release/build_release.sh <version> UAL_DINPUT8_DLL UAL_LICENSE NVNGX_DLSS_DLL DLSS_LICENSE
#   UAL_DINPUT8_DLL = dinput8.dll from Ultimate ASI Loader v10.0.2 (Ultimate-ASI-Loader-NoPDB_x64.zip), shipped as version.dll
#   UAL_LICENSE     = that project's LICENSE (MIT)
#   NVNGX_DLSS_DLL  = nvngx_dlss.dll (DLSS 310.x; version read from the file by the script)
#   DLSS_LICENSE    = NVIDIA RTX SDKs licence text (the DLSS SDK's LICENSE.txt)
# Needs tools/asi/out/KCDDLSS.asi (run tools/asi/build.sh first; or ASI=<file> env to use another binary), tools/windows/KCDDLSS.ini and 7z.
# Fills @VERSION@ @ASI_SHA@ @UAL_SHA@ @DLSS_SHA@ @ASI_SIZE@ @UAL_SIZE@ @DLSS_SIZE@ @DLSS_VERSION@, inlines launch_option.py.
# .bat/.ini/README get CRLF; .sh stay LF and executable.
# OptiScaler download: OPTISCALER=<folder of the extracted Optiscaler_0.9.4-final.20260718._MM.7z> (env) builds
#   dist/KCDDLSS-<version>-OptiScaler.7z instead (top folder KCDDLSS-OptiScaler): no nvngx_dlss.dll; install/OptiScaler/ =
#   OptiScaler.dll, OptiScaler.ini (Dx11Upscaler=fsr31_12, the only change), the FSR / XeSS DLLs, D3D12_Optiscaler/D3D12Core.dll, Licenses (the release's
#   AMD / Intel texts + OptiScaler's GPL-3.0 text, OPTISCALER_LICENSE env, default downloads/optiscaler-license/LICENSE).
#   Template lines starting @OPTI@ are used instead of lines starting @STD@; OPTISCALER-included.txt ships as OPTISCALER.txt.
set -euo pipefail
[ $# -eq 5 ] || { echo "usage: $0 <version> UAL_DINPUT8_DLL UAL_LICENSE NVNGX_DLSS_DLL DLSS_LICENSE"; exit 2; }
VER=$1; UAL=$(realpath "$2"); LIC=$(realpath "$3"); DLSS=$(realpath "$4"); DLIC=$(realpath "$5")
cd "$(dirname "$0")/.."
R=release; OPTI=${OPTISCALER:-}
OPTI_FILES="OptiScaler.dll libxess.dll libxess_dx11.dll libxell.dll amd_fidelityfx_dx12.dll amd_fidelityfx_upscaler_dx12.dll amd_fidelityfx_vk.dll"
if [ -n "$OPTI" ]; then
  OPTI=$(realpath "$OPTI"); OPTI_LIC=$(realpath "${OPTISCALER_LICENSE:-downloads/optiscaler-license/LICENSE}")
  OPTI_VERSION=0.9.4; OPTI_SHA=fbfb6676b829dad7e020fb830586a16aa0ec6add78016db48ef12e2ae1803231
  [ "$(sha256sum "$OPTI/OptiScaler.dll" | cut -d' ' -f1)" = "$OPTI_SHA" ] || { echo "$OPTI/OptiScaler.dll is not OptiScaler $OPTI_VERSION"; exit 1; }
  head -2 "$OPTI_LIC" | grep -q 'GNU GENERAL PUBLIC LICENSE' || { echo "$OPTI_LIC is not the GPL text"; exit 1; }
  OPTI_SIZE=$(stat -c %s "$OPTI/OptiScaler.dll"); KEEP=@OPTI@; DROP=@STD@
  OUT=dist/KCDDLSS-$VER-OptiScaler.7z; STAGE=dist/single-optiscaler; TOP=KCDDLSS-OptiScaler
else
  OPTI_VERSION=; OPTI_SHA=; OPTI_SIZE=; KEEP=@STD@; DROP=@OPTI@
  OUT=dist/KCDDLSS-$VER.7z; STAGE=dist/single; TOP=KCDDLSS
fi
S=$STAGE/$TOP
ASI=${ASI:-tools/asi/out/KCDDLSS.asi}; DASI=${DEBUG_ASI:-tools/asi/out/KCDDLSS-debug.asi}; INI=tools/windows/KCDDLSS.ini   # ASI=path overrides the plugin binary (release builds vs. a dev build in out/)
[ -f "$ASI" ] || { echo "$ASI missing: run tools/asi/build.sh first"; exit 1; }
[ -f "$DASI" ] || { echo "$DASI missing: run tools/asi/build.sh first (debug build)"; exit 1; }
[ "$(sha256sum "$UAL" | cut -d' ' -f1)" = 57ec56390c92a18b82f58855e63c93985e254d1e29d5f75cbc28dc08f96c8a12 ] ||
  { echo "$UAL is not dinput8.dll of Ultimate ASI Loader v10.0.2 (x64)"; exit 1; }
DLSS_VERSION=$(python3 -I - "$DLSS" <<'PY'
import re,sys
b=open(sys.argv[1],'rb').read(); m=re.search(rb'F\x00i\x00l\x00e\x00V\x00e\x00r\x00s\x00i\x00o\x00n\x00(?:\x00\x00)+((?:[0-9.,]\x00)+)',b)
v=m.group(1).decode('utf-16-le').replace(',','.').split('.') if m else []
print('.'.join(v[:3]) if len(v)>=3 else 'unknown')
PY
)
ASI_SHA=$(sha256sum "$ASI" | cut -d' ' -f1); UAL_SHA=$(sha256sum "$UAL" | cut -d' ' -f1); DLSS_SHA=$(sha256sum "$DLSS" | cut -d' ' -f1)
rm -rf "$STAGE"; mkdir -p "$S/install"
mkdir -p "$S/debug"; cp -p "$DASI" "$S/debug/KCDDLSS.asi"; cp -p "$ASI" "$S/install/KCDDLSS.asi"; cp -p "$UAL" "$S/install/version.dll"
[ -n "$OPTI" ] || cp -p "$DLSS" "$S/install/nvngx_dlss.dll"
sed 's/\r$//; s/$/\r/' "$INI" > "$S/install/KCDDLSS.ini"
sed 's/\r$//; s/$/\r/' "$LIC" > "$S/install/Ultimate-ASI-Loader-LICENSE.txt"
if [ -z "$OPTI" ]; then sed 's/\r$//; s/$/\r/' "$DLIC" > "$S/install/NVIDIA-DLSS-LICENSE.txt"
else
  O=$S/install/OptiScaler; mkdir -p "$O/Licenses"
  for f in $OPTI_FILES; do cp -p "$OPTI/$f" "$O/$f"; done
  mkdir -p "$O/D3D12_Optiscaler"; cp -p "$OPTI/D3D12_Optiscaler/D3D12Core.dll" "$O/D3D12_Optiscaler/D3D12Core.dll"   # user 11-09: ship it (Agility SDK, FSR 4 on Windows 10)
  python3 -I - "$OPTI/OptiScaler.ini" "$O/OptiScaler.ini" <<'PY'
import sys
s = open(sys.argv[1], 'rb').read()
old = b'\nDx11Upscaler=auto'; assert s.count(old) == 1, 'Dx11Upscaler=auto not found once'
open(sys.argv[2], 'wb').write(s.replace(old, b'\nDx11Upscaler=fsr31_12', 1))   # FSR 3.1 via DX12: upgrades to FSR 4 on RDNA 3/4
PY
  for f in FidelityFX_v1_LICENSE.md FidelityFX_v2_LICENSE.md XeSS_LICENSE.txt DirectX_LICENSE.txt; do cp -p "$OPTI/Licenses/$f" "$O/Licenses/$f"; done
  sed 's/\r$//; s/$/\r/' "$OPTI_LIC" > "$O/Licenses/OptiScaler-LICENSE.txt"
fi
# 0.8.0: no Mods/KCDDLSS pak any more; the plugin adds the menu rows in memory (tools/asi/menu_rows.h from tools/menu-mod/MM_AdvancedGraphics.xml)
fill() { sed -e "/^$DROP/d" -e "s/^$KEEP//" "$1" | sed -e "s/@OPTI_VERSION@/$OPTI_VERSION/g; s/@OPTI_SHA@/$OPTI_SHA/; s/@OPTI_SIZE@/$OPTI_SIZE/" -e "s/@VERSION@/$VER/g; s/@ASI_SHA@/$ASI_SHA/; s/@UAL_SHA@/$UAL_SHA/; s/@DLSS_SHA@/$DLSS_SHA/; s/@ASI_SIZE@/$(stat -c %s "$ASI")/; s/@UAL_SIZE@/$(stat -c %s "$UAL")/; s/@DLSS_SIZE@/$(stat -c %s "$DLSS")/; s/@DLSS_VERSION@/$DLSS_VERSION/g"; }
for f in Install-Windows.bat Uninstall-Windows.bat Report-Windows.bat README.txt OPTISCALER.txt; do src=$f; [ -n "$OPTI" ] && [ "$f" = OPTISCALER.txt ] && src=OPTISCALER-included.txt; fill "$R/$src" | sed 's/\r$//; s/$/\r/' > "$S/$f"; done
for f in Install-Linux-SteamDeck.sh Uninstall-Linux-SteamDeck.sh; do
  fill "$R/$f" | python3 -I -c 'import sys; s=sys.stdin.read(); py=open(sys.argv[1]).read().rstrip("\n"); assert s.count("@LAUNCH_OPTION_PY@\n")==1; sys.stdout.write(s.replace("@LAUNCH_OPTION_PY@\n", py+"\n"))' "$R/launch_option.py" > "$S/$f"
  chmod 755 "$S/$f"; bash -n "$S/$f"
done
! grep -l '@[A-Z_]*@' "$S"/*.sh "$S"/*.bat "$S"/*.txt
mkdir -p dist; rm -f "$OUT"; (cd "$STAGE" && 7z a -t7z -mx=9 "$OLDPWD/$OUT" "$TOP" >/dev/null)
7z t "$OUT" >/dev/null && echo "built $OUT $(stat -c %s "$OUT") bytes, sha256 $(sha256sum "$OUT" | cut -c1-12), DLSS $DLSS_VERSION"
