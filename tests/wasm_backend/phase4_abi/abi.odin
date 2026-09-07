package main

import "core:fmt"

S2 :: struct {
	a, b: i32,
}

S3F :: struct {
	x, y, z: f32,
}

SPad :: struct {
	a: u8,
	b: i64,
	c: u16,
}

S1 :: struct {
	v: f64,
}

SNest :: struct {
	inner: S2,
	tag:   i32,
}

SBig :: struct {
	data: [5]i64,
}

SStr :: struct {
	name: string,
	n:    int,
}

SZero :: struct {
}

Ops :: struct {
	add:  proc "c" (a, b: i32) -> i32,
	pair: proc "contextless" (s: S2) -> S2,
	tail: proc (s: string, n: int) -> string,
}

c_add :: proc "c" (a, b: i32) -> i32 {
	return a * 10 + b
}

c_s2 :: proc "c" (s: S2) -> S2 {
	r := s
	r.a += 1
	r.b *= 2
	return r
}

c_s3f :: proc "c" (s: S3F) -> S3F {
	return S3F{s.x + 1, s.y + 2, s.z + 3}
}

c_spad :: proc "c" (s: SPad) -> SPad {
	return SPad{s.a + 1, s.b + 2, s.c + 3}
}

c_padsum :: proc "c" (s: SPad) -> i64 {
	return i64(s.a) + s.b + i64(s.c)
}

c_s1 :: proc "c" (s: S1) -> S1 {
	return S1{s.v * 2}
}

c_snest :: proc "c" (s: SNest) -> SNest {
	s := s
	s.inner.a += 10
	s.inner.b -= 1
	s.tag += 1
	return s
}

c_sbig :: proc "c" (s: SBig) -> SBig {
	r := s
	for i in 0 ..< 5 {
		r.data[i] += i64(i) * 100
	}
	return r
}

c_sstr :: proc "c" (s: SStr) -> SStr {
	return SStr{s.name[1:], s.n + 1}
}

c_str :: proc "c" (s: string, n: int) -> string {
	return s[n:]
}

c_slice_sum :: proc "c" (xs: []int) -> int {
	total := 0
	for x in xs {
		total += x
	}
	return total
}

c_slice_tail :: proc "c" (xs: []int) -> []int {
	return xs[1:]
}

c_a3f :: proc "c" (a: [3]f32) -> [3]f32 {
	return [3]f32{a[0] + 1, a[1] * 2, a[2] - 1}
}

c_a4u8 :: proc "c" (a: [4]u8) -> [4]u8 {
	return [4]u8{a[3], a[2], a[1], a[0]}
}

c_a2i128 :: proc "c" (a: [2]i128) -> [2]i128 {
	return [2]i128{a[0] + a[1], a[0] * 3}
}

c_i128 :: proc "c" (a: i128, b: i128) -> i128 {
	r := a * b + (a << 3) - (b >> 2)
	if a < b {
		r = -r
	}
	return r
}

c_u128 :: proc "c" (a: u128, b: u128) -> u128 {
	r := a * b + (a >> 5)
	if a > b {
		r += 7
	}
	return r
}

c_mix128 :: proc "c" (a: i32, b: i128, c: f64) -> i128 {
	return b * i128(a) + i128(c)
}

c_c64 :: proc "c" (z: complex64, w: complex64) -> complex64 {
	return z * w + z
}

c_c128 :: proc "c" (z: complex128, w: complex128) -> complex128 {
	return z * w - w
}

c_q128 :: proc "c" (a: quaternion128, b: quaternion128) -> quaternion128 {
	return a * b
}

c_q256 :: proc "c" (a: quaternion256, b: quaternion256) -> quaternion256 {
	return a * b + a
}

c_m22 :: proc "c" (m: matrix[2, 2]f32, n: matrix[2, 2]f32) -> matrix[2, 2]f32 {
	return m * n
}

c_m33 :: proc "c" (m: matrix[3, 3]f32, n: matrix[3, 3]f32) -> matrix[3, 3]f32 {
	return m * n + m
}

c_any :: proc "c" (v: any) -> int {
	if v.id == int {
		return (^int)(v.data)^ + 1
	}
	return -1
}

c_maybe :: proc "c" (m: Maybe(^int)) -> Maybe(^int) {
	if p, ok := m.?; ok {
		p^ += 1
		return p
	}
	return nil
}

c_zero :: proc "c" (a: i32, z: SZero, b: i32, z2: SZero, c: f64) -> f64 {
	return f64(a) * 100 + f64(b) * 10 + c
}

c_many :: proc "c" (a: i32, s: S2, str: string, d: f64, v: [3]f32, big: i128, flag: bool, b: u8) -> i128 {
	r := i128(a)
	r += i128(s.a) * 2 + i128(s.b) * 3
	r += i128(len(str)) * 5
	r += i128(d) * 7
	r += i128(v[0]) + i128(v[1]) * 11 + i128(v[2]) * 13
	r += big * 17
	r += i128(b) * 19
	if flag {
		r = -r
	}
	return r
}

