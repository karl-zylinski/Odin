// The bound variable grows in the body: it is re-evaluated every iteration
package main

import "core:fmt"

main :: proc() {
	s := make([]int, 4)
	n := 3
	for i in 0..<n {
		fmt.println(i, s[i])
		if i == 1 {
			n = 8
		}
	}
}
