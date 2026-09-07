// Packs the parts of ODIN_ROOT that the browser compiler needs into a single
// file that the playground fetches once and serves to the compiler as its
// in-memory file system.
//
// Only files that the compiler could actually use for the `js_wasm32` target
// are packed: sources whose file name suffix or `#+build` tags exclude the
// target are dropped (the compiler applies the same rules again, so the packer
// only has to be conservative), together with the files they `#load`. `base`
// and `core` are packed in full, `vendor` only for the packages reachable from
// the Karl2D library and `vendor:box2d`, plus the wasm objects they link.
//
// The Karl2D library itself is packed under `karl2d/` (so `/odin/karl2d` in
// the browser). Its web capable examples are not packed: they are copied to a
// separate directory, one file each, and listed in `examples.json` so that the
// page can fetch an example (and its assets) only when it is selected.
//
// Pack format: "ODINPK01", then u32le file count, then for every file:
//   u32le path length, path (relative to ODIN_ROOT, forward slashes),
//   u32le data length, data
//
// Usage: odin run playground/pack_root -- <odin root> <output pack> <karl2d dir> <examples output dir>
package pack_root

import "core:fmt"
import "core:os"
import "core:path/filepath"
import "core:slice"
import "core:strings"

TARGET_OS   :: "js"
TARGET_ARCH :: "wasm32"

OS_NAMES   :: []string{"windows", "darwin", "linux", "freebsd", "openbsd", "netbsd", "wasi", "js", "orca", "freestanding"}
ARCH_NAMES :: []string{"amd64", "i386", "arm32", "arm64", "wasm32", "wasm64p32", "riscv64"}

// Parts of `core` that clearly have no use in the browser: the machine code
// libraries (17 MB of tables) and the packages for other operating systems
// (their files carry no target suffix, so the target rules keep them).
CORE_SKIP :: []string{
	"core/rexcode",
	"core/sys/darwin", "core/sys/freebsd", "core/sys/kqueue", "core/sys/linux", "core/sys/llvm",
	"core/sys/orca", "core/sys/posix", "core/sys/unix", "core/sys/windows",
}

Entry :: struct {
	path: string, // path inside the pack
	data: []byte,
}

Packer :: struct {
	entries: [dynamic]Entry,
	seen:    map[string]bool, // pack paths already added
}

join :: proc(elems: ..string) -> string {
	joined, _ := filepath.join(elems, context.temp_allocator)
	return joined
}

fatal :: proc(format: string, args: ..any) -> ! {
	fmt.eprintfln(format, ..args)
	os.exit(1)
}

is_os_name :: proc(s: string) -> bool {
	for n in OS_NAMES {
		if strings.equal_fold(n, s) {
			return true
		}
	}
	return false
}

is_arch_name :: proc(s: string) -> bool {
	for n in ARCH_NAMES {
		if strings.equal_fold(n, s) {
			return true
		}
	}
	return false
}

// Mirrors `is_excluded_target_filename` in src/build_settings.cpp: the last
// one or two `_` separated parts of the file name may name an OS and/or an
// architecture, and then the file only belongs to that target.
is_excluded_target_filename :: proc(file_name: string) -> bool {
	name := strings.trim_suffix(file_name, filepath.ext(file_name))
	if strings.has_prefix(name, ".") {
		return true
	}
	last_part :: proc(s: string) -> (part, rest: string) {
		i := strings.last_index_byte(s, '_')
		if i < 0 {
			return s, ""
		}
		return s[i+1:], s[:i]
	}
	str1, rest := last_part(name)
	str2, _ := last_part(rest)
	if str1 == name {
		return false
	}
	os1, arch1 := is_os_name(str1), is_arch_name(str1)
	os2, arch2 := is_os_name(str2), is_arch_name(str2)
	os_ok   :: proc(s: string) -> bool { return strings.equal_fold(s, TARGET_OS) }
	arch_ok :: proc(s: string) -> bool { return strings.equal_fold(s, TARGET_ARCH) }
	if os1 && arch2 {
		return !os_ok(str1) || !arch_ok(str2)
	} else if arch1 && os2 {
		return !arch_ok(str1) || !os_ok(str2)
	} else if os1 {
		return !os_ok(str1)
	} else if arch1 {
		return !arch_ok(str1)
	}
	return false
}

