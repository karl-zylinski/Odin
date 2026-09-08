// Differential test: the value level rewrites of the wasm backend optimizer
// (src/wasm_backend_opt.cpp).  Everything here is observable through printed
// values only, so a wrong rewrite shows up as a different number:
//   - loads from constant aggregate data folded to constants (arrays,
//     matrices, structs with narrow/negative fields, enumerated arrays,
//     strings, enums), including local copies that are then mutated,
//   - stores to a local that is stored again before being read (removed only
//     when no branch can read the first value, and never when the value
//     comes from a call with side effects),
//   - reads of a local assigned once from a constant replaced by that
//     constant,
//   - the float identities (`x*1`, `x/1`, `x-0`, `x*-1`, `-const`, ...) and
//     the non identities (`x*0`, `x+0`) that must survive for signed zeroes.
package main

import "core:fmt"
import "core:math"
import "core:math/linalg"

// ------------------------------------------- 1. loads from constant data

Color :: enum u8 {
	Red,
	Green,
	Blue,
}

Mixed :: struct {
	i:  i8,
	u:  u16,
	w:  i32,
	q:  i64,
	f:  f32,
	d:  f64,
	ok: bool,
}

CONST_INTS :: [3]int{10, -20, 30}
CONST_F32S :: [4]f32{1.5, -2.25, 0, 3.75}
CONST_MAT :: matrix[4, 4]f32{
	1, 2, 3, 4,
	5, 6, 7, 8,
	9, 10, 11, 12,
	13, 14, 15, 16,
}
CONST_MIXED :: Mixed{-5, 60000, -123456, -1234567890123, -1.5, 2.25, true}
CONST_NAMES :: [3]string{"alpha", "beta", "gamma"}
CONST_COLORS :: [4]Color{.Blue, .Red, .Green, .Blue}
CONST_BY_COLOR :: [Color]i32{.Red = -1, .Green = 2, .Blue = -3}

g_index := 2

opaque_int :: #force_no_inline proc(i: int) -> int {
	return i
}

const_aggregates :: proc() {
	a := CONST_INTS
	f := CONST_F32S
	m := CONST_MAT
	s := CONST_MIXED
	names := CONST_NAMES
	cols := CONST_COLORS
	by := CONST_BY_COLOR

	fmt.println("ints", a, a[0], a[1], a[2], a[0] + a[1] * a[2])
	fmt.println("f32s", f, f[1] + f[3], f[0] * f[1])
	fmt.println("mat", m, m[0, 0], m[1, 2], m[3, 3])
	fmt.println("mixed", s.i, s.u, s.w, s.q, s.f, s.d, s.ok)
	fmt.println("mixed widened", int(s.i), int(s.u), int(s.w), i64(s.i) + s.q)
	fmt.println("names", names, names[1])
	fmt.println("colors", cols, cols[0], cols[3])
	fmt.println("by color", by, by[.Blue], by[.Red] + by[.Green])

	// variable indices, into the copy and into the constant itself
	i := opaque_int(1)
	fmt.println("var index", a[i], f[i], names[i], cols[i], by[Color(i)])
	gi := g_index
	fmt.println("global index", a[gi], f[gi], names[gi], cols[gi])
	fmt.println("const index", CONST_INTS[2], CONST_F32S[0], CONST_NAMES[2], CONST_BY_COLOR[.Green], CONST_MAT[2, 1])

	// the local copies must be independent of the constant data
	a[0] = 111
	f[1] = -9.5
	s.i = 7
	s.q = 42
	names[2] = "delta"
	m[2, 2] = -1
	fmt.println("mutated", a, f, s.i, s.q, names, m[2, 2])
	fmt.println("untouched", CONST_INTS, CONST_F32S, CONST_MIXED.i, CONST_MIXED.q, CONST_NAMES, CONST_MAT[2, 2])

	// read/write through a pointer to the local copy
	b := CONST_INTS
	p := &b
	p[1] = 5
	fmt.println("via pointer", b, p[0], p[1] + p[2], CONST_INTS[1])

	id := linalg.MATRIX4F32_IDENTITY
	fmt.println("identity", id, id[0, 0], id[0, 1], id[3, 3])
	id[0, 3] = 5
	fmt.println("identity mutated", id[0, 3], linalg.MATRIX4F32_IDENTITY[0, 3])
}

