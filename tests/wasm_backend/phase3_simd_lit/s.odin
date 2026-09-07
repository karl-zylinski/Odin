package sv
import "core:fmt"
import "core:simd"
rcon :: proc(rc: u8) -> simd.u8x16 {
	return simd.u8x16{1, 2, 4, 8, rc, rc, rc, rc, 0, 0, 0, 0, 0, 0, 0, 0}
}
main :: proc() {
	v := rcon(7)
	fmt.println(v)
	x := f32(1.5)
	w := #simd[4]f32{x, x*2, 3, x+1}
	fmt.println(w, simd.reduce_add_ordered(w))
	n := 5
	z := #simd[2]i64{i64(n), i64(n)*3}
	fmt.println(z)
}
