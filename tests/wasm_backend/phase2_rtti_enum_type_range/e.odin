package main

import "core:fmt"

Color :: enum {
	Red,
	Green,
	Blue = 10,
}

main :: proc() {
	// Ranging directly over an enum type. Reference prints each enum value.
	// The wasm backend errors with:
	//   Error: wasm backend: unsupported range over a type
	for c in Color {
		fmt.println("color:", c, int(c))
	}
}
