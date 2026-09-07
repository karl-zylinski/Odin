#+feature dynamic-literals
package sp

import "core:fmt"
import "base:intrinsics"
import "base:runtime"

main :: proc() {
	h := intrinsics.type_hasher_proc(string)
	for s in ([]string{"A", "B", "C", "hello"}) {
		s := s
		fmt.println(s, h(&s, 0))
	}
	hi := intrinsics.type_hasher_proc(int)
	for i in ([]int{1, 2, 3, 100}) {
		i := i
		fmt.println(i, hi(&i, 0))
	}
	m := map[string]int{"A" = 1, "C" = 9, "B" = 4}
	fmt.println(len(m), cap(m))
	// iteration order is seeded by the map data's address, so look keys up instead
	for k in ([]string{"A", "B", "C"}) { fmt.println(k, m[k]) }
	mi := map[int]int{1 = 1, 3 = 9, 2 = 4}
	for k in 1..=3 { fmt.println(k, mi[k]) }
	fmt.println(runtime.map_seed_from_map_data({}))
}
