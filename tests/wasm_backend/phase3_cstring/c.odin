package cs
import "core:fmt"
import "core:strings"
S :: struct { name: cstring, n: int }
cmp :: proc(a, b: $T) -> bool { return a == b }
main :: proc() {
	more()
	a := strings.clone_to_cstring("hello")
	b := strings.clone_to_cstring("hello")
	c: cstring = "hello"
	fmt.println(a == b, a == c, b == "hello", a != b, a == "nope")
	fmt.println(cmp(a, b), cmp(a, c), cmp(a, cstring("x")))
	s1 := S{a, 1}
	s2 := S{b, 1}
	fmt.println(s1 == s2, s1 != s2, cmp(s1, s2))
	arr1 := [2]cstring{a, c}
	arr2 := [2]cstring{b, "hello"}
	fmt.println(arr1 == arr2)
	fmt.println(a < b, a <= b, c > "abc")
	e: cstring
	fmt.println(e == nil, a == nil, e == "", e != nil)
}
more :: proc() {
	a := strings.clone_to_cstring("hello")
	s := "hello"
	fmt.println(string(a) == s, a == "hello")
	m := map[cstring]int{}
	m[a] = 1
	fmt.println(m["hello"], "hello" in m, len(m))
	w: cstring16 = "wide"
	w2: cstring16 = "wide"
	fmt.println(w == w2, w != w2, w < w2)
}