c_addrmod :: proc "c" (s: S3F, arr: [3]f32, str: string, big: i128, qq: quaternion128, m: matrix[2, 2]f32) -> f32 {
	s, arr, str, big, qq, m := s, arr, str, big, qq, m
	ps := &s
	ps.x += 1
	pa := &arr
	pa[0] += 2
	pstr := &str
	pstr^ = pstr^[1:]
	pb := &big
	pb^ += 100
	pq := &qq
	pq.x += 3
	pm := &m
	pm[0, 0] += 4
	return s.x + arr[0] + f32(len(str)) + f32(big) + qq.x + m[0, 0]
}

c_rec :: proc "c" (s: SPad) -> i64 {
	if s.a == 0 {
		return s.b
	}
	return c_rec(SPad{s.a - 1, s.b + i64(s.a) * i64(s.c), s.c})
}

cl_add :: proc "contextless" (a, b: i32) -> i32 {
	return a * 10 + b
}

cl_s2 :: proc "contextless" (s: S2) -> S2 {
	r := s
	r.a += 1
	r.b *= 2
	return r
}

cl_s3f :: proc "contextless" (s: S3F) -> S3F {
	return S3F{s.x + 1, s.y + 2, s.z + 3}
}

cl_spad :: proc "contextless" (s: SPad) -> SPad {
	return SPad{s.a + 1, s.b + 2, s.c + 3}
}

cl_padsum :: proc "contextless" (s: SPad) -> i64 {
	return i64(s.a) + s.b + i64(s.c)
}

cl_s1 :: proc "contextless" (s: S1) -> S1 {
	return S1{s.v * 2}
}

cl_snest :: proc "contextless" (s: SNest) -> SNest {
	s := s
	s.inner.a += 10
	s.inner.b -= 1
	s.tag += 1
	return s
}

cl_sbig :: proc "contextless" (s: SBig) -> SBig {
	r := s
	for i in 0 ..< 5 {
		r.data[i] += i64(i) * 100
	}
	return r
}

cl_sstr :: proc "contextless" (s: SStr) -> SStr {
	return SStr{s.name[1:], s.n + 1}
}

cl_str :: proc "contextless" (s: string, n: int) -> string {
	return s[n:]
}

cl_slice_sum :: proc "contextless" (xs: []int) -> int {
	total := 0
	for x in xs {
		total += x
	}
	return total
}

cl_slice_tail :: proc "contextless" (xs: []int) -> []int {
	return xs[1:]
}

cl_a3f :: proc "contextless" (a: [3]f32) -> [3]f32 {
	return [3]f32{a[0] + 1, a[1] * 2, a[2] - 1}
}

cl_a4u8 :: proc "contextless" (a: [4]u8) -> [4]u8 {
	return [4]u8{a[3], a[2], a[1], a[0]}
}

cl_a2i128 :: proc "contextless" (a: [2]i128) -> [2]i128 {
	return [2]i128{a[0] + a[1], a[0] * 3}
}

cl_i128 :: proc "contextless" (a: i128, b: i128) -> i128 {
	r := a * b + (a << 3) - (b >> 2)
	if a < b {
		r = -r
	}
	return r
}

cl_u128 :: proc "contextless" (a: u128, b: u128) -> u128 {
	r := a * b + (a >> 5)
	if a > b {
		r += 7
	}
	return r
}

cl_mix128 :: proc "contextless" (a: i32, b: i128, c: f64) -> i128 {
	return b * i128(a) + i128(c)
}

cl_c64 :: proc "contextless" (z: complex64, w: complex64) -> complex64 {
	return z * w + z
}

cl_c128 :: proc "contextless" (z: complex128, w: complex128) -> complex128 {
	return z * w - w
}

cl_q128 :: proc "contextless" (a: quaternion128, b: quaternion128) -> quaternion128 {
	return a * b
}

cl_q256 :: proc "contextless" (a: quaternion256, b: quaternion256) -> quaternion256 {
	return a * b + a
}

cl_m22 :: proc "contextless" (m: matrix[2, 2]f32, n: matrix[2, 2]f32) -> matrix[2, 2]f32 {
	return m * n
}

cl_m33 :: proc "contextless" (m: matrix[3, 3]f32, n: matrix[3, 3]f32) -> matrix[3, 3]f32 {
	return m * n + m
}

cl_any :: proc "contextless" (v: any) -> int {
	if v.id == int {
		return (^int)(v.data)^ + 1
	}
	return -1
}

cl_maybe :: proc "contextless" (m: Maybe(^int)) -> Maybe(^int) {
	if p, ok := m.?; ok {
		p^ += 1
		return p
	}
	return nil
}

cl_zero :: proc "contextless" (a: i32, z: SZero, b: i32, z2: SZero, c: f64) -> f64 {
	return f64(a) * 100 + f64(b) * 10 + c
}

cl_many :: proc "contextless" (a: i32, s: S2, str: string, d: f64, v: [3]f32, big: i128, flag: bool, b: u8) -> i128 {
	r := i128(a)
	r += i128(s.a) * 2 + i128(s.b) * 3
	r += i128(len(str)) * 5
	r += i128(d) * 7
	r += i128(v[0]) + i128(v[1]) * 11 + i128(v[2]) * 13
	r += big * 17
	r += i128(b) * 19
	if flag {
		r = -r
	}
	return r
}

