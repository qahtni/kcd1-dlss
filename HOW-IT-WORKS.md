# How it works

Game: Kingdom Come: Deliverance 1.9.8 (Steam, build 22623230; also Epic Games Store 1.9.8b, see [Game builds](#game-builds)), CryEngine, 64-bit DirectX 11. The plugin is `tools/asi/kcd_dlss.c`, one C file with no C runtime, loaded by Ultimate ASI Loader (`version.dll`) when `KingdomCome.exe` starts.

## Where DLSS goes

The game already has a lower-resolution render path: the "native resolution" option (`r_FullscreenNativeRes`) renders the scene at 720p or 900p into a smaller target and then stretches it to the screen with a plain filter, the UI drawn on top at full size. That stretch is the natural place for an upscaler: the scene is finished (lighting, post effects) but not yet resized, and depth and motion data for the same frame still exist.

The plugin:

1. Intercepts `D3D11CreateDevice` through `WHGame.dll`'s imports, so it sees the game's device and immediate context, the adapter vendor and the swap chain sizes.
2. Loads the NGX core: NVIDIA's `_nvngx.dll` from the driver, or a drop-in `nvngx.dll` / the file named by `NgxDll` in the ini (OptiScaler) from the game folder, and `nvngx_dlss.dll` next to the plugin. The NGX calls are made through the C view of the SDK's parameter interface.
3. Forces the game to render at the chosen render size with native-resolution output (`r_Width`/`r_Height` for the scene, the window stays at the screen size). Render sizes come from NGX's optimal-settings query for the mode, or from the NVIDIA factor table when the core is not up yet. Ultra Quality is the mod's own 0.83; DLSS has no such size, so its feature is created with the Quality value at that size.
4. Hooks the stretch draw. Instead of the game's filter it hands DLSS the colour target, the depth buffer, the motion vectors, the jitter offset and the exposure settings, and copies the DLSS output into the game's full-size target. Everything on the immediate context that DLSS or the mod's own pass touches is saved before and restored after, because the game caches its D3D state and would otherwise keep stale bindings.

## Motion vectors

DLSS needs per-pixel motion. The game has two sources and the plugin uses both:

- **Object motion**: the game's motion-blur velocity buffer (`$VelocityObjects`, kept on by `r_MotionBlurGBufferVelocity 1`), written by moving and skinned objects within the motion-blur view distance, in the game's own sign-square-root encoding, which the plugin decodes.
- **Camera motion**: a small full-screen pass of the plugin's own (compiled at run time with `D3DCompile`) reprojects every pixel from depth with the previous and current view and projection matrices, read from the snapshots the game takes at the start of post-processing (the plugin keeps its own copy of the previous frame's matrices, so FOV changes are handled too), and writes the result into the plugin's motion-vector texture where the object buffer has nothing.

## Jitter and mip bias

- Each frame the plugin adds a sub-pixel offset (a Halton(2,3) sequence; the phase count grows with the upscale ratio, 8 to 64) to the camera projection, so DLSS gets different samples over time. The same offset is passed to DLSS so it can undo it.
- Texture sampling at a lower render size would pick blurrier mip levels. The plugin biases the game's samplers by `log2(renderW / nativeW)` plus the ini offset (NVIDIA's and AMD's guidance: -1.0; Intel: 0), by swapping each sampler the game binds for a biased twin from a cache, so a live mode change re-biases everything at once.

## Settings the game needs

At start, and again whenever the settings menu reapplies the profile, the plugin sets in memory: `r_FullscreenNativeRes 1`, `r_AntialiasingMode 0` (DLSS does the anti-aliasing), `r_MotionVectors 1`, `r_MotionBlurGBufferVelocity 1`, and `e_svoTI_ResScaleBase 1` so the global illumination cone tracing runs at the render size instead of half of it (otherwise at Performance the lighting would be a quarter of the screen size, upscaled twice). A watchdog re-forces them the same frame the menu's Apply puts the game's values back. The cvars are reached through the engine's `IConsole` interface (the game's copy of `gEnv->pConsole`, at a known offset in `WHGame.dll` 1.9.8).

## The menu rows

