package main

import "base:intrinsics"
import "core:fmt"

u8x8   :: #simd[8]u8
u8x16  :: #simd[16]u8
i8x8   :: #simd[8]i8
i8x16  :: #simd[16]i8
i16x8  :: #simd[8]i16
u16x8  :: #simd[8]u16
i32x4  :: #simd[4]i32
i32x8  :: #simd[8]i32
u32x4  :: #simd[4]u32
i64x2  :: #simd[2]i64
u64x2  :: #simd[2]u64
f32x4  :: #simd[4]f32
f32x8  :: #simd[8]f32
f64x2  :: #simd[2]f64
b8x8   :: #simd[8]bool

approx :: proc() {
	fmt.println("-- approx")
	a := f32x4{1, 2, 4, 3}
	fmt.println(intrinsics.simd_approx_recip(a))
	fmt.println(intrinsics.simd_approx_recip_sqrt(a))
	b := f64x2{2, 8}
	fmt.println(intrinsics.simd_approx_recip(b))
	c := f32x8{0.5, 1.5, 2.5, 3.5, 10, 100, 0.25, 7}
	fmt.println(intrinsics.simd_approx_recip(c))
	fmt.println(intrinsics.simd_approx_recip_sqrt(c))
}

saturating :: proc() {
	fmt.println("-- saturating")
	a8 := u8x8{0, 1, 100, 200, 250, 255, 128, 7}
	b8 := u8x8{0, 255, 200, 100, 10, 1, 128, 3}
	fmt.println(intrinsics.simd_saturating_add(a8, b8))
	fmt.println(intrinsics.simd_saturating_sub(a8, b8))

	s8 := i8x8{0, 100, -100, 127, -128, 60, -60, 5}
	t8 := i8x8{0, 100, -100, 1, -1, -70, 70, -5}
	fmt.println(intrinsics.simd_saturating_add(s8, t8))
	fmt.println(intrinsics.simd_saturating_sub(s8, t8))

	s16 := i16x8{0, 32767, -32768, 20000, -20000, 1, -1, 300}
	t16 := i16x8{0, 1, -1, 20000, 20000, -32768, 32767, -300}
	fmt.println(intrinsics.simd_saturating_add(s16, t16))
	fmt.println(intrinsics.simd_saturating_sub(s16, t16))

	u16v := u16x8{0, 65535, 40000, 1, 65535, 2, 30000, 9}
	v16v := u16x8{1, 1, 40000, 2, 65535, 3, 30000, 0}
	fmt.println(intrinsics.simd_saturating_add(u16v, v16v))
	fmt.println(intrinsics.simd_saturating_sub(u16v, v16v))

	s32 := i32x4{max(i32), min(i32), 5, -5}
	t32 := i32x4{1, -1, -5, 5}
	fmt.println(intrinsics.simd_saturating_add(s32, t32))
	fmt.println(intrinsics.simd_saturating_sub(s32, t32))

	u32v := u32x4{max(u32), 0, 7, 3}
	v32v := u32x4{1, 1, 3, 7}
	fmt.println(intrinsics.simd_saturating_add(u32v, v32v))
	fmt.println(intrinsics.simd_saturating_sub(u32v, v32v))

	s64 := i64x2{max(i64), min(i64)}
	t64 := i64x2{1, -1}
	fmt.println(intrinsics.simd_saturating_add(s64, t64))
	fmt.println(intrinsics.simd_saturating_sub(s64, t64))

	u64v := u64x2{max(u64), 3}
	v64v := u64x2{1, 9}
	fmt.println(intrinsics.simd_saturating_add(u64v, v64v))
	fmt.println(intrinsics.simd_saturating_sub(u64v, v64v))
}

