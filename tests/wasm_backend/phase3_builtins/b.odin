package bu
import "core:fmt"
import "base:intrinsics"

VT :: struct { add: proc(v: ^VT, a, b: int) -> int, name: string }
get_count := 0
get :: proc(v: ^VT) -> ^VT { get_count += 1; return v }

main :: proc() {
	v := VT{add = proc(v: ^VT, a, b: int) -> int { return a + b + len(v.name) }, name = "abc"}
	fmt.println(v->add(1, 2))
	pv := &v
	fmt.println(pv->add(10, 20), get(pv)->add(5, 5), get_count)

	fmt.println(intrinsics.reverse_bits(u8(1)), intrinsics.reverse_bits(u16(0x1234)), intrinsics.reverse_bits(u32(0x80000001)), intrinsics.reverse_bits(u64(0x0123456789abcdef)), intrinsics.reverse_bits(i32(-2)))
	fmt.println(intrinsics.reverse_bits(u128(1)), intrinsics.reverse_bits(u128(0x0123456789abcdef0123456789abcdef)))
	x := 7
	if intrinsics.unlikely(x == 7) { fmt.println("unlikely") }
	if intrinsics.likely(x != 7) { fmt.println("bad") } else { fmt.println("likely") }
	fmt.println(intrinsics.expect(x, 3))

	n := 40
	buf := intrinsics.alloca(n, 16)
	for i in 0..<n { buf[i] = u8(i) }
	fmt.println(buf[0], buf[39], uintptr(buf) % 16)
	fn(3)

	a: i128 = -123456789012345678901234567
	b: u128 = 0xffffffffffffffffffffffffffffffff
	fmt.println(f64(a), f32(a), f64(b), f64(i128(5)))
	fa, fb, fc, fd := f64(-1e30), f64(1e30), f32(1000.5), f64(1e20)
	fmt.println(i128(fa), u128(fb), i128(fc), i128le(fd), u128be(fd))
	fmt.println(i128(a & 0xff != 0), int(a == 0), u8(x > 3))
	fmt.println(intrinsics.count_ones(b), intrinsics.count_leading_zeros(u128(1)), intrinsics.count_trailing_zeros(u128(1) << 70), intrinsics.count_trailing_zeros(u128(0)), intrinsics.count_leading_zeros(u128(0)), intrinsics.count_ones(a))
}
fn :: proc(depth: int) {
	if depth == 0 { return }
	b := intrinsics.alloca(100, 8)
	b[0] = u8(depth)
	fn(depth-1)
	fmt.println("alloca depth", depth, b[0])
}
