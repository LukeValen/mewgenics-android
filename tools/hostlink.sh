#!/bin/sh
# Link build/host/mewhost from the generated objects (gen0/ overrides gen/) and the runtime.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
mkdir -p build/host/rt
for f in runtime/src/*.c runtime/include/eh64.c host/main.c; do
  o=build/host/rt/$(basename $f .c).o
  if [ ! -f $o ] || [ $f -nt $o ] || [ runtime/src/w64.h -nt $o ] || [ runtime/include/cpu64.h -nt $o ]; then
    clang -O1 -g -Wall -Wno-unused-function -Wno-unused-variable -fwrapv -fno-strict-aliasing -ffp-contract=off \
      -Iruntime/include -Iruntime/src -Igen -Ibuild/sdl3-host/install/include -c $f -o $o
  fi
done
XL=""
if [ -d build/xlate-host/lib ]; then
  if [ ! -f build/host/rt/glsl_xlate.oxx ] || [ runtime/src/glsl_xlate.cpp -nt build/host/rt/glsl_xlate.oxx ]; then
    clang++ -O2 -g -std=c++17 -Ibuild/xlate-host/include -Ibuild/xlate-host/include/glslang -Ibuild/xlate-host/include/spirv_cross -c runtime/src/glsl_xlate.cpp -o build/host/rt/glsl_xlate.oxx
  fi
  XL="build/host/rt/glsl_xlate.oxx -Wl,--start-group $(ls build/xlate-host/lib/*.a | tr '\n' ' ') -Wl,--end-group -lstdc++"
else
  echo 'char *w64_glsl_to_essl(const char *s, int t, int v) { (void)s; (void)t; (void)v; return 0; }' > build/host/rt/xl_stub.c
  clang -c build/host/rt/xl_stub.c -o build/host/rt/xl_stub.oxx
  XL=build/host/rt/xl_stub.oxx
fi
objs=""
for o in build/host/gen/*.o; do b=$(basename $o); [ -f build/host/gen0/$b ] && continue; objs="$objs $o"; done
for o in build/host/gen0/*.o; do objs="$objs $o"; done
clang -o build/host/mewhost build/host/rt/*.o $XL $objs -Lbuild/sdl3-host/install/lib -Wl,-rpath,$ROOT/build/sdl3-host/install/lib -lSDL3 -lGL -lpthread -ldl -lm
ls -la build/host/mewhost