bits :: proc() {
	fmt.println("-- lsbs/msbs")
	a := u8x8{0x80, 0x01, 0xFF, 0x00, 0x7F, 0x81, 0x02, 0x40}
	fmt.println(intrinsics.simd_extract_msbs(a), intrinsics.simd_extract_lsbs(a))
	b := i16x8{-1, 1, min(i16), max(i16), 0, -2, 2, 3}
	fmt.println(intrinsics.simd_extract_msbs(b), intrinsics.simd_extract_lsbs(b))
	c := u32x4{0x8000_0000, 1, 0, 0xFFFF_FFFF}
	fmt.println(intrinsics.simd_extract_msbs(c), intrinsics.simd_extract_lsbs(c))
	d := i64x2{-1, 1}
	fmt.println(intrinsics.simd_extract_msbs(d), intrinsics.simd_extract_lsbs(d))
	e := b8x8{true, false, true, true, false, false, true, false}
	fmt.println(intrinsics.simd_extract_msbs(e), intrinsics.simd_extract_lsbs(e))
	f := i8x16{-1, 0, 1, -128, 127, 2, -3, 4, 5, -6, 7, -8, 9, 10, -11, 12}
	fmt.println(intrinsics.simd_extract_msbs(f), intrinsics.simd_extract_lsbs(f))
}

interleaving :: proc() {
	fmt.println("-- interleave")
	a := i32x4{0, 1, 2, 3}
	b := i32x4{10, 11, 12, 13}
	c := i32x4{20, 21, 22, 23}
	d := i32x4{30, 31, 32, 33}
	fmt.println(intrinsics.simd_interleave(a, b))
	fmt.println(intrinsics.simd_interleave(a, b, c, d))

	ua := u8x8{1, 2, 3, 4, 5, 6, 7, 8}
	ub := u8x8{11, 12, 13, 14, 15, 16, 17, 18}
	fmt.println(intrinsics.simd_interleave(ua, ub))

	fa := f32x4{1, 2, 3, 4}
	fb := f32x4{5, 6, 7, 8}
	fmt.println(intrinsics.simd_interleave(fa, fb))

	la := i64x2{1, 2}
	lb := i64x2{3, 4}
	fmt.println(intrinsics.simd_interleave(la, lb))

	fmt.println("-- deinterleave")
	v := i32x8{0, 1, 2, 3, 4, 5, 6, 7}
	x, y := intrinsics.simd_deinterleave(v, 2)
	fmt.println(x, y)
	p, q, r, s := intrinsics.simd_deinterleave(v, 4)
	fmt.println(p, q, r, s)

	w := u8x16{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}
	w0, w1 := intrinsics.simd_deinterleave(w, 2)
	fmt.println(w0, w1)
	g0, g1, g2, g3 := intrinsics.simd_deinterleave(w, 4)
	fmt.println(g0, g1, g2, g3)

	fv := f32x8{1, 2, 3, 4, 5, 6, 7, 8}
	f0, f1 := intrinsics.simd_deinterleave(fv, 2)
	fmt.println(f0, f1)
}

