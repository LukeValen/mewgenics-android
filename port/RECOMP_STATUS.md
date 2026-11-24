# Mewgenics (Steam, Win64) → Android (arm64) — static recompilation status

Evidence ledger. "Verified" means executed and observed; host-verified and device-verified kept apart.

## Target (static inspection)
| Item | Evidence |
|---|---|
| Main module | `Mewgenics.exe` PE32+ x86-64, 22,019,072 bytes, SHA-256 `4127cd6a…0eb77ea`, MSVC static CRT, image base 0x140000000 |
| Engine | Glaiel engine (SWF renderer), SDL 3.2.12 statically linked **with dynapi**, OpenGL 3.2 core (GLSL 150) via GLEW-style loader, miniz, stb_dxt |
| Data | `resources.gpak` 5.1 GB (19,979 entries: u32 count, {u16 len, name, u32 size}…, data back to back) |
| Imports | 456 (KERNEL32 211, USER32 138, GDI32 24, OPENGL32 27, steam_api64 7, …) — most belong to SDL's Win32 backend, which never runs |

## Route
pcrecomp `generate64` lift (56,259 functions, 0 lift errors) → own post passes (direct calls, shadow CPU) →
own Win64 runtime (`runtime/src`): loader at the preferred base (identity memory), TEB/PEB/TLS, kernel32,
Win32 GUI stubs, Steam fakes. SDL boundary: `SDL3_DYNAMIC_API=w64sdl.dll` makes the game's SDL fill its
dynapi jump table from our `SDL_DYNAPI_entry`: 1232 slots → host SDL3 (1062 generated thunks, ~60 by hand).
GL: 3293 generated thunks from Khronos gl.xml; on GLES hosts GLSL 150 → ESSL 3.10 via glslang + SPIRV-Cross,
DXT → RGBA8 decode when S3TC is missing, ES blit/caps/format fixes (`gl_es.c`).

## Milestones
| Milestone | State |
|---|---|
| M1 lift | **verified** 2026-11-24: 56,259 functions (incl. 101 found after fixing `plausible_code` for functions after `ret`/`int3` padding, 3 two-instruction `lea; jmp` atexit thunks and one empty `ret 0` atexit entry); every relocated code pointer and every entry-like `lea` target in .text is catalogued; 146 TUs; lifter fixes: 128-bit div, bswap r32, pmuludq, sqrtpd, cvt(t)pd2dq, rcr/rcl, tzcnt/lzcnt, cmpxchg8/16, LOCK RMW + xchg as host atomics, pause as barrier |
| M2 host boots | **verified** (Linux, Xvfb, Mesa llvmpipe): CRT start-up, SDL via dynapi, Steam fake, GL 3.2 core, loading, intro cut-scene, **main menu**, Play → save slots → new game intro dialogue |
| M2b host GLES path | **verified** (Mesa EGL, OpenGL ES 3.2, S3TC disabled): main menu renders through translated shaders (48/48) and DXT→RGBA8 decode |
| M3 Android boots | **verified on device** 2026-11-24: Galaxy Tab S8 Ultra (Adreno 730, ES 3.2, Android 16), APK 0.1.0: loading ~25 s, intro, main menu (50–80 fps), fullscreen |
| M4 interactive | **verified on device**: touch (tap = left click), Play → save slot → story dialogue → cat choice → world map "The Path" → first battle (~100 fps); save file written (`saves/steamcampaign02.sav`) |
| M5 memory | **verified on device**: full quality needs ~4.7 GB of guest heap; Samsung's Heimdall kills foreground apps above 4 GB anonymous memory. Low-memory profile (forced settings `texture_scale_big 0.5`, `curve_resolution 2.5`, `soundeffect_cache_size_mb 64`, `antialias 0`) + memfd arena for blocks >= 64 KB: native heap 2.7 GB, ~1 GB shared memory, no kill through the first battle |
| M6 device session | **verified on device** 2026-11-24 (APK 0.1.0): audio output non-silent (measured peak ~0.2; not listened to), save written and reloaded across app restarts, settings menu, back key → pause menu, two-finger tap → right click, suspend/resume (GL context and audio survive, 93–95 fps after resume), battle tutorial (move, attack, ability tooltip, end turn, ally turn), memory stable (~2.7 GB native + 0.6–1 GB memfd) |
| M7 game loop (host, GLES + touch path) | **verified on the host** 2026-11-24 with `W64_GLES=1 W64_NO_S3TC=1 W64_LOWMEM=1 W64_TOUCH=1` (same event filter as Android), ~3 h session: whole tutorial battle won, level-up + ability choice, map events (Butch), treasure, inventory (equip by tap), boss fight "Pebbles" (status effects, knock-back, downed cat) won, house phase: drag cats into the house by finger, End Day → night → next day with a kitten born; save/reload of that progress. No guest faults, no unknown dispatch. |
| M8 device 0.2.0 | **verified on device** 2026-11-24 (Tab S8 Ultra): launcher → game, home + relaunch returns to the running game, house phase from a save made on the host (End Day → night → next day), stray dragged into the house by a real finger swipe, storage ↔ trash drags (8/8 fast 400 ms swipes), battle tutorial taps (preview → confirm, attack), suspend/resume keeps the scaled surface; storage screen 60–66 fps (was 22), house/battle 120 fps; 0 guest faults |
| M9 0.2.1 | **verified on device** 2026-11-24 (Tab S8 Ultra): game data in the app's OBB folder (where Plum Hub's store puts the private companion files) found by the launcher and loaded; "Save and Quit to Desktop" exits cleanly; second adventure ("The Alley", class collars, equipment, real battle, defeat → back home), several End Day cycles, adventure box via finger drag; memory stable (native heap 0.9–1.5 GB), 100–120 fps in battle/house |
| M10 second device | **verified on device** 2026-11-24 (Galaxy Z Fold8 SM-F971B, Snapdragon SM8850 / Adreno 840, Android 17, inner display 2448x1848, render 1920x1449): game data downloaded through the Plum Hub store into the OBB folder, menu (German), save slot, map, battle with floor/scenery, touch move (preview → confirm) and attack, 120 fps, 0 guest faults |
| Open | Odin 3 untested (secure lock screen); the native S3TC path (`W64_S3TC=1`) is known broken on ES (see fixes) |