// ------------------------------------------------- 2. dead store removal

g_side := 0

side_effect :: #force_no_inline proc(v: int) -> int {
	g_side += v
	return g_side
}

compute :: #force_no_inline proc(a, b: int) -> int {
	return a * b + 1
}

Vec :: struct {
	x, y, z: f32,
}

dead_stores :: proc(n: int) {
	x := 0
	x = compute(3, n)
	fmt.println("overwritten", x)

	v := Vec{1, 2, 3}
	v = Vec{f32(n), 2 * f32(n), -1}
	fmt.println("compound", v, v.x + v.y*v.z)

	// a `break` between the two assignments: the first value survives
	y := 1
	for i in 0 ..< n {
		y = i
		if i == 3 {
			break
		}
		y = i * 2
	}
	fmt.println("break", y)

	// a `continue` between the two assignments
	z := -1
	for i in 0 ..< n {
		z = i
		if i % 2 == 0 {
			continue
		}
		z = i * 10
	}
	fmt.println("continue", z)

	// nested `if`s around the second assignment
	w := 100
	for i in 0 ..< n {
		w = i + 1
		if i > 1 {
			if i % 3 == 0 {
				continue
			}
			w = -i
		}
		w = w * 2
	}
	fmt.println("nested", w)

	// the overwritten value comes from a call: the side effect stays
	g_side = 0
	q := side_effect(2)
	q = 5
	fmt.println("side effect", q, g_side)
	q = side_effect(3)
	q = side_effect(4)
	fmt.println("side effect chain", q, g_side)
}

// -------------------------------------- 3. locals assigned once, constant

constant_locals :: proc(n: int) {
	c: f32 = 0.99995
	sn: f32 = 0.0099998
	k: i64 = -1234567890123
	b := true
	u: u8 = 200

	fmt.printf("scalars %.5f %.5f %v %v %v\n", c, sn, k, b, u)
	fmt.println("arith", c * 2, sn + c, k / 3, k % 7, u + 55, u * 2)
	fmt.println("logic", b && n > 0, !b, b || n < 0)
	fmt.println("cmp", c > sn, k < 0, u > 199, int(u) * 2, i64(u) - k)

	bound := 5
	total := 0
	for i in 0 ..< bound {
		total += i * int(u)
		if f32(i) > c {
			total += 1
		}
	}
	fmt.println("loop bound", total)

	m := linalg.MATRIX4F32_IDENTITY
	r := linalg.Matrix4f32{
		c, -sn, 0, 0,
		sn, c, 0, 0,
		0, 0, 1, 0,
		0, 0, 0, 1,
	}
	t := linalg.matrix4_translate_f32({1, 2, 3})
	for _ in 0 ..< 100 {
		m = r * m * t
		m[3, 3] = 1
	}
	fmt.printf("matrix %.3f %.3f %.3f %.3f\n", m[0, 0], m[0, 1], m[1, 0], m[1, 1])
	fmt.printf("matrix col %.3f %.3f %.3f %.3f\n", m[0, 3], m[1, 3], m[2, 3], m[3, 3])
}

// -------------------------------------------------- 4. float identities

bits32 :: proc(x: f32) -> u32 {
	return transmute(u32)x
}

bits64 :: proc(x: f64) -> u64 {
	return transmute(u64)x
}

identities_f32 :: #force_no_inline proc(name: string, x: f32) {
	fmt.println("f32", name, x*1, 1*x, x/1, x-0, x + (-0.0), -0.0 + x, x * -1, -1 * x, x / -1)
	fmt.printf(
		"f32 %s bits %08x %08x %08x %08x %08x %08x %08x\n",
		name,
		bits32(x * 1),
		bits32(x / 1),
		bits32(x - 0),
		bits32(x + (-0.0)),
		bits32(-0.0 + x),
		bits32(x * -1),
		bits32(x / -1),
	)
	fmt.println("f32", name, "sign", math.sign_bit(x*1), math.sign_bit(x-0), math.sign_bit(x + (-0.0)), math.sign_bit(-0.0 + x), math.sign_bit(x * -1), math.sign_bit(x / -1))
	// `x*0`, `0*x` and `x+0` must not be folded away
	fmt.printf("f32 %s nofold %08x %08x %08x\n", name, bits32(x * 0), bits32(0 * x), bits32(x + 0))
}