odd_even_pairwise :: proc() {
	fmt.println("-- odd_even/pairwise")
	a := i32x8{0, 1, 2, 3, 4, 5, 6, 7}
	b := i32x8{10, 11, 12, 13, 14, 15, 16, 17}
	fmt.println(intrinsics.simd_odd_even(a, b))
	fmt.println(intrinsics.simd_pairwise_add(a, b))
	fmt.println(intrinsics.simd_pairwise_sub(a, b))

	fa := f32x4{1, 2, 3, 4}
	fb := f32x4{10, 20, 30, 40}
	fmt.println(intrinsics.simd_odd_even(fa, fb))
	fmt.println(intrinsics.simd_pairwise_add(fa, fb))
	fmt.println(intrinsics.simd_pairwise_sub(fa, fb))

	ua := u8x8{1, 2, 3, 4, 5, 6, 7, 8}
	ub := u8x8{200, 201, 202, 203, 204, 205, 206, 207}
	fmt.println(intrinsics.simd_odd_even(ua, ub))
	fmt.println(intrinsics.simd_pairwise_add(ua, ub))
	fmt.println(intrinsics.simd_pairwise_sub(ua, ub))

	la := i64x2{5, 9}
	lb := i64x2{100, 200}
	fmt.println(intrinsics.simd_odd_even(la, lb))
	fmt.println(intrinsics.simd_pairwise_add(la, lb))
	fmt.println(intrinsics.simd_pairwise_sub(la, lb))

	da := f64x2{1.5, 2.25}
	db := f64x2{8, 16}
	fmt.println(intrinsics.simd_odd_even(da, db))
	fmt.println(intrinsics.simd_pairwise_add(da, db))
	fmt.println(intrinsics.simd_pairwise_sub(da, db))

	sa := i16x8{1, -2, 3, -4, 5, -6, 7, -8}
	sb := i16x8{100, 200, 300, 400, 500, 600, 700, 800}
	fmt.println(intrinsics.simd_odd_even(sa, sb))
	fmt.println(intrinsics.simd_pairwise_add(sa, sb))
	fmt.println(intrinsics.simd_pairwise_sub(sa, sb))
}

reduce_pairs :: proc() {
	fmt.println("-- reduce pairs")
	a := i32x8{1, 2, 3, 4, 5, 6, 7, 8}
	fmt.println(intrinsics.simd_reduce_add_pairs(a), intrinsics.simd_reduce_mul_pairs(a))
	f := f32x4{1.5, 2.25, 3.125, 4}
	fmt.println(intrinsics.simd_reduce_add_pairs(f), intrinsics.simd_reduce_mul_pairs(f))
	u := u8x8{1, 2, 3, 4, 5, 6, 7, 8}
	fmt.println(intrinsics.simd_reduce_add_pairs(u), intrinsics.simd_reduce_mul_pairs(u))
	l := i64x2{1_000_000, 2_000_000}
	fmt.println(intrinsics.simd_reduce_add_pairs(l), intrinsics.simd_reduce_mul_pairs(l))
	d := f64x2{0.5, 0.25}
	fmt.println(intrinsics.simd_reduce_add_pairs(d), intrinsics.simd_reduce_mul_pairs(d))
	s := i16x8{-1, 2, -3, 4, -5, 6, -7, 8}
	fmt.println(intrinsics.simd_reduce_add_pairs(s), intrinsics.simd_reduce_mul_pairs(s))
}

sums_of_n :: proc() {
	fmt.println("-- sums_of_n")
	a := i32x8{1, 2, 3, 4, 5, 6, 7, 8}
	fmt.println(intrinsics.simd_sums_of_n(a, 2))
	fmt.println(intrinsics.simd_sums_of_n(a, 4))
	fmt.println(intrinsics.simd_sums_of_n(a, 8))
	u := u8x16{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}
	fmt.println(intrinsics.simd_sums_of_n(u, 2))
	fmt.println(intrinsics.simd_sums_of_n(u, 4))
	f := f32x8{1, 2, 3, 4, 5, 6, 7, 8}
	fmt.println(intrinsics.simd_sums_of_n(f, 2))
	fmt.println(intrinsics.simd_sums_of_n(f, 4))
	fmt.println(intrinsics.simd_sums_of_n(f, 8))
	d := f64x2{1.25, 2.5}
	fmt.println(intrinsics.simd_sums_of_n(d, 2))
	l := i64x2{7, 11}
	fmt.println(intrinsics.simd_sums_of_n(l, 2))
	s := i16x8{1, -2, 3, -4, 5, -6, 7, -8}
	fmt.println(intrinsics.simd_sums_of_n(s, 2))
	fmt.println(intrinsics.simd_sums_of_n(s, 4))
}

