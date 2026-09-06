#!/usr/bin/env bash
# Differential test for the direct wasm backend: build each test package with
# both the LLVM backend and the wasm backend and compare the results.
#
# Two kinds of test package:
#   - with a `calls.txt`: built freestanding without an entry point; every
#     line of calls.txt names an exported procedure (and arguments) that is
#     invoked with wasmtime and whose result is compared
#   - with a `main` procedure (no calls.txt): built for wasi and run; the
#     standard output of both builds is compared
#
# Requires `wasmtime` in PATH (and `wasm-ld` for the LLVM reference build).
#
# Environment (paths are relative to this directory):
#   ODIN      compiler under test               (default ../../odin)
#   ODIN_REF  compiler for the reference build  (default $ODIN; use an LLVM build
#             when ODIN is a compiler built with build_odin_nollvm.sh)
#   VERBOSE   print every result
set -u

cd "$(dirname "$0")"
ODIN=${ODIN:-../../odin}          # compiler under test (wasm backend)
ODIN_REF=${ODIN_REF:-$ODIN}      # compiler used for the LLVM reference build
OUT=${OUT:-./out}
TARGET=${TARGET:-freestanding_wasm32}
WASI_TARGET=${WASI_TARGET:-wasi_wasm32}
mkdir -p "$OUT"

failures=0
total=0

check() {
	local name=$1; shift
	local expected=$1; shift
	local actual=$1; shift
	total=$((total+1))
	if [ -z "$expected" ]; then
		echo "FAIL $name: reference produced no output"
		failures=$((failures+1))
	elif [ "$expected" != "$actual" ]; then
		echo "FAIL $name: expected '$expected', got '$actual'"
		failures=$((failures+1))
	fi
}

invoke() {
	wasmtime run --invoke "$2" "$1" "${@:3}" 2>/dev/null
}

for pkg in */; do
	pkg=${pkg%/}
	[ "$pkg" = "$(basename "$OUT")" ] && continue
	ls "$pkg"/*.odin >/dev/null 2>&1 || continue

	if [ ! -f "$pkg/calls.txt" ]; then
		# stdout comparison of a wasi program
		ref="$OUT/${pkg}_llvm.wasm"
		new="$OUT/${pkg}_wasm.wasm"
		if ! "$ODIN_REF" build "$pkg" -target:"$WASI_TARGET" -out:"$ref"; then
			echo "FAIL $pkg: LLVM reference build failed"; failures=$((failures+1)); total=$((total+1)); continue
		fi
		if ! "$ODIN" build "$pkg" -target:"$WASI_TARGET" -backend:wasm -out:"$new"; then
			echo "FAIL $pkg: wasm backend build failed"; failures=$((failures+1)); total=$((total+1)); continue
		fi
		expected=$(wasmtime run "$ref" 2>&1)
		actual=$(wasmtime run "$new" 2>&1)
		total=$((total+1))
		if [ "$expected" != "$actual" ]; then
			echo "FAIL $pkg: output differs"
			diff <(echo "$expected") <(echo "$actual") | head -20
			failures=$((failures+1))
		elif [ -n "${VERBOSE:-}" ]; then
			echo "$pkg => ok"
		fi
		continue
	fi

	ref="$OUT/${pkg}_llvm.wasm"
	new="$OUT/${pkg}_wasm.wasm"
	if ! "$ODIN_REF" build "$pkg" -target:"$TARGET" -no-entry-point -out:"$ref"; then
		echo "FAIL $pkg: LLVM reference build failed"; failures=$((failures+1)); continue
	fi
	if ! "$ODIN" build "$pkg" -target:"$TARGET" -no-entry-point -backend:wasm -out:"$new"; then
		echo "FAIL $pkg: wasm backend build failed"; failures=$((failures+1)); continue
	fi

	while read -r line; do
		[ -z "$line" ] && continue
		case "$line" in \#*) continue;; esac
		# shellcheck disable=SC2086
		set -- $line
		expected=$(invoke "$ref" "$@")
		actual=$(invoke "$new" "$@")
		[ -n "${VERBOSE:-}" ] && echo "$pkg/$line => $actual"
		check "$pkg/$line" "$expected" "$actual"
	done < "$pkg/calls.txt"
done

echo "$((total-failures))/$total passed"
[ "$failures" -eq 0 ]
