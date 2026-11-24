# Third-party components

## Included in this repository

| Component | Where | License |
|---|---|---|
| SDL 3.2.12 Android Java glue (`org.libsdl.app`), with small changes for a fixed-size render surface | `android/app/src/main/java/org/libsdl/app/` | zlib (© Sam Lantinga) |
| SDL dynapi function list (generated thunks) | `runtime/src/sdl_thunks.c` | zlib (© Sam Lantinga) |
| OpenGL API registry `gl.xml` (generated thunks) | `runtime/src/gl_thunks.c` | Apache-2.0 (© The Khronos Group Inc.) |
| pcrecomp runtime headers (`cpu64.h`, `eh64.c/.h`), adapted | `runtime/include/` | MIT (© sp00nz) |
| Gradle wrapper | `android/gradlew`, `android/gradle/wrapper/` | Apache-2.0 (© Gradle Inc.) |

## Fetched at build time (`tools/fetch_deps.sh`) and linked into the APK

| Component | Version | License |
|---|---|---|
| [pcrecomp](https://github.com/sp00nznet/pcrecomp) (lifter, build-time only) | 35548b8 + `tools/patches/pcrecomp-mewgenics.patch` | MIT |
| [SDL](https://github.com/libsdl-org/SDL) | 3.2.12 | zlib |
| [glslang](https://github.com/KhronosGroup/glslang) | 14.3.0 | BSD-3-Clause and others, see its `LICENSE.txt` |
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) | vulkan-sdk-1.4.304.0 | Apache-2.0 |
| [miniz](https://github.com/richgel999/miniz) | 3.0.2 | MIT |
| [stb_dxt](https://github.com/nothings/stb) | 6b5799b | MIT or public domain |
| [bcdec](https://github.com/iOrange/bcdec) | 80859ed | MIT or Unlicense |

The full license texts ship with each fetched source tree in `third_party/`.

## Not included

Mewgenics (© Glaiel Games / Edmund McMillen and Tyler Glaiel): neither its executable code, nor
code generated from it (`gen/`), nor its data (`resources.gpak`) are part of this repository.
