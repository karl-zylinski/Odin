package main

import "core:fmt"
import "core:strings"
import "core:unicode/utf8"

main :: proc() {
	// builder
	b := strings.builder_make()
	defer strings.builder_destroy(&b)
	strings.write_string(&b, "hello")
	strings.write_rune(&b, ' ')
	strings.write_string(&b, "world")
	built := strings.to_string(b)
	fmt.println(built)

	// concatenate
	cat, cat_err := strings.concatenate([]string{"foo", "bar", "baz"})
	fmt.println(cat, cat_err)
	delete(cat)

	// split
	parts, split_err := strings.split("a,b,,c", ",")
	fmt.println(len(parts), parts, split_err)
	delete(parts)

	// join
	joined := strings.join([]string{"x", "y", "z"}, "-")
	fmt.println(joined)
	delete(joined)

	// contains (substr matching at position 0, and single-byte substr - see
	// phase2_strings_rk_pow_default for a wasm-backend bug affecting
	// multi-byte substrings that match away from position 0)
	fmt.println(strings.contains("hello world", "hello"), strings.contains("hello world", "q"))

	// index
	fmt.println(strings.index("hello world", "hello"), strings.index("hello world", "q"))

	// trim / trim_space / trim_left / trim_right
	fmt.println(strings.trim("**hi**", "*"))
	fmt.println(strings.trim_space("  padded  "))
	fmt.println(strings.trim_left("xxhixx", "x"))
	fmt.println(strings.trim_right("xxhixx", "x"))

	// to_upper / to_lower
	up := strings.to_upper("Hello World")
	fmt.println(up)
	delete(up)
	lo := strings.to_lower("Hello World")
	fmt.println(lo)
	delete(lo)

	// clone
	cl := strings.clone("cloned string")
	fmt.println(cl)
	delete(cl)

	// has_prefix / has_suffix
	fmt.println(strings.has_prefix("filename.odin", "filename"), strings.has_suffix("filename.odin", ".odin"))

	// replace
	rep, was_alloc := strings.replace("banana", "a", "o", -1)
	fmt.println(rep, was_alloc)
	delete(rep)

	// fields
	flds, fields_err := strings.fields("  the quick  brown fox  ")
	fmt.println(len(flds), flds, fields_err)
	delete(flds)

	// iterate runes with index
	sum_idx := 0
	rune_count := 0
	for r, i in "héllo" {
		sum_idx += i
		rune_count += 1
		fmt.println(i, r)
	}
	fmt.println("sum_idx", sum_idx, "count", rune_count)

	// utf8 encode_rune / decode_rune / rune_count
	buf, n := utf8.encode_rune('é')
	fmt.println(n, buf[0], buf[1])
	r, size := utf8.decode_rune("é")
	fmt.println(r, size)
	fmt.println(utf8.rune_count("héllo"))

	// string comparison
	fmt.println("abc" == "abc", "abc" == "abd", "abc" < "abd", "abd" < "abc")
	fmt.println(strings.compare("abc", "abd"), strings.compare("abc", "abc"), strings.compare("abd", "abc"))

	// substring slicing
	s := "hello world"
	fmt.println(s[2:5])
	fmt.println(s[:5])
	fmt.println(s[6:])

	// multi-byte utf-8 literal
	multi := "héllo wörld ünïcode"
	fmt.println(multi)
	fmt.println(len(multi), utf8.rune_count(multi))
}
