// A negative variable lower bound
package main

import "core:fmt"

main :: proc() {
	s := make([]int, 4)
	lo := -1
	for i in lo..<len(s) {
		fmt.println(i, s[i])
	}
}