The plugin registers two console variables, `r_DLSSMode` and `r_DLSSPreset`. Since BETA 3 the plugin hooks `ISystem::LoadXmlFromFile` (CSystem vtable slot 112), reads the game's `MM_AdvancedGraphics.xml` through `gEnv->pCryPak`, inserts the two rows as text and parses the result with `LoadXmlFromBuffer` (slot 111); before that a small mod in `Mods/KCDDLSS` replaced the Advanced Graphics page definition (`Libs/UI/UIActions/MM_AdvancedGraphics.xml`, a flowgraph) with one that adds two rows built from the same node types the page already uses (`AddChoicesButton`, `AddChoiceOption`, `SetChoice`, `GetChoice`, and a `Debug:ConsoleVariable` node per row). The page reads the cvar when it opens and writes it when a choice changes, and the plugin polls its cvars every frame, applies the mode or model live and saves the choice to `KCDDLSS.ini`. On a PC without an upscaler both cvars are pinned to the "Unsupported" choice. `tools/menu-mod/MM_AdvancedGraphics.patch` holds exactly the lines added; `build_menu_mod.py` validates the patched flowgraph against every shipped UIAction file before it packs it.

## Game builds

Everything the plugin touches in `WHGame.dll` is at a fixed address of one game build: 22 `RVA_*` values (the renderer object, its vtable slot `+0xab8`, the jitter zeroing, the two `$VelocityObjects` creation sites, the `CSystem` vtable, `gEnv->pConsole` / `pLog` / `pCryPak`, the depth SRV, the camera matrices and a few cvars), plus two byte patterns that contain relative jumps and RIP displacements (`ctx_l` at the velocity site, `zero_ctx` at the jitter site). The grass fix and the sharpening removal search for their code and need none of them.

These values live in one table, `g_wh_builds[]` in `kcd_dlss.c`, one row per build; the `RVA_*` names read from the selected row. At start `select_build()` picks the row by reading, never writing, two identity checks: the renderer vtable slot `+0xab8` holding that row's stretch function, and `CSystem` slots 111 / 112 holding its `LoadXmlFromBuffer` / `LoadXmlFromFile` (the same comparisons the hooks make before they install). Either check is enough, so a vtable another mod has already hooked does not hide the build; the log then says "1/2 identity checks: partial match". Every patch still checks its own bytes or slot before writing. With no matching row the build is unknown: the grass fix and the sharpening removal still run (they search for their code), `dlss_install` and `menu_xml_install` are skipped and `g_wh_base` stays NULL, which switches off everything that reads the game's globals (console cvars, the watchdog, mip bias sizes, the game-console log lines).

The Epic 1.9.8b row (`WHGame.dll` PE timestamp `0x69ccdca1`) was found in that DLL without running the game: the patch sites by their byte checks with the displacements masked (the jitter and `OnActivate` patterns match once; the velocity pattern matches both creation sites, which are the left and right sites the plugin patches), `$VelocityObjects` and the renderer from the velocity site's own RIP operands, the vtables through RTTI, `gEnv` from where `new CLog` / `new CXConsole` / `new CCryPak` are stored, the camera matrices from the `GetModelViewMatrix` / `GetProjectionMatrix` calls that fill them, the depth SRV from its bind at t16, the eye index from the `$VelocityObjects[eye]` reads, and the AA copy and the post-effect cvars from their registrations. Each value was matched to its use in code, not computed: the data shifts against Steam differ by group (renderer, depth SRV, camera matrices and bind records `-0x5480`, `$VelocityObjects` `-0x5410`, eye index `-0x54a4`, AA copy `-0x5484`, the post-effect cvars `-0x54ec`, `gEnv` `-0x5040`), so a new row cannot be derived from one shift. Confirmed in game on Epic 1.9.8b.

Not per build: the struct offsets (`R_*`, `R_DEVINFO`, `R_FULLSCREEN`, `PFX_MGR_OFF`, `CT_*`) and the vtable slot numbers (renderer, console, CryPak, post effects) are shared by all rows. They are unchanged in Epic 1.9.8b (the renderer's depth SRV member is `0x29130` from the object in both builds), but a new build has to re-check them too. A new build gets a new row the same way. Addresses quoted in the source comments are Steam 1.9.8 ones.

## Also in the plugin

- The 30 FPS Grass Fix, same code and byte signature as the standalone mod (`sig.h`), so the two need not be installed together.
- The forced sharpening in conversations is removed, and the water-ripple post effect is wrapped so that it restores the renderer's state after its render (it otherwise leaves a binding behind that breaks the upscaled frame).
- The debug build (`-DKCDDLSS_DEBUG`) adds a blue-frame probe, NVIDIA's NGX log messages and post-effect tracing to `KCDDLSS.log`; the release build logs the minimum and mirrors a few lines into the game's console and `kcd.log` with the prefix `[KCDDLSS]`.
