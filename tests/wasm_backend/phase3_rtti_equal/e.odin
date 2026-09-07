// The `equal` procedures in the type info of comparable structs and unions
// that need more than a byte comparison
package main

import "base:runtime"
import "core:fmt"

S :: struct {
	s: string,
	x: int,
}

U :: union {
	string,
	int,
}

P :: struct {
	a: u8,
	b: u32, // padding between the fields
}

main :: proc() {
	si := runtime.type_info_base(type_info_of(S)).variant.(runtime.Type_Info_Struct)
	a := S{"hi", 1}
	b := S{"hi", 1}
	c := S{"ho", 1}
	fmt.println(si.equal != nil, si.equal(&a, &b), si.equal(&a, &c))

	ui := runtime.type_info_base(type_info_of(U)).variant.(runtime.Type_Info_Union)
	x: U = "abc"
	y: U = "abc"
	z: U = 3
	w: U = 3
	fmt.println(ui.equal != nil, ui.equal(&x, &y), ui.equal(&x, &z), ui.equal(&z, &w))

	pi := runtime.type_info_base(type_info_of(P)).variant.(runtime.Type_Info_Struct)
	fmt.println(pi.equal == nil)

	m := map[S]int{}
	m[a] = 1
	m[c] = 2
	fmt.println(m[b], m[c], len(m))
}
