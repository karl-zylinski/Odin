#+feature dynamic-literals
package main

import "core:fmt"

Key :: struct { a: int, b: u8 }
U :: union { int, string }

main :: proc() {
	m: map[string]int
	defer delete(m)
	m["one"] = 1
	m["two"] = 2
	m["three"] = 3
	m["two"] += 40
	fmt.println(len(m), m["one"], m["two"], m["three"], m["missing"])
	v, ok := m["three"]
	fmt.println(v, ok)
	v2, ok2 := m["nope"]
	fmt.println(v2, ok2)
	fmt.println("one" in m, "nope" in m, "nope" not_in m)
	delete_key(&m, "one")
	fmt.println(len(m), "one" in m)
	if p := &m["two"]; p != nil {
		p^ = 7
	}
	fmt.println(m["two"])
	sum := 0
	for k, val in m {
		sum += val + len(k)
	}
	fmt.println("sum", sum)
	for k, &val in m {
		val *= 2
	}
	fmt.println(m["two"], m["three"])
	clear(&m)
	fmt.println(len(m), m["two"])

	// literal, int keys, struct values
	m2 := map[int][2]f32{1 = {1.5, 2.5}, 2 = {3, 4}}
	defer delete(m2)
	fmt.println(m2[1], m2[2], m2[3])
	for i in 0..<100 {
		m2[i*7] = {f32(i), f32(-i)}
	}
	fmt.println(len(m2), cap(m2) >= 100, m2[0], m2[693], m2[7])
	total: f32
	for _, val in m2 {
		total += val[0] - val[1]
	}
	fmt.println(total)

	// struct keys, union keys, float keys
	m3: map[Key]string
	defer delete(m3)
	m3[{1, 2}] = "x"
	m3[{3, 4}] = "y"
	fmt.println(m3[{1, 2}], m3[{3, 4}], m3[{1, 3}] == "", Key{3, 4} in m3)

	m4: map[U]int
	defer delete(m4)
	m4[3] = 1
	m4["s"] = 2
	m4[4] = 3
	fmt.println(m4[3], m4["s"], m4[4], m4["t"], len(m4))

	m5: map[f64]int
	defer delete(m5)
	m5[1.5] = 1
	m5[2.5] = 2
	fmt.println(m5[1.5], m5[2.5], m5[3.5])

	// map through pointer
	pm := &m
	pm["p"] = 9
	fmt.println(pm["p"], len(pm^), "p" in pm)

	// nested: map of maps values via struct
	S :: struct { inner: map[string]int }
	s: S
	s.inner["k"] = 5
	s.inner["k"] += 1
	fmt.println(s.inner["k"], m == nil, s.inner != nil)
	delete(s.inner)

	// array keys
	m6: map[[3]u8]int
	defer delete(m6)
	m6[{1, 2, 3}] = 1
	m6[{1, 2, 4}] = 2
	fmt.println(m6[{1, 2, 3}], m6[{1, 2, 4}], m6[{0, 0, 0}])
	fmt.println(m6)
}
