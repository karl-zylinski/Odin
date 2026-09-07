// Differential test: aggregates that are passed indirectly (by pointer)
// combined with every way of obtaining a pointer into the caller's local
// variable.  The wasm backend elides the argument copy when the argument is a
// plain local that nothing can point into, so each case here has to defeat
// that analysis.
//
// NOTE: what the *callee* observes when its aggregate parameter is written to
// through such an alias *during* the call is deliberately not printed: the two
// backends genuinely disagree (the LLVM backend passes the caller's variable
// itself and therefore sees the write, the wasm backend copies and does not),
// so it cannot be part of a differential comparison.  What is checked here is
// that the parameter reads correctly on entry, that the alias write reaches
// the caller's variable, and that none of these constructs miscompiles.
package main

import "core:fmt"

Big :: struct {
	a: [4]int,   // nested aggregate => Big is always passed indirectly
	b: [2]f32,
	c: int,
}

mk :: proc(c: int) -> Big {
	return Big{a = {1, 2, 3, 4}, b = {1.5, 2.5}, c = c}
}

// Only integers are printed so that float formatting can never make the two
// backends differ for reasons unrelated to argument passing.
sum :: proc(v: Big) -> int {
	s := v.c
	for e in v.a do s += e
	s += int(v.b[0] * 2)
	s += int(v.b[1] * 2)
	return s
}

sum8 :: proc(v: [8]int) -> int {
	s := 0
	for e in v do s += e
	return s
}

// ---------------------------------------------------------------- case 1
// plain `&local`
callee1 :: proc(v: Big, p: ^Big) {
	before := sum(v)
	p.c = 99
	fmt.println("case1", before, v.a[3], p.c)
}

case1 :: proc() {
	x := mk(10)
	p := &x
	callee1(x, p)
	fmt.println("case1 caller", sum(x), x.c)
}

// address taken in the call expression itself
case1b :: proc() {
	x := mk(11)
	callee1(x, &x)
	fmt.println("case1b caller", sum(x), x.c)
}

// ---------------------------------------------------------------- case 2
// `for &v in arr` yields a pointer to an element of the local array
callee2 :: proc(v: [8]int, p: ^int, i: int) {
	before := v[i]
	bsum := sum8(v)
	p^ = 100 + i
	after := v[i]
	fmt.println("case2", i, before, after, bsum, sum8(v))
}

case2 :: proc() {
	arr: [8]int = {0, 1, 2, 3, 4, 5, 6, 7}
	i := 0
	for &v in arr {
		callee2(arr, &v, i)
		i += 1
	}
	fmt.println("case2 caller", sum8(arr))
}

// ---------------------------------------------------------------- case 3
// a slice of the local array
callee3 :: proc(v: [8]int, s: []int) {
	before := v[0]
	s[0] = 7
	after := v[0]
	fmt.println("case3", before, after, sum8(v), s[0])
}

case3 :: proc() {
	arr: [8]int = {10, 11, 12, 13, 14, 15, 16, 17}
	s := arr[:]
	callee3(arr, s)
	fmt.println("case3 caller", sum8(arr))
}

// ---------------------------------------------------------------- case 4
// `any` made from a local, and a `..any` variadic
callee4 :: proc(v: Big, a: any) {
	before := sum(v)
	(^Big)(a.data).c = 5
	fmt.println("case4", before, v.a[0], (^Big)(a.data).c)
}

callee4v :: proc(v: Big, args: ..any) {
	before := sum(v)
	(^Big)(args[0].data).c = 6
	fmt.println("case4v", before, v.a[0], (^Big)(args[0].data).c, len(args))
}

case4 :: proc() {
	x := mk(20)
	a: any = x
	callee4(x, a)
	fmt.println("case4 caller", sum(x), x.c)

	y := mk(21)
	callee4v(y, y)
	fmt.println("case4v caller", sum(y), y.c)
}

// ---------------------------------------------------------------- case 5
// `using` a local struct variable, pointer to one of its fields
callee1i :: proc(v: Big, p: ^int) {
	before := sum(v)
	p^ = 99
	fmt.println("case5", before, v.a[3], p^)
}

case5 :: proc() {
	using u := mk(30)
	p := &c
	callee1i(u, p)
	fmt.println("case5 caller", sum(u), u.c, c)
}

// ---------------------------------------------------- cases 6, 7, 8 and 9
// an `any` that reached the callee through a container
callee5 :: proc(v: Big, a: any, tag: string) {
	before := sum(v)
	(^Big)(a.data).c = 77
	fmt.println(tag, before, v.a[1], (^Big)(a.data).c)
}

case6 :: proc() {  // dynamic array of any
	x := mk(40)
	arr2: [dynamic]any
	defer delete(arr2)
	append(&arr2, x)
	callee5(x, arr2[0], "case6")
	fmt.println("case6 caller", sum(x), x.c)
}

