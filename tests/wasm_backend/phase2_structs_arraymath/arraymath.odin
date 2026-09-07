package main

import "core:fmt"

main :: proc() {
	va := [3]f32{1, 2, 3}
	vb := [3]f32{10, 20, 30}
	vc := va + vb
	vd := va * 2
	fmt.println(vc, vd, va == [3]f32{1, 2, 3}, va == vb)
}
