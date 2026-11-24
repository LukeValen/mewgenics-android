#!/bin/sh
# Lift ref/Mewgenics.exe (catalog in work/funcs.txt) into gen.new, apply the post passes (direct calls,
# shadow CPU), then replace only the files of gen/ whose content changed, so unchanged translation
# units keep their timestamps and are not recompiled.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
mkdir -p gen work
rm -rf gen.new
PYTHONPATH=$ROOT/third_party/pcrecomp python3 third_party/pcrecomp/tools/lift/generate64.py ref/Mewgenics.exe work/funcs.txt gen.new > work/lift.log 2>&1 \
  || { tail -20 work/lift.log; exit 1; }
python3 tools/directcalls.py gen.new config/keep_dispatch.txt >> work/lift.log
python3 tools/shadowcpu.py gen.new config/o0_units.txt >> work/lift.log
n=0
for f in gen.new/*; do
  b=$(basename $f)
  if ! cmp -s $f gen/$b; then cp $f gen/$b; n=$((n+1)); echo "changed $b"; fi
done
rm -rf gen.new
[ -n "$CHOWN" ] && chown -R "$CHOWN" gen
echo "$n files changed"; grep -E "recomp_todo_sites|lift_errors|functions " work/lift.log
