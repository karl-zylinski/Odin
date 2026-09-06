// Phase 1 test program for the direct wasm backend: aggregates, memory,
// globals, control flow, defers, multiple results and indirect calls.
package phase1

import "base:runtime"

Vec2 :: struct {
	x, y: f32,
}

Particle :: struct {
	pos, vel: Vec2,
	life:     i32,
	tags:     [4]u8,
}

Color :: enum u8 {
	Red,
	Green,
	Blue,
}

vec_add :: proc "contextless" (a, b: Vec2) -> Vec2 {
	return {a.x + b.x, a.y + b.y}
}

vec_dot :: proc "contextless" (a, b: Vec2) -> f32 {
	return a.x*b.x + a.y*b.y
}

@(export)
struct_basics :: proc "c" (x: f32, y: f32) -> f32 {
	a := Vec2{x, y}
	b := Vec2{y = 2, x = 1}
	c := vec_add(a, b)
	c.x *= 2
	return vec_dot(c, a) + c.y
}

@(export)
nested_struct :: proc "c" (n: i32) -> i32 {
	p: Particle
	p.pos = {1, 2}
	p.vel.x = f32(n)
	p.life = n * 2
	p.tags[1] = 7
	p.tags[3] = u8(n)
	q := p
	q.life += 1
	q.tags[1] = 9
	return p.life + i32(p.tags[1]) + i32(q.tags[1]) + i32(q.tags[3]) + q.life + i32(p.vel.x + q.pos.y)
}

@(export)
array_sum :: proc "c" (n: i32) -> i32 {
	arr: [8]i32
	for i in 0..<len(arr) {
		arr[i] = i32(i) * n
	}
	total: i32
	for v in arr {
		total += v
	}
	#reverse for v, i in arr {
		total += v - i32(i)
	}
	return total
}

@(export)
array_by_reference :: proc "c" (n: i32) -> i32 {
	arr := [5]i32{1, 2, 3, 4, 5}
	for &v in arr {
		v *= n
	}
	arr[2] += 100
	sum: i32
	for v in arr {
		sum += v
	}
	return sum
}

sum_slice :: proc "contextless" (s: []i32) -> (total: i32) {
	for v in s {
		total += v
	}
	return
}

@(export)
slices :: proc "c" (n: i32) -> i32 {
	arr: [10]i32
	for i in 0..<10 {
		arr[i] = i32(i) + n
	}
	s := arr[:]
	t := arr[2:5]
	u := s[7:]
	t[0] = 1000
	return sum_slice(s) + sum_slice(t) + sum_slice(u) + i32(len(t)) * 10 + i32(len(u))
}

@(export)
slice_literal :: proc "c" (n: i32) -> i32 {
	s := []i32{n, 2, 3, n * 2}
	r: i32
	for v, i in s {
		r += v * i32(i + 1)
	}
	return r + i32(len(s))
}

greeting := "Hello, World!"
unicode := "héllo, wörld ✓"

@(export)
string_bytes :: proc "c" (i: i32) -> i32 {
	s := greeting
	return i32(s[i]) + i32(len(s)) + i32(len(unicode))
}

@(export)
string_runes :: proc "c" (which: i32) -> i32 {
	s := which == 0 ? greeting : unicode
	count: i32
	sum: i32
	for r, i in s {
		count += 1
		sum += i32(r) + i32(i)
	}
	#reverse for r, i in s {
		sum -= i32(r) * 2 - i32(i)
	}
	return count * 1000 + sum
}

@(export)
substring :: proc "c" (lo: i32, hi: i32) -> i32 {
	s := greeting[lo:hi]
	total: i32
	for i in 0..<len(s) {
		total += i32(s[i])
	}
	return total + i32(len(s))
}

counter: i32
table := [5]i32{10, 20, 30, 40, 50}
origin := Vec2{3, 4}
computed := compute_global() // initialized by the start function (not run by the LLVM reference without an entry point)
colors := [Color]i32{.Red = 1, .Green = 2, .Blue = 4}

compute_global :: proc "contextless" () -> i32 {
	return sum_slice(table[:]) * 2
}

@(export)
globals :: proc "c" (n: i32) -> i32 {
	counter += n
	table[1] += counter
	origin.x += 1
	return counter + table[1] + i32(origin.x + origin.y) + colors[.Blue]
}

