package main

import "core:fmt"

Foo_Enum :: enum {
	A = 1,
	B,
	C = 6,
	D,
}

Colour :: enum u8 {
	Red,
	Green,
	Blue,
}

main :: proc() {
	#unroll for x, i in 1..<4 {
		fmt.println(x, i)
	}
	#unroll for x, i in 3..=6 {
		fmt.println(x, i)
	}
	#unroll for x in -2..<2 {
		fmt.println(x)
	}

	#unroll for r, i in "Hello, 世界" {
		fmt.println(r, i)
	}

	#unroll for elem, idx in ([4]int{1, 4, 9, 16}) {
		fmt.println(elem, idx)
	}
	arr := [3]string{"a", "bb", "ccc"}
	#unroll for elem, idx in arr {
		fmt.println(elem, idx)
	}

	ea := [Colour]int{.Red = 10, .Green = 20, .Blue = 30}
	#unroll for elem, idx in ea {
		fmt.println(elem, idx)
	}

	#unroll for elem, idx in Foo_Enum {
		fmt.println(elem, idx)
	}

	// nested loops, a variable per iteration and a defer in the body
	total := 0
	#unroll for x in 1..=3 {
		y := x * x
		#unroll for z in 0..<2 {
			total += y + z
		}
	}
	fmt.println(total)
	#unroll for x in 1..=2 {
		defer fmt.println("deferred", x)
		fmt.println("body", x)
	}

	// #unroll(N) over a runtime length
	s := []int{1, 2, 3, 4, 5, 6, 7}
	sum := 0
	#unroll(3) for v, i in s {
		sum += v * (i + 1)
	}
	fmt.println(sum)

	d: [dynamic]int
	append(&d, 5, 6, 7, 8, 9)
	sum2 := 0
	#unroll(2) for v, i in d {
		sum2 += v + i
	}
	fmt.println(sum2)
	delete(d)

	fixed := [5]int{2, 4, 6, 8, 10}
	sum3 := 0
	#unroll(4) for v, i in fixed {
		sum3 += v * i
	}
	fmt.println(sum3)

	empty := []int{}
	sum4 := 0
	#unroll(2) for v in empty {
		sum4 += v
	}
	fmt.println(sum4)

	one := []int{42}
	#unroll(1) for v, i in one {
		fmt.println(v, i)
	}
}
