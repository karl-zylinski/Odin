package un
import "core:fmt"
import "core:reflect"
A :: struct { x: [4]u64, init: bool }
B :: struct { y: [8]u32, init: bool, k: int }
C :: struct { z: u8 }
U :: union { A, B, C }
Ctx :: struct { algo: int, impl: U }
IDS := [3]typeid{ typeid_of(A), typeid_of(B), typeid_of(C) }
init_b :: proc(b: ^B) { b.init = true; b.k = 7 }
main :: proc() {
	more2()
	more()
	ctx: Ctx
	fmt.println(ctx.impl == nil, reflect.union_variant_typeid(ctx.impl) == nil)
	reflect.set_union_variant_typeid(ctx.impl, IDS[1])
	fmt.println(reflect.union_variant_typeid(ctx.impl) == typeid_of(B), reflect.get_union_variant_raw_tag(ctx.impl))
	init_b(&ctx.impl.(B))
	b := ctx.impl.(B)
	fmt.println(b.init, b.k)
	fmt.println((&ctx.impl.(B)).init)
	p := &ctx.impl.(B)
	fmt.println(p.init, p.k)
	reflect.set_union_variant_typeid(ctx.impl, typeid_of(A))
	fmt.println(reflect.get_union_variant_raw_tag(ctx.impl))
	reflect.set_union_variant_raw_tag(ctx.impl, 3)
	fmt.println(reflect.union_variant_typeid(ctx.impl) == typeid_of(C))
	fmt.println(IDS[0] == typeid_of(A), IDS[2] == typeid_of(C), IDS[1] == typeid_of(A))
	u: U = C{5}
	fmt.println(reflect.union_variant_typeid(u) == IDS[2])
	fmt.println(reflect.union_variant_type_info(u) == type_info_of(C))
}
more :: proc() {
	u: U = B{init = false, k = 3}
	pb, ok := &u.(B)
	fmt.println(ok, pb != nil, pb.k)
	pa, ok2 := &u.(A)
	fmt.println(ok2, pa == nil)
	pb.k = 9
	fmt.println(u.(B).k)
	pu := &u
	pb2 := &pu.(B)
	pb2.k = 11
	fmt.println(u.(B).k)
	a: any = u.(B)
	pab, ok3 := &a.(B)
	fmt.println(ok3, pab.k)
	pab.k = 13
	fmt.println(a.(B).k)
	paa, ok4 := &a.(A)
	fmt.println(ok4, paa == nil)
	m: Maybe(int) = 5
	pm := &m.(int)
	pm^ = 6
	fmt.println(m)
	mp: Maybe(^int)
	x := 3
	mp = &x
	ppm := &mp.(^int)
	fmt.println(ppm^^)
}
more2 :: proc() {
	mp: Maybe(^int)
	x, y := 3, 4
	mp = &x
	ppm := &mp.(^int)
	ppm^ = &y
	fmt.println(mp.(^int)^)
	m: Maybe(int) = 5
	pm := &m.(int)
	pm^ = 6
	fmt.println(m)
}
