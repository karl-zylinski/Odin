// Differential test: interval loops whose bounds checks the wasm backend
// removes (wb_bce_analyze), or replaces by a single test before the loop.
// Every loop here stays in range; the phase4_bce_trap_* packages cover the
// loops that do not.
package main

import "core:fmt"

Particles :: struct {
	x, y: []f32,
	n:    int,
	arr:  [6]int,
}

Outer :: struct {
	p:    Particles,
	name: string,
}

sum_slice :: proc(s: []int) -> (total: int) {
	for i in 0..<len(s) {
		total += s[i]
	}
	return
}

// The bounds are a variable: tested once before the loop
fill :: proc(s: []int, n: int, base: int) {
	for i in 0..<n {
		s[i] = base + i
	}
}

// Two loops over the same variable, one in range (the fast version), one
// with `n` larger than one of the slices (the checked version, without a trap
// as the body guards the access)
mixed :: proc(a, b: []int, n: int) -> (total: int) {
	for i in 0..<n {
		total += a[i]
		if i < len(b) {
			total += b[i]
		}
	}
	return
}

fields :: proc(o: ^Outer, n: int) -> (total: f32) {
	p := o.p
	for i in 0..<n {
		p.x[i] += p.y[i] * 2
		total += p.x[i]
	}
	// `p.n` as the bound
	for i in 0..<p.n {
		total += p.y[i]
	}
	// a fixed array field, constant bounds
	for i in 0..<6 {
		p.arr[i] = i * i
	}
	for i in 0..=5 {
		total += f32(p.arr[i])
	}
	// the struct itself assigned in the body: checked
	for i in 0..<len(p.x) {
		total += p.x[i]
		if i == 2 {
			p = o.p
		}
	}
	return
}

strings_and_dynamic :: proc(str: string, d: ^[dynamic]int) -> (total: int) {
	for i in 0..<len(str) {
		total += int(str[i])
	}
	arr := d^ // the header, elements shared
	for i in 0..<len(arr) {
		total += arr[i]
	}
	// appending inside the loop: `arr` is addressed, so the loop is checked
	// (and the upper bound re-evaluated every iteration)
	local := make([dynamic]int)
	defer delete(local)
	append(&local, 1, 2, 3)
	for i in 0..<len(local) {
		total += local[i] * 10
		if len(local) < 6 {
			append(&local, local[i] + 100)
		}
	}
	return
}

small_types :: proc(bytes: []u8) -> (total: int) {
	n := u8(len(bytes))
	for i in u8(0)..<n {
		total += int(bytes[i])
	}
	for i in i16(0)..<i16(len(bytes)) {
		total += int(bytes[i]) * 2
	}
	lo := 2
	for i in lo..<len(bytes) {
		total += int(bytes[i]) * 3
	}
	return
}

control_flow :: proc(s: []int, n: int) -> (total: int) {
	outer: for i in 0..<n {
		v := s[i]
		defer total += 1
		if v == 3 {
			continue
		}
		for j in 0..<n {
			if j > i {
				break
			}
			if s[j] == 7 {
				break outer
			}
			total += s[j] * v
		}
	}
	return
}

nested :: proc(rows: [][]int) -> (total: int) {
	for r in 0..<len(rows) {
		row := rows[r]
		for c in 0..<len(row) {
			total += row[c] * (r + 1)
		}
	}
	return
}

elements_written :: proc(s: []int) -> (total: int) {
	for i in 0..<len(s) {
		s[i] = s[i] * 2
		total += s[i]
	}
	return
}

negative_count :: proc(s: []int, n: int) -> (count: int) {
	for i in 0..<n {
		count += s[i]
	}
	return
}

main :: proc() {
	s := make([]int, 10)
	defer delete(s)
	fill(s, len(s), 5)
	fmt.println("sum", sum_slice(s))
	fmt.println("mixed", mixed(s, s[:4], len(s)))

	o := Outer{name = "o"}
	o.p.x = make([]f32, 8)
	o.p.y = make([]f32, 8)
	o.p.n = 8
	for i in 0..<8 {
		o.p.x[i] = f32(i)
		o.p.y[i] = f32(i) * 0.5
	}
	fmt.println("fields", fields(&o, 8))
	delete(o.p.x)
	delete(o.p.y)

	d := make([dynamic]int)
	append(&d, 4, 5, 6)
	fmt.println("strings_and_dynamic", strings_and_dynamic("hello", &d))
	delete(d)

	bytes := []u8{1, 2, 3, 4, 5}
	fmt.println("small_types", small_types(bytes))

	fmt.println("control_flow", control_flow({1, 2, 3, 4, 7, 6}, 6))

	rows := [][]int{{1, 2}, {3, 4, 5}, {}}
	fmt.println("nested", nested(rows))
	fmt.println("elements_written", elements_written(s), s[0], s[9])
	fmt.println("negative_count", negative_count(s, -3))
}
