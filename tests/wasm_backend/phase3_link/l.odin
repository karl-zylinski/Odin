// Static linking of relocatable wasm objects (vendor:stb .o files) into the
// module: code, data segments and libc-shim imports.
package main

import "core:fmt"
import stbrp "vendor:stb/rect_pack"
import stbsp "vendor:stb/sprintf"

main :: proc() {
	nodes: [64]stbrp.Node
	ctx: stbrp.Context
	stbrp.init_target(&ctx, 128, 128, raw_data(nodes[:]), len(nodes))
	rects := [?]stbrp.Rect{
		{id = 0, w = 50, h = 40},
		{id = 1, w = 30, h = 60},
		{id = 2, w = 70, h = 20},
		{id = 3, w = 25, h = 25},
		{id = 4, w = 100, h = 50},
	}
	ok := stbrp.pack_rects(&ctx, raw_data(rects[:]), len(rects))
	fmt.println("packed:", ok)
	for r in rects {
		fmt.println(r.id, r.x, r.y, r.was_packed)
	}

	buf: [128]byte
	n := stbsp.snprintf(raw_data(buf[:]), len(buf), "%d %5.2f %s %x", i32(42), f64(3.14159), cstring("hi"), u32(0xbeef))
	fmt.println(n, string(buf[:n]))

	// Default argument promotions and alignment of the variadic buffer
	small: i8 = -7
	half: f32 = 2.5
	flag: bool = true
	big: i64 = -1234567890123
	n = stbsp.snprintf(raw_data(buf[:]), len(buf), "%d %f %d %lld %d %u %lld %.1f", small, half, flag, big, u16(65535), u8(200), i64(99), f32(0.25))
	fmt.println(n, string(buf[:n]))

	// No variadic arguments, and a call whose arguments contain calls
	n = stbsp.snprintf(raw_data(buf[:]), len(buf), "plain")
	fmt.println(n, string(buf[:n]))
	n = stbsp.snprintf(raw_data(buf[:]), len(buf), "%d %d", stbsp.snprintf(raw_data(buf[64:]), 64, "%s", cstring("abc")), i32(len(rects)))
	fmt.println(n, string(buf[:n]))
}
