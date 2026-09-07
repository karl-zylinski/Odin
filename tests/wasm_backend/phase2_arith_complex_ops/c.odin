package main

import "core:fmt"

main :: proc() {
	// Arithmetic (+, *) and abs() on complex64/complex128 values.
	// Construction/printing/real()/imag() work fine on the wasm backend
	// (see phase2_arith); these operations do not:
	//   Error: wasm backend: unsupported type 'complex64'
	//   Error: wasm backend: unsupported conversion from 'complex64' to 'f32'
	c64a: complex64 = complex(1.5, 2.5)
	c64b: complex64 = complex(3.0, -1.0)
	fmt.println("c64 add:", c64a + c64b)
	fmt.println("c64 mul:", c64a * c64b)
	fmt.println("c64 abs:", abs(c64a))

	c128a: complex128 = complex(1.5, 2.5)
	c128b: complex128 = complex(3.0, -1.0)
	fmt.println("c128 add:", c128a + c128b)
	fmt.println("c128 mul:", c128a * c128b)
	fmt.println("c128 abs:", abs(c128a))
}
