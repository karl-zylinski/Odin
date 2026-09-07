package dproc

import "core:fmt"
import "base:runtime"
import "core:strings"

Big :: struct { a, b, c: int, name: string }

end_in :: proc(x: int, s: string) { fmt.println("end_in", x, s) }
@(deferred_in=end_in)
begin_in :: proc(x: int, s: string) { fmt.println("begin_in", x, s) }

end_out :: proc(r: Big) { fmt.println("end_out", r) }
@(deferred_out=end_out)
begin_out :: proc(x: int) -> Big { fmt.println("begin_out", x); return Big{x, x*2, x*3, "big"} }

end_out2 :: proc(a: int, ok: bool) { fmt.println("end_out2", a, ok) }
@(deferred_out=end_out2)
begin_out2 :: proc(x: int) -> (int, bool) { fmt.println("begin_out2", x); return x+1, x > 2 }

end_in_out :: proc(x: int, b: Big, r: int) { fmt.println("end_in_out", x, b, r) }
@(deferred_in_out=end_in_out)
begin_in_out :: proc(x: int, b: Big) -> int { fmt.println("begin_in_out", x, b); return x*10 }

end_none :: proc() { fmt.println("end_none") }
@(deferred_none=end_none)
begin_none :: proc(x: int) { fmt.println("begin_none", x) }

end_in_ptr :: proc(x: ^int, b: ^Big) { fmt.println("end_in_ptr", x^, b^); x^ += 1 }
@(deferred_in_by_ptr=end_in_ptr)
begin_in_ptr :: proc(x: int, b: Big) { fmt.println("begin_in_ptr", x, b) }

end_out_ptr :: proc(b: ^Big, ok: ^bool) { fmt.println("end_out_ptr", b^, ok^) }
@(deferred_out_by_ptr=end_out_ptr)
begin_out_ptr :: proc(x: int) -> (Big, bool) { fmt.println("begin_out_ptr", x); return Big{x, 0, 0, "ptr"}, x == 1 }

end_in_out_ptr :: proc(x: ^int, r: ^int) { fmt.println("end_in_out_ptr", x^, r^) }
@(deferred_in_out_by_ptr=end_in_out_ptr)
begin_in_out_ptr :: proc(x: int) -> int { fmt.println("begin_in_out_ptr", x); return x + 100 }

counter := 0
next :: proc() -> int { counter += 1; return counter }

early :: proc(n: int) -> int {
	begin_in(n, "early")
	if n > 5 {
		begin_in(n, "inner")
		return n * 2
	}
	for i in 0..<3 {
		begin_in(i, "loop")
		if i == 1 do break
	}
	return n
}

main :: proc() {
	{
		begin_in(1, "one")
		begin_in(next(), "two")
		v := 7
		begin_in(v, "var")
		v = 8
		fmt.println("body", v)
	}
	{
		b := begin_out(3)
		fmt.println("got", b)
		b.a = 99
	}
	{
		a, ok := begin_out2(5)
		fmt.println("got", a, ok)
		begin_out2(1)
	}
	{
		r := begin_in_out(4, Big{1, 2, 3, "x"})
		fmt.println("got", r)
	}
	begin_none(9)
	{
		x := 10
		bb := Big{7, 8, 9, "bb"}
		begin_in_ptr(x, bb)
		x = 11
		bb.a = 70
		fmt.println("body", x, bb)
	}
	{
		bg, ok := begin_out_ptr(1)
		fmt.println("got", bg, ok)
		r := begin_in_out_ptr(2)
		fmt.println("got", r)
	}
	fmt.println(early(10))
	fmt.println(early(2))

	// real core usage
	{
		runtime.DEFAULT_TEMP_ALLOCATOR_TEMP_GUARD()
		y := make([]int, 10, context.temp_allocator)
		y[0] = 1
		fmt.println("temp in", len(y))
	}
	sb := strings.builder_make()
	strings.write_string(&sb, "hi")
	fmt.println(strings.to_string(sb))
	fmt.println("done")
}