@(export)
static_local :: proc "c" (n: i32) -> i32 {
	@(static) calls: i32
	@(static) history: [4]i32
	calls += 1
	history[calls % 4] += n
	return calls * 100 + history[calls % 4]
}

@(export)
switch_stmt :: proc "c" (n: i32) -> i32 {
	r: i32
	switch n {
	case 0:
		r = 10
	case 1, 2:
		r = 20
		fallthrough
	case 3:
		r += 30
	case 4..<8:
		r = 40
	case 8..=10:
		r = 50
	case:
		r = 99
	}

	switch {
	case n < 0:
		r += 1000
	case n > 100:
		r += 2000
		break
	}

	c := Color(n % 3)
	#partial switch c {
	case .Green: r += 5
	case .Blue:  r += 6
	}
	return r
}

@(export)
labelled_break :: proc "c" (n: i32) -> i32 {
	found: i32 = -1
	outer: for i in 0..<n {
		for j in 0..<n {
			if i * j == 12 {
				found = i * 100 + j
				break outer
			}
			if j > i {
				continue outer
			}
		}
	}
	return found
}

defer_log: i32

@(export)
defers :: proc "c" (n: i32) -> i32 {
	defer_log = 0
	r := defer_inner(n)
	return r * 1000 + defer_log
}

defer_inner :: proc "contextless" (n: i32) -> i32 {
	x := n
	defer defer_log = defer_log * 10 + 1
	{
		defer defer_log = defer_log * 10 + 2
		x += 1
	}
	for i in 0..<3 {
		defer defer_log = defer_log * 10 + 3
		if i == 1 {
			continue
		}
		x += 1
	}
	defer x += 100 // must not affect the returned value
	if n > 5 {
		defer defer_log = defer_log * 10 + 4
		return x
	}
	return x * 2
}

divmod :: proc "contextless" (a, b: i32) -> (q, r: i32) {
	q = a / b
	r = a % b
	return
}

minmax :: proc "contextless" (s: []i32) -> (lo, hi: i32, ok: bool) {
	if len(s) == 0 {
		return
	}
	lo, hi = s[0], s[0]
	for v in s {
		lo = min(lo, v)
		hi = max(hi, v)
	}
	return lo, hi, true
}

@(export)
multiple_results :: proc "c" (a: i32, b: i32) -> i32 {
	q, r := divmod(a, b)
	arr := [?]i32{a, b, a - b, a + b, 3}
	lo, hi, ok := minmax(arr[:])
	_, _, ok2 := minmax(arr[:0])
	x, y := divmod(hi, 3)
	return q * 1000 + r * 100 + lo + hi + (ok ? 7 : 0) + (ok2 ? 1 : 0) + x + y
}

Op :: proc "contextless" (a, b: i32) -> i32

op_add :: proc "contextless" (a, b: i32) -> i32 { return a + b }
op_mul :: proc "contextless" (a, b: i32) -> i32 { return a * b }

apply :: proc "contextless" (op: Op, a, b: i32) -> i32 {
	return op(a, b)
}

ops := [2]Op{op_add, op_mul}

@(export)
indirect_calls :: proc "c" (a: i32, b: i32) -> i32 {
	op: Op = a > b ? op_add : op_mul
	r := apply(op, a, b)
	r += apply(proc "contextless" (a, b: i32) -> i32 { return a - b }, a, b)
	for f in ops {
		r += f(a, b)
	}
	if op != nil {
		r += 1
	}
	return r
}

increment :: proc "contextless" (p: ^i32, by: i32 = 1) {
	p^ += by
}

@(export)
pointers :: proc "c" (n: i32) -> i32 {
	x := n
	increment(&x)
	increment(&x, by = 10)
	p := &x
	p^ *= 2
	v := Vec2{1, 2}
	pv := &v
	pv.x += f32(n)
	pv^.y += 3
	arr: [4]i32
	pa := &arr[2]
	pa^ = x
	ptr := &arr
	ptr[0] = 5
	return x + arr[2] + arr[0] + i32(v.x + v.y)
}

