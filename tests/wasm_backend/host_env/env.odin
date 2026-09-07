// Host math procedures for differential tests: both builds import the same
// implementations, so exact accuracy does not matter, only determinism.
package host_env

TAU :: 6.283185307179586

reduce :: proc "c" (x: f64) -> f64 {
	k := f64(i64(x / TAU))
	r := x - k*TAU
	if r > TAU/2 { r -= TAU }
	if r < -TAU/2 { r += TAU }
	return r
}

is_finite :: proc "c" (x: f64) -> bool {
	return x == x && x != INF && x != -INF
}
INF :: f64(1e308) * 10

@(export) sin :: proc "c" (x: f64) -> f64 {
	if !is_finite(x) { return x - x } // NaN
	r := reduce(x)
	term := r
	sum := r
	for i := 1; i < 12; i += 1 {
		term *= -r*r / f64((2*i)*(2*i+1))
		sum += term
	}
	return sum
}
@(export) cos :: proc "c" (x: f64) -> f64 {
	if !is_finite(x) { return x - x } // NaN
	r := reduce(x)
	term := 1.0
	sum := 1.0
	for i := 1; i < 12; i += 1 {
		term *= -r*r / f64((2*i-1)*(2*i))
		sum += term
	}
	return sum
}
@(export) exp :: proc "c" (x: f64) -> f64 {
	if x != x { return x }
	if x > 710 { return INF }
	if x < -746 { return 0 }
	// exp(x) = exp(x/2^k)^(2^k)
	k := 0
	y := x
	for y > 0.5 || y < -0.5 { y *= 0.5; k += 1 }
	term := 1.0
	sum := 1.0
	for i := 1; i < 20; i += 1 {
		term *= y / f64(i)
		sum += term
	}
	for _ in 0..<k { sum *= sum }
	return sum
}
@(export) log :: proc "c" (x: f64) -> f64 {
	if x != x || x == INF { return x }
	if x == 0 { return -INF }
	if x < 0 { return x - x } // NaN
	// ln(x) = 2 atanh((x-1)/(x+1)), after scaling x into [0.5, 2)
	k := 0
	y := x
	for y >= 2 { y *= 0.5; k += 1 }
	for y < 0.5 { y *= 2; k -= 1 }
	z := (y-1)/(y+1)
	term := z
	sum := z
	for i := 1; i < 30; i += 1 {
		term *= z*z
		sum += term / f64(2*i+1)
	}
	return 2*sum + f64(k)*0.6931471805599453
}
@(export) pow :: proc "c" (x, y: f64) -> f64 {
	if x == 0 { return 0 }
	return exp(y * log(x))
}
@(export) sinf :: proc "c" (x: f32) -> f32 { return f32(sin(f64(x))) }
@(export) cosf :: proc "c" (x: f32) -> f32 { return f32(cos(f64(x))) }
@(export) expf :: proc "c" (x: f32) -> f32 { return f32(exp(f64(x))) }
@(export) logf :: proc "c" (x: f32) -> f32 { return f32(log(f64(x))) }
@(export) powf :: proc "c" (x, y: f32) -> f32 { return f32(pow(f64(x), f64(y))) }
