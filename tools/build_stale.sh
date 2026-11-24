#!/bin/sh
# Rebuild host objects of generated units whose C is newer than the object. Units listed in
# config/o0_units.txt (giant functions clang -O1 takes ages on) go to build/host/gen0 at -O0.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
F="-w -fwrapv -fno-strict-aliasing -ffp-contract=off -g0 -fno-unwind-tables -fno-asynchronous-unwind-tables -Iruntime/include -Igen"
mkdir -p build/host/gen build/host/gen0
for f in gen/recomp_funcs_*.c gen/recomp_dispatch.c gen/recomp_imports.c; do
  b=$(basename $f .c)
  if grep -qx "$b" config/o0_units.txt 2>/dev/null; then o=build/host/gen0/$b.o; opt=-O0; else o=build/host/gen/$b.o; opt=-O1; fi
  if [ ! -f $o ] || [ $f -nt $o ]; then echo "$opt $f -> $o"; fi
done > build/host/todo.txt
cat build/host/todo.txt | xargs -P ${JOBS:-14} -L 1 sh -c 'clang $0 '"$F"' -c $1 -o $3' 
echo "rebuilt $(wc -l < build/host/todo.txt) units"
