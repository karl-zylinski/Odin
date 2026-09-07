package main

import "core:fmt"
import "core:strings"

Point :: struct {
	x: int,
	y: int,
}

Nested :: struct {
	name: string,
	pt:   Point,
	vals: [3]int,
}

Color :: enum {
	Red,
	Green,
	Blue,
}

Val :: union {
	int,
	string,
	f32,
}

main :: proc() {
	// integers, all sizes, signed and unsigned, min/max, negative
	fmt.println(i8(-128), i8(127))
	fmt.println(i16(-32768), i16(32767))
	fmt.println(i32(-2147483648), i32(2147483647))
	fmt.println(i64(-9223372036854775808), i64(9223372036854775807))
	fmt.println(u8(0), u8(255))
	fmt.println(u16(0), u16(65535))
	fmt.println(u32(0), u32(4294967295))
	fmt.println(u64(0), u64(18446744073709551615))
	fmt.println(int(-42), int(42))
	fmt.println(uint(42))

	// floats f32 and f64
	fmt.println(f32(3.5), f64(3.5))
	fmt.println(f32(-1.25), f64(1.25))
	fmt.println(f32(3.14159), f64(3.14159))
	fmt.println(f32(12345.6789), f64(12345.6789))

	// bools
	fmt.println(true, false)

	// runes
	fmt.println('A', 'z', '0')

	// strings
	fmt.println("hello", "world")

	// structs and nested structs
	p := Point{x = 1, y = 2}
	fmt.println(p)
	n := Nested{name = "n1", pt = Point{x = 10, y = 20}, vals = [3]int{7, 8, 9}}
	fmt.println(n)

	// fixed arrays
	arr := [4]int{1, 2, 3, 4}
	fmt.println(arr)

	// slices
	sl := arr[1:3]
	fmt.println(sl)

	// enums
	c := Color.Green
	fmt.println(c, Color.Red, Color.Blue)

	// unions
	v1: Val = 42
	v2: Val = "hi"
	v3: Val = f32(1.5)
	fmt.println(v1, v2, v3)

	// nil pointer printed as %v (println uses default formatting, same code path)
	var_ptr: ^int
	fmt.println(var_ptr)

	// any values
	a1: any = 42
	a2: any = "str"
	a3: any = true
	fmt.println(a1, a2, a3)

	// print (no newline) plus explicit newline via println
	fmt.print("print: ", 1, 2, 3)
	fmt.println()

	// sbprint into strings.Builder (no format string, does not hit printf machinery)
	b: strings.Builder
	strings.builder_init(&b)
	defer strings.builder_destroy(&b)
	fmt.sbprint(&b, "sb", 1, 2)
	fmt.sbprint(&b, " more", true)
	fmt.println(strings.to_string(b))

	// map printed via struct wrapper, 1-element map to avoid iteration order issues
	Wrap :: struct {
		m: map[string]int,
	}
	w: Wrap
	w.m["only"] = 1
	fmt.println(w.m)
	delete(w.m)

	// nested union in struct, pointer to struct
	pp := &p
	fmt.println(pp^)
	fmt.println(pp == nil, pp != nil)
}