identities_f64 :: #force_no_inline proc(name: string, x: f64) {
	fmt.println("f64", name, x*1, 1*x, x/1, x-0, x + (-0.0), -0.0 + x, x * -1, -1 * x, x / -1)
	fmt.printf(
		"f64 %s bits %016x %016x %016x %016x %016x %016x %016x\n",
		name,
		bits64(x * 1),
		bits64(x / 1),
		bits64(x - 0),
		bits64(x + (-0.0)),
		bits64(-0.0 + x),
		bits64(x * -1),
		bits64(x / -1),
	)
	fmt.println("f64", name, "sign", math.sign_bit(x*1), math.sign_bit(x-0), math.sign_bit(x + (-0.0)), math.sign_bit(-0.0 + x), math.sign_bit(x * -1), math.sign_bit(x / -1))
	fmt.printf("f64 %s nofold %016x %016x %016x\n", name, bits64(x * 0), bits64(0 * x), bits64(x + 0))
}

nan_identities :: proc() {
	n32 := transmute(f32)u32(0x7fc0_0000)
	n64 := transmute(f64)u64(0x7ff8_0000_0000_0000)
	fmt.println(
		"nan f32",
		math.is_nan(n32 * 1), math.is_nan(1 * n32), math.is_nan(n32 / 1),
		math.is_nan(n32 - 0), math.is_nan(n32 + (-0.0)), math.is_nan(-0.0 + n32),
		math.is_nan(n32 * -1), math.is_nan(-1 * n32), math.is_nan(n32 / -1),
		math.is_nan(n32 * 0), math.is_nan(0 * n32), math.is_nan(n32 + 0),
	)
	fmt.println(
		"nan f64",
		math.is_nan(n64 * 1), math.is_nan(1 * n64), math.is_nan(n64 / 1),
		math.is_nan(n64 - 0), math.is_nan(n64 + (-0.0)), math.is_nan(-0.0 + n64),
		math.is_nan(n64 * -1), math.is_nan(-1 * n64), math.is_nan(n64 / -1),
		math.is_nan(n64 * 0), math.is_nan(0 * n64), math.is_nan(n64 + 0),
	)
}

// negation of a local holding a constant, for both widths
negated_constants :: proc() {
	cf: f32 = 2.5
	zf: f32 = 0
	cd: f64 = -7.25
	zd: f64 = 0
	fmt.printf("negconst %v %08x %v %016x\n", -cf, bits32(-zf), -cd, bits64(-zd))
	fmt.printf("negconst bits %08x %016x\n", bits32(-cf), bits64(-cd))
}

// An `if` whose condition folds to a constant in the same optimizer pass
// that empties its body (the flag is set from constants once the call is
// inlined): the condition must still be dropped
verb_flags :: proc(fi: ^bool, verb: rune) {
	x := verb == 'b' || verb == 'o'
	if x && !fi^ {
		fi^ = true
	}
	fmt.println("verb_flags", fi^, verb)
}

// `i64` operations and integer conversions on constants fold
i64_folding :: proc() {
	n := i64(359)
	m := u64(0xFFFF_FFFF_0000_0001)
	neg := i32(-5)
	wide := i32(200)
	fmt.println("i64", n * 3, m % 7, m >> 3, i32(n) << 2, u32(m), u64(transmute(u32)neg), i64(i8(wide)), n == 359, m < 12)
}

main :: proc() {
	const_aggregates()
	flag := false
	verb_flags(&flag, 'v')
	i64_folding()

	dead_stores(opaque_int(6))
	dead_stores(opaque_int(2))
	dead_stores(opaque_int(0))

	constant_locals(opaque_int(3))

	nz32 := transmute(f32)u32(0x8000_0000)
	nz64 := transmute(f64)u64(0x8000_0000_0000_0000)
	identities_f32("pos", 3.5)
	identities_f32("neg", -3.5)
	identities_f32("zero", 0)
	identities_f32("nzero", nz32)
	identities_f64("pos", 3.5)
	identities_f64("neg", -3.5)
	identities_f64("zero", 0)
	identities_f64("nzero", nz64)
	nan_identities()
	negated_constants()
}