@(export)
swap_and_multi_assign :: proc "c" (a: i32, b: i32) -> i32 {
	x, y := a, b
	x, y = y, x
	arr := [3]i32{1, 2, 3}
	arr[0], arr[2] = arr[2], arr[0]
	i := 0
	i, arr[i] = 2, 100 // rhs and lhs addresses evaluated before assignment
	return x * 100 + y + arr[0] * 10 + arr[2] + i32(i) + arr[1]
}

@(export)
builtins :: proc "c" (a: i32, b: i32) -> i32 {
	r := min(a, b, 3) + max(a, b) * 2 + abs(a - b) + clamp(a, -5, 5)
	f := clamp(f32(a), 0.5, 2.5) + min(f32(a), f32(b)) + abs(f32(b))
	return r + i32(f * 10)
}

Config :: struct {
	width, height: i32,
	scale:         f32,
	name:          string,
	color:         Color,
}

make_config :: proc "contextless" (width: i32, height: i32 = 100, scale: f32 = 1.5, color := Color.Green) -> Config {
	return {width = width, height = height, scale = scale, color = color, name = "cfg"}
}

@(export)
named_args :: proc "c" (n: i32) -> i32 {
	a := make_config(n)
	b := make_config(height = n, width = 2, color = .Blue)
	c := make_config(n, 7, 3)
	return a.width + a.height + i32(a.scale * 2) + i32(a.color) +
	       b.width * 10 + b.height + i32(b.color) * 100 +
	       c.height + i32(c.scale) + i32(len(c.name))
}

@(export)
inclusive_range :: proc "c" (n: i32) -> i32 {
	sum: i32
	for i in 0..=n {
		sum += i
	}
	for i in i8(120)..=i8(127) { // must terminate at the maximum value
		sum += 1
	}
	for c in 'a'..='e' {
		sum += i32(c - 'a')
	}
	return sum
}

@(export)
array_of_structs :: proc "c" (n: i32) -> i32 {
	ps: [3]Particle
	for &p, i in ps {
		p.pos = {f32(i), f32(n)}
		p.life = i32(i) * n
	}
	total: i32
	for p in ps {
		total += p.life + i32(p.pos.x + p.pos.y)
	}
	ps[1].pos.x = 50
	return total + i32(ps[1].pos.x)
}

@(export)
runtime_call :: proc "c" (which: i32) -> i32 {
	s := which == 0 ? greeting : unicode
	r, w := runtime.string_decode_rune(s[1:])
	return i32(r) * 10 + i32(w)
}

@(export)
ternary_and_when :: proc "c" (n: i32) -> i32 {
	v := n > 5 ? Vec2{1, 2} : Vec2{3, 4}
	c := 1 when ODIN_ARCH == .wasm32 else 2
	return i32(v.x + v.y) + i32(c)
}

// Evaluation order and value semantics

side_effect_target: i32

bump :: proc "contextless" (by: i32) -> i32 {
	side_effect_target += by
	return side_effect_target
}

pair_sum :: proc "contextless" (a, b: i32) -> i32 {
	return a * 10 + b
}

@(export)
eval_order :: proc "c" (n: i32) -> i32 {
	side_effect_target = n
	x := side_effect_target
	r := pair_sum(x, bump(1))            // x is read before the call
	r += pair_sum(side_effect_target, bump(1)) // the variable is read before the call
	r += side_effect_target + bump(5)     // left operand read first
	return r
}

modify_copy :: proc "contextless" (v: Vec2, arr: [3]i32) -> i32 {
	v := v
	arr := arr
	v.x = 100
	arr[0] = 100
	return i32(v.x) + arr[0]
}

@(export)
value_semantics :: proc "c" (n: i32) -> i32 {
	v := Vec2{f32(n), 1}
	arr := [3]i32{n, 2, 3}
	r := modify_copy(v, arr)
	arr2 := arr
	arr2[1] = 50
	return r + i32(v.x) + arr[0] + arr[1] + arr2[1]
}

@(export)
defer_named_results :: proc "c" (n: i32) -> i32 {
	return defer_named_inner(n) * 10 + defer_named_inner2(n)
}

defer_named_inner :: proc "contextless" (n: i32) -> (r: i32) {
	defer r += 100 // does not change the returned value
	r = n
	return r + 1
}

defer_named_inner2 :: proc "contextless" (n: i32) -> (r: i32) {
	defer r += 100
	r = n
	return // returns n
}

