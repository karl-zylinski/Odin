package phase3_conv
import "core:fmt"
import "base:intrinsics"
main :: proc() {
	for b in ([]u8{0x00, 0x41, 0x80, 0xC0, 0xE0, 0xF0, 0xFF}) {
		val := i8(b)
		mark := int(-1)
		for val < 0 {
			val <<= 1
			mark += 1
		}
		fmt.println(b, mark, val)
	}
	x: i8 = -64
	x <<= 1
	fmt.println(x, x < 0)
	x <<= 1
	fmt.println(x, x < 0)
	y: i16 = -16384
	y <<= 2
	fmt.println(y, y < 0)
	buf := [8]u8{1,2,3,4,5,6,7,8}
	code := intrinsics.unaligned_load((^u32)(&buf[1]))
	fmt.println(code, intrinsics.byte_swap(code))
	code2 := intrinsics.unaligned_load((^u64)(&buf[0]))
	fmt.println(code2)
	p: u8 = 3
	fmt.println(u32(p) >> 1, (code >> p) & u32(p))
}
