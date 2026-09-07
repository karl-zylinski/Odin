package main

import "core:fmt"

main :: proc() {
	// Arithmetic (+, *) on quaternion128 values
	q1: quaternion128 = quaternion(w = 1, x = 2, y = 3, z = 4)
	q2: quaternion128 = quaternion(w = 2, x = 0, y = 1, z = -1)
	fmt.println("q add:", q1 + q2)
	fmt.println("q mul:", q1 * q2)
}
