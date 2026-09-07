// Minimal repro: `==` / `!=` on non-scalar (struct / array) values fails on the
// wasm backend with "unsupported comparison of non-scalar values".
package main

import "core:fmt"

Big :: struct {
	id:   int,
	data: [16]u8,
	name: string,
}

V :: struct {
	x, y: f32,
}

main :: proc() {
	a := Big{id = 1, name = "a"}
	b := Big{id = 1, name = "a"}
	c := Big{id = 2, name = "a"}
	fmt.println(a == b, a == c, a != c)

	z := new(Big)
	defer free(z)
	fmt.println(z^ == Big{}, z^ != a)

	p := V{1, 2}
	q := V{1, 2}
	r := V{1, 3}
	fmt.println(p == q, p == r)

	arr1 := [3]int{1, 2, 3}
	arr2 := [3]int{1, 2, 3}
	arr3 := [3]int{1, 2, 4}
	fmt.println(arr1 == arr2, arr1 == arr3)
}
