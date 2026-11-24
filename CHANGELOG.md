# Changelog

## 0.2.2 — 2026-11-24
- DXT texture atlases are decoded to RGBA8 on every OpenGL ES driver. Drivers that expose S3TC
  (Adreno 840, Galaxy Z Fold8) showed battles without floor and with broken scenery, because the
  game allocates compressed atlases with `glTexImage3D`, which ES does not allow.

## 0.2.1
- "Save and Quit to Desktop" exits cleanly (two CRT atexit thunks were missing from the lift).
- x87 control-word instructions of the CRT are handled.
- Game data is also found in the app's OBB folder (`Android/obb/ch.plum.mewgenics/`).

## 0.2.0
- Renders at up to 1920 px width and lets the compositor scale (storage/inventory screens 22 → 60+
  fps on a 2960×1848 tablet); launcher option for full resolution.
- Reliable finger drags (cats, items, collars), first tap previews / second tap confirms.
- Relaunching from the home screen returns to the running game.
- Short-lived game threads free their memory.
- Lifter: functions after `ret`/`int3` padding are found (fixed a crash in the first battle).

## 0.1.0
- First Android build: boot, menus, saves, map, first battle; low-memory profile and memfd arena so
  the game stays under vendor memory limits; GLSL 150 → ESSL translation; DXT decode; touch → mouse.
