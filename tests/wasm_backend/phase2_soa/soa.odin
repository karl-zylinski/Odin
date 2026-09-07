package main

import "core:fmt"

V :: struct {
	x: int,
	y: u8,
}

main :: proc() {
	// #soa[dynamic]V with append
	arr := make(#soa[dynamic]V, 0, 4)
	defer delete(arr)
	append(&arr, V{1, 2}, V{3, 4}, V{5, 6})
	fmt.println(len(arr), cap(arr) >= 3)

	// indexing and field access s[0].x and s.x[0]
	fmt.println(arr[0].x, arr[1].y, arr.x[2], arr.y[0])

	arr[1].x = 30
	fmt.println(arr[1].x)

	// #soa[]V slice from dynamic
	s: #soa[]V = arr[:]
	fmt.println(len(s), s[2].x, s[2].y)

	sum := 0
	for e in arr {
		sum += e.x + int(e.y)
	}
	fmt.println(sum)

	// soa_zip / soa_unzip
	xs := []int{100, 200, 300}
	ys := []u8{7, 8, 9}
	zipped := soa_zip(x = xs, y = ys)
	fmt.println(len(zipped), zipped[1].x, zipped[1].y)

	xs2, ys2 := soa_unzip(zipped)
	fmt.println(xs2[0], xs2[2], ys2[0], ys2[2])
}
