// Minimal repro: the `make(map[K]V)` builtin fails on the wasm backend, even
// though plain `m: map[K]V` declarations (see phase2_maps) work fine.
package main

import "core:fmt"

main :: proc() {
	m := make(map[string]int)
	defer delete(m)
	m["a"] = 1
	m["b"] = 2
	fmt.println(len(m), m["a"], m["b"])

	m2 := make(map[int]int, 64)
	defer delete(m2)
	for i in 0 ..< 50 {
		m2[i] = i * 2
	}
	total := 0
	for _, v in m2 {
		total += v
	}
	fmt.println(len(m2), m2[7], total, cap(m2) >= 50)
}
