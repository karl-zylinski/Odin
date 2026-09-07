package ctx
import "core:fmt"

foo2 :: proc() -> (a, b: int) {
	a = 321
	b = 567
	return b, a
}
foo3 :: proc() -> (a, b, c: int) {
	a = 1; b = 2; c = 3
	return c, a, b
}
foo4 :: proc() -> (a, b: int) {
	defer fmt.println("defer sees", a, b)
	a = 1; b = 2
	return b, a
}

main :: proc() {
	fmt.println(foo2())
	fmt.println(foo3())
	fmt.println(foo4())
	c := context
	fmt.println(context.user_index)
	context.user_index = 456
	fmt.println(context.user_index)
	{
		context.user_index = 123
		fmt.println(context.user_index)
	}
	fmt.println(context.user_index)
	{
		context.user_ptr = nil
		context.user_index = 123
		fmt.println(context.user_index)
		{
			context.user_index = 7
			defer fmt.println("defer", context.user_index)
			fmt.println(context.user_index)
		}
		fmt.println(context.user_index)
	}
	fmt.println(context.user_index)
	if true {
		context.user_index = 1
	}
	for i in 0..<2 {
		context.user_index = i
		fmt.println(context.user_index)
	}
	fmt.println(context.user_index)
	_ = c
}