// Mirrors `parse_build_tag` in src/parser.cpp for one `#+build` tag: comma
// separated groups of space separated (possibly `!` negated) OS/arch names,
// the file is kept if any group matches. Anything the packer does not
// understand counts as a match, since the compiler decides in the end anyway.
build_tag_matches :: proc(tag: string) -> bool {
	for group in strings.split(tag, ",", context.temp_allocator) {
		group_ok := true
		for token in strings.fields(group, context.temp_allocator) {
			p := token
			notted := false
			if strings.has_prefix(p, "!") {
				notted = true
				p = p[1:]
			}
			if p == "" {
				continue
			}
			if p == "ignore" {
				group_ok = false
				continue
			}
			if p == "bedrock" {
				group_ok &&= notted
				continue
			}
			os_name := p
			subtarget := ""
			if i := strings.index_byte(p, ':'); i >= 0 {
				os_name = p[:i]
				subtarget = p[i+1:]
			}
			matches := false
			if is_os_name(os_name) {
				matches = strings.equal_fold(os_name, TARGET_OS)
				if subtarget != "" && !strings.equal_fold(subtarget, "generic") && !strings.equal_fold(subtarget, "default") {
					// Unknown subtarget: let the compiler decide
					continue
				}
			} else if is_arch_name(p) {
				matches = strings.equal_fold(p, TARGET_ARCH)
			} else {
				continue
			}
			group_ok &&= (matches != notted)
		}
		if group_ok {
			return true
		}
	}
	return false
}

// Looks at the `#+` file tags before the package declaration and reports
// whether the compiler could include the file when building for the target.
file_tags_allow :: proc(data: []byte) -> bool {
	text := string(data)
	for line in strings.split_lines_iterator(&text) {
		l := strings.trim_space(line)
		if l == "" || strings.has_prefix(l, "//") {
			continue
		}
		if !strings.has_prefix(l, "#+") {
			break // The package declaration or something the compiler rejects
		}
		tag := l[2:]
		if i := strings.index_byte(tag, '/'); i >= 0 {
			tag = tag[:i]
		}
		tag = strings.trim_space(tag)
		switch {
		case strings.has_prefix(tag, "build-project-name"):
			// Depends on the name of the directory being built, let the compiler decide
		case strings.has_prefix(tag, "build"):
			body := strings.trim_space(tag[len("build"):])
			if body != "" && !build_tag_matches(body) {
				return false
			}
		case strings.has_prefix(tag, "test"):
			return false // The playground never runs `odin test`
		case strings.has_prefix(tag, "ignore"):
			return false
		}
	}
	return true
}

// Whether an object file is one the wasm target can link.
is_wasm_object :: proc(name: string) -> bool {
	ext := filepath.ext(name)
	return (ext == ".o" || ext == ".a") && strings.contains(name, "wasm")
}

add_file :: proc(p: ^Packer, pack_path: string, data: []byte) -> bool {
	if pack_path in p.seen {
		return false
	}
	p.seen[strings.clone(pack_path)] = true
	append(&p.entries, Entry{path = strings.clone(pack_path), data = data})
	return true
}

read_file :: proc(path: string) -> []byte {
	data, err := os.read_entire_file(path, context.allocator)
	if err != nil {
		fatal("Cannot read file %s: %v", path, err)
	}
	return data
}

read_dir :: proc(path: string) -> []os.File_Info {
	infos, err := os.read_all_directory_by_path(path, context.temp_allocator)
	if err != nil {
		fatal("Cannot read directory %s: %v", path, err)
	}
	slice.sort_by(infos, proc(a, b: os.File_Info) -> bool { return a.name < b.name })
	return infos
}

// The string literals of the `#load("...")` expressions in a source file
load_paths :: proc(text: string) -> [dynamic]string {
	paths := make([dynamic]string, context.temp_allocator)
	text := text
	for {
		i := strings.index(text, "#load")
		if i < 0 {
			break
		}
		text = text[i+len("#load"):]
		rest := strings.trim_left_space(text)
		if !strings.has_prefix(rest, "(") {
			continue
		}
		rest = strings.trim_left_space(rest[1:])
		if !strings.has_prefix(rest, "\"") {
			continue
		}
		rest = rest[1:]
		end := strings.index_byte(rest, '"')
		if end < 0 {
			continue
		}
		append(&paths, rest[:end])
	}
	return paths
}

