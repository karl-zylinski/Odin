#!/usr/bin/env sh
# Builds the Odin compiler as a WebAssembly (WASI) module with wasi-sdk.
#
# Like build_odin_nollvm.sh the result can only generate code with
# `-backend:wasm`. It runs under a WASI runtime (`wasmtime`) or in a browser
# with a WASI shim, which is how the playground runs the compiler.
#
# Usage: WASI_SDK=/path/to/wasi-sdk ./build_odin_wasi.sh [debug|release]
set -eu

: ${WASI_SDK=$HOME/wasi-sdk}
: ${CPPFLAGS=}
: ${CXXFLAGS=}
: ${LDFLAGS=}
: ${OUT=odin.wasm}

CXX="$WASI_SDK/bin/clang++"

MODE=${1:-release}
case $MODE in
debug)   EXTRAFLAGS="-g" ;;
release) EXTRAFLAGS="${OPT:--O2}" ;;
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
# wasi-libc emulations of the process related libc pieces the compiler uses
CPPFLAGS="$CPPFLAGS -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_GETPID"

CXXFLAGS="$CXXFLAGS --target=wasm32-wasip1 -std=c++14 -fno-exceptions"
# The generated asm tables narrow u32 counts to isize, which is 32 bits here
DISABLED_WARNINGS="-Wno-switch -Wno-macro-redefined -Wno-unused-value -Wno-c++11-narrowing"
LDFLAGS="$LDFLAGS -lwasi-emulated-signal -lwasi-emulated-process-clocks -lwasi-emulated-getpid"
# The compiler keeps large arenas and deep recursion (the parser and checker)
LDFLAGS="$LDFLAGS -Wl,-z,stack-size=8388608 -Wl,--initial-memory=67108864 -Wl,--max-memory=4294967296"
if [ "$MODE" = release ]; then
	# wasi-sdk ships its libraries with DWARF, which the linker would keep:
	# 300 KB of a module whose whole point is to be downloaded
	LDFLAGS="$LDFLAGS -Wl,--strip-debug"
fi

set -x
$CXX src/main.cpp src/libtommath.cpp $DISABLED_WARNINGS $CPPFLAGS $CXXFLAGS $EXTRAFLAGS $LDFLAGS -o "$OUT"
