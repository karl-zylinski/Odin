package main

import "core:fmt"

BF :: bit_field u32 {
	a: u8   | 3,
	b: u16  | 9,
	c: bool | 1,
	d: u8   | 7,
}

main :: proc() {
	bf: BF
	bf.a = 5
	bf.b = 300
	bf.c = true
	bf.d = 100
	fmt.println(bf.a, bf.b, bf.c, bf.d)
	backing := transmute(u32)bf
	bf2 := transmute(BF)backing
	fmt.println(bf2.a, bf2.b, bf2.c, bf2.d)
}
