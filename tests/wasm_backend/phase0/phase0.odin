// Phase 0 test program for the direct wasm backend.
// Every exported procedure is invoked by tests/wasm_backend/run.sh with both
// the LLVM backend and the wasm backend and the results are compared.
package phase0

@(export)
add :: proc "c" (a, b: i32) -> i32 {
	return a + b
}

@(export)
arith :: proc "c" (a, b: i32) -> i32 {
	x := a * 3 - b / 2
	x += a % 7
	x -= b & 0xff
	x = x | (a ~ b)
	x = x &~ 8
	x <<= 1
	x >>= 2
	return -x
}

@(export)
fib :: proc "c" (n: i32) -> i32 {
	if n < 2 {
		return n
	}
	return fib(n-1) + fib(n-2)
}

@(export)
sum_to :: proc "c" (n: i32) -> i64 {
	total: i64
	for i := i32(0); i < n; i += 1 {
		if i % 2 == 0 {
			continue
		}
		total += i64(i) * 2
	}
	return total
}

@(export)
collatz :: proc "c" (n0: i32) -> i32 {
	n := n0
	steps: i32 = 0
	for n != 1 {
		if n % 2 == 0 {
			n /= 2
		} else {
			n = 3*n + 1
		}
		steps += 1
		if steps > 1000 {
			break
		}
	}
	return steps
}

@(export)
nested :: proc "c" (n: i32) -> i32 {
	count: i32
	outer: for i := i32(0); i < n; i += 1 {
		for j := i32(0); j < n; j += 1 {
			if j > i {
				continue outer
			}
			if i * j > 50 {
				break outer
			}
			count += 1
		}
	}
	return count
}

@(export)
fmul :: proc "c" (a, b: f32) -> f32 {
	return a * b + 0.5
}

@(export)
fmix :: proc "c" (a: f64, b: i32) -> f64 {
	x := a / 3.0
	if x > f64(b) {
		return x - f64(b)
	}
	return f64(b) - x
}

@(export)
trunc_f :: proc "c" (a: f64) -> i32 {
	return i32(a) + i32(f32(a) * 2)
}

@(export)
narrow :: proc "c" (a: i32) -> i32 {
	x := u8(a)
	y := i8(a)
	z := i16(a) + 1
	return i32(x) + i32(y) * 256 + i32(z) * 65536
}

@(export)
unsigned_div :: proc "c" (a, b: u32) -> u32 {
	return a / b + a % b + (a >> 1) + (a < b ? 1 : 0)
}

@(export)
big :: proc "c" (a, b: u64) -> u64 {
	return a * b ~ (a >> 3) - b
}

@(export)
logic :: proc "c" (a, b: i32) -> i32 {
	r: i32 = 0
	if a > 0 && b > 0 {
		r += 1
	}
	if a > 0 || b > 0 {
		r += 2
	}
	if !(a == b) {
		r += 4
	}
	return r
}

helper :: proc "contextless" (x: i32) -> i32 {
	return x * x
}

odin_helper :: proc "contextless" (x: i32) -> i32 {
	return x + 1
}

@(export)
calls :: proc "c" (a: i32) -> i32 {
	return helper(a) + odin_helper(a)
}

@(export)
named_result :: proc "c" (a: i32) -> (r: i32) {
	r = a * 2
	if a > 10 {
		return
	}
	r += 1
	return
}

@(export)
shifts :: proc "c" (a: i32, s: u32) -> i32 {
	return (a << s) + (a >> s)
}

@(export)
floored_mod :: proc "c" (a, b: i32) -> i32 {
	return a %% b
}
