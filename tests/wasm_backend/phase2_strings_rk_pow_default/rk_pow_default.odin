package main

import "core:fmt"
import "core:strings"

// Isolates a wasm-backend bug: a named result parameter with a default
// initializer (e.g. `-> (pow: u32 = 1)`) is not initialized to that default
// on the wasm backend -- it comes back as the type's zero value instead.
//
// This is the root cause behind core:strings.index / strings.contains giving
// wrong answers on the wasm backend for any multi-byte substring search whose
// match is not at position 0: strings.index's Rabin-Karp rolling hash uses a
// `pow` value produced exactly this way (see core/strings/strings.odin,
// hash_str_rabin_karp), so on the wasm backend `pow` is silently 0 and the
// rolling hash never recovers correctly, causing real matches to be missed.

named_return_with_default :: proc() -> (pow: u32 = 1) {
	return
}

main :: proc() {
	// Root cause, minimal: should print 1, wasm backend prints 0.
	fmt.println(named_return_with_default())

	// Real-world symptom via core:strings.
	fmt.println(strings.index("hello world", "wor"))    // ref: 6
	fmt.println(strings.contains("hello world", "wor")) // ref: true
	fmt.println(strings.index("abcabcabc", "bca"))       // ref: 1
}
