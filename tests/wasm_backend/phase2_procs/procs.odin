package main

import "core:fmt"
import "core:mem"

add :: proc(a, b: int) -> int {
	return a + b
}

mul :: proc(a, b: int) -> int {
	return a * b
}

apply :: proc(f: proc(int, int) -> int, a, b: int) -> int {
	return f(a, b)
}

fib :: proc(n: int) -> int {
	if n < 2 {
		return n
	}
	return fib(n - 1) + fib(n - 2)
}

ackermann :: proc(m, n: int) -> int {
	if m == 0 {
		return n + 1
	}
	if n == 0 {
		return ackermann(m - 1, 1)
	}
	return ackermann(m - 1, ackermann(m, n - 1))
}

greet :: proc(name: string, greeting: string = "hello") {
	fmt.println(greeting, name)
}

sum_variadic :: proc(nums: ..int) -> int {
	total := 0
	for n in nums {
		total += n
	}
	return total
}

where_this :: proc(x: $T) -> T where T == int || T == f32 {
	return x
}

identity :: proc(x: $T) -> T {
	return x
}

force_inlined_add :: #force_inline proc(a, b: int) -> int {
	return a + b
}

location_reporter :: proc(loc := #caller_location) {
	fmt.println(loc.procedure, loc.line)
}

Stack :: struct($T: typeid) {
	items: [8]T,
	count: int,
}

stack_push :: proc(s: ^Stack($T), v: T) {
	s.items[s.count] = v
	s.count += 1
}

stack_pop :: proc(s: ^Stack($T)) -> (T, bool) {
	if s.count == 0 {
		zero: T
		return zero, false
	}
	s.count -= 1
	return s.items[s.count], true
}

main :: proc() {
	// procedure values in variables
	f: proc(int, int) -> int = add
	fmt.println(f(3, 4))
	f = mul
	fmt.println(f(3, 4))

	// procedure pointers stored in an array
	ops := [2]proc(int, int) -> int{add, mul}
	fmt.println(ops[0](5, 6), ops[1](5, 6))

	// procedure literal
	square := proc(x: int) -> int {
		return x * x
	}
	fmt.println(square(9))

	// passing procs as parameters
	fmt.println(apply(add, 10, 20), apply(mul, 10, 20))

	// #caller_location
	location_reporter()
	location_reporter()

	// default parameter values
	greet("world")
	greet("world", "hi")

	// variadic procs
	fmt.println(sum_variadic(1, 2, 3, 4))
	nums := []int{5, 6, 7}
	fmt.println(sum_variadic(..nums))

	// recursion
	fmt.println(fib(10))
	fmt.println(ackermann(2, 3))

	// context usage
	context.user_index = 42
	fmt.println(context.user_index)

	val := 100
	context.user_ptr = &val
	p := (^int)(context.user_ptr)
	fmt.println(p^)

	// custom allocator via mem.Arena
	{
		buf: [256]byte
		arena: mem.Arena
		mem.arena_init(&arena, buf[:])
		old_allocator := context.allocator
		context.allocator = mem.arena_allocator(&arena)
		xs := make([]int, 4)
		for i in 0 ..< 4 {
			xs[i] = i * i
		}
		fmt.println(xs[0], xs[1], xs[2], xs[3])
		context.allocator = old_allocator
	}

	// #force_inline
	fmt.println(force_inlined_add(7, 8))

	// polymorphic procedures
	fmt.println(identity(5), identity("str"), identity(1.5))

	// where clauses
	fmt.println(where_this(3), where_this(f32(2.5)))

	// generic struct with push/pop
	s: Stack(int)
	stack_push(&s, 1)
	stack_push(&s, 2)
	stack_push(&s, 3)
	v1, ok1 := stack_pop(&s)
	v2, ok2 := stack_pop(&s)
	fmt.println(v1, ok1, v2, ok2, s.count)

	sf: Stack(f32)
	stack_push(&sf, 1.5)
	stack_push(&sf, 2.5)
	vf, okf := stack_pop(&sf)
	fmt.println(vf, okf)
}