swizzle :: proc() {
	fmt.println("-- runtime_swizzle")
	// NOTE: 16 lanes of bytes is left out on purpose: the LLVM backend picks
	// `llvm.wasm.swizzle` for that shape on a wasm target and then aborts with
	// "Do not know how to split the result of this operator", so there is no
	// reference to compare against

	u := u8x8{10, 11, 12, 13, 14, 15, 16, 17}
	ui := u8x8{7, 6, 5, 4, 8, 9, 200, 0}
	fmt.println(intrinsics.simd_runtime_swizzle(u, ui))

	w := i16x8{10, 20, 30, 40, 50, 60, 70, 80}
	wi := i16x8{7, 0, 3, 9, -1, 4, 100, 2}
	fmt.println(intrinsics.simd_runtime_swizzle(w, wi))

	v := i32x4{10, 20, 30, 40}
	vi := i32x4{3, 0, 7, -1}
	fmt.println(intrinsics.simd_runtime_swizzle(v, vi))

	q := u64x2{111, 222}
	qi := u64x2{1, 5}
	fmt.println(intrinsics.simd_runtime_swizzle(q, qi))
}

memory :: proc() {
	fmt.println("-- masked expand/compress")
	buf := [8]i32{100, 101, 102, 103, 104, 105, 106, 107}
	val := i32x4{-1, -2, -3, -4}
	// only the lowest bit of a mask lane counts: 1, 0, 1, 0
	mask := i32x4{1, 0, 3, 2}
	all := i32x4{1, 1, 1, 1}
	none := i32x4{0, 2, 4, 8}

	fmt.println(intrinsics.simd_masked_expand_load(&buf[0], val, mask))
	fmt.println(intrinsics.simd_masked_expand_load(&buf[0], val, all))
	fmt.println(intrinsics.simd_masked_expand_load(&buf[0], val, none))

	dst := [4]i32{-9, -9, -9, -9}
	intrinsics.simd_masked_compress_store(&dst[0], i32x4{7, 8, 9, 10}, mask)
	fmt.println(dst)
	dst2 := [4]i32{-9, -9, -9, -9}
	intrinsics.simd_masked_compress_store(&dst2[0], i32x4{7, 8, 9, 10}, all)
	fmt.println(dst2)
	dst3 := [4]i32{-9, -9, -9, -9}
	intrinsics.simd_masked_compress_store(&dst3[0], i32x4{7, 8, 9, 10}, none)
	fmt.println(dst3)

	// bytes with a boolean mask
	bbuf := [8]u8{1, 2, 3, 4, 5, 6, 7, 8}
	bmask := b8x8{true, true, false, true, false, false, true, false}
	bval := u8x8{200, 201, 202, 203, 204, 205, 206, 207}
	fmt.println(intrinsics.simd_masked_expand_load(&bbuf[0], bval, bmask))
	bdst := [8]u8{}
	intrinsics.simd_masked_compress_store(&bdst[0], bval, bmask)
	fmt.println(bdst)

	// floats with a 64 bit mask
	fbuf := [4]f32{1.5, 2.5, 3.5, 4.5}
	fval := f32x4{-1, -2, -3, -4}
	fmask := #simd[4]u64{1, 0, 0xFFFF_FFFF_FFFF_FFFF, 6}
	fmt.println(intrinsics.simd_masked_expand_load(&fbuf[0], fval, fmask))
	fdst := [4]f32{}
	intrinsics.simd_masked_compress_store(&fdst[0], f32x4{9, 8, 7, 6}, fmask)
	fmt.println(fdst)

	// NOTE: `simd_masked_load`, `simd_masked_store`, `simd_gather` and
	// `simd_scatter` are left out on purpose: for LLVM >= 22 the LLVM backend
	// attaches their `align` attribute with `LLVMAddAttributeAtIndex`, which
	// only accepts a function, so building any of them corrupts memory and
	// usually crashes the compiler. There is no reference to compare against
	// until that is fixed.
}

main :: proc() {
	approx()
	saturating()
	bits()
	interleaving()
	odd_even_pairwise()
	reduce_pairs()
	sums_of_n()
	swizzle()
	memory()
}
