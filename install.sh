#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

BUILD_DIR=build-release
APP_NAME=OpenSCAD.app
DEST=/Applications

if pgrep -f "$DEST/$APP_NAME/Contents/MacOS/OpenSCAD" >/dev/null; then
  echo "ERROR: $DEST/$APP_NAME is currently running. Quit it first." >&2
  exit 1
fi

git submodule update --init --recursive

cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DEXPERIMENTAL=1

cmake --build "$BUILD_DIR" -j"$(sysctl -n hw.ncpu)"

if [[ -d "$DEST/$APP_NAME" ]]; then
  rm -rf "$DEST/$APP_NAME"
fi
ditto "$BUILD_DIR/$APP_NAME" "$DEST/$APP_NAME"

echo "Installed $DEST/$APP_NAME"
