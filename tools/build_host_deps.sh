#!/bin/sh
# Linux debug host only: SDL3 (X11, GL/GLES) and glslang + SPIRV-Cross into build/.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
J=${JOBS:-8}
cmake -S third_party/SDL3-3.2.12 -B build/sdl3-host/b -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX=$ROOT/build/sdl3-host/install -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF \
  -DSDL_EXAMPLES=OFF -DSDL_WAYLAND=OFF -DSDL_X11=ON -DSDL_OPENGL=ON -DSDL_OPENGLES=ON -DSDL_VULKAN=OFF \
  -DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF
ninja -C build/sdl3-host/b -j$J install
cmake -S third_party/glslang-14.3.0 -B build/glslang-host -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_OPT=OFF \
  -DGLSLANG_TESTS=OFF -DENABLE_GLSLANG_BINARIES=OFF -DENABLE_HLSL=OFF -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX=$ROOT/build/xlate-host
ninja -C build/glslang-host -j$J install
cmake -S third_party/SPIRV-Cross-vulkan-sdk-1.4.304.0 -B build/spvc-host -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_ENABLE_HLSL=OFF \
  -DSPIRV_CROSS_ENABLE_MSL=OFF -DSPIRV_CROSS_ENABLE_CPP=OFF -DSPIRV_CROSS_ENABLE_REFLECT=OFF \
  -DSPIRV_CROSS_ENABLE_C_API=OFF -DSPIRV_CROSS_ENABLE_UTIL=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_INSTALL_PREFIX=$ROOT/build/xlate-host
ninja -C build/spvc-host -j$J install
echo "host deps ready"
