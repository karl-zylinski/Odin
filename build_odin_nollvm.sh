#!/usr/bin/env sh
# Builds the Odin compiler without LLVM.
#
# The resulting compiler can only generate code with `-backend:wasm` (the
# direct WebAssembly backend); all other build commands still work (check,
# doc, ...). This is the configuration that will be compiled to WebAssembly
# itself with wasi-sdk so the compiler can run in a browser.
#
# Usage: ./build_odin_nollvm.sh [debug|release]   (default: release)
set -eu

: ${CXX=clang++}
: ${CPPFLAGS=}
: ${CXXFLAGS=}
: ${LDFLAGS=}
: ${OUT=odin-nollvm}

MODE=${1:-release}
case $MODE in
debug)   EXTRAFLAGS="-g" ;;
release) EXTRAFLAGS="-O2" ;;
*)       echo "ERROR: Build mode \"$MODE\" unsupported!"; exit 1 ;;
esac

if [ -d ".git" ] && [ -n "$(command -v git)" ]; then
	gitnosig="-c log.showSignature=false"
	GIT_SHA=$(git $gitnosig show --pretty='%h' --no-patch --no-notes HEAD)
	GIT_DATE=$(git $gitnosig show "--pretty=%cd" "--date=format:%Y-%m" --no-patch --no-notes HEAD)
	CPPFLAGS="$CPPFLAGS -DGIT_SHA=\"$GIT_SHA\""
else
	GIT_DATE=$(date +"%Y-%m")
fi
CPPFLAGS="$CPPFLAGS -DODIN_VERSION_RAW=\"dev-$GIT_DATE\" -DODIN_NO_LLVM"

CXXFLAGS="$CXXFLAGS -std=c++14"
DISABLED_WARNINGS="-Wno-switch -Wno-macro-redefined -Wno-unused-value"
LDFLAGS="$LDFLAGS -pthread -lm -lstdc++"
case "$(uname -s)" in
Linux) LDFLAGS="$LDFLAGS -ldl" ;;
esac

set -x
$CXX src/main.cpp src/libtommath.cpp $DISABLED_WARNINGS $CPPFLAGS $CXXFLAGS $EXTRAFLAGS $LDFLAGS -o "$OUT"
