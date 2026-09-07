#!/usr/bin/env sh
# Builds the playground into playground/dist: the compiler as a WASI module,
# the packed ODIN_ROOT sources, the Odin JS runtime and the web page.
#
# Usage: ./playground/build_playground.sh   (run from the repository root, needs ./odin and wasi-sdk)
set -eu
cd "$(dirname "$0")/.."

DIST=playground/dist
mkdir -p "$DIST"
OUT="$DIST/odin.wasm" ./build_odin_wasi.sh release
./odin run playground/pack_root -- . "$DIST/odin_root.pack"
gzip -9 -f "$DIST/odin_root.pack"
cp core/sys/wasm/js/odin.js playground/web/* "$DIST/"
echo "Playground built in $DIST. Serve it with e.g.: python3 -m http.server -d $DIST 8000"
