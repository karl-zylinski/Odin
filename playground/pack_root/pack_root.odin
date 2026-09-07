// Packs the parts of ODIN_ROOT that the compiler reads (the `base`, `core`
// and `vendor` sources and the wasm objects it links against) into a single
// file that the playground fetches once and serves to the compiler as its
// in-memory file system.
//
// Format: "ODINPK01", then u32le file count, then for every file:
//   u32le path length, path (relative to ODIN_ROOT, forward slashes),
//   u32le data length, data
//
// Usage: odin run playground/pack_root -- <odin root> <output file>
package pack_root

import "core:fmt"
import "core:os"
import "core:path/filepath"
import "core:strings"

Entry :: struct {
	path: string,
	data: []byte,
}

collect :: proc(root, dir: string, entries: ^[dynamic]Entry) {
	full, _ := filepath.join({root, dir}, context.temp_allocator)
	infos, read_err := os.read_all_directory_by_path(full, context.temp_allocator)
	if read_err != nil {
		fmt.eprintfln("Cannot read directory %s: %v", full, read_err)
		os.exit(1)
	}
	for info in infos {
		rel := strings.concatenate({dir, "/", info.name})
		if info.type == .Directory {
			collect(root, rel, entries)
			continue
		}
		ext := filepath.ext(info.name)
		keep := ext == ".odin"
		if ext == ".o" && strings.contains(info.name, "wasm") {
			keep = true
		}
		if !keep {
			continue
		}
		data, data_err := os.read_entire_file(info.fullpath, context.allocator)
		if data_err != nil {
			fmt.eprintfln("Cannot read file %s: %v", info.fullpath, data_err)
			os.exit(1)
		}
		append(entries, Entry{path = rel, data = data})
	}
}

main :: proc() {
	if len(os.args) != 3 {
		fmt.eprintln("Usage: pack_root <odin root> <output file>")
		os.exit(1)
	}
	root := os.args[1]
	entries: [dynamic]Entry
	collect(root, "base", &entries)
	collect(root, "core", &entries)
	collect(root, "vendor", &entries)

	out: [dynamic]byte
	put_u32 :: proc(out: ^[dynamic]byte, v: u32) {
		append(out, byte(v), byte(v >> 8), byte(v >> 16), byte(v >> 24))
	}
	append(&out, "ODINPK01")
	put_u32(&out, u32(len(entries)))
	total := 0
	for e in entries {
		put_u32(&out, u32(len(e.path)))
		append(&out, e.path)
		put_u32(&out, u32(len(e.data)))
		append(&out, ..e.data)
		total += len(e.data)
	}
	if write_err := os.write_entire_file(os.args[2], out[:]); write_err != nil {
		fmt.eprintfln("Cannot write %s: %v", os.args[2], write_err)
		os.exit(1)
	}
	fmt.printfln("%s: %d files, %d bytes", os.args[2], len(entries), total)
}
