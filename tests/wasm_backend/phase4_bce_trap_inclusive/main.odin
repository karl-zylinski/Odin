// `0..=len(s)` reaches one past the end
package main

import "core:fmt"

main :: proc() {
	s := make([]int, 4)
	for i in 0..=len(s) {
		fmt.println(i, s[i])
	}
}
