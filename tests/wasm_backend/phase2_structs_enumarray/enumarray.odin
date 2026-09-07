package main

import "core:fmt"

Color :: enum {
	Red,
	Green,
	Blue,
}

main :: proc() {
	names := [Color]string{.Red = "red", .Green = "green", .Blue = "blue"}
	for name, c in names {
		fmt.println(c, name)
	}

	scores: [Color]int
	scores[.Red] = 10
	scores[.Green] = 20
	scores[.Blue] = 30
	total := 0
	for s, _ in scores {
		total += s
	}
	fmt.println(total)
}
