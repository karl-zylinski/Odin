package main

import "core:fmt"

main :: proc() {
	// A matrix[2,2]f32 compound literal. The wasm backend errors with:
	//   Error: wasm backend: unsupported compound literal type
	m1 := matrix[2, 2]f32{
		1, 2,
		3, 4,
	}
	m2 := matrix[2, 2]f32{
		5, 6,
		7, 8,
	}
	m3 := m1 * m2
	fmt.println("matrix mul:", m3)
}
