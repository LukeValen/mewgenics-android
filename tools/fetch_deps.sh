#!/bin/sh
# Fetch the third-party sources the build uses into third_party/ (not committed), all pinned.
#   pcrecomp (MIT, the x86-64 -> C lifter) at a fixed commit + tools/patches/pcrecomp-mewgenics.patch
#   SDL 3.2.12, glslang 14.3.0, SPIRV-Cross vulkan-sdk-1.4.304.0, miniz 3.0.2, stb_dxt, bcdec,
#   Khronos gl.xml (only needed to regenerate runtime/src/gl_thunks.c)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TP=$ROOT/third_party
PCRECOMP_REPO=https://github.com/sp00nznet/pcrecomp
PCRECOMP_COMMIT=35548b8d0fd8aa853da67edcf7cd6134f6b9cdc7
STB_COMMIT=6b5799bae8a1fe3792691ef4b72ae26434c3e799
BCDEC_COMMIT=80859ed3b7afb1c527a2a99d70c61457bea72d0c
GLXML_COMMIT=1cdd228e34966dd6b95bd203e9f84faba0f371a1
mkdir -p "$TP/dl" "$TP/stb" "$TP/miniz"
cd "$TP"
get() { [ -s "$2" ] || curl -fsSL -o "$2" "$1"; }

if [ ! -d pcrecomp/.git ]; then
  git clone -q "$PCRECOMP_REPO" pcrecomp
  (cd pcrecomp && git checkout -q "$PCRECOMP_COMMIT" && git apply "$ROOT/tools/patches/pcrecomp-mewgenics.patch")
  echo "pcrecomp $PCRECOMP_COMMIT + mewgenics patch"
fi
[ -d SDL3-3.2.12 ] || { get https://github.com/libsdl-org/SDL/releases/download/release-3.2.12/SDL3-3.2.12.tar.gz dl/SDL3-3.2.12.tar.gz; tar xzf dl/SDL3-3.2.12.tar.gz; }
[ -d glslang-14.3.0 ] || { get https://github.com/KhronosGroup/glslang/archive/refs/tags/14.3.0.tar.gz dl/glslang14.tar.gz; tar xzf dl/glslang14.tar.gz; }
[ -d SPIRV-Cross-vulkan-sdk-1.4.304.0 ] || { get https://github.com/KhronosGroup/SPIRV-Cross/archive/refs/tags/vulkan-sdk-1.4.304.0.tar.gz dl/spirv-cross.tar.gz; tar xzf dl/spirv-cross.tar.gz; }
[ -f miniz/miniz.c ] || { get https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip dl/miniz.zip; (cd miniz && unzip -o -q ../dl/miniz.zip); }
get https://raw.githubusercontent.com/nothings/stb/$STB_COMMIT/stb_dxt.h stb/stb_dxt.h
get https://raw.githubusercontent.com/iOrange/bcdec/$BCDEC_COMMIT/bcdec.h stb/bcdec.h
get https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/$GLXML_COMMIT/xml/gl.xml dl/gl.xml
echo "third_party ready"