cl_addrmod :: proc "contextless" (s: S3F, arr: [3]f32, str: string, big: i128, qq: quaternion128, m: matrix[2, 2]f32) -> f32 {
	s, arr, str, big, qq, m := s, arr, str, big, qq, m
	ps := &s
	ps.x += 1
	pa := &arr
	pa[0] += 2
	pstr := &str
	pstr^ = pstr^[1:]
	pb := &big
	pb^ += 100
	pq := &qq
	pq.x += 3
	pm := &m
	pm[0, 0] += 4
	return s.x + arr[0] + f32(len(str)) + f32(big) + qq.x + m[0, 0]
}

cl_rec :: proc "contextless" (s: SPad) -> i64 {
	if s.a == 0 {
		return s.b
	}
	return cl_rec(SPad{s.a - 1, s.b + i64(s.a) * i64(s.c), s.c})
}

od_add :: proc (a, b: i32) -> i32 {
	return a * 10 + b
}

od_s2 :: proc (s: S2) -> S2 {
	r := s
	r.a += 1
	r.b *= 2
	return r
}

od_s3f :: proc (s: S3F) -> S3F {
	return S3F{s.x + 1, s.y + 2, s.z + 3}
}

od_spad :: proc (s: SPad) -> SPad {
	return SPad{s.a + 1, s.b + 2, s.c + 3}
}

od_padsum :: proc (s: SPad) -> i64 {
	return i64(s.a) + s.b + i64(s.c)
}

od_s1 :: proc (s: S1) -> S1 {
	return S1{s.v * 2}
}

od_snest :: proc (s: SNest) -> SNest {
	s := s
	s.inner.a += 10
	s.inner.b -= 1
	s.tag += 1
	return s
}

od_sbig :: proc (s: SBig) -> SBig {
	r := s
	for i in 0 ..< 5 {
		r.data[i] += i64(i) * 100
	}
	return r
}

od_sstr :: proc (s: SStr) -> SStr {
	return SStr{s.name[1:], s.n + 1}
}

od_str :: proc (s: string, n: int) -> string {
	return s[n:]
}

od_slice_sum :: proc (xs: []int) -> int {
	total := 0
	for x in xs {
		total += x
	}
	return total
}

od_slice_tail :: proc (xs: []int) -> []int {
	return xs[1:]
}

od_a3f :: proc (a: [3]f32) -> [3]f32 {
	return [3]f32{a[0] + 1, a[1] * 2, a[2] - 1}
}

od_a4u8 :: proc (a: [4]u8) -> [4]u8 {
	return [4]u8{a[3], a[2], a[1], a[0]}
}

od_a2i128 :: proc (a: [2]i128) -> [2]i128 {
	return [2]i128{a[0] + a[1], a[0] * 3}
}

od_i128 :: proc (a: i128, b: i128) -> i128 {
	r := a * b + (a << 3) - (b >> 2)
	if a < b {
		r = -r
	}
	return r
}

od_u128 :: proc (a: u128, b: u128) -> u128 {
	r := a * b + (a >> 5)
	if a > b {
		r += 7
	}
	return r
}

od_mix128 :: proc (a: i32, b: i128, c: f64) -> i128 {
	return b * i128(a) + i128(c)
}

od_c64 :: proc (z: complex64, w: complex64) -> complex64 {
	return z * w + z
}

od_c128 :: proc (z: complex128, w: complex128) -> complex128 {
	return z * w - w
}

od_q128 :: proc (a: quaternion128, b: quaternion128) -> quaternion128 {
	return a * b
}

od_q256 :: proc (a: quaternion256, b: quaternion256) -> quaternion256 {
	return a * b + a
}

od_m22 :: proc (m: matrix[2, 2]f32, n: matrix[2, 2]f32) -> matrix[2, 2]f32 {
	return m * n
}

od_m33 :: proc (m: matrix[3, 3]f32, n: matrix[3, 3]f32) -> matrix[3, 3]f32 {
	return m * n + m
}

od_any :: proc (v: any) -> int {
	if v.id == int {
		return (^int)(v.data)^ + 1
	}
	return -1
}

od_maybe :: proc (m: Maybe(^int)) -> Maybe(^int) {
	if p, ok := m.?; ok {
		p^ += 1
		return p
	}
	return nil
}

od_zero :: proc (a: i32, z: SZero, b: i32, z2: SZero, c: f64) -> f64 {
	return f64(a) * 100 + f64(b) * 10 + c
}

od_many :: proc (a: i32, s: S2, str: string, d: f64, v: [3]f32, big: i128, flag: bool, b: u8) -> i128 {
	r := i128(a)
	r += i128(s.a) * 2 + i128(s.b) * 3
	r += i128(len(str)) * 5
	r += i128(d) * 7
	r += i128(v[0]) + i128(v[1]) * 11 + i128(v[2]) * 13
	r += big * 17
	r += i128(b) * 19
	if flag {
		r = -r
	}
	return r
}

