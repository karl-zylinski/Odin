// Differential test: the block level peephole rewrites of the wasm backend
// optimizer (src/wasm_backend_opt.cpp).  Every construct here is one the
// optimizer rewrites into a shorter shape, and every result is printed, so a
// wrong rewrite shows up as a different number:
//   - a comparator result (a block producing -1/0/1) compared to a constant,
//   - a block producing 0/1 that becomes a `select` or the condition itself,
//   - `if` statements with an empty arm,
//   - the reuse of a load whose address was loaded from before (only correct
//     when nothing wrote to that address in between),
//   - the code after a diverging call,
//   - a branch out of a block with the same constant the block end produces.
// Most inputs come from slices built in `main` and from parameters of
// `#force_no_inline` procedures, so that nothing folds away; the calls with
// literal arguments cover the specialized and inlined versions.
package main

import "core:fmt"
import "core:slice"

Ordering :: slice.Ordering

Item :: struct {
	key: int,
	tag: int,
}

Node :: struct {
	v: int,
	w: int,
}

g_calls := 0

// -------------------------------------------------- 1. comparator results

cmp_int :: #force_no_inline proc(a, b: int) -> Ordering {
	if a < b {
		return .Less
	} else if a > b {
		return .Greater
	}
	return .Equal
}

cmp_item :: proc(a, b: Item) -> Ordering {
	if a.key < b.key do return .Less; else if a.key > b.key do return .Greater
	return .Equal
}

// The comparator result compared against every relevant constant.
compare_pairs :: proc(a, b: int) -> (ge, lt, ne, le, gt: bool) {
	ge = cmp_int(a, b) >= .Equal
	lt = cmp_int(a, b) == .Less
	ne = cmp_int(a, b) != .Equal
	le = cmp_int(a, b) <= .Equal
	gt = cmp_int(a, b) > .Equal
	return
}

// The result kept in a variable and used twice, plus the raw value.
compare_kept :: #force_no_inline proc(a, b: int) -> (int, bool, int) {
	o := cmp_int(a, b)
	n := 0
	if o != .Equal do n += 1
	if o >= .Equal do n += 10
	return int(o), o == .Greater, n
}

sorting :: proc(nums: []int, items: []Item, stable: []Item) {
	slice.sort_by(nums, proc(i, j: int) -> bool { return i < j })
	fmt.println("sort_by", nums)

	slice.sort_by_cmp(items, cmp_item)
	for it in items {
		fmt.println("sort_by_cmp", it.key, it.tag)
	}

	slice.stable_sort_by(stable, proc(i, j: Item) -> bool { return i.key < j.key })
	for it in stable {
		fmt.println("stable_sort_by", it.key, it.tag)
	}
}

// ------------------------------------------------ 2. short circuit chains

bit_set_p :: #force_no_inline proc(x: int) -> bool { return x & 4 != 0 }
less_p :: #force_no_inline proc(x, y: int) -> bool { return x < y }

bools :: proc(x, y: int) -> int {
	r := 0
	a := bit_set_p(x)
	b := less_p(x, y)
	if a && b do r += 1
	if a || b do r += 2
	if !(x < y) do r += 4
	if x & 4 != 0 do r += 8
	if bit_set_p(y) do r += 16
	// the condition value is not 0/1 here
	v := x & 4
	nb := bool(v != 0)
	if nb do r += 32
	m := u32(x) & 8
	if m == 8 do r += 64
	// a chain whose first operand is not 0/1 either
	if v != 0 && y & 1 == 0 do r += 128
	sel := a ? 1000 : 2000
	return r + sel + (b ? 1 : 0) * 3
}

// --------------------------------------------------------- 3. empty `if`s

side_effect :: #force_no_inline proc(x: int) -> bool {
	g_calls += 1
	return x > 0
}

empty_ifs :: proc(x: int) -> int {
	r := 0
	if side_effect(x) {}
	if x > 100 {} else do r += 1
	if x > 0 do r += 2; else {}
	if side_effect(-x) {} else {}
	if side_effect(x) && side_effect(x + 1) {}
	return r
}

// --------------------------------------------------------- 4. load reuse

// three reads of the same field, nothing writes in between
reuse_field :: #force_no_inline proc(n: ^Node) -> (int, int) {
	s := n.v + n.v + n.w
	return s, n.v
}

// the write goes through a pointer that may alias the field
alias_write :: #force_no_inline proc(n: ^Node, p: ^int) -> (before, after: int) {
	before = n.v
	p^ = before + 100
	after = n.v
	return
}

// two pointers to the same array element
same_elem :: #force_no_inline proc(p, q: ^int) -> (a, b: int) {
	a = p^
	q^ = a + 5
	b = p^
	return
}

