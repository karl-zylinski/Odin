// Minimal repro: a bit_set compound literal whose element is a runtime value.
// Constant element literals (`{.A, .C}`) work; non-constant ones do not.
package main

import "core:fmt"

E :: enum {A, B, C, D}

main :: proc() {
	// constant bit_set literals: fine
	s: bit_set[E]
	s = {.A, .C}
	s -= {.A}
	s += {.D}
	fmt.println(s, card(s))

	// non-constant element: `wasm backend: unsupported compound literal type`
	e := E.B
	s += {e}
	fmt.println(s, card(s), e in s)

	n: bit_set[0 ..< 8]
	for i in 0 ..< 5 {
		n += {i * 2 % 8}
	}
	fmt.println(n, card(n))

	i := 3
	n -= {i}
	fmt.println(n, i in n)
}
