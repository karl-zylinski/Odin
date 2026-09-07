// The struct holding the slice is reassigned in the body
package main

import "core:fmt"

P :: struct {
	x: []int,
	k: int,
}

main :: proc() {
	p := P{x = make([]int, 6)}
	short := P{x = p.x[:3]}
	for i in 0..<6 {
		fmt.println(i, p.x[i])
		if i == 2 {
			p.x = short.x
		}
	}
}
