package en
import "core:fmt"
import "base:intrinsics"

G_BE: u32be = 0x01020304
G_F: f64be = 1.5
G_ARR := [2]u16be{1, 2}

S :: struct { a: u16be, b: i32le, c: f32be, d: u64be }

main :: proc() {
	a: u32be = 0x11223344
	b: u32be = 5
	fmt.println(a, transmute(u32)a, transmute([4]u8)a)
	fmt.println(a + b, a - b, a * b, a / b, a % b, transmute([4]u8)(a + b))
	fmt.println(a < b, a > b, a == 0x11223344, a != b, a <= b, a >= b)
	fmt.println(a << 4, a >> 4, transmute([4]u8)(a << 4))
	fmt.println(a & 0xff00, a | 1, a ~ a, transmute([4]u8)(a & 0xff00))
	fmt.println(-b, transmute([4]u8)(-b))
	fmt.println(u32(a), u64(a), u16(a), int(a), u64be(a), u16le(a), f32(a), f64(b))
	a += 1
	fmt.println(a)
	a *= 2
	fmt.println(a)

	s: i16be = -300
	fmt.println(s, s / 7, s >> 2, i32(s), i32be(s), u16(s), transmute([2]u8)s)
	x: i64be = -1234567890123
	fmt.println(x, x + 1, x < 0, transmute([8]u8)x, f64(x))

	f: f32be = 3.25
	g: f64le = -2.5
	fmt.println(f, transmute([4]u8)f, f * 2, f + f, f < 4, f64(f), int(f), g, transmute([8]u8)g, g * g, -g, g > f64le(f))
	h: f64be = 1e10
	fmt.println(h, transmute([8]u8)h, h/3, i64be(f))

	fmt.println(G_BE, transmute([4]u8)G_BE, G_F, transmute([8]u8)G_F, G_ARR, transmute([4]u8)G_ARR)

	st := S{a = 0x1234, b = -7, c = 1.0, d = 0x0102030405060708}
	fmt.println(st, transmute([4]u8)st.c, transmute([8]u8)st.d, st.a + 1, st.b * 3)
	bytes := transmute([size_of(S)]u8)st
	fmt.println(bytes)

	w: u128be = 0x0102030405060708090a0b0c0d0e0f10
	fmt.println(w, transmute([16]u8)w, w + 1, u64(w), u64(w >> 64), w == 0x0102030405060708090a0b0c0d0e0f10, w < 5)
	wl: i128le = -12345678901234567890
	fmt.println(wl, wl + 5, i128(wl), transmute([16]u8)wl)

	fmt.println(intrinsics.byte_swap(a), intrinsics.byte_swap(u16(0x1234)), intrinsics.byte_swap(f32(1)), intrinsics.byte_swap(w))

	arr: [3]u32be = {1, 2, 3}
	sum: u32be
	for v in arr { sum += v }
	fmt.println(sum, arr, transmute([12]u8)arr)
	p := &arr[1]
	p^ = 100
	p^ += 5
	fmt.println(arr, arr[1] == 105)

	sw: u16be = 2
	switch sw {
	case 1: fmt.println("one")
	case 2: fmt.println("two")
	case: fmt.println("other")
	}
	m: map[u32be]int
	m[a] = 1
	fmt.println(m[a], a in m)
	delete(m)
	bs2()

	bs: bit_set[0..<32; u32be] = {1, 5, 31}
	fmt.println(bs, 5 in bs, 6 in bs, transmute([4]u8)bs, bs + {2}, bs - {1}, card(bs))
}
bs2 :: proc() {
	e1, e2 := 3, 9
	bs: bit_set[0..<16; u16be] = {e1, e2}
	fmt.println(bs, transmute([2]u8)bs, e1 in bs, 4 in bs)
	bs += {12}
	bs -= {3}
	fmt.println(bs, transmute([2]u8)bs)
	Big :: bit_set[0..<128; u128be]
	wb: Big = {0, 64, 127}
	fmt.println(wb, 64 in wb, 65 in wb, transmute([16]u8)wb)
}