## Fixes found on the way
- Galaxy Z Fold8 (Adreno 840) showed battles without floor and with fragmented scenery: its driver exposes S3TC, so DXT atlases went to the driver unchanged - but the game creates them desktop-style with `glTexImage3D(compressed internal format, NULL)`, which OpenGL ES rejects, so the atlases had no storage. On ES the runtime now always stores DXT textures as RGBA8 and decodes the blocks on upload (the path verified on Adreno 730); `W64_S3TC=1` keeps the experiment.
- "Save and Quit to Desktop": after `main` returned, the CRT's atexit list called `0x140E3CF00` (a `lea rcx, [obj]; jmp dtor` destructor thunk) and `0x140E3E618` (an empty `ret 0`), which `plausible_code` rejected (one/two instructions before int3 padding) → FATAL unknown dispatch at exit. Such short jmp/ret sequences before padding are accepted now (additive: 4 new functions, none lost); verified on device: `main returned 0` → `ExitProcess(0)`. x87 control instructions (`fnstcw`/`fldcw`/`fnclex`, CRT setjmp/longjmp) are modelled like stmxcsr/ldmxcsr now (no more TODO log).
- Fill rate: the game renders every frame at the surface size and stacks masks/copies in some screens (storage: 308 draws, 192 stencil masks, 2 full-screen copies per frame): 22 fps at 2960x1848. MewActivity fixes the SurfaceView buffer at <= 1920 px wide (MATCH_PARENT layout so the compositor scales it up), the runtime re-applies `ANativeWindow_setBuffersGeometry` after SDL (re)creates the EGL surface, the guest sees that size as the display mode, SDLSurface maps touches from view to buffer coordinates. Launcher option "Volle Bildschirmauflösung" turns it off.
- Fast drags failed sometimes: the game reads `SDL_GetMouseState`, which ran ahead of the (held back) events → the runtime answers it from the events actually delivered, and a drag's press is delivered one frame before its first motion.
- Thread churn: the game starts short-lived threads all the time (and thread-pool work ran on a new host thread per item); guest stack (16 MB map), TEB, TLS block and CPU struct were never freed and the registry filled up → `w64_thread_detach` at thread end, registry entries reused.
- Battle crash `dispatch to unknown guest address 0x1408e3c20`: functions that follow `ret` + `int3` padding were rejected by the lifter's `plausible_code` check → fixed in `generate64.py` (101 more functions).
- Touch: Android delivers a finger as touch-mouse events; the left press is held back 140 ms so a second finger can turn it into a right click. A finger that moves while the press is held back now releases the press first (at the touch-down point), so dragging (cats into the house, items) works. A first tap on a battle tile previews the path, a second tap confirms — the game's own mouse behaviour (hover, then click), kept as the touch UX.
- Relaunching from the home screen while the game runs showed the launcher on top → the launcher finishes itself when it is not the task root.
- Device only: a bit reader reads a few bytes past its buffer (benign on Windows, guard page on Android) → 64-byte tail slack on heap blocks >= 1 KB.
- Device only: S3TC missing on Adreno → DXT5 atlases decoded to RGBA8 on upload (bcdec); MSAA resolve blit with offset rejected by ES → two-step resolve.
- Display refresh rate: the game's fixed update rate = refresh rate. Xvfb reports 0 Hz → no updates (frozen
  dark screen); phones report 90/120 Hz. The guest always sees 60 Hz (`SDL_GetCurrent/DesktopDisplayMode`).
- Performance counter: guest sees a Windows-shaped 10 MHz counter from process start (host ns since boot overflowed
  `counter * 1000000 / freq`).
- Spin locks hung: `xchg mem` / `lock` RMW were lifted as plain load/store (and hoisted out of the loop).
- Speed: shadow CPU (per-function local register copy, flags not written back at calls/ret) shrank code 40 %;
  host replacements for miniz `tinfl_decompress` and the stb_dxt DXT5 image compressor took the bitmap atlas
  generation (1815 bitmaps) from 322 s to well under a minute on the host.
