package atom

import "core:fmt"
import "base:intrinsics"
import "core:sync"

Counter :: struct { n: int, flag: bool }

next :: proc(c: ^Counter) -> int { c.n += 100; return c.n }

main :: proc() {
	x: int = 5
	fmt.println(intrinsics.atomic_load(&x))
	intrinsics.atomic_store(&x, 7)
	fmt.println(x)
	fmt.println(intrinsics.atomic_add(&x, 3), x)
	fmt.println(intrinsics.atomic_sub(&x, 1), x)
	fmt.println(intrinsics.atomic_exchange(&x, 42), x)
	fmt.println(intrinsics.atomic_add_explicit(&x, 1, .Relaxed), x)
	fmt.println(intrinsics.atomic_load_explicit(&x, .Acquire))
	intrinsics.atomic_store_explicit(&x, 9, .Release)
	fmt.println(x)

	u: u8 = 0b1100
	fmt.println(intrinsics.atomic_and(&u, 0b1010), u)
	fmt.println(intrinsics.atomic_or(&u, 0b0001), u)
	fmt.println(intrinsics.atomic_xor(&u, 0b1111), u)
	fmt.println(intrinsics.atomic_nand(&u, 0b0110), u)

	i16v: i16 = -3
	fmt.println(intrinsics.atomic_sub(&i16v, 5), i16v)

	l: i64 = 1 << 40
	fmt.println(intrinsics.atomic_add(&l, 1), l)

	// compare exchange
	old, ok := intrinsics.atomic_compare_exchange_strong(&x, 9, 10)
	fmt.println(old, ok, x)
	old, ok = intrinsics.atomic_compare_exchange_strong(&x, 9, 11)
	fmt.println(old, ok, x)
	old2 := intrinsics.atomic_compare_exchange_weak(&x, 10, 12)
	fmt.println(old2, x)
	old, ok = intrinsics.atomic_compare_exchange_weak_explicit(&x, 12, 13, .Acq_Rel, .Acquire)
	fmt.println(old, ok, x)
	old, ok = intrinsics.atomic_compare_exchange_strong_explicit(&x, 12, 14, .Seq_Cst, .Seq_Cst)
	fmt.println(old, ok, x)

	// pointers and bools
	b := false
	fmt.println(intrinsics.atomic_exchange(&b, true), b)
	pb, pok := intrinsics.atomic_compare_exchange_strong(&b, true, false)
	fmt.println(pb, pok, b)
	ptr: rawptr = nil
	q := &x
	fmt.println(intrinsics.atomic_exchange(&ptr, rawptr(q)) == nil, ptr == rawptr(q))
	pp, ppok := intrinsics.atomic_compare_exchange_strong(&ptr, rawptr(q), nil)
	fmt.println(pp == rawptr(q), ppok, ptr == nil)

	// call in args
	c := Counter{}
	fmt.println(intrinsics.atomic_add(&c.n, next(&c)), c.n)
	o3, ok3 := intrinsics.atomic_compare_exchange_strong(&c.n, next(&c), next(&c))
	fmt.println(o3, ok3, c.n)

	intrinsics.atomic_thread_fence(.Seq_Cst)
	intrinsics.atomic_signal_fence(.Seq_Cst)
	intrinsics.cpu_relax()
	fmt.println(intrinsics.atomic_type_is_lock_free(int))

	// core:sync
	m: sync.Mutex
	sync.mutex_lock(&m)
	fmt.println(sync.mutex_try_lock(&m))
	sync.mutex_unlock(&m)
	fmt.println(sync.mutex_try_lock(&m))
	sync.mutex_unlock(&m)
	a: sync.Atomic_Mutex
	sync.atomic_mutex_lock(&a)
	sync.atomic_mutex_unlock(&a)
	once: sync.Once
	sync.once_do(&once, proc() { fmt.println("once") })
	sync.once_do(&once, proc() { fmt.println("once") })
	fmt.println("done")
}