@(export)
tuple_assign :: proc "c" (a: i32, b: i32) -> i32 {
	q, r: i32
	q, r = divmod(a, b)
	lo, hi: i32
	ok: bool
	arr := [3]i32{b, a, a - b}
	lo, hi, ok = minmax(arr[:])
	return q * 1000 + r * 100 + lo + hi + (ok ? 1 : 0)
}

@(export)
reverse_ranges :: proc "c" (n: i32) -> i32 {
	arr := [4]i32{1, 2, 3, 4}
	r: i32
	#reverse for v, i in arr[:] {
		r = r * 10 + v + i32(i) * n
	}
	p := &arr
	for v, i in p {
		r += v * i32(i)
	}
	for &v in p {
		v += 1
	}
	return r + arr[3]
}

Make_Vec :: proc "contextless" (x, y: f32) -> Vec2

make_vec :: proc "contextless" (x, y: f32) -> Vec2 {
	return {x, y}
}

@(export)
indirect_aggregate :: proc "c" (n: i32) -> i32 {
	f: Make_Vec = make_vec
	v := f(f32(n), 2)
	fs := [2]Make_Vec{make_vec, proc "contextless" (x, y: f32) -> Vec2 { return {y, x} }}
	w := fs[1](f32(n), 3)
	return i32(v.x * 10 + v.y + w.x * 100 + w.y)
}

Node :: struct {
	value: i32,
	next:  ^Node,
}

@(export)
linked_list :: proc "c" (n: i32) -> i32 {
	nodes: [8]Node
	head: ^Node
	for i in 0..<8 {
		nodes[i].value = i32(i) * n
		nodes[i].next = head
		head = &nodes[i]
	}
	sum: i32
	for it := head; it != nil; it = it.next {
		sum = sum * 2 + it.value
	}
	return sum
}

@(export)
narrow_arithmetic :: proc "c" (a: i32, b: i32) -> i32 {
	x := u8(a)
	y := i8(b)
	x += 200
	y -= 100
	z := u16(x) * 300
	w := i16(y) * 300
	r := i32(x) + i32(y) * 1000 + i32(z) + i32(w) * 2
	if x > 100 { r += 1 }
	if y < 0   { r += 2 }
	if u8(a) < u8(b) { r += 4 }
	return r
}

@(export)
wide_arithmetic :: proc "c" (a: i64, b: i64) -> i64 {
	x := a * b + (a << 3) - (b >> 1)
	y := f64(a) / 3.0 + f64(b) * 0.5
	u := u64(a) / 3
	return x + i64(y) + i64(u) + i64(a % 7 == 0 ? 1 : 0)
}

Shape :: enum { Circle, Square, Triangle }

describe :: proc "contextless" (s: Shape) -> i32 {
	switch s {
	case .Circle:   return 1
	case .Square:   return 4
	case .Triangle: return 3
	}
	return -1
}

@(export)
enum_switch :: proc "c" (n: i32) -> i32 {
	shapes := [3]Shape{.Circle, .Square, .Triangle}
	r: i32
	for s in shapes {
		r = r * 10 + describe(s)
	}
	r += describe(Shape(n % 3)) * 1000
	if shapes[0] == .Circle && shapes[1] != .Circle {
		r += 1
	}
	return r
}

@(export)
nested_defers_and_loops :: proc "c" (n: i32) -> i32 {
	log: i32
	for i in 0..<n {
		defer log = log * 10 + 1
		for j in 0..<3 {
			defer log = log * 10 + 2
			if j == 1 {
				break
			}
			if i == 1 {
				continue
			}
			log = log * 10 + 3
		}
		if i == 2 {
			break
		}
	}
	return log
}

@(export)
recursive_aggregates :: proc "c" (n: i32) -> i32 {
	v := sum_vec_recursive({f32(n), 1}, n)
	return i32(v.x + v.y)
}

sum_vec_recursive :: proc "contextless" (v: Vec2, depth: i32) -> Vec2 {
	if depth == 0 {
		return v
	}
	local := [4]f32{v.x, v.y, 1, 2}
	r := sum_vec_recursive({local[0] + local[2], local[1] + local[3]}, depth - 1)
	return {r.x + local[2], r.y}
}
