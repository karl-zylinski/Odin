// The variable bound exceeds the slice: the checked loop runs, and traps at the end
package main

import "core:fmt"

main :: proc() {
	s := make([]int, 4)
	n := 6
	total := 0
	for i in 0..<n {
		total += s[i]
		fmt.println(i, total)
	}
}
