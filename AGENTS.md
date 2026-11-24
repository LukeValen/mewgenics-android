# Project: mewgenics-android

Static recompilation of Mewgenics (PC, Steam build 1.1.21239)
for Android arm64. x86-64 `Mewgenics.exe` → C (pcrecomp `lift64_cpu` via `generate64.py`, plus own
post passes) → NDK clang → `libmain.so`, own Win64 runtime, SDL3 + OpenGL ES on the device.
No Wine/Box64/emulator. Status/evidence ledger: `port/RECOMP_STATUS.md`.

## Source and legal boundary
- `ref/Mewgenics.exe` SHA-256 4127cd6a792ae528bca6f65a8873dd61789591937d87656c2b586a5e30eb77ea (gitignored).
- Data: `resources.gpak` 5.1 GB, read at run time from the device, never in the APK/repo. The launcher runs
  from `files/game` (import / USB) or else from the app's OBB folder `Android/obb/ch.plum.mewgenics/`.
- Hub/store publishing of APKs: keep the store app's visibility as the owner set it (friends), never reset it.
- Never commit game files, `gen/`, `build/`, `work/`, `.so` or APKs.

## Target facts
- PE32+ x64, MSVC static CRT, image base 0x140000000, entry 0x140D3E8AC, relocations + TLS.
- SDL 3.2.12 statically linked **with dynapi**: jump table at 0x1412E7000, 1232 slots (SDL_dynapi_procs.h order).
- OpenGL 3.2 core, GLSL 150, GLEW-style loader through `wglGetProcAddress`; multi-draw indirect and
  persistent mapped buffers when available. Textures: the game DXT5-compresses its atlases itself.
- Fixed update rate = display refresh rate (must never be 0); Steam API 7 imports (faked).

## Pipeline (build image from `docker/Dockerfile`, checkout mounted at /w)
- Everything at once: `tools/build.sh /path/to/Mewgenics.exe` (see README).
- Catalog: `tools/pdata_catalog.py ref/Mewgenics.exe work/funcs.txt`.
- Lift + post passes: `sh tools/regen.sh` → generate64 into gen.new, `tools/directcalls.py` (known targets
  become direct C calls; `config/keep_dispatch.txt` keeps dispatch for host overrides), `tools/shadowcpu.py`
  (per-function local CPU copy; skips EH functions and > 6000-line init functions), then copies only the
  changed files into gen/ so unchanged units are not recompiled.
- Lifter fixes (atomics, div128, bswap, plausible_code, x87 control, …) live in `third_party/pcrecomp`
  (pinned by `tools/fetch_deps.sh`); after changing them refresh the patch:
  `(cd third_party/pcrecomp && git diff 35548b8 -- tools/lift/generate64.py tools/lift/lift64_cpu.py) > tools/patches/pcrecomp-mewgenics.patch`.
- Thunk generators: `tools/gen_sdl_thunks.py` (SDL dynapi → host SDL3), `tools/gen_gl_thunks.py` (gl.xml).

## Runtime (`runtime/src`)
core.c (loader, dispatch, TEB/PEB/TLS, threads registry, watchdog `W64_WATCH`, profiler `W64_PROF`, overrides),
k32_*.c (kernel32), w32_stubs.c (GUI DLLs), steam.c, sdl_bridge.c (+ generated sdl_thunks.c), gl_bridge.c
(+ generated gl_thunks.c), gl_es.c (ES translation), glsl_xlate.cpp (glslang + SPIRV-Cross), hle_libs.c
(host miniz/stb_dxt for the hottest library code), fmt.c (Win64 varargs printf), input_script.c.

## Host loop
- Build (in the build image): `sh tools/build_host_deps.sh` once, then `sh tools/build_stale.sh && sh tools/hostlink.sh`
  → `build/host/mewhost`.
- Run: `GAME_DIR=<Mewgenics dir> XVFB_SIZE=1280x720 tools/hostrun.sh <name> <secs> -e W64_SHOT_EVERY=10 [-e W64_GLES=1 -e W64_NO_S3TC=1]
  [-e W64_INPUT="120:click:90:466,…"]` → `work/<name>.log`, `work/shots/<name>/*.ppm`.
- Live interactive session: run `build/host/mewhost` in a detached container with `W64_INJECT_FILE=<file>`
  (append lines: `click x y`, `move x y`, `rclick x y`, `tap x y`, `tap2 x y`, `tdown/tmove/tup x y` = finger
  drag, `down/up x y`, `key Name`) and `W64_SHOT_DIR=<dir>`; `touch <dir>/req` makes the next frame land in
  `<dir>/req.ppm`. `W64_TOUCH=1` enables the Android touch filter on the host, `W64_EVLOG=1` logs mouse events.
- Debug: gdb works in the container with `--cap-add=SYS_PTRACE --security-opt seccomp=unconfined`;
  host symbols are `L_<guest va>`; `c_->r14` is at offset 112 of the CPU struct.
- The game's own log: `work/user/pref/Glaiel Games/Mewgenics/<steamid>/logs/debug.log`.

## Android
- `.so`: `sh tools/build_android_so.sh` (incremental; ANDROID_SDK with NDK 27.3.13750724 + CMake 3.31.6),
  APK: `cd android && ./gradlew assembleRelease` (or `tools/build.sh` for everything).
- Plum Code sessions: `tools/plum/devdeploy.sh [serial]` (android-builder MCP) with settings in the
  uncommitted `tools/local.env` (HOST_ROOT, IMAGE, SDK_HOST_DIR, GAME_DIR, PLUM_BUILDER_*, DEVICE_SERIAL).
- Game data on a test device: `adb push` (or a temporary HTTP server + `curl` on the device) into
  `/sdcard/Android/data/ch.plum.mewgenics/files/game/` or `/sdcard/Android/obb/ch.plum.mewgenics/`.
- Debug switches on a device: `setprop debug.mew.env "W64_TRACE=1;W64_GLCHECK=1"` (clear it afterwards);
  input injection: `setprop debug.mew.inject "<n>:tap:x:y"` (new `<n>` per event; also tap2/key; clear it afterwards);
  runtime log also in `files/user/runtime.log`.
- Render size: MewActivity limits the surface to 1920 px wide (W64_RENDER_SIZE for the runtime) unless the
  launcher option "Volle Bildschirmauflösung" is set; `MewActivity` is not exported, start it through the launcher.
- Profiling: `W64_PROF=1` (top guest functions + imports per 20 s window), `W64_GLSTAT=1` (import calls per
  frame, glCopyTexSubImage2D volume) in `debug.mew.env`.
- Shared test devices: check the foreground activity before taking focus.