// Adds a source file if the target rules allow it, together with the files it
// `#load`s. `disk_dir` and `pack_dir` are the directory of the file on disk and
// in the pack, `pack_root` is where the `#load` paths may not escape from.
add_source :: proc(p: ^Packer, disk_root, disk_path, pack_root, pack_path: string) -> bool {
	if is_excluded_target_filename(filepath.base(disk_path)) {
		return false
	}
	data := read_file(disk_path)
	if !file_tags_allow(data) {
		delete(data)
		return false
	}
	if !add_file(p, pack_path, data) {
		return true
	}
	// Files referenced by `#load("...")`
	for loaded in load_paths(string(data)) {
		loaded_disk := join(filepath.dir(disk_path), loaded)
		loaded_rel, rel_err := filepath.rel(disk_root, loaded_disk, context.temp_allocator)
		if rel_err != nil || strings.has_prefix(loaded_rel, "..") || !os.is_file(loaded_disk) {
			continue
		}
		loaded_pack := loaded_rel if pack_root == "" else fmt.tprintf("%s/%s", pack_root, loaded_rel)
		add_file(p, loaded_pack, read_file(loaded_disk))
	}
	return true
}

// Packs every allowed source below `disk_root/rel_dir` under `pack_root/rel_dir`
// (recursively unless `recurse` is false), plus wasm objects. Directories
// whose name or pack path is in `skip_dirs` are left out.
collect :: proc(p: ^Packer, disk_root, pack_root, rel_dir: string, recurse: bool, skip_dirs: []string = {}) {
	dir := join(disk_root, rel_dir)
	any_source_kept := false
	smallest_excluded: os.File_Info
	for info in read_dir(dir) {
		rel := info.name if rel_dir == "" else fmt.tprintf("%s/%s", rel_dir, info.name)
		pack_path := rel if pack_root == "" else fmt.tprintf("%s/%s", pack_root, rel)
		if info.type == .Directory {
			// Hidden directories (.git, .claude, ...) are never sources
			if recurse && !strings.has_prefix(info.name, ".") && !slice.contains(skip_dirs, info.name) && !slice.contains(skip_dirs, pack_path) {
				collect(p, disk_root, pack_root, rel, recurse, skip_dirs)
			}
			continue
		}
		if filepath.ext(info.name) == ".odin" {
			if add_source(p, disk_root, info.fullpath, pack_root, pack_path) {
				any_source_kept = true
			} else if smallest_excluded.name == "" || info.size < smallest_excluded.size {
				smallest_excluded = info
			}
		} else if is_wasm_object(info.name) {
			add_file(p, pack_path, read_file(info.fullpath))
		}
	}
	// The compiler refuses to import a package directory without any .odin
	// file, even when the target rules exclude all of them, so keep one of the
	// excluded files (the compiler skips it again itself).
	if !any_source_kept && smallest_excluded.name != "" {
		rel := smallest_excluded.name if rel_dir == "" else fmt.tprintf("%s/%s", rel_dir, smallest_excluded.name)
		add_file(p, rel if pack_root == "" else fmt.tprintf("%s/%s", pack_root, rel), read_file(smallest_excluded.fullpath))
	}
}

// The `vendor:` packages imported by the packed sources under `pack_prefix`.
vendor_imports :: proc(p: ^Packer, pack_prefix: string, out: ^[dynamic]string) {
	for e in p.entries {
		if !strings.has_prefix(e.path, pack_prefix) || filepath.ext(e.path) != ".odin" {
			continue
		}
		text := string(e.data)
		for {
			i := strings.index(text, "\"vendor:")
			if i < 0 {
				break
			}
			text = text[i+len("\"vendor:"):]
			end := strings.index_byte(text, '"')
			if end < 0 {
				break
			}
			pkg := text[:end]
			if !slice.contains(out[:], pkg) {
				append(out, strings.clone(pkg))
			}
		}
	}
}

