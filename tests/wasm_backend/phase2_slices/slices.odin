package main

import "core:fmt"
import "core:mem"
import "core:slice"

P :: struct {
	name: string,
	age:  int,
}

main :: proc() {
	// --- basic slicing ---
	arr := [8]int{5, 3, 9, 1, 7, 2, 8, 4}
	s := arr[:]
	fmt.println("full", s, len(s))
	fmt.println("a:b", s[2:5], len(s[2:5]))
	fmt.println(":n", s[:3])
	fmt.println("n:", s[5:])
	fmt.println("empty", s[3:3], len(s[3:3]))
	fmt.println("nested", s[1:7][2:4])

	// slice of a slice, aliasing
	sub := s[2:5]
	sub[0] = 99
	fmt.println("alias", arr, s[2])
	sub[0] = 9

	// --- slice.clone / sort ---
	c := slice.clone(s)
	defer delete(c)
	slice.sort(c)
	fmt.println("sorted", c)
	fmt.println("orig unchanged", s)
	fmt.println("is_sorted", slice.is_sorted(c), slice.is_sorted(s))

	// sort_by with a comparison proc
	people := []P{
		{"carol", 30},
		{"alice", 41},
		{"bob", 22},
		{"dave", 30},
	}
	pc := slice.clone(people)
	defer delete(pc)
	slice.sort_by(pc, proc(a, b: P) -> bool {
		if a.age != b.age {
			return a.age < b.age
		}
		return a.name < b.name
	})
	for p in pc {
		fmt.print(p.name, "=", p.age, " ")
	}
	fmt.println()

	// sort_by_key
	pk := slice.clone(people)
	defer delete(pk)
	slice.sort_by_key(pk, proc(p: P) -> string { return p.name })
	fmt.println("by name", pk[0].name, pk[1].name, pk[2].name, pk[3].name)

	// --- searching ---
	fmt.println("contains", slice.contains(s, 9), slice.contains(s, 100))
	li, lok := slice.linear_search(s, 7)
	fmt.println("linear_search", li, lok)
	lm, lmok := slice.linear_search(s, 42)
	fmt.println("linear_search missing", lm, lmok)
	idx, found := slice.binary_search(c, 7)
	fmt.println("binary_search 7", idx, found)
	idx2, found2 := slice.binary_search(c, 6)
	fmt.println("binary_search 6", idx2, found2)

	// --- min / max / reduce ---
	fmt.println("min", slice.min(s), "max", slice.max(s))
	fmt.println("min_index", slice.min_index(s), "max_index", slice.max_index(s))
	fmt.println("count", slice.count(s, 9))
	fmt.println("equal", slice.equal(s, s), slice.equal(s, c))

	// --- reverse ---
	r := slice.clone(s)
	defer delete(r)
	slice.reverse(r)
	fmt.println("reversed", r)
	slice.reverse(r)
	fmt.println("back", r)

	// --- copy ---
	dst := make([]int, 4)
	defer delete(dst)
	n := copy(dst, s)
	fmt.println("copy", n, dst)
	n2 := copy(dst, s[6:])
	fmt.println("copy short", n2, dst)

	// overlapping copy (memmove semantics)
	ov := [6]int{1, 2, 3, 4, 5, 6}
	copy(ov[1:], ov[:5])
	fmt.println("overlap fwd", ov)
	ov2 := [6]int{1, 2, 3, 4, 5, 6}
	copy(ov2[:5], ov2[1:])
	fmt.println("overlap bwd", ov2)

	// --- fill / zero ---
	f := make([]u8, 6)
	defer delete(f)
	slice.fill(f, 0xAB)
	fmt.println("fill", f)
	mem.zero(raw_data(f), 3)
	fmt.println("zero", f)

	// --- mem.copy / mem.compare ---
	a1 := [4]u8{1, 2, 3, 4}
	a2 := [4]u8{0, 0, 0, 0}
	mem.copy(raw_data(a2[:]), raw_data(a1[:]), 4)
	fmt.println("mem.copy", a2)
	fmt.println("mem.compare eq", mem.compare(a1[:], a2[:]))
	a2[2] = 9
	fmt.println("mem.compare lt", mem.compare(a1[:], a2[:]))
	fmt.println("mem.compare gt", mem.compare(a2[:], a1[:]))
	fmt.println("mem.compare len", mem.compare(a1[:], a1[:2]))

	// --- raw_data and multipointers ---
	mp := raw_data(s)
	fmt.println("mp0", mp[0], "mp3", mp[3], "mp7", mp[7])
	mp[1] = 33
	fmt.println("after mp write", s[1])
	mp[1] = 3
	mp2 := mp[2:]
	fmt.println("mp slice", mp2[0], mp2[1])
	back := mp[1:4]
	fmt.println("mp to slice", back, len(back))

	// pointer walking
	total := 0
	for i in 0 ..< len(s) {
		total += mp[i]
	}
	fmt.println("mp total", total)

	// byte slices via raw_data on a string
	str := "hello"
	sb := raw_data(str)
	fmt.println("str bytes", sb[0], sb[4], len(str))
	fmt.println("transmute str", transmute([]u8)str)

	// --- multi-dimensional arrays ---
	grid: [3][3]int
	for i in 0 ..< 3 {
		for j in 0 ..< 3 {
			grid[i][j] = i*3 + j
		}
	}
	fmt.println("grid", grid)
	fmt.println("row1", grid[1], "elem", grid[2][1])
	gs := grid[:]
	fmt.println("grid slice", len(gs), gs[0], gs[2])
	flat := transmute([9]int)grid
	fmt.println("flat", flat)

	cube: [2][2][2]int
	cube[1][0][1] = 5
	cube[0][1][0] = 3
	fmt.println("cube", cube)

	// slice of arrays
	rows := [][3]int{{1, 2, 3}, {4, 5, 6}}
	fmt.println("rows", rows, rows[1][2])

	// --- slice.to_dynamic / from_ptr ---
	d := slice.clone_to_dynamic(s)
	defer delete(d)
	append(&d, 100)
	fmt.println("dyn from slice", len(d), d[8])

	fp := slice.from_ptr(mp, 3)
	fmt.println("from_ptr", fp)

	// --- slice.concatenate ---
	cat := slice.concatenate([][]int{s[:2], s[6:], {77}})
	defer delete(cat)
	fmt.println("concat", cat)

	// --- last / first ---
	fmt.println("first", s[0], "last", s[len(s)-1])
	fmt.println("slice.last", slice.last(s), "first", slice.first(s))

	// --- mem.set / mem.zero_item ---
	buf: [8]u8
	mem.set(raw_data(buf[:]), 7, 4)
	fmt.println("mem.set", buf)

	// --- slice of struct, mutation through for &v ---
	pm := slice.clone(people)
	defer delete(pm)
	for &p in pm {
		p.age += 1
	}
	fmt.println("aged", pm[0].age, pm[1].age, pm[2].age, pm[3].age)

	// --- nil slices ---
	var_nil: []int
	fmt.println("nil slice", var_nil == nil, len(var_nil), var_nil)

	// --- slice.map-ish via manual loop with index ---
	sum := 0
	for v, i in s {
		sum += v * (i + 1)
	}
	fmt.println("weighted", sum)
}
