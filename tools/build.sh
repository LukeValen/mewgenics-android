#!/bin/sh
# One-shot build: your own Mewgenics.exe -> out/Mewgenics-Android-<version>.apk
#   tools/build.sh /path/to/Mewgenics.exe
# Run inside the build image (docker/Dockerfile), or on a Linux box with the same tools and
# ANDROID_SDK pointing at an SDK with NDK 27.3.13750724 and CMake 3.31.6.
# Optional: MEW_KEYSTORE / MEW_KEYSTORE_PASSWORD / MEW_KEY_ALIAS / MEW_KEY_PASSWORD sign the
# release APK with your key (otherwise the SDK's debug key is used). JOBS sets build parallelism.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
EXE=${1:?usage: tools/build.sh /path/to/Mewgenics.exe}
WANT=4127cd6a792ae528bca6f65a8873dd61789591937d87656c2b586a5e30eb77ea
HAVE=$(sha256sum "$EXE" | cut -d' ' -f1)
if [ "$HAVE" != "$WANT" ]; then
  echo "Mewgenics.exe SHA-256 $HAVE"
  echo "this port was made for $WANT (Steam build 1.1.21239)."
  [ -n "$FORCE" ] || { echo "other builds will most likely not work; set FORCE=1 to try anyway"; exit 1; }
fi
SDK=${ANDROID_SDK:-/sdk}
[ -d "$SDK/ndk/27.3.13750724" ] || { echo "ANDROID_SDK ($SDK) has no ndk/27.3.13750724"; exit 1; }

echo "== third-party sources"; sh tools/fetch_deps.sh
echo "== function catalog"; mkdir -p ref work gen; cp "$EXE" ref/Mewgenics.exe
python3 tools/pdata_catalog.py ref/Mewgenics.exe work/funcs.txt
echo "== lift x86-64 -> C (a few minutes)"; sh tools/regen.sh
echo "== native libraries (arm64, ~30-60 minutes the first time)"; sh tools/build_android_so.sh "$ROOT/gen" > work/android_build.log 2>&1 \
  || { tail -30 work/android_build.log; exit 1; }
echo "== APK"
echo "sdk.dir=$SDK" > android/local.properties
(cd android && sh ./gradlew -q --no-daemon assembleRelease)
VER=$(grep -o 'versionName = "[^"]*"' android/app/build.gradle.kts | cut -d'"' -f2)
mkdir -p out; cp android/app/build/outputs/apk/release/app-release.apk out/Mewgenics-Android-$VER.apk
ls -la out/Mewgenics-Android-$VER.apk
echo "done. Install with: adb install -r out/Mewgenics-Android-$VER.apk"