// Packs the vendor packages reachable from `roots` and the wasm objects that
// belong to them (in the package directory or a sibling `lib` directory, named
// after the package).
collect_vendor :: proc(p: ^Packer, odin_root: string, roots: []string) {
	pending := slice.clone_to_dynamic(roots)
	done: [dynamic]string
	for len(pending) > 0 {
		pkg := pop(&pending)
		if slice.contains(done[:], pkg) {
			continue
		}
		append(&done, pkg)
		rel := fmt.tprintf("vendor/%s", pkg)
		if !os.is_directory(join(odin_root, rel)) {
			fatal("Vendor package %s does not exist", pkg)
		}
		before := len(p.entries)
		collect(p, odin_root, "", rel, false)
		found: [dynamic]string
		for e in p.entries[before:] {
			if filepath.ext(e.path) != ".odin" {
				continue
			}
			text := string(e.data)
			for {
				i := strings.index(text, "\"vendor:")
				if i < 0 {
					break
				}
				text = text[i+len("\"vendor:"):]
				end := strings.index_byte(text, '"')
				if end < 0 {
					break
				}
				append(&pending, strings.clone(text[:end]))
			}
		}
		// Objects: `lib/` inside the package (box2d) or next to it (stb)
		pkg_name, _ := strings.replace_all(filepath.base(pkg), "-", "_", context.temp_allocator)
		for lib_dir in ([]string{fmt.tprintf("%s/lib", rel), fmt.tprintf("%s/lib", filepath.dir(rel))}) {
			full := join(odin_root, lib_dir)
			if !os.is_directory(full) {
				continue
			}
			for info in read_dir(full) {
				if info.type != .Directory && is_wasm_object(info.name) && strings.contains(info.name, pkg_name) {
					add_file(p, fmt.tprintf("%s/%s", lib_dir, info.name), read_file(info.fullpath))
				}
			}
		}
	}
	slice.sort(done[:])
	fmt.printfln("vendor packages: %s", strings.join(done[:], " ", context.temp_allocator))
}

Example :: struct {
	dir:   string,   // relative to the examples directory
	main:  string,   // the source file shown in the editor
	files: [dynamic]string,
}

// Finds the Karl2D examples that have the `init`/`step`/`shutdown` procedures
// the web entry point needs and copies them to `out_dir`.
collect_examples :: proc(examples_root, out_dir: string) -> [dynamic]Example {
	examples: [dynamic]Example
	walk :: proc(examples_root, rel_dir, out_dir: string, examples: ^[dynamic]Example) {
		dir := join(examples_root, rel_dir)
		main_file := ""
		has_sources := false
		for info in read_dir(dir) {
			if info.type == .Directory || filepath.ext(info.name) != ".odin" {
				continue
			}
			has_sources = true
			text := string(read_file(info.fullpath))
			if main_file == "" && strings.contains(text, "init :: proc") && strings.contains(text, "step :: proc") && strings.contains(text, "shutdown :: proc") {
				main_file = strings.clone(info.name)
			}
		}
		if has_sources && main_file == "" {
			return // desktop only example
		}
		if !has_sources {
			for info in read_dir(dir) {
				if info.type == .Directory && !slice.contains([]string{"bin", "build", "scraps"}, info.name) {
					walk(examples_root, info.name if rel_dir == "" else fmt.tprintf("%s/%s", rel_dir, info.name), out_dir, examples)
				}
			}
			return
		}
		ex := Example{dir = strings.clone(rel_dir), main = main_file}
		copy_files :: proc(dir, rel_dir, out_dir: string, ex: ^Example) {
			for info in read_dir(dir) {
				rel := info.name if rel_dir == "" else fmt.tprintf("%s/%s", rel_dir, info.name)
				if info.type == .Directory {
					if info.name != "bin" && info.name != "build" {
						copy_files(info.fullpath, rel, out_dir, ex)
					}
					continue
				}
				dst := join(out_dir, ex.dir, rel)
				if err := os.make_directory_all(filepath.dir(dst)); err != nil && err != .Exist {
					fatal("Cannot create directory for %s: %v", dst, err)
				}
				if err := os.copy_file(dst, info.fullpath); err != nil {
					fatal("Cannot copy %s to %s: %v", info.fullpath, dst, err)
				}
				append(&ex.files, strings.clone(rel))
			}
		}
		copy_files(dir, "", out_dir, &ex)
		// Assets borrowed from other examples (`#load("../basics/sixten.jpg")`)
		// are listed with their relative path and copied to where it leads
		for info in read_dir(dir) {
			if info.type == .Directory || filepath.ext(info.name) != ".odin" {
				continue
			}
			for loaded in load_paths(string(read_file(info.fullpath))) {
				if !strings.has_prefix(loaded, "../") || slice.contains(ex.files[:], loaded) {
					continue
				}
				src := join(dir, loaded)
				if !os.is_file(src) {
					continue
				}
				dst := join(out_dir, ex.dir, loaded)
				if err := os.make_directory_all(filepath.dir(dst)); err != nil && err != .Exist {
					fatal("Cannot create directory for %s: %v", dst, err)
				}
				if err := os.copy_file(dst, src); err != nil {
					fatal("Cannot copy %s to %s: %v", src, dst, err)
				}
				append(&ex.files, strings.clone(loaded))
			}
		}
		append(examples, ex)
	}
	walk(examples_root, "", out_dir, &examples)
	return examples
}

