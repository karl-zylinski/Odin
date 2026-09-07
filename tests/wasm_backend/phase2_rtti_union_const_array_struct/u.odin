package main

import "core:fmt"

Point :: struct {
	x: int,
	y: int,
}

Shape :: union {
	int,
	Point,
}

main :: proc() {
	// Constant array literal containing a union value whose active variant
	// is a struct type. This crashes the wasm backend with:
	//   src/wasm_backend.cpp(1270): Panic: wasm backend: cannot serialize constant of type Shape
	shapes := [2]Shape{42, Point{1, 2}}
	fmt.println(shapes)
}
