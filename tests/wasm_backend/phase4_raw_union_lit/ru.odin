package main

import "core:fmt"

U :: struct #raw_union {
	u: u128,
	h: struct {
		lo, hi: u64,
	},
}

U2 :: struct #raw_union {
	f: f32,
	u: u32,
}

U3 :: struct #raw_union {
	b: [8]u8,
	u: u64,
}

S :: struct {
	name: string,
	u:    U,
	n:    int,
}

global_u: U = U{u = 42}
global_u3: U3 = U3{u = 0x0102030405060708}

make_u :: proc(v: u128) -> U {
	return U{u = v}
}

take_u :: proc(x: U) -> u128 {
	return x.u
}

main :: proc() {
	// basic raw union compound literals
	a := U{u = 5}
	fmt.println(a.u)

	b := U{h = {1, 2}}
	fmt.println(b.h.lo, b.h.hi)

	// f32 / u32 bit punning via raw union
	f1 := U2{f = 1.0}
	fmt.println(f1.u)

	f2 := U2{u = 0x3f800000}
	fmt.println(f2.f)

	// raw union nested inside a struct literal
	s := S{name = "a", u = U{u = 7}, n = 3}
	fmt.println(s.name, s.u.u, s.n)

	// raw union inside an array literal, printed in a loop
	arr := [3]U{U{u = 1}, U{h = {2, 3}}, {}}
	for i in 0 ..< 3 {
		fmt.println(arr[i].u)
	}

	// global raw union initialised with a literal
	fmt.println(global_u.u)

	// global raw union with [8]u8 aliased with u64 (little endian)
	fmt.println(global_u3.b[0], global_u3.b[1], global_u3.b[2], global_u3.b[3], global_u3.b[4], global_u3.b[5], global_u3.b[6], global_u3.b[7])
	fmt.println(global_u3.u)

	// raw union returned from a proc, assigned via proc result
	u_from_proc := make_u(99)
	fmt.println(u_from_proc.u)

	// raw union passed as a proc parameter
	fmt.println(take_u(u_from_proc))

	// assigning a whole raw union value from one variable to another
	orig := U{u = 5}
	copy_of_orig := orig
	orig = U{u = 123}
	fmt.println(orig.u, copy_of_orig.u)

	// taking a pointer into a raw union field and writing through it
	w := U{h = {1, 2}}
	p := &w.h.lo
	p^ = 999
	fmt.println(w.h.lo, w.h.hi)
}
