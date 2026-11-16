# Offline tests (one command: `tools/asi/test.sh`, about 70 s, no game, no GPU)

Needs mingw-w64, wine, python3 and your own stock Steam 1.9.8 `WHGame.dll`: `WHGAME_DLL=/path/to/WHGame.dll tools/asi/test.sh`.
The game file is only read; it is not part of this repository.

| Step | What it proves | How |
|---|---|---|
| build | both plugins build, no warning besides the 2 known ones | `build.sh` |
| table | the 76 signatures find all 28 addresses in the stock Steam `WHGame.dll` | `addr_check.py` |
| resolver | the C resolver: every kind and outcome | `addr_test.c` under wine |
| unit | parsers, ini loading, masked compare, defaults; all shaders compile | `test/unit_test.c` = the plugin source compiled into a test program, wine |
| host-rel, host-dbg | the SHIPPED binaries install into the real game DLL: lookups, every byte patch and vtable hook, 9 s of the plugin's threads with no game behind them | `test/host_test.c` maps `WHGame.dll` without running it, loads the plugin, checks `KCDDLSS.log` against `test/checks.txt` |
| hot | hot-reload swap under load (only when `tools/asi-hot` exists) | its own `run_test.sh` |

Proven to fail when it should (2026-11-15): two bytes changed in a DLL copy -> 7 host checks fail, and the plugin
turns the affected features off instead of crashing.

Add a check: a line in `test/checks.txt` (`+text` must be logged, `-text` must not), or a `T(...)` line in `unit_test.c`.

Not covered, needs one real run: the picture, the frame path (D3D11 draw hooks, DLSS evaluate, the inventory fix's
draw), the settings menu rows, cvars and live commands (they need the game console), GPU driver behaviour,
GOG/Epic builds (no DLL here: with one, `host_test.exe` runs against it unchanged).
Scratch output: `test/tmp/` (safe to delete).
