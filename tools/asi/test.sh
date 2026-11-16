#!/bin/bash
# test.sh: every offline check of the plugin in one command (no game, no GPU, about 30 s). Run before every install.
#   ./test.sh          all steps          ./test.sh -v    also print each step's full output
# Steps: 1 build (both plugins, no new warnings)  2 address table vs the stock Steam DLL (python)  3 resolver test (wine)
#        4 unit test: parsers, ini, masked compare, every shader through D3DCompile (wine)
#        5 host test: the SHIPPED release and debug plugins install themselves into the real WHGame.dll image (wine), log checked
#        6 hot-reload swap test, when tools/asi-hot is there
# Cannot test: the picture, the GPU driver / DLSS itself, other store builds of the game (no DLL here). See test/README.md.
set -u
cd "$(dirname "$0")"
V=0; [ "${1:-}" = "-v" ] && V=1
DLL=${WHGAME_DLL:?set WHGAME_DLL to the path of your own stock Steam 1.9.8 WHGame.dll}
T=test/tmp; mkdir -p $T; fails=0; export WINEDEBUG=-all
step() {  # step NAME CMD...: run, print one line, keep the output in test/tmp/NAME.log
  local name=$1; shift; local t0=$(date +%s.%N)
  if "$@" > $T/$name.log 2>&1; then printf 'ok    %-9s %5.1fs  %s\n' "$name" "$(echo "$(date +%s.%N) - $t0" | bc)" "$(grep -E '^ok|passed|resolved|OK' $T/$name.log | tail -1 | cut -c1-90)"
  else fails=$((fails+1)); printf 'FAIL  %-9s (output: %s)\n' "$name" "$T/$name.log"; grep -E 'FAIL|rror|warning' $T/$name.log | head -8 | cut -c1-200; fi
  [ $V = 1 ] && cat $T/$name.log
  return 0
}
s_build() {
  ./build.sh > $T/build.out 2>&1 || { tail -5 $T/build.out; return 1; }
  # the two known warnings (unused parameters of ngx_log_cb) are allowed, anything else fails
  local w; w=$(grep -c 'warning:' $T/build.out); local k; k=$(grep 'warning:' $T/build.out | grep -c "unused parameter ‘\(message\|level\)’")
  [ "$w" = "$k" ] || { grep 'warning:' $T/build.out | grep -v "unused parameter ‘\(message\|level\)’"; echo "FAIL new warnings"; return 1; }
  echo "ok both plugins built, $w known warnings, release $(sha256sum out/KCDDLSS.asi | cut -c1-12)"
}
s_table() { python3 addr_check.py addr_sig.h "$DLL" --steam; }
s_resolver() { x86_64-w64-mingw32-gcc -O2 -o $T/addr_test.exe addr_test.c && wine $T/addr_test.exe "$(winepath -w "$DLL")"; }
s_unit() {
  x86_64-w64-mingw32-gcc -DKCDDLSS_DEBUG -Os -ffreestanding -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables \
    -fno-tree-loop-distribute-patterns -mno-stack-arg-probe -nostdlib -nostartfiles -Wl,-e,unit_entry -I. -o $T/unit_test.exe test/unit_test.c \
    -lkernel32 -ladvapi32 -luser32 || return 1
  mkdir -p $T/unit; ( cd $T/unit && timeout 120 wine ../unit_test.exe )
}
s_host() {  # s_host release|debug
  local d=$T/host-$1 asi=out/KCDDLSS.asi; [ "$1" = debug ] && asi=out/KCDDLSS-debug.asi
  x86_64-w64-mingw32-gcc -O2 -o $T/host_test.exe test/host_test.c || return 1
  mkdir -p $d; cp $asi $d/KCDDLSS.asi; printf 'Mode = Performance\r\nGIFullRes = 2\r\nInventoryFix = 0\r\nSharpness = 0.5\r\n' > $d/KCDDLSS.ini
  ( cd $d && timeout 120 wine ../host_test.exe . "$(winepath -w "$DLL")" ../../checks.txt )
}
[ -f "$DLL" ] || { echo "FAIL  stock WHGame.dll not found at $DLL"; exit 1; }
step build s_build
step table s_table
step resolver s_resolver
step unit s_unit
step host-rel s_host release
step host-dbg s_host debug
if [ -x ../asi-hot/hot/test/run_test.sh ]; then step hot ../asi-hot/hot/test/run_test.sh; fi
if [ $fails = 0 ]; then echo "ALL OK"; else echo "$fails STEP(S) FAILED"; fi
exit $fails