case7 :: proc() {  // map value of type any
	x := mk(50)
	m := map[string]any{}
	defer delete(m)
	m["k"] = x
	callee5(x, m["k"], "case7")
	fmt.println("case7 caller", sum(x), x.c)
}

W :: struct { v: any }

case8 :: proc() {  // struct field of type any
	x := mk(60)
	w := W{v = x}
	callee5(x, w.v, "case8")
	fmt.println("case8 caller", sum(x), x.c)
}

case9 :: proc() {  // any by declaration and by assignment
	x := mk(70)
	a: any = x
	callee5(x, a, "case9a")
	fmt.println("case9a caller", sum(x), x.c)

	y := mk(71)
	a2: any
	a2 = y
	callee5(y, a2, "case9b")
	fmt.println("case9b caller", sum(y), y.c)
}

// --------------------------------------------------------- cases 10 and 11
// `raw_data` of the local array
callee6 :: proc(v: [8]int, p: [^]int) {
	before := v[1]
	p[1] = 42
	after := v[1]
	fmt.println("case10", before, after, sum8(v), p[1])
}

case10 :: proc() {
	arr: [8]int = {20, 21, 22, 23, 24, 25, 26, 27}
	p := raw_data(arr[:])
	callee6(arr, p)
	fmt.println("case10 caller", sum8(arr))
}

// a byte view over the local struct
callee6b :: proc(v: Big, bytes: []u8) {
	before := sum(v)
	for i in 0 ..< len(bytes) do bytes[i] = 0
	fmt.println("case11", before, len(bytes), bytes[0])
}

case11 :: proc() {
	x := mk(80)
	bytes := (cast([^]u8)&x)[:size_of(Big)]
	callee6b(x, bytes)
	fmt.println("case11 caller", sum(x), x.c)
}

// ---------------------------------------------------------------- case 12
// the non-aliased happy paths: the pointer cannot point at the argument
g_big := Big{a = {9, 9, 9, 9}, b = {0.5, 0.5}, c = 1}
g :: proc() -> ^Big { return &g_big }

callee7 :: proc(v: Big, p: ^Big) {
	before := sum(v)
	p.c = 123
	after := sum(v)
	fmt.println("case12", before, after, v.c, p.c)
}

callee8 :: proc(v: Big, p: ^Big) {
	before := sum(v)
	if p != nil do p.c = 1
	after := sum(v)
	fmt.println("case12b", before, after, v.c, p == nil)
}

case12 :: proc() {
	x := mk(90)
	callee7(x, g())
	fmt.println("case12 caller", sum(x), x.c, g_big.c)

	y := mk(91)
	np: ^Big = nil
	callee8(y, np)
	fmt.println("case12b caller", sum(y), y.c)

	// arguments that are not plain local variables at all
	callee8(mk(92), nil)
	z := mk(93)
	callee8(z.c > 0 ? z : mk(94), nil)
}

// ---------------------------------------------------------------- case 15
// recursion over a local copy
rec :: proc(x: Big, depth: int) {
	fmt.println("case15", depth, sum(x), x.c)
	if depth == 0 do return
	y := x
	y.c += 1
	rec(y, depth - 1)
	fmt.println("case15 back", depth, sum(x), x.c)
}

case15 :: proc() {
	x := mk(100)
	rec(x, 3)
	fmt.println("case15 caller", sum(x), x.c)
}

// ---------------------------------------------------------------- case 16
// deferred_in with an indirectly passed argument
cleanup :: proc(b: Big) { fmt.println("case16 cleanup", sum(b), b.c) }

@(deferred_in = cleanup)
f16 :: proc(b: Big) { fmt.println("case16 enter", sum(b), b.c) }

case16 :: proc() {
	x := mk(110)
	{
		y := x
		f16(y)
		fmt.println("case16 body", sum(y), y.c)
	}
	fmt.println("case16 caller", sum(x), x.c)
}

// deferred_in where the argument is aliased by the second argument
cleanup2 :: proc(b: Big, p: ^Big) { fmt.println("case16b cleanup", p.c) }

@(deferred_in = cleanup2)
f16b :: proc(b: Big, p: ^Big) {
	before := sum(b)
	p.c = 555
	fmt.println("case16b enter", before, p.c)
}

case16b :: proc() {
	y := mk(120)
	{
		f16b(y, &y)
		fmt.println("case16b body", sum(y), y.c)
	}
	fmt.println("case16b caller", sum(y), y.c)
}

main :: proc() {
	case1()
	case1b()
	case2()
	case3()
	case4()
	case5()
	case6()
	case7()
	case8()
	case9()
	case10()
	case11()
	case12()
	case15()
	case16()
	case16b()
}
