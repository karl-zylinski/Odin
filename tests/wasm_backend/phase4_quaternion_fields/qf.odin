package main

import "core:fmt"

S64 :: struct {
	q: quaternion64,
}

S128 :: struct {
	q: quaternion128,
}

S256 :: struct {
	q: quaternion256,
}

sum3_16 :: proc(v: [3]f16) -> f16 {
	return v[0] + v[1] + v[2]
}

sum3_32 :: proc(v: [3]f32) -> f32 {
	return v[0] + v[1] + v[2]
}

sum3_64 :: proc(v: [3]f64) -> f64 {
	return v[0] + v[1] + v[2]
}

make_q64 :: proc() -> quaternion64 {
	return quaternion(w = 1, x = 2, y = 2.5, z = -3)
}

make_q128 :: proc() -> quaternion128 {
	return quaternion(w = 1, x = 2, y = 2.5, z = -3)
}

make_q256 :: proc() -> quaternion256 {
	return quaternion(w = 1, x = 2, y = 2.5, z = -3)
}

read_w64 :: proc(q: quaternion64) -> f16 {
	return q.w
}

read_w128 :: proc(q: quaternion128) -> f32 {
	return q.w
}

read_w256 :: proc(q: quaternion256) -> f64 {
	return q.w
}

main :: proc() {
	// --- quaternion64 ---
	{
		q: quaternion64 = quaternion(w = 1, x = 2, y = 2.5, z = -3)
		fmt.println(q.x, q.y, q.z, q.w)
		fmt.println(q.xyz)

		q.x = 2
		q.w = -3
		fmt.println(q.x, q.w)

		p := &q.y
		p^ = 0.5
		fmt.println(q.y)

		v := q.xyz
		fmt.println(v[1])
		fmt.println(sum3_16(q.xyz))
		fmt.println(q.xyz[1])

		qarr: [2]quaternion64
		qarr[1] = quaternion(w = 4, x = 1, y = 2.5, z = -3)
		fmt.println(qarr[1].z, qarr[1].w, qarr[0].w)
		qarr[1].z = 2
		fmt.println(qarr[1].z, qarr[1].xyz)

		s: S64
		s.q = quaternion(w = 1, x = 2, y = 3, z = 4)
		fmt.println(s.q.w)
		s.q.z = 0.5
		fmt.println(s.q.z)

		arr: [2]S64
		arr[1].q = quaternion(w = 1, x = 2, y = 3, z = 4)
		arr[1].q.z = 0.5
		fmt.println(arr[1].q.z)

		sp: ^S64 = &s
		fmt.println(sp.q.x)
		sp.q.x = -3
		fmt.println(s.q.x)

		qr := make_q64()
		fmt.println(qr.w, qr.x, qr.y, qr.z)
		fmt.println(read_w64(qr))
	}

	// --- quaternion128 ---
	{
		q: quaternion128 = quaternion(w = 1, x = 2, y = 2.5, z = -3)
		fmt.println(q.x, q.y, q.z, q.w)
		fmt.println(q.xyz)

		q.x = 2
		q.w = -3
		fmt.println(q.x, q.w)

		p := &q.y
		p^ = 0.5
		fmt.println(q.y)

		v := q.xyz
		fmt.println(v[1])
		fmt.println(sum3_32(q.xyz))
		fmt.println(q.xyz[1])

		qarr: [2]quaternion128
		qarr[1] = quaternion(w = 4, x = 1, y = 2.5, z = -3)
		fmt.println(qarr[1].z, qarr[1].w, qarr[0].w)
		qarr[1].z = 2
		fmt.println(qarr[1].z, qarr[1].xyz)

		s: S128
		s.q = quaternion(w = 1, x = 2, y = 3, z = 4)
		fmt.println(s.q.w)
		s.q.z = 0.5
		fmt.println(s.q.z)

		arr: [2]S128
		arr[1].q = quaternion(w = 1, x = 2, y = 3, z = 4)
		arr[1].q.z = 0.5
		fmt.println(arr[1].q.z)

		sp: ^S128 = &s
		fmt.println(sp.q.x)
		sp.q.x = -3
		fmt.println(s.q.x)

		qr := make_q128()
		fmt.println(qr.w, qr.x, qr.y, qr.z)
		fmt.println(read_w128(qr))
	}

	// --- quaternion256 ---
	{
		q: quaternion256 = quaternion(w = 1, x = 2, y = 2.5, z = -3)
		fmt.println(q.x, q.y, q.z, q.w)
		fmt.println(q.xyz)

		q.x = 2
		q.w = -3
		fmt.println(q.x, q.w)

		p := &q.y
		p^ = 0.5
		fmt.println(q.y)

		v := q.xyz
		fmt.println(v[1])
		fmt.println(sum3_64(q.xyz))
		fmt.println(q.xyz[1])

		qarr: [2]quaternion256
		qarr[1] = quaternion(w = 4, x = 1, y = 2.5, z = -3)
		fmt.println(qarr[1].z, qarr[1].w, qarr[0].w)
		qarr[1].z = 2
		fmt.println(qarr[1].z, qarr[1].xyz)

		s: S256
		s.q = quaternion(w = 1, x = 2, y = 3, z = 4)
		fmt.println(s.q.w)
		s.q.z = 0.5
		fmt.println(s.q.z)

		arr: [2]S256
		arr[1].q = quaternion(w = 1, x = 2, y = 3, z = 4)
		arr[1].q.z = 0.5
		fmt.println(arr[1].q.z)

		sp: ^S256 = &s
		fmt.println(sp.q.x)
		sp.q.x = -3
		fmt.println(s.q.x)

		qr := make_q256()
		fmt.println(qr.w, qr.x, qr.y, qr.z)
		fmt.println(read_w256(qr))
	}
}
