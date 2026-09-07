package main

import "core:fmt"

Point :: struct {
	x, y: int,
}

Rect :: struct {
	top_left: Point,
	size:     Point,
}

Named_Point :: struct {
	using p: Point,
	name:    string,
}

U :: union {
	int,
	string,
	Point,
}

NN :: union($T: typeid) #no_nil {
	T,
	f32,
}

Raw :: struct #raw_union {
	i: i32,
	f: f32,
	b: [4]u8,
}

Packed :: struct #packed {
	a: u8,
	b: u32,
	c: u8,
}

main :: proc() {
	// nested structs
	r := Rect{top_left = Point{1, 2}, size = Point{10, 20}}
	fmt.println(r.top_left.x, r.top_left.y, r.size.x, r.size.y)

	// struct copy value semantics
	r2 := r
	r2.top_left.x = 999
	fmt.println(r.top_left.x, r2.top_left.x)

	// arrays of structs
	pts := [3]Point{{1, 1}, {2, 2}, {3, 3}}
	sum := Point{0, 0}
	for p in pts {
		sum.x += p.x
		sum.y += p.y
	}
	fmt.println(sum.x, sum.y)

	// pointer to struct field, mutate through it
	px := &pts[1].x
	px^ = 42
	fmt.println(pts[1].x)

	py := &r.top_left
	py.y = 77
	fmt.println(r.top_left.y)

	// using fields
	np := Named_Point{p = Point{5, 6}, name = "hello"}
	fmt.println(np.x, np.y, np.name)
	np.x = 50
	fmt.println(np.p.x)

	// struct comparison
	a1 := Point{1, 2}
	a2 := Point{1, 2}
	a3 := Point{1, 3}
	fmt.println(a1 == a2, a1 != a2, a1 == a3, a1 != a3)

	// unions: assignment + type switch
	u: U
	u = 5
	switch v in u {
	case int:
		fmt.println("int", v)
	case string:
		fmt.println("string", v)
	case Point:
		fmt.println("point", v.x, v.y)
	}
	u = "abc"
	switch v in u {
	case int:
		fmt.println("int", v)
	case string:
		fmt.println("string", v)
	case Point:
		fmt.println("point", v.x, v.y)
	}
	u = Point{7, 8}
	switch v in u {
	case int:
		fmt.println("int", v)
	case string:
		fmt.println("string", v)
	case Point:
		fmt.println("point", v.x, v.y)
	}

	// union #no_nil
	nn: NN(int)
	nn = 3
	switch v in nn {
	case int:
		fmt.println("nn int", v)
	case f32:
		fmt.println("nn f32", v)
	}
	nn = f32(1.5)
	switch v in nn {
	case int:
		fmt.println("nn int", v)
	case f32:
		fmt.println("nn f32", v)
	}

	// Maybe(T)
	m: Maybe(int)
	v0, ok0 := m.?
	fmt.println(v0, ok0)
	m = 42
	v1, ok1 := m.?
	fmt.println(v1, ok1)

	// struct #raw_union
	raw: Raw
	raw.i = 1065353216 // bit pattern of 1.0f
	fmt.println(raw.f)

	// struct #packed
	fmt.println(size_of(Packed))

	// transmute between same-sized types
	fi := transmute(f32)i32(1065353216)
	fmt.println(fi)
	ii := transmute(i32)f32(1.0)
	fmt.println(ii)
}
