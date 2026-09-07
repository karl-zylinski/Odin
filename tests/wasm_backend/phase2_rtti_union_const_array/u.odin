package main

import "core:fmt"

Shape :: union {
	int,
	f64,
}

main :: proc() {
	// Constant array literal containing union values with only primitive
	// (non-struct) variants. Reference prints [42, 3.5, 7].
	shapes := [3]Shape{42, 3.5, 7}
	fmt.println(shapes)
}