// a load before and inside a loop that stores; `write_zero` decides whether
// element 0 is written at all
loop_loads :: #force_no_inline proc(buf: []int, write_zero: bool) -> (first_before, first_after, total: int) {
	first_before = buf[0]
	for i in 0 ..< len(buf) {
		if i == 0 && !write_zero do continue
		buf[i] += 1
		total += buf[0]
	}
	first_after = buf[0]
	return
}

// a load through a pointer to a pointer, around a write of the outer pointer
ptr_to_ptr :: #force_no_inline proc(pp: ^^int, other: ^int) -> (a, b: int) {
	a = pp^^
	pp^ = other
	b = pp^^
	return
}

// both arms load the same address, one of them writes it, the load after the
// `if` has to see that
branch_loads :: #force_no_inline proc(n: ^Node, take: bool) -> (int, int) {
	a := 0
	if take {
		a = n.v + 1
	} else {
		n.v = n.v * 2
		a = n.v
	}
	return a, n.v
}

// ----------------------------------------------------- 5. diverging calls

fail :: proc(msg: string) -> ! {
	panic(msg)
}

// the call is guarded and never reached, the loads after it still happen
checked_get :: #force_no_inline proc(s: []int, i: int) -> (int, int) {
	if i < 0 || i >= len(s) do fail("index out of range")
	v := s[i]
	if len(s) == 0 do fail("empty slice")
	return v, s[0] + s[len(s) - 1]
}

// ---------------------------------- 6. branches producing the same constant

classify :: #force_no_inline proc(x: int) -> int {
	if x < 0 do return 1
	if x > 100 do return 1
	return 1
}

classify2 :: #force_no_inline proc(x: int) -> int {
	switch {
	case x < 0:  return -1
	case x == 0: return 7
	case x < 10: return 7
	case:        return 7
	}
}

switch_val :: #force_no_inline proc(x: int) -> int {
	switch x {
	case 0, 1: return 5
	case 2:    return 5
	case 3:    return 9
	}
	return 5
}

main :: proc() {
	pairs := [][2]int{{1, 2}, {2, 2}, {3, 2}, {-5, -5}, {-7, 3}}
	for p in pairs {
		fmt.println("compare_pairs", p[0], p[1], compare_pairs(p[0], p[1]))
		fmt.println("compare_kept", compare_kept(p[0], p[1]))
	}
	// constant arguments: specialized and inlined
	fmt.println("compare_pairs const", compare_pairs(4, 9), compare_pairs(9, 9))
	fmt.println("compare_kept const", compare_kept(-1, -2))

	nums := []int{5, -3, 12, 0, 5, -20}
	items := []Item{{3, 1}, {1, 2}, {3, 3}, {-2, 4}}
	stable := []Item{{3, 1}, {1, 2}, {3, 3}, {-2, 4}}
	sorting(nums, items, stable)

	bool_inputs := []int{0, 4, 5, 12}
	for x in bool_inputs {
		fmt.println("bools", x, bools(x, 6))
	}
	fmt.println("bools const", bools(4, 6), bools(1, 0))

	g_calls = 0
	fmt.println("empty_ifs", empty_ifs(nums[0]), g_calls)
	fmt.println("empty_ifs", empty_ifs(-1), g_calls)
	fmt.println("empty_ifs const", empty_ifs(3), g_calls)

	n := Node{v = 10, w = 20}
	fmt.println("reuse_field", reuse_field(&n))
	fmt.println("alias_write", alias_write(&n, &n.v), n.v)
	fmt.println("alias_write other", alias_write(&n, &n.w), n.v, n.w)

	arr := [4]int{1, 2, 3, 4}
	fmt.println("same_elem", same_elem(&arr[2], &arr[2]), arr[2])
	fmt.println("same_elem apart", same_elem(&arr[0], &arr[1]), arr[0], arr[1])

	buf := []int{7, 1, 1, 1}
	fmt.println("loop_loads", loop_loads(buf, false), buf)
	fmt.println("loop_loads zero", loop_loads(buf, true), buf)

	a, b := 100, 200
	pa := &a
	fmt.println("ptr_to_ptr", ptr_to_ptr(&pa, &b), pa^)

	n2 := Node{v = 3, w = 0}
	fmt.println("branch_loads", branch_loads(&n2, true), n2.v)
	fmt.println("branch_loads", branch_loads(&n2, false), n2.v)

	fmt.println("checked_get", checked_get(buf, len(buf) - 1))
	fmt.println("checked_get", checked_get(buf, 0))

	class_inputs := []int{-1, 0, 5, 200}
	for x in class_inputs {
		fmt.println("classify", x, classify(x), classify2(x), switch_val(x))
	}
	fmt.println("classify const", classify(-3), classify2(0), switch_val(3))
	fmt.println("g_calls", g_calls)
}
