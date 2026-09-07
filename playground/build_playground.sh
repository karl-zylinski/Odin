#!/usr/bin/env sh
# Builds the Karl2D playground into playground/dist: the compiler as a WASI
# module, the packed ODIN_ROOT sources, the Karl2D examples, the Odin and
# Karl2D JS runtimes and the web pages.
#
# Usage: ./playground/build_playground.sh [karl2d dir]
#   (run from the repository root, needs ./odin and wasi-sdk; the Karl2D
#   checkout defaults to ../karl2d. Set ODIN_WASM=path to reuse an already
#   built compiler module instead of rebuilding it.)
set -eu
cd "$(dirname "$0")/.."

KARL2D=${1:-../karl2d}
DIST=playground/dist
mkdir -p "$DIST"
rm -rf "$DIST/examples"
if [ -n "${ODIN_WASM:-}" ]; then
	cp "$ODIN_WASM" "$DIST/odin.wasm"
else
	OUT="$DIST/odin.wasm" ./build_odin_wasi.sh release
fi
./odin run playground/pack_root -- . "$DIST/odin_root.pack" "$KARL2D" "$DIST/examples"
gzip -9 -f "$DIST/odin_root.pack"
cp core/sys/wasm/js/odin.js playground/web/* "$DIST/"
cp "$KARL2D/audio_backend_web_audio.js" "$KARL2D/audio_backend_web_audio_processor.js" "$DIST/"
cp "$KARL2D/build_web/web_entry_templates/web_entry_template.odin" "$DIST/web_entry.odin"
echo "Playground built in $DIST. Serve it with e.g.: python3 -m http.server -d $DIST 8000"
