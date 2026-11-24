#!/bin/sh
# Plum Code: mirror android/ (incl. the prebuilt jniLibs) into the android-builder workspace
# (PLUM_BUILDER_WS from tools/local.env, owned by uid 1100), replacing the template app sources.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
[ -f tools/local.env ] && . tools/local.env
: "${PLUM_BUILDER_WS:?set in tools/local.env}"
docker run --rm -u 1100:1100 -v "${HOST_ROOT:-$ROOT}/android":/src:ro -v "$PLUM_BUILDER_WS":/dst debian:bookworm-slim \
  sh -c 'rm -rf /dst/app/src/main/java /dst/app/src/main/kotlin /dst/app/src/main/res /dst/app/build.gradle /dst/build.gradle /dst/settings.gradle;
         cd /src && tar --exclude=./app/build --exclude=./.gradle --exclude=./build --exclude=./local.properties --exclude=./native -cf - . | (cd /dst && tar -xf -)'
echo "synced to $PLUM_BUILDER_WS"