write_manifest :: proc(examples: []Example, path: string) {
	b := strings.builder_make()
	strings.write_string(&b, "[\n")
	for ex, i in examples {
		fmt.sbprintf(&b, "\t{{\"dir\": %q, \"main\": %q, \"files\": [", ex.dir, ex.main)
		for f, j in ex.files {
			fmt.sbprintf(&b, "%s%q", ", " if j > 0 else "", f)
		}
		fmt.sbprintf(&b, "]}%s\n", "," if i+1 < len(examples) else "")
	}
	strings.write_string(&b, "]\n")
	if err := os.write_entire_file(path, b.buf[:]); err != nil {
		fatal("Cannot write %s: %v", path, err)
	}
}

main :: proc() {
	if len(os.args) != 5 {
		fatal("Usage: pack_root <odin root> <output pack> <karl2d dir> <examples output dir>")
	}
	odin_root, out_path, karl2d_root, examples_out := os.args[1], os.args[2], os.args[3], os.args[4]
	// Directory listings give absolute paths, so the roots must be absolute too
	abs :: proc(path: string) -> string {
		absolute, err := os.get_absolute_path(path, context.allocator)
		if err != nil {
			fatal("Cannot resolve %s: %v", path, err)
		}
		return absolute
	}
	odin_root, karl2d_root = abs(odin_root), abs(karl2d_root)

	p: Packer
	collect(&p, odin_root, "", "base", true)
	collect(&p, odin_root, "", "core", true, CORE_SKIP)
	collect(&p, karl2d_root, "karl2d", "", true, {"examples", "bin", "build", "tests", "tools", "build_web", "platform_bindings"})

	roots: [dynamic]string
	append(&roots, "box2d")
	vendor_imports(&p, "karl2d/", &roots)
	collect_vendor(&p, odin_root, roots[:])

	if err := os.make_directory_all(examples_out); err != nil && err != .Exist {
		fatal("Cannot create %s: %v", examples_out, err)
	}
	examples := collect_examples(join(karl2d_root, "examples"), examples_out)
	write_manifest(examples[:], join(examples_out, "examples.json"))

	out: [dynamic]byte
	put_u32 :: proc(out: ^[dynamic]byte, v: u32) {
		append(out, byte(v), byte(v >> 8), byte(v >> 16), byte(v >> 24))
	}
	append(&out, "ODINPK01")
	put_u32(&out, u32(len(p.entries)))
	total := 0
	for e in p.entries {
		put_u32(&out, u32(len(e.path)))
		append(&out, e.path)
		put_u32(&out, u32(len(e.data)))
		append(&out, ..e.data)
		total += len(e.data)
	}
	if err := os.write_entire_file(out_path, out[:]); err != nil {
		fatal("Cannot write %s: %v", out_path, err)
	}
	fmt.printfln("%s: %d files, %d bytes; %d examples in %s", out_path, len(p.entries), total, len(examples), examples_out)
}
