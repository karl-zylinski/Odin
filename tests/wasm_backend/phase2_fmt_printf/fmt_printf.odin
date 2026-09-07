package main

import "core:fmt"
import "core:strings"

main :: proc() {
	// floats f32 and f64, %v %f %.3f %e
	fmt.printf("%v %v\n", f32(3.5), f64(3.5))
	fmt.printf("%f %f\n", f32(-1.25), f64(1.25))
	fmt.printf("%.3f %.3f\n", f32(3.14159), f64(3.14159))
	fmt.printf("%e %e\n", f32(12345.6789), f64(12345.6789))

	// %x %X %b %o
	fmt.printf("%x %X %b %o\n", 255, 255, 10, 8)
	fmt.printf("%x %X\n", -255, -255)

	// padding
	fmt.printf("[%5d][%-5d][%05d]\n", 42, 42, 42)
	fmt.printf("[%5d][%-5d][%05d]\n", -42, -42, -42)

	// tprintf
	s1 := fmt.tprintf("tprintf: %d %s", 7, "x")
	fmt.println(s1)

	// aprintf + delete
	s2 := fmt.aprintf("aprintf: %d %s", 8, "y")
	fmt.println(s2)
	delete(s2)

	// sbprintf into strings.Builder
	b: strings.Builder
	strings.builder_init(&b)
	defer strings.builder_destroy(&b)
	fmt.sbprintf(&b, "sbf:%d", 99)
	fmt.println(strings.to_string(b))

	// %T
	fmt.printf("%T %T\n", 1, "s")

	// %q
	fmt.printf("%q\n", "quoted\nstring")

	// %c
	fmt.printf("%c%c%c\n", 72, 105, 33)
}
