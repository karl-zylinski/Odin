package main

import "core:fmt"

Vec4 :: [4]f32

main :: proc() {
	v4 := Vec4{1, 2, 3, 4}
	xy := v4.xy
	xyz := v4.xyz
	zyx := v4.zyx
	fmt.println(xy, xyz, zyx)
}
