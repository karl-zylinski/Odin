package main

import "core:fmt"

V :: struct {
	x: int,
	y: u8,
}

main :: proc() {
	fixed := #soa[3]V{{10, 1}, {20, 2}, {30, 3}}
	fmt.println(fixed[0].x, fixed[1].x, fixed[2].y, fixed.x[2])
}
