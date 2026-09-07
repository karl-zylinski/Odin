#+feature using-stmt
package main

import "core:fmt"

Enum_Color :: enum {
	Red,
	Green,
	Blue,
}

main :: proc() {
	// `using` an enum as a statement inside a proc, bringing its members
	// into scope unqualified. The wasm backend errors with:
	//   Error: wasm backend: unsupported statement
	using Enum_Color
	c := Green
	fmt.println("enum_using:", c)
}