od_addrmod :: proc (s: S3F, arr: [3]f32, str: string, big: i128, qq: quaternion128, m: matrix[2, 2]f32) -> f32 {
	s, arr, str, big, qq, m := s, arr, str, big, qq, m
	ps := &s
	ps.x += 1
	pa := &arr
	pa[0] += 2
	pstr := &str
	pstr^ = pstr^[1:]
	pb := &big
	pb^ += 100
	pq := &qq
	pq.x += 3
	pm := &m
	pm[0, 0] += 4
	return s.x + arr[0] + f32(len(str)) + f32(big) + qq.x + m[0, 0]
}

od_rec :: proc (s: SPad) -> i64 {
	if s.a == 0 {
		return s.b
	}
	return od_rec(SPad{s.a - 1, s.b + i64(s.a) * i64(s.c), s.c})
}

// a struct parameter passed unchanged to a procedure with another calling convention
c_cross :: proc "c" (s: SPad) -> i64 {
	return cl_padsum(s) + i64(s.c)
}

cl_cross :: proc "contextless" (s: SPad) -> i64 {
	return c_padsum(s) + i64(s.b)
}

od_cross :: proc(s: SPad) -> i64 {
	return c_padsum(s) + cl_padsum(s) + i64(s.a)
}

global_ops := Ops{c_add, cl_s2, od_str}
global_adders := [3]proc "c" (a, b: i32) -> i32{c_add, c_add2, c_add3}

c_add2 :: proc "c" (a, b: i32) -> i32 {
	return a + b * 100
}

c_add3 :: proc "c" (a, b: i32) -> i32 {
	return a - b
}

