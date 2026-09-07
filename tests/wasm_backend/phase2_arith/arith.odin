package main

import "core:fmt"
import "core:math"
import "base:intrinsics"

Flag :: enum {
	A,
	B,
	C,
	D,
}
Flags :: bit_set[Flag]

main :: proc() {
	fmt.println("--- integer overflow wrapping (unsigned) ---")
	u8v: u8 = 250
	u8v += 10
	fmt.println("u8 wrap:", u8v)
	u16v: u16 = 65530
	u16v += 10
	fmt.println("u16 wrap:", u16v)
	u32v: u32 = 4294967290
	u32v += 10
	fmt.println("u32 wrap:", u32v)
	u64v: u64 = 18446744073709551610
	u64v += 10
	fmt.println("u64 wrap:", u64v)

	fmt.println("--- integer overflow wrapping (signed, explicit wrapping ops) ---")
	i8v: i8 = 120
	i8v = i8(u8(i8v) + u8(10))
	fmt.println("i8 wrap via u8:", i8v)
	i32v: i32 = 2147483640
	i32v = i32(u32(i32v) + u32(10))
	fmt.println("i32 wrap via u32:", i32v)
	i64v: i64 = 9223372036854775800
	i64v = i64(u64(i64v) + u64(20))
	fmt.println("i64 wrap via u64:", i64v)

	fmt.println("--- shifts ---")
	sv: u32 = 1
	fmt.println("shift left 4:", sv << 4)
	fmt.println("shift left by var:", sv << u32(31))
	fmt.println("shift left >= bit width (should be 0):", sv << u32(32))
	sv2: u32 = 0xFFFF_FFFF
	fmt.println("shift right >= bit width (should be 0):", sv2 >> u32(32))
	si: i32 = -8
	fmt.println("arith shift right:", si >> u32(1))
	fmt.println("arith shift right >= bit width:", si >> u32(32))

	fmt.println("--- division / modulo of negatives ---")
	a, b := -7, 2
	fmt.println("div:", a / b, "mod:", a % b, "mod_mod:", a %% b)
	a2, b2 := 7, -2
	fmt.println("div:", a2 / b2, "mod:", a2 % b2, "mod_mod:", a2 %% b2)
	a3, b3 := -7, -2
	fmt.println("div:", a3 / b3, "mod:", a3 % b3, "mod_mod:", a3 %% b3)

	fmt.println("--- 128-bit integers ---")
	x128: i128 = 170141183460469231731687303715884105000
	y128: i128 = 500
	fmt.println("i128 add:", x128 + y128)
	fmt.println("i128 sub:", x128 - y128)
	fmt.println("i128 mul:", i128(1_000_000_000) * i128(1_000_000_000))
	fmt.println("i128 div:", x128 / y128)
	fmt.println("i128 mod:", x128 % y128)
	fmt.println("i128 shift:", (i128(1) << 100))
	fmt.println("i128 compare:", x128 > y128, x128 < y128)
	neg128: i128 = -123456789012345678901234567890
	fmt.println("i128 negative:", neg128)

	ux128: u128 = 340282366920938463463374607431768211000
	uy128: u128 = 500
	fmt.println("u128 add:", ux128 + uy128)
	fmt.println("u128 sub:", ux128 - uy128)
	fmt.println("u128 div:", ux128 / uy128)
	fmt.println("u128 mod:", ux128 % uy128)
	fmt.println("u128 shift:", (u128(1) << 100))
	fmt.println("u128 compare:", ux128 > uy128)

	fmt.println("--- core:math f32/f64 ---")
	// NOTE: math.sin, math.cos and math.pow are excluded here because the
	// *reference* LLVM wasi_wasm32 build in this environment cannot run them
	// at all -- wasmtime reports missing imports env::sin / env::sinf /
	// env::pow (the wasi libm sysroot used by the reference build does not
	// provide these), so there is no "correct" output to diff against. This
	// is unrelated to the wasm backend under test.
	f32v: f32 = 2.0
	fmt.println("sqrt f32:", math.sqrt(f32v))
	f64v: f64 = 2.0
	fmt.println("sqrt f64:", math.sqrt(f64v))
	fmt.println("floor f64:", math.floor(f64(3.7)))
	fmt.println("ceil f64:", math.ceil(f64(3.2)))
	fmt.println("round f64:", math.round(f64(3.5)))
	fmt.println("trunc f64:", math.trunc(f64(3.9)))
	fmt.println("abs f64:", math.abs(f64(-3.5)))
	fmt.println("min f64:", math.min(f64(3.0), f64(5.0)))
	fmt.println("max f64:", math.max(f64(3.0), f64(5.0)))
	fmt.println("clamp f64:", math.clamp(f64(10.0), f64(0.0), f64(5.0)))
	fmt.println("floor f32:", math.floor(f32(3.7)))
	fmt.println("ceil f32:", math.ceil(f32(3.2)))
	fmt.println("round f32:", math.round(f32(3.5)))
	fmt.println("trunc f32:", math.trunc(f32(3.9)))
	fmt.println("abs f32:", math.abs(f32(-3.5)))
	fmt.println("min f32:", math.min(f32(3.0), f32(5.0)))
	fmt.println("max f32:", math.max(f32(3.0), f32(5.0)))
	fmt.println("clamp f32:", math.clamp(f32(10.0), f32(0.0), f32(5.0)))

	fmt.println("--- float <-> int conversions ---")
	f_pos: f64 = 3.9
	fi1 := int(f_pos)
	fmt.println("int(3.9):", fi1)
	f_neg: f64 = -3.9
	fi2 := int(f_neg)
	fmt.println("int(-3.9):", fi2)
	fi3 := f64(1234567)
	fmt.println("f64(1234567):", fi3)
	fi4 := i64(f64(1e18))
	fmt.println("i64(1e18):", fi4)
	fi5 := f32(int(-1000000))
	fmt.println("f32(-1000000):", fi5)

	fmt.println("--- intrinsics bit ops ---")
	bv: u32 = 0b1011_0000
	fmt.println("count_ones:", intrinsics.count_ones(bv))
	fmt.println("count_leading_zeros:", intrinsics.count_leading_zeros(bv))
	fmt.println("count_trailing_zeros:", intrinsics.count_trailing_zeros(bv))
	fmt.println("byte_swap u32:", intrinsics.byte_swap(u32(0x01020304)))
	bv64: u64 = 0x0000_0000_0000_00F0
	fmt.println("count_ones u64:", intrinsics.count_ones(bv64))
	fmt.println("byte_swap u16:", intrinsics.byte_swap(u16(0x0102)))

	fmt.println("--- bit_set ---")
	fs1: Flags = {.A, .C}
	fs2: Flags = {.B, .C}
	fmt.println("union:", fs1 | fs2)
	fmt.println("intersection:", fs1 & fs2)
	fmt.println("difference:", fs1 - fs2)
	fmt.println("in:", .A in fs1, .B in fs1)
	fmt.println("card:", card(fs1), card(fs2))
	fmt.println("card union:", card(fs1 | fs2))

	// NOTE: complex/quaternion arithmetic (+, *, abs) and matrix literals
	// are unsupported by the wasm backend; they are tested separately in
	// phase2_arith_complex_ops, phase2_arith_quaternion_ops and
	// phase2_arith_matrix. Only construction, printing, real() and imag()
	// (which do work on the wasm backend) are exercised here.
	fmt.println("--- complex64 / complex128 ---")
	c64a: complex64 = complex(1.5, 2.5)
	fmt.println("c64a:", c64a, "real:", real(c64a), "imag:", imag(c64a))

	c128a: complex128 = complex(1.5, 2.5)
	fmt.println("c128a:", c128a, "real:", real(c128a), "imag:", imag(c128a))

	fmt.println("--- quaternion128 ---")
	q1: quaternion128 = quaternion(w = 1, x = 2, y = 3, z = 4)
	fmt.println("q1:", q1)

	fmt.println("done")
}
