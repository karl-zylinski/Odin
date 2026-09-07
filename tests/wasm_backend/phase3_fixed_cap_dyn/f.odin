package main

import "core:fmt"

V :: struct { a: [dynamic; 4]int, tag: int }

g_const: [dynamic; 8]int = {1, 2, 3}
g_splat: [dynamic; 3]u8 = {7, 8}

main :: proc() {
	a: [dynamic; 8]int = {10, 20, 30}
	fmt.println(len(a), cap(a), a[0], a[1], a[2])
	x := 5
	b: [dynamic; 6]int = {x, x*2}
	fmt.println(len(b), cap(b), b[0], b[1])
	append(&b, 99)
	append(&b, 100, 101)
	fmt.println(len(b), b[2], b[3], b[4])
	fmt.println(b[:])
	fmt.println(b[1:3])
	for v, i in b {
		fmt.print(i, v, " ")
	}
	fmt.println()
	for &v in b { v += 1 }
	fmt.println(b[:])
	p := &b
	fmt.println(len(p), cap(p), p[0])
	fmt.println(raw_data(p) == &b[0], raw_data(p) != nil)
	v := pop(&b)
	fmt.println(v, len(b))
	clear(&b)
	fmt.println(len(b), cap(b))
	ok := append(&b, 1); fmt.println(ok, len(b))
	fmt.println(len(g_const), cap(g_const), g_const[:])
	fmt.println(len(g_splat), cap(g_splat), g_splat[:])
	s := V{a = {1, 2}, tag = 3}
	fmt.println(len(s.a), s.a[1], s.tag)
	append(&s.a, 3)
	fmt.println(s.a[:])
	c := s
	c.a[0] = 42
	fmt.println(s.a[0], c.a[0])
	t: [dynamic; 3]u8
	for i in 0..<5 { append(&t, u8(i)) }
	fmt.println(len(t), t[:])
	fmt.println()
	fmt.println(a)
	ps := p[:]
	ps[0] = 7
	fmt.println(b[0])
}