main :: proc() {

	fmt.println("--- c ---")
	{
		r := c_s2(S2{1, 2})
		fmt.println(r.a, r.b)
		f_s2 := c_s2
		r2 := f_s2(S2{3, 4})
		fmt.println(r2.a, r2.b)

		v3 := c_s3f(S3F{1, 2.5, -3})
		fmt.println(v3.x, v3.y, v3.z)
		f_s3f := c_s3f
		v3b := f_s3f(v3)
		fmt.println(v3b.x, v3b.y, v3b.z)

		pd := c_spad(SPad{1, 2, 3})
		fmt.println(pd.a, pd.b, pd.c, size_of(SPad))
		f_spad := c_spad
		pd2 := f_spad(pd)
		fmt.println(pd2.a, pd2.b, pd2.c)
		fmt.println(c_padsum(pd2))

		one := c_s1(S1{2.5})
		fmt.println(one.v)
		f_s1 := c_s1
		fmt.println(f_s1(one).v)

		nst := c_snest(SNest{S2{5, 6}, 7})
		fmt.println(nst.inner.a, nst.inner.b, nst.tag)
		f_nest := c_snest
		nst2 := f_nest(nst)
		fmt.println(nst2.inner.a, nst2.inner.b, nst2.tag)

		big := c_sbig(SBig{{1, 2, 3, 4, 5}})
		fmt.println(big.data)
		f_big := c_sbig
		big2 := f_big(big)
		fmt.println(big2.data, size_of(SBig))

		ss := c_sstr(SStr{"hello", 1})
		fmt.println(ss.name, ss.n)
		f_sstr := c_sstr
		ss2 := f_sstr(ss)
		fmt.println(ss2.name, ss2.n)

		fmt.println(c_str("abcdef", 2))
		f_str := c_str
		fmt.println(f_str("abcdef", 4))

		xs := []int{10, 20, 30, 40}
		fmt.println(c_slice_sum(xs))
		f_ssum := c_slice_sum
		fmt.println(f_ssum(xs))
		tail := c_slice_tail(xs)
		fmt.println(len(tail), tail)
		f_stail := c_slice_tail
		fmt.println(c_slice_sum(f_stail(tail)))

		a3 := c_a3f([3]f32{1, 2, 3})
		fmt.println(a3)
		f_a3 := c_a3f
		fmt.println(f_a3(a3))

		a4 := c_a4u8([4]u8{1, 2, 3, 4})
		fmt.println(a4)
		f_a4 := c_a4u8
		fmt.println(f_a4(a4))

		a2 := c_a2i128([2]i128{3, 100000000000000000000})
		fmt.println(a2[0], a2[1])
		f_a2 := c_a2i128
		a2b := f_a2(a2)
		fmt.println(a2b[0], a2b[1])

		i1 := c_i128(1234567890123456789, 987654321)
		fmt.println(i1)
		f_i128 := c_i128
		fmt.println(f_i128(-5, 1000000007))

		u1 := c_u128(340282366920938463463374607431768211455, 3)
		fmt.println(u1)
		f_u128 := c_u128
		fmt.println(f_u128(1 << 100, 12345))

		mx := c_mix128(7, 1 << 90, 2.5)
		fmt.println(mx)
		f_mix := c_mix128
		fmt.println(f_mix(-3, 1000000, 9.75))

		z := c_c64(complex(1, 2), complex(3, -1))
		fmt.println(real(z), imag(z))
		f_c64 := c_c64
		z2 := f_c64(z, complex(f32(0.5), f32(0.5)))
		fmt.println(real(z2), imag(z2))

		w := c_c128(complex(1.5, -2), complex(0.5, 4))
		fmt.println(real(w), imag(w))
		f_c128 := c_c128
		w2 := f_c128(w, complex(f64(2), f64(0)))
		fmt.println(real(w2), imag(w2))

		q1 := c_q128(quaternion(w = 1, x = 2, y = -3, z = 0.5), quaternion(w = 0, x = 1, y = 1, z = 2))
		fmt.println(real(q1), imag(q1), jmag(q1), kmag(q1))
		f_q128 := c_q128
		q1b := f_q128(q1, q1)
		fmt.println(real(q1b), imag(q1b), jmag(q1b), kmag(q1b))

		q2 := c_q256(quaternion(w = 2, x = -1, y = 0.5, z = 3), quaternion(w = 1, x = 1, y = 0, z = -2))
		fmt.println(real(q2), imag(q2), jmag(q2), kmag(q2))
		f_q256 := c_q256
		q2b := f_q256(q2, q2)
		fmt.println(real(q2b), imag(q2b), jmag(q2b), kmag(q2b))

		m2 := c_m22(matrix[2, 2]f32{1, 2, 3, 4}, matrix[2, 2]f32{0, 1, -1, 2})
		fmt.println(m2[0, 0], m2[0, 1], m2[1, 0], m2[1, 1])
		f_m22 := c_m22
		m2b := f_m22(m2, m2)
		fmt.println(m2b[0, 0], m2b[0, 1], m2b[1, 0], m2b[1, 1])

		m3 := c_m33(matrix[3, 3]f32{1, 2, 3, 4, 5, 6, 7, 8, 9}, matrix[3, 3]f32{1, 0, 0, 0, 2, 0, 0, 0, 1})
		fmt.println(m3[0, 0], m3[1, 1], m3[2, 2], m3[0, 2], m3[2, 0])
		f_m33 := c_m33
		m3b := f_m33(m3, m3)
		fmt.println(m3b[0, 0], m3b[1, 1], m3b[2, 2], m3b[0, 2], m3b[2, 0])

		anyval := 41
		fmt.println(c_any(anyval), c_any("nope"))
		f_any := c_any
		fmt.println(f_any(anyval))

		mv := 10
		mres := c_maybe(&mv)
		mp, mok := mres.?
		fmt.println(mv, mok, mp^)
		f_maybe := c_maybe
		mres2 := f_maybe(nil)
		_, mok2 := mres2.?
		fmt.println(mok2)
		mres3 := f_maybe(&mv)
		mp3, _ := mres3.?
		fmt.println(mv, mp3^)

		fmt.println(c_zero(1, SZero{}, 2, {}, 0.5))
		f_zero := c_zero
		fmt.println(f_zero(9, {}, 8, {}, 0.25))

		mny := c_many(3, S2{4, 5}, "abcdefg", 2.5, [3]f32{1, 2, 3}, 1 << 64, false, 200)
		fmt.println(mny)
		f_many := c_many
		fmt.println(f_many(-1, S2{0, 1}, "xy", -4.5, [3]f32{-1, 0, 2}, -7, true, 3))

		amod := c_addrmod(S3F{1, 2, 3}, [3]f32{4, 5, 6}, "hello", 5, quaternion(w = 0, x = 1, y = 0, z = 0), matrix[2, 2]f32{1, 2, 3, 4})
		fmt.println(amod)
		f_amod := c_addrmod
		fmt.println(f_amod(S3F{0, 0, 0}, [3]f32{0, 0, 0}, "xyz", -50, quaternion(w = 1, x = 2, y = 3, z = 4), matrix[2, 2]f32{9, 8, 7, 6}))

		fmt.println(c_rec(SPad{6, 0, 3}))
		f_rec := c_rec
		fmt.println(f_rec(SPad{10, 1, 2}))

		fmt.println(c_cross(SPad{7, 8, 9}))
		f_cross := c_cross
		fmt.println(f_cross(SPad{1, 2, 3}))
	}

	fmt.println("--- contextless ---")
	{
		r := cl_s2(S2{1, 2})
		fmt.println(r.a, r.b)
		f_s2 := cl_s2
		r2 := f_s2(S2{3, 4})
		fmt.println(r2.a, r2.b)

		v3 := cl_s3f(S3F{1, 2.5, -3})
		fmt.println(v3.x, v3.y, v3.z)
		f_s3f := cl_s3f
		v3b := f_s3f(v3)
		fmt.println(v3b.x, v3b.y, v3b.z)

		pd := cl_spad(SPad{1, 2, 3})
		fmt.println(pd.a, pd.b, pd.c, size_of(SPad))
		f_spad := cl_spad
		pd2 := f_spad(pd)
		fmt.println(pd2.a, pd2.b, pd2.c)
		fmt.println(cl_padsum(pd2))

		one := cl_s1(S1{2.5})
		fmt.println(one.v)
		f_s1 := cl_s1
		fmt.println(f_s1(one).v)

		nst := cl_snest(SNest{S2{5, 6}, 7})
		fmt.println(nst.inner.a, nst.inner.b, nst.tag)
		f_nest := cl_snest
		nst2 := f_nest(nst)
		fmt.println(nst2.inner.a, nst2.inner.b, nst2.tag)

		big := cl_sbig(SBig{{1, 2, 3, 4, 5}})
		fmt.println(big.data)
		f_big := cl_sbig
		big2 := f_big(big)
		fmt.println(big2.data, size_of(SBig))

		ss := cl_sstr(SStr{"hello", 1})
		fmt.println(ss.name, ss.n)
		f_sstr := cl_sstr
		ss2 := f_sstr(ss)
		fmt.println(ss2.name, ss2.n)

		fmt.println(cl_str("abcdef", 2))
		f_str := cl_str
		fmt.println(f_str("abcdef", 4))

		xs := []int{10, 20, 30, 40}
		fmt.println(cl_slice_sum(xs))
		f_ssum := cl_slice_sum
		fmt.println(f_ssum(xs))
		tail := cl_slice_tail(xs)
		fmt.println(len(tail), tail)
		f_stail := cl_slice_tail
		fmt.println(cl_slice_sum(f_stail(tail)))

		a3 := cl_a3f([3]f32{1, 2, 3})
		fmt.println(a3)
		f_a3 := cl_a3f
		fmt.println(f_a3(a3))

		a4 := cl_a4u8([4]u8{1, 2, 3, 4})
		fmt.println(a4)
		f_a4 := cl_a4u8
		fmt.println(f_a4(a4))

		a2 := cl_a2i128([2]i128{3, 100000000000000000000})
		fmt.println(a2[0], a2[1])
		f_a2 := cl_a2i128
		a2b := f_a2(a2)
		fmt.println(a2b[0], a2b[1])

		i1 := cl_i128(1234567890123456789, 987654321)
		fmt.println(i1)
		f_i128 := cl_i128
		fmt.println(f_i128(-5, 1000000007))

		u1 := cl_u128(340282366920938463463374607431768211455, 3)
		fmt.println(u1)
		f_u128 := cl_u128
		fmt.println(f_u128(1 << 100, 12345))

		mx := cl_mix128(7, 1 << 90, 2.5)
		fmt.println(mx)
		f_mix := cl_mix128
		fmt.println(f_mix(-3, 1000000, 9.75))

		z := cl_c64(complex(1, 2), complex(3, -1))
		fmt.println(real(z), imag(z))
		f_c64 := cl_c64
		z2 := f_c64(z, complex(f32(0.5), f32(0.5)))
		fmt.println(real(z2), imag(z2))

		w := cl_c128(complex(1.5, -2), complex(0.5, 4))
		fmt.println(real(w), imag(w))
		f_c128 := cl_c128
		w2 := f_c128(w, complex(f64(2), f64(0)))
		fmt.println(real(w2), imag(w2))

		q1 := cl_q128(quaternion(w = 1, x = 2, y = -3, z = 0.5), quaternion(w = 0, x = 1, y = 1, z = 2))
		fmt.println(real(q1), imag(q1), jmag(q1), kmag(q1))
		f_q128 := cl_q128
		q1b := f_q128(q1, q1)
		fmt.println(real(q1b), imag(q1b), jmag(q1b), kmag(q1b))

		q2 := cl_q256(quaternion(w = 2, x = -1, y = 0.5, z = 3), quaternion(w = 1, x = 1, y = 0, z = -2))
		fmt.println(real(q2), imag(q2), jmag(q2), kmag(q2))
		f_q256 := cl_q256
		q2b := f_q256(q2, q2)
		fmt.println(real(q2b), imag(q2b), jmag(q2b), kmag(q2b))

		m2 := cl_m22(matrix[2, 2]f32{1, 2, 3, 4}, matrix[2, 2]f32{0, 1, -1, 2})
		fmt.println(m2[0, 0], m2[0, 1], m2[1, 0], m2[1, 1])
		f_m22 := cl_m22
		m2b := f_m22(m2, m2)
		fmt.println(m2b[0, 0], m2b[0, 1], m2b[1, 0], m2b[1, 1])

		m3 := cl_m33(matrix[3, 3]f32{1, 2, 3, 4, 5, 6, 7, 8, 9}, matrix[3, 3]f32{1, 0, 0, 0, 2, 0, 0, 0, 1})
		fmt.println(m3[0, 0], m3[1, 1], m3[2, 2], m3[0, 2], m3[2, 0])
		f_m33 := cl_m33
		m3b := f_m33(m3, m3)
		fmt.println(m3b[0, 0], m3b[1, 1], m3b[2, 2], m3b[0, 2], m3b[2, 0])

		anyval := 41
		fmt.println(cl_any(anyval), cl_any("nope"))
		f_any := cl_any
		fmt.println(f_any(anyval))

		mv := 10
		mres := cl_maybe(&mv)
		mp, mok := mres.?
		fmt.println(mv, mok, mp^)
		f_maybe := cl_maybe
		mres2 := f_maybe(nil)
		_, mok2 := mres2.?
		fmt.println(mok2)
		mres3 := f_maybe(&mv)
		mp3, _ := mres3.?
		fmt.println(mv, mp3^)

		fmt.println(cl_zero(1, SZero{}, 2, {}, 0.5))
		f_zero := cl_zero
		fmt.println(f_zero(9, {}, 8, {}, 0.25))

		mny := cl_many(3, S2{4, 5}, "abcdefg", 2.5, [3]f32{1, 2, 3}, 1 << 64, false, 200)
		fmt.println(mny)
		f_many := cl_many
		fmt.println(f_many(-1, S2{0, 1}, "xy", -4.5, [3]f32{-1, 0, 2}, -7, true, 3))

		amod := cl_addrmod(S3F{1, 2, 3}, [3]f32{4, 5, 6}, "hello", 5, quaternion(w = 0, x = 1, y = 0, z = 0), matrix[2, 2]f32{1, 2, 3, 4})
		fmt.println(amod)
		f_amod := cl_addrmod
		fmt.println(f_amod(S3F{0, 0, 0}, [3]f32{0, 0, 0}, "xyz", -50, quaternion(w = 1, x = 2, y = 3, z = 4), matrix[2, 2]f32{9, 8, 7, 6}))

		fmt.println(cl_rec(SPad{6, 0, 3}))
		f_rec := cl_rec
		fmt.println(f_rec(SPad{10, 1, 2}))

		fmt.println(cl_cross(SPad{7, 8, 9}))
		f_cross := cl_cross
		fmt.println(f_cross(SPad{1, 2, 3}))
	}

	fmt.println("--- odin ---")
	{
		r := od_s2(S2{1, 2})
		fmt.println(r.a, r.b)
		f_s2 := od_s2
		r2 := f_s2(S2{3, 4})
		fmt.println(r2.a, r2.b)

		v3 := od_s3f(S3F{1, 2.5, -3})
		fmt.println(v3.x, v3.y, v3.z)
		f_s3f := od_s3f
		v3b := f_s3f(v3)
		fmt.println(v3b.x, v3b.y, v3b.z)

		pd := od_spad(SPad{1, 2, 3})
		fmt.println(pd.a, pd.b, pd.c, size_of(SPad))
		f_spad := od_spad
		pd2 := f_spad(pd)
		fmt.println(pd2.a, pd2.b, pd2.c)
		fmt.println(od_padsum(pd2))

		one := od_s1(S1{2.5})
		fmt.println(one.v)
		f_s1 := od_s1
		fmt.println(f_s1(one).v)

		nst := od_snest(SNest{S2{5, 6}, 7})
		fmt.println(nst.inner.a, nst.inner.b, nst.tag)
		f_nest := od_snest
		nst2 := f_nest(nst)
		fmt.println(nst2.inner.a, nst2.inner.b, nst2.tag)

		big := od_sbig(SBig{{1, 2, 3, 4, 5}})
		fmt.println(big.data)
		f_big := od_sbig
		big2 := f_big(big)
		fmt.println(big2.data, size_of(SBig))

		ss := od_sstr(SStr{"hello", 1})
		fmt.println(ss.name, ss.n)
		f_sstr := od_sstr
		ss2 := f_sstr(ss)
		fmt.println(ss2.name, ss2.n)

		fmt.println(od_str("abcdef", 2))
		f_str := od_str
		fmt.println(f_str("abcdef", 4))

		xs := []int{10, 20, 30, 40}
		fmt.println(od_slice_sum(xs))
		f_ssum := od_slice_sum
		fmt.println(f_ssum(xs))
		tail := od_slice_tail(xs)
		fmt.println(len(tail), tail)
		f_stail := od_slice_tail
		fmt.println(od_slice_sum(f_stail(tail)))

		a3 := od_a3f([3]f32{1, 2, 3})
		fmt.println(a3)
		f_a3 := od_a3f
		fmt.println(f_a3(a3))

		a4 := od_a4u8([4]u8{1, 2, 3, 4})
		fmt.println(a4)
		f_a4 := od_a4u8
		fmt.println(f_a4(a4))

		a2 := od_a2i128([2]i128{3, 100000000000000000000})
		fmt.println(a2[0], a2[1])
		f_a2 := od_a2i128
		a2b := f_a2(a2)
		fmt.println(a2b[0], a2b[1])

		i1 := od_i128(1234567890123456789, 987654321)
		fmt.println(i1)
		f_i128 := od_i128
		fmt.println(f_i128(-5, 1000000007))

		u1 := od_u128(340282366920938463463374607431768211455, 3)
		fmt.println(u1)
		f_u128 := od_u128
		fmt.println(f_u128(1 << 100, 12345))

		mx := od_mix128(7, 1 << 90, 2.5)
		fmt.println(mx)
		f_mix := od_mix128
		fmt.println(f_mix(-3, 1000000, 9.75))

		z := od_c64(complex(1, 2), complex(3, -1))
		fmt.println(real(z), imag(z))
		f_c64 := od_c64
		z2 := f_c64(z, complex(f32(0.5), f32(0.5)))
		fmt.println(real(z2), imag(z2))

		w := od_c128(complex(1.5, -2), complex(0.5, 4))
		fmt.println(real(w), imag(w))
		f_c128 := od_c128
		w2 := f_c128(w, complex(f64(2), f64(0)))
		fmt.println(real(w2), imag(w2))

		q1 := od_q128(quaternion(w = 1, x = 2, y = -3, z = 0.5), quaternion(w = 0, x = 1, y = 1, z = 2))
		fmt.println(real(q1), imag(q1), jmag(q1), kmag(q1))
		f_q128 := od_q128
		q1b := f_q128(q1, q1)
		fmt.println(real(q1b), imag(q1b), jmag(q1b), kmag(q1b))

		q2 := od_q256(quaternion(w = 2, x = -1, y = 0.5, z = 3), quaternion(w = 1, x = 1, y = 0, z = -2))
		fmt.println(real(q2), imag(q2), jmag(q2), kmag(q2))
		f_q256 := od_q256
		q2b := f_q256(q2, q2)
		fmt.println(real(q2b), imag(q2b), jmag(q2b), kmag(q2b))

		m2 := od_m22(matrix[2, 2]f32{1, 2, 3, 4}, matrix[2, 2]f32{0, 1, -1, 2})
		fmt.println(m2[0, 0], m2[0, 1], m2[1, 0], m2[1, 1])
		f_m22 := od_m22
		m2b := f_m22(m2, m2)
		fmt.println(m2b[0, 0], m2b[0, 1], m2b[1, 0], m2b[1, 1])

		m3 := od_m33(matrix[3, 3]f32{1, 2, 3, 4, 5, 6, 7, 8, 9}, matrix[3, 3]f32{1, 0, 0, 0, 2, 0, 0, 0, 1})
		fmt.println(m3[0, 0], m3[1, 1], m3[2, 2], m3[0, 2], m3[2, 0])
		f_m33 := od_m33
		m3b := f_m33(m3, m3)
		fmt.println(m3b[0, 0], m3b[1, 1], m3b[2, 2], m3b[0, 2], m3b[2, 0])

		anyval := 41
		fmt.println(od_any(anyval), od_any("nope"))
		f_any := od_any
		fmt.println(f_any(anyval))

		mv := 10
		mres := od_maybe(&mv)
		mp, mok := mres.?
		fmt.println(mv, mok, mp^)
		f_maybe := od_maybe
		mres2 := f_maybe(nil)
		_, mok2 := mres2.?
		fmt.println(mok2)
		mres3 := f_maybe(&mv)
		mp3, _ := mres3.?
		fmt.println(mv, mp3^)

		fmt.println(od_zero(1, SZero{}, 2, {}, 0.5))
		f_zero := od_zero
		fmt.println(f_zero(9, {}, 8, {}, 0.25))

		mny := od_many(3, S2{4, 5}, "abcdefg", 2.5, [3]f32{1, 2, 3}, 1 << 64, false, 200)
		fmt.println(mny)
		f_many := od_many
		fmt.println(f_many(-1, S2{0, 1}, "xy", -4.5, [3]f32{-1, 0, 2}, -7, true, 3))

		amod := od_addrmod(S3F{1, 2, 3}, [3]f32{4, 5, 6}, "hello", 5, quaternion(w = 0, x = 1, y = 0, z = 0), matrix[2, 2]f32{1, 2, 3, 4})
		fmt.println(amod)
		f_amod := od_addrmod
		fmt.println(f_amod(S3F{0, 0, 0}, [3]f32{0, 0, 0}, "xyz", -50, quaternion(w = 1, x = 2, y = 3, z = 4), matrix[2, 2]f32{9, 8, 7, 6}))

		fmt.println(od_rec(SPad{6, 0, 3}))
		f_rec := od_rec
		fmt.println(f_rec(SPad{10, 1, 2}))

		fmt.println(od_cross(SPad{7, 8, 9}))
		f_cross := od_cross
		fmt.println(f_cross(SPad{1, 2, 3}))
	}

	fmt.println("--- proc values in aggregates ---")
	ops := global_ops
	fmt.println(ops.add(3, 4))
	sp := ops.pair(S2{7, 8})
	fmt.println(sp.a, sp.b)
	fmt.println(ops.tail("abcdef", 3))
	local_ops := Ops{c_add2, cl_s2, od_str}
	fmt.println(local_ops.add(3, 4), local_ops.tail("wxyz", 1))
	for f, i in global_adders {
		fmt.println(i, f(6, 2))
	}
	adders := [2]proc "contextless" (s: S2) -> S2{cl_s2, cl_snest_helper}
	for f, i in adders {
		r := f(S2{2, 3})
		fmt.println(i, r.a, r.b)
	}
	pops := &global_ops
	fmt.println(pops.add(1, 2))
}

cl_snest_helper :: proc "contextless" (s: S2) -> S2 {
	return S2{s.b, s.a}
}
