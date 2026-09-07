package main

import "core:fmt"
import "core:mem"

Node :: struct {
	value: int,
	next:  ^Node,
}

Big :: struct {
	id:   int,
	data: [16]u8,
	name: string,
}

main :: proc() {
	// --- new / free ---
	p := new(int)
	p^ = 42
	fmt.println("new int", p^, p != nil)
	p^ += 1
	fmt.println("mutated", p^)
	free(p)

	b := new(Big)
	b.id = 7
	b.name = "big"
	for i in 0 ..< 16 {
		b.data[i] = u8(i * 3)
	}
	fmt.println("new struct", b.id, b.name, b.data[0], b.data[5], b.data[15])
	zb := new(Big)
	fmt.println("zeroed by default", zb.id, zb.name == "", zb.data[3])
	free(zb)
	free(b)

	// --- new_clone ---
	orig := Big{id = 3, name = "clone-me"}
	orig.data[2] = 99
	nc := new_clone(orig)
	fmt.println("new_clone", nc.id, nc.name, nc.data[2])
	nc.id = 100
	fmt.println("independent", orig.id, nc.id)
	free(nc)

	nci := new_clone(1234)
	fmt.println("new_clone int", nci^)
	free(nci)

	// --- make / delete slices ---
	s := make([]int, 5)
	fmt.println("make slice", len(s), s)
	for i in 0 ..< len(s) {
		s[i] = i * i
	}
	fmt.println("filled", s)
	delete(s)

	s2 := make([]Big, 3)
	s2[1].id = 5
	fmt.println("slice of structs", len(s2), s2[0].id, s2[1].id, s2[2].id)
	delete(s2)

	s3, err := make([]u8, 8)
	fmt.println("make with err", len(s3), err)
	delete(s3)

	// --- make / delete maps ---
	m: map[string]int
	m["a"] = 1
	m["b"] = 2
	m["c"] = 3
	sum := 0
	for k, v in m {
		sum += v * len(k)
	}
	fmt.println("map", len(m), m["b"], sum)
	delete(m)

	m2: map[int]int
	reserve(&m2, 64)
	for i in 0 ..< 50 {
		m2[i] = i * 2
	}
	total := 0
	for _, v in m2 {
		total += v
	}
	fmt.println("map2", len(m2), m2[7], total)
	delete(m2)

	// --- make / delete dynamic arrays ---
	d := make([dynamic]int, 0, 8)
	for i in 0 ..< 20 {
		append(&d, i)
	}
	fmt.println("dyn", len(d), d[0], d[19], cap(d) >= 20)
	delete(d)

	// --- linked list built with new ---
	head: ^Node
	for i in 0 ..< 6 {
		n := new(Node)
		n.value = i * 10
		n.next = head
		head = n
	}
	walk := head
	acc := 0
	count := 0
	for walk != nil {
		acc += walk.value
		count += 1
		walk = walk.next
	}
	fmt.println("list", count, acc, head.value, head.next.value)

	// reverse the list in place
	prev: ^Node
	cur := head
	for cur != nil {
		nxt := cur.next
		cur.next = prev
		prev = cur
		cur = nxt
	}
	head = prev
	fmt.println("reversed head", head.value, head.next.value)

	// free the list
	cur = head
	for cur != nil {
		nxt := cur.next
		free(cur)
		cur = nxt
	}

	// --- mem.Arena + arena_allocator ---
	backing := make([]u8, 4096)
	defer delete(backing)
	arena: mem.Arena
	mem.arena_init(&arena, backing)
	aa := mem.arena_allocator(&arena)

	ai := new(int, aa)
	ai^ = 5
	as := make([]int, 10, aa)
	for i in 0 ..< 10 {
		as[i] = i + 1
	}
	fmt.println("arena", ai^, as[0], as[9], arena.offset > 0, arena.offset <= 4096)

	// allocate inside a context override
	{
		context.allocator = aa
		x := new(Big)
		x.id = 77
		y := make([]int, 4)
		y[3] = 9
		fmt.println("arena ctx", x.id, y[3], len(y))
	}
	before_free := arena.offset > 0
	free_all(aa)
	fmt.println("arena freed", before_free, arena.offset)

	// reuse the arena after free_all
	az := make([]u8, 32, aa)
	az[31] = 8
	fmt.println("arena reuse", len(az), az[31], arena.offset >= 32)

	// --- mem.Scratch_Allocator ---
	scratch: mem.Scratch_Allocator
	serr := mem.scratch_allocator_init(&scratch, 1024)
	sa := mem.scratch_allocator(&scratch)
	sp := new(int, sa)
	sp^ = 314
	ss := make([]int, 6, sa)
	ss[5] = 60
	fmt.println("scratch", serr, sp^, len(ss), ss[5])
	free_all(sa)
	sp2 := new(int, sa)
	sp2^ = 271
	fmt.println("scratch reuse", sp2^)
	mem.scratch_allocator_destroy(&scratch)

	// --- temp allocator ---
	t1 := make([]int, 4, context.temp_allocator)
	t1[0] = 11
	t2 := make([]int, 4, context.temp_allocator)
	t2[0] = 22
	fmt.println("temp", t1[0], t2[0], len(t1), len(t2))
	free_all(context.temp_allocator)
	t3 := make([]int, 4, context.temp_allocator)
	t3[3] = 33
	fmt.println("temp after free_all", t3[3], len(t3))
	free_all(context.temp_allocator)

	// many temp allocations to force growth
	tsum := 0
	for i in 0 ..< 100 {
		tv := make([]int, 8, context.temp_allocator)
		tv[7] = i
		tsum += tv[7]
	}
	fmt.println("temp loop", tsum)
	free_all(context.temp_allocator)

	// --- resize via the allocator interface ---
	rs := make([]int, 4)
	rs[0] = 1
	rs[3] = 4
	rp, rerr := mem.resize(raw_data(rs), 4 * size_of(int), 16 * size_of(int))
	fmt.println("resize", rerr, rp != nil)
	rs2 := mem.slice_ptr(cast(^int)rp, 16)
	fmt.println("resize kept", rs2[0], rs2[3])
	rs2[15] = 99
	fmt.println("resize wrote", rs2[15])
	free(rp)

	// --- alloc / free raw bytes ---
	raw, aerr := mem.alloc(64)
	fmt.println("alloc", aerr, raw != nil)
	bytes := mem.byte_slice(raw, 64)
	bytes[0] = 1
	bytes[63] = 2
	fmt.println("alloc bytes", bytes[0], bytes[63], len(bytes))
	fmt.println("alloc zeroed", bytes[10], bytes[30])
	free(raw)

	// alloc_bytes
	ab, aberr := mem.alloc_bytes(24, 8)
	fmt.println("alloc_bytes", aberr, len(ab))
	delete(ab)

	// --- nested / repeated allocation stress ---
	ptrs: [dynamic]^Node
	for i in 0 ..< 32 {
		n := new(Node)
		n.value = i
		append(&ptrs, n)
	}
	psum := 0
	for n in ptrs {
		psum += n.value
	}
	fmt.println("stress", len(ptrs), psum)
	for n in ptrs {
		free(n)
	}
	delete(ptrs)

	// --- allocator stored in a struct field via context ---
	fmt.println("ctx allocator is nil proc", context.allocator.procedure == nil)
	fmt.println("done")
}
