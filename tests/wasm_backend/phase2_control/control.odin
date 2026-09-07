package main

import "core:fmt"

Enum_Color :: enum {
	Red,
	Green,
	Blue,
}

Point :: struct {
	x: int,
	y: int,
}

Error :: enum {
	None,
	Bad,
}

named_result :: proc() -> (result: int) {
	result = 10
	defer result += 5
	return
}

multi_return :: proc() -> (int, string) {
	return 42, "hi"
}

take_two :: proc(a: int, b: string) {
	fmt.println("take_two:", a, b)
}

might_fail :: proc(x: int) -> (int, Error) {
	if x < 0 {
		return 0, .Bad
	}
	return x * 2, .None
}

or_return_test :: proc(x: int) -> (result: int, err: Error) {
	v := might_fail(x) or_return
	result = v + 1
	return
}

defer_scope_test :: proc() -> int {
	defer fmt.println("defer_scope_test: outer defer")
	{
		defer fmt.println("defer_scope_test: inner defer")
		if true {
			return 99
		}
	}
	return -1
}

defer_loop_test :: proc() {
	for i in 0..<3 {
		defer fmt.println("defer_loop: deferred", i)
		fmt.println("defer_loop: body", i)
	}
	fmt.println("defer_loop: after loop")
}

defer_nested_test :: proc() {
	defer fmt.println("defer_nested: 1 (proc-level, runs last)")
	{
		defer fmt.println("defer_nested: 2 (outer block)")
		{
			defer fmt.println("defer_nested: 3 (inner block, runs first)")
			fmt.println("defer_nested: body")
		}
	}
}

struct_using_test :: proc() {
	Inner :: struct {
		using base: Point,
		z: int,
	}
	p: Inner
	p.x = 1
	p.y = 2
	p.z = 3
	fmt.println("struct_using:", p.x, p.y, p.z)
}

partial_test :: proc(c: Enum_Color) {
	#partial switch c {
	case .Red:
		fmt.println("partial: red")
	case .Green:
		fmt.println("partial: green")
	}
}

main :: proc() {
	fmt.println("--- labelled break/continue ---")
	outer: for i in 0..<3 {
		inner: for j in 0..<3 {
			if j == 1 {
				continue outer
			}
			if i == 2 {
				break outer
			}
			fmt.println("nested:", i, j)
		}
	}

	fmt.println("--- switch fallthrough/multi-value/range ---")
	for i in 0..=7 {
		switch i {
		case 0, 1:
			fmt.println("switch: zero_or_one", i)
		case 2:
			fmt.println("switch: two_start", i)
			fallthrough
		case 3:
			fmt.println("switch: two_or_three", i)
		case 4..=5:
			fmt.println("switch: range_4_5", i)
		case:
			fmt.println("switch: default", i)
		}
	}

	fmt.println("--- switch no condition ---")
	x := 5
	switch {
	case x > 10:
		fmt.println("switch-no-cond: >10")
	case x > 3:
		fmt.println("switch-no-cond: >3")
	case:
		fmt.println("switch-no-cond: else")
	}

	fmt.println("--- defer ordering ---")
	r := defer_scope_test()
	fmt.println("defer_scope_test returned:", r)
	defer_loop_test()
	defer_nested_test()

	fmt.println("--- when ---")
	MY_FLAG :: true
	when MY_FLAG {
		fmt.println("when: true branch taken")
	} else {
		fmt.println("when: false branch taken")
	}

	fmt.println("--- or_else ---")
	maybe_val: Maybe(int) = 7
	v1 := maybe_val.? or_else -1
	fmt.println("or_else present:", v1)
	maybe_val2: Maybe(int)
	v2 := maybe_val2.? or_else -1
	fmt.println("or_else absent:", v2)

	fmt.println("--- or_return ---")
	r1, e1 := or_return_test(5)
	fmt.println("or_return success:", r1, e1)
	r2, e2 := or_return_test(-3)
	fmt.println("or_return fail:", r2, e2)

	fmt.println("--- named result values ---")
	fmt.println("named_result:", named_result())

	fmt.println("--- f(g()) multi-return passthrough ---")
	take_two(multi_return())

	fmt.println("--- #partial switch ---")
	partial_test(.Red)
	partial_test(.Green)
	partial_test(.Blue)

	fmt.println("--- for i in 0..=n ---")
	n := 4
	for i in 0..=n {
		fmt.println("range_eq:", i)
	}

	fmt.println("--- #reverse for over array and slice ---")
	arr := [5]int{10, 20, 30, 40, 50}
	#reverse for v, i in arr {
		fmt.println("reverse_array:", i, v)
	}
	sl := arr[1:4]
	#reverse for v, i in sl {
		fmt.println("reverse_slice:", i, v)
	}

	fmt.println("--- using struct field inside a proc ---")
	struct_using_test()

	// NOTE: `using` an enum (as a statement) is tested separately in
	// phase2_control_using_enum_stmt because the wasm backend does not
	// support it yet ("unsupported statement").

	fmt.println("--- ternary ---")
	t := 10 if true else 20
	fmt.println("ternary:", t)
	t2 := 10 if false else 20
	fmt.println("ternary2:", t2)

	fmt.println("--- boolean compound logic ---")
	b := true
	c := false
	b = b && c
	fmt.println("bool_compound:", b)
	b2 := false
	c2 := true
	b2 = b2 || c2
	fmt.println("bool_compound2:", b2)

	fmt.println("--- variable shadowing ---")
	sx := 1
	{
		sx := 2
		{
			sx := 3
			fmt.println("shadow inner:", sx)
		}
		fmt.println("shadow mid:", sx)
	}
	fmt.println("shadow outer:", sx)

	fmt.println("--- break out of for with switch inside ---")
	loop_label: for i in 0..<10 {
		switch i {
		case 3:
			fmt.println("loop_switch: break switch at", i)
			break
		case 7:
			fmt.println("loop_switch: break loop at", i)
			break loop_label
		}
		fmt.println("loop_switch: after switch, i =", i)
	}

	fmt.println("done")
}
