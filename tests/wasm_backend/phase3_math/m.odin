package mt
import "core:fmt"
import "core:math"
import "base:intrinsics"

main :: proc() {
	x := 2.5
	y := f32(1.75)
	h := f16(0.5)
	fmt.println(math.sqrt(x), math.sqrt(y), math.floor(x), math.ceil(y), math.trunc(-x), math.round(x), math.round(y), math.abs(-x))
	fmt.println(math.sin(x), math.cos(y), math.pow(x, 3), math.exp(y), math.ln(x))
	fmt.println(math.sin(h), math.cos(h), math.pow(h, 2), math.exp(h))
	fmt.println(math.fmuladd(x, 2, 1), math.fmuladd(y, 2, 1), math.fmuladd(h, 2, 1))
	fmt.println(math.min(x, 1.0), math.max(y, 3), math.copy_sign(x, -1.0), math.remainder(x, 2.0), math.mod(x, 2.0))
	fmt.println(math.tan(x), math.atan2(x, 1.0), math.log2(f64(8)), math.log10(f64(100)))
	more()
}
more :: proc() {
	h := f16(-0.5)
	g := f16(2.25)
	fmt.println(min(h, g), max(h, g), abs(h), clamp(g, h, f16(1)), intrinsics.sqrt(g), intrinsics.fused_mul_add(h, g, g), intrinsics.fused_mul_add(f32(2), 3, 4))
	fmt.println(math.abs(h), math.min(h, g), math.max(h, g), math.clamp(h, -1, 0))
}
