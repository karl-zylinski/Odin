package main

import "core:fmt"

Item :: struct {
	id:    int,
	label: string,
}

sum_by_value :: proc(arr: [dynamic]int) -> int {
	total := 0
	for v in arr {
		total += v
	}
	return total
}

double_by_pointer :: proc(arr: ^[dynamic]int) {
	for &v in arr {
		v *= 2
	}
}

main :: proc() {
	// append single and multiple values
	a: [dynamic]int
	defer delete(a)
	append(&a, 1)
	append(&a, 2, 3, 4)
	fmt.println(len(a), a)

	// append_elems
	extra := [3]int{5, 6, 7}
	append_elems(&a, ..extra[:])
	fmt.println(len(a), a)

	// len/cap checks
	fmt.println(len(a), cap(a) >= len(a))

	// reserve
	reserve(&a, 100)
	fmt.println(len(a), cap(a) >= 100)

	// resize
	resize(&a, 5)
	fmt.println(len(a), a)
	resize(&a, 8)
	fmt.println(len(a), a)

	// pop
	popped := pop(&a)
	fmt.println(popped, len(a), a)

	// pop_safe
	v, ok := pop_safe(&a)
	fmt.println(v, ok, len(a))

	empty: [dynamic]int
	defer delete(empty)
	v2, ok2 := pop_safe(&empty)
	fmt.println(v2, ok2)

	// unordered_remove
	ur := make([dynamic]int, 0, 8)
	defer delete(ur)
	append(&ur, 10, 20, 30, 40, 50)
	unordered_remove(&ur, 1)
	fmt.println(ur)

	// ordered_remove
	or_ := make([dynamic]int, 0, 8)
	defer delete(or_)
	append(&or_, 10, 20, 30, 40, 50)
	ordered_remove(&or_, 1)
	fmt.println(or_)

	// inject_at
	inj := make([dynamic]int, 0, 8)
	defer delete(inj)
	append(&inj, 1, 2, 4, 5)
	inject_at(&inj, 2, 3)
	fmt.println(inj)

	// clear
	clear(&inj)
	fmt.println(len(inj), inj)

	// slicing a dynamic array
	sl_base := make([dynamic]int, 0, 8)
	defer delete(sl_base)
	append(&sl_base, 100, 200, 300, 400, 500)
	full_slice := sl_base[:]
	sub_slice := sl_base[1:3]
	fmt.println(full_slice, sub_slice)

	// for v in arr
	sum := 0
	for v in sl_base {
		sum += v
	}
	fmt.println("sum", sum)

	// for &v in arr mutating
	for &v in sl_base {
		v += 1
	}
	fmt.println(sl_base)

	// nested [dynamic] of structs
	items: [dynamic]Item
	defer delete(items)
	append(&items, Item{id = 1, label = "a"})
	append(&items, Item{id = 2, label = "b"})
	fmt.println(len(items), items)

	// [dynamic][dynamic]int
	grid: [dynamic][dynamic]int
	defer {
		for row in grid {
			delete(row)
		}
		delete(grid)
	}
	row0 := make([dynamic]int, 0, 4)
	append(&row0, 1, 2, 3)
	row1 := make([dynamic]int, 0, 4)
	append(&row1, 4, 5, 6)
	append(&grid, row0, row1)
	grid_sum := 0
	for row in grid {
		for v in row {
			grid_sum += v
		}
	}
	fmt.println(len(grid), grid_sum)

	// make([dynamic]int, 3, 10)
	made := make([dynamic]int, 3, 10)
	defer delete(made)
	fmt.println(len(made), cap(made) >= 10, made)

	// make([]int, n) + delete
	sl := make([]int, 5)
	for i in 0 ..< len(sl) {
		sl[i] = i * i
	}
	fmt.println(len(sl), sl)
	delete(sl)

	// pass dynamic array by value and by pointer
	pass_val := make([dynamic]int, 0, 8)
	defer delete(pass_val)
	append(&pass_val, 1, 2, 3)
	fmt.println(sum_by_value(pass_val))
	double_by_pointer(&pass_val)
	fmt.println(pass_val)
}
