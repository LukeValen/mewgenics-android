#!/bin/sh
# usage: GAME_DIR=/path/to/Mewgenics tools/hostrun.sh <name> <seconds> [-e VAR=value ...]
# Runs build/host/mewhost under Xvfb in the build image with the game data mounted read-only;
# log in work/<name>.log, frame captures in work/shots/<name>/.
# env: GAME_DIR (required), IMAGE (default mewgenics-build), HOST_ROOT (path of this checkout as
#      the docker daemon sees it, default: the checkout), XVFB_SIZE (default 1600x900)
ROOT=$(cd "$(dirname "$0")/.." && pwd)
[ -f "$ROOT/tools/local.env" ] && . "$ROOT/tools/local.env"
: "${GAME_DIR:?set GAME_DIR to the folder with Mewgenics.exe and resources.gpak}"
name=$1; secs=$2; shift 2
docker rm -f mew-run-$name >/dev/null 2>&1
docker run --rm --name mew-run-$name \
  -v "${HOST_ROOT:-$ROOT}":/w -v "$GAME_DIR":/game:ro -w /w \
  -e W64_TIMEOUT=$secs -e XVFB_SIZE -e W64_SHOT_DIR=/w/work/shots/$name -e SDL_AUDIO_DRIVER=dummy "$@" \
  ${IMAGE:-mewgenics-build} sh -c "rm -rf work/shots/$name; mkdir -p work/user work/shots; Xvfb :9 -screen 0 ${XVFB_SIZE:-1600x900}x24 >/dev/null 2>&1 & sleep 1; DISPLAY=:9 build/host/mewhost /game /game/Mewgenics.exe /w/work/user > work/$name.log 2>&1; echo EXIT \$? >> work/$name.log"
tail -40 "$ROOT/work/$name.log"
