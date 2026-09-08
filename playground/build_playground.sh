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

# The playground ships Karl2D's sources, so it is built from origin/master and
# cannot quietly fall behind. A checkout that is dirty, on another branch or
# diverged is packed as it is (someone may be trying a change in the
# playground on purpose) but says so. KARL2D_NO_UPDATE=1 skips the fetch.
if [ -d "$KARL2D/.git" ]; then
	branch=$(git -C "$KARL2D" symbolic-ref --quiet --short HEAD || echo "(detached)")
	if [ -n "${KARL2D_NO_UPDATE:-}" ]; then
		echo "NOTE: not updating $KARL2D (KARL2D_NO_UPDATE is set)"
	elif [ -n "$(git -C "$KARL2D" status --porcelain)" ]; then
		echo "WARNING: $KARL2D has local changes: packing it as it is, not origin/master"
	elif [ "$branch" != master ]; then
		echo "WARNING: $KARL2D is on '$branch': packing it as it is, not origin/master"
	elif ! git -C "$KARL2D" fetch -q origin; then
		echo "WARNING: cannot reach $KARL2D's origin: packing what is checked out"
	elif ! git -C "$KARL2D" merge --ff-only -q origin/master; then
		echo "WARNING: $KARL2D cannot fast-forward to origin/master: packing it as it is"
	fi
	echo "Karl2D: $(git -C "$KARL2D" log --oneline -1)"
fi

rm -rf "$DIST/examples"
if [ -n "${ODIN_WASM:-}" ]; then
	cp "$ODIN_WASM" "$DIST/odin.wasm"
else
	OUT="$DIST/odin.wasm" ./build_odin_wasi.sh release
fi
rm -rf "$DIST/packs"
rm -f "$DIST/odin_root.pack" "$DIST/odin_root.pack.gz" # the one pack of every source, from before
./odin run playground/pack_root -- . "$DIST/packs" "$KARL2D" "$DIST/examples"
# One gzipped pack per package: the worker fetches the ones a program imports
# -n leaves out the timestamp, so that a pack whose sources did not change
# stays byte identical: caches (and the site's git history) are spared
find "$DIST/packs" -name '*.pack' -exec gzip -9 -n -f {} +
cp core/sys/wasm/js/odin.js playground/web/* "$DIST/"
cp "$KARL2D/audio_backend_web_audio.js" "$KARL2D/audio_backend_web_audio_processor.js" "$DIST/"
cp "$KARL2D/build_web/web_entry_templates/web_entry_template.odin" "$DIST/web_entry.odin"
echo "Playground built in $DIST. Serve it with e.g.: python3 -m http.server -d $DIST 8000"
