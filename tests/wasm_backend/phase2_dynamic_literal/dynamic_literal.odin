#+feature dynamic-literals
package main

import "core:fmt"

Point :: struct {
	x, y: int,
}

main :: proc() {
	a := [dynamic]int{1, 2, 3}
	fmt.println(a, len(a), cap(a) >= 3)

	i := 2
	b := [dynamic]int {
		0 = 123,
		5..=9 = 54,
		10..<16 = i*3 + (i-1)*2,
	}
	fmt.println(len(b), b)

	c := [dynamic]string{"hello", "world"}
	fmt.println(c)

	d := [dynamic]Point{{1, 2}, {3, 4}}
	fmt.println(d)

	e := [dynamic][2]f32{{1, 2}, {3, 4}, {5, 6}}
	fmt.println(e)

	// appending to a literal keeps working
	f := [dynamic]int{7}
	append(&f, 8, 9)
	fmt.println(f, len(f))

	delete(a)
	delete(b)
	delete(c)
	delete(d)
	delete(e)
	delete(f)
}
