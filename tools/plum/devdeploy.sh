#!/bin/sh
# Plum Code dev loop (needs the Plum android-builder MCP): rebuild libmain.so incrementally in the build
# image, sync the android-builder workspace, build the APK there and install it on a device.
#   tools/plum/devdeploy.sh [adb-serial]
# Settings come from tools/local.env (not committed): HOST_ROOT, IMAGE, SDK_HOST_DIR (an Android SDK
# on the docker host, mounted at /sdk), PLUM_BUILDER_PROJECT, PLUM_BUILDER_WS, DEVICE_SERIAL.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
[ -f tools/local.env ] && . tools/local.env
SERIAL=${1:-$DEVICE_SERIAL}
: "${PLUM_BUILDER_PROJECT:?set in tools/local.env}" "${SERIAL:?pass an adb serial}"
SDKMOUNT=""; [ -n "$SDK_HOST_DIR" ] && SDKMOUNT="-v $SDK_HOST_DIR:/sdk:ro"
docker run --rm -v "${HOST_ROOT:-$ROOT}":/w $SDKMOUNT -w /w -e JOBS=10 ${IMAGE:-mewgenics-build} \
  sh -c 'sh tools/build_android_so.sh /w/gen > work/android_build.log 2>&1; echo EXIT $? >> work/android_build.log'
tail -1 work/android_build.log | grep -q "EXIT 0" || { tail -20 work/android_build.log; exit 1; }
sh tools/plum/sync_builder.sh > /dev/null
MCP="node /app/scripts/mcp-call.mjs call android-builder"
$MCP android_build "{\"projectId\":\"$PLUM_BUILDER_PROJECT\",\"buildType\":\"debug\"}" > /dev/null 2>&1 || true
for i in $(seq 1 90); do
  st=$($MCP android_get_project "{\"projectId\":\"$PLUM_BUILDER_PROJECT\"}" 2>&1 | grep -o '"status": *"[a-z_]*"' | head -1)
  case "$st" in *building*) sleep 10;; *) break;; esac
done
echo "build: $st"
$MCP android_install "{\"projectId\":\"$PLUM_BUILDER_PROJECT\",\"serial\":\"$SERIAL\"}" 2>&1 | grep -o '"output": "[^"]*"' | head -1
