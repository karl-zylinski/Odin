package main

import "base:intrinsics"
import "core:fmt"

Vector3 :: distinct [3]f32

cross :: proc(a, b: Vector3) -> Vector3 {
	i := swizzle(a, 1, 2, 0) * swizzle(b, 2, 0, 1)
	j := swizzle(a, 2, 0, 1) * swizzle(b, 1, 2, 0)
	return i - j
}

main :: proc() {
	a := [3]f32{1, 2, 3}
	b := swizzle(a, 2, 1, 0)
	fmt.println(b, b == [3]f32{3, 2, 1})

	c := swizzle(a, 0, 0)
	fmt.println(c, c == [2]f32{1, 1}, c == 1)

	// more indices than the array has elements
	fmt.println(swizzle(a, 2, 2, 2, 1, 1, 0, 0, 0))

	fmt.println(cross(Vector3{1, 0, 0}, Vector3{0, 1, 0}))

	big := [6]int{10, 20, 30, 40, 50, 60}
	fmt.println(swizzle(big, 5, 0, 3))

	Named :: distinct [4]u8
	fmt.println(swizzle(Named{1, 2, 3, 4}, 3, 2, 1, 0))

	// an aggregate element type
	strs := [3]string{"a", "bb", "ccc"}
	fmt.println(swizzle(strs, 2, 0))

	// the source is a call result, and a swizzle written back into its source
	get :: proc() -> [4]int { return {9, 8, 7, 6} }
	fmt.println(swizzle(get(), 1, 3))
	d := [3]int{1, 2, 3}
	d = swizzle(d, 2, 0, 1)
	fmt.println(d)

	v := #simd[4]i32{1, 2, 3, 4}
	w := swizzle(v, 3, 2, 1, 0)
	fmt.println(intrinsics.simd_extract(w, 0), intrinsics.simd_extract(w, 3))
}
