// The slice is replaced through a pointer while the loop runs
package main

import "core:fmt"

main :: proc() {
	s := make([]int, 6)
	q := &s
	for i in 0..<6 {
		fmt.println(i, s[i])
		if i == 1 {
			q^ = s[:3]
		}
	}
}
