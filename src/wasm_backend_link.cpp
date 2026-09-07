// Static linking of relocatable WebAssembly object files.
//
// `foreign import lib "x.o"` on a wasm target names an object produced by
// clang (vendor:stb, vendor:box2d, ...). With LLVM these are handed to
// wasm-ld; here the object's functions and data are merged into the module
// directly, following the WebAssembly object file linking conventions
// (tool-conventions/Linking.md): the `linking` custom section describes the
// symbols and data segments, the `reloc.CODE`/`reloc.DATA` sections list the
// places whose indices/addresses must be rewritten.
//
// The object's imports are resolved against the procedures of this module by
// link name (vendor:libc-shim provides malloc, memcpy, ... that way), against
// the other objects, and finally become `env` imports. The Odin foreign
// procedures declared against the object are aliased to its functions. Only
// the object functions reachable from those aliases are written out
// (wb_link_gc), like wasm-ld's --gc-sections.

enum wbSymbolKind : u8 {
	wbSym_Function = 0,
	wbSym_Data     = 1,
	wbSym_Global   = 2,
	wbSym_Section  = 3,
	wbSym_Event    = 4,
	wbSym_Table    = 5,
};

enum wbSymbolFlag : u32 {
	wbSymFlag_Weak          = 0x001,
	wbSymFlag_BindingLocal  = 0x002,
	wbSymFlag_Hidden        = 0x004,
	wbSymFlag_Undefined     = 0x010,
	wbSymFlag_Exported      = 0x020,
	wbSymFlag_ExplicitName  = 0x040,
	wbSymFlag_NoStrip       = 0x080,
	wbSymFlag_TLS           = 0x100,
	wbSymFlag_Absolute      = 0x200,
};

enum wbRelocType : u8 {
	wbReloc_FunctionIndexLEB = 0,
	wbReloc_TableIndexSLEB   = 1,
	wbReloc_TableIndexI32    = 2,
	wbReloc_MemoryAddrLEB    = 3,
	wbReloc_MemoryAddrSLEB   = 4,
	wbReloc_MemoryAddrI32    = 5,
	wbReloc_TypeIndexLEB     = 6,
	wbReloc_GlobalIndexLEB   = 7,
	wbReloc_FunctionOffsetI32 = 8,
	wbReloc_SectionOffsetI32 = 9,
	wbReloc_EventIndexLEB    = 10,
	wbReloc_MemoryAddrRelSLEB = 11,
	wbReloc_TableIndexRelSLEB = 12,
	wbReloc_GlobalIndexI32   = 13,
	wbReloc_TableNumberLEB   = 20,
};

enum wbLinkingSubsection : u8 {
	wbLinking_SegmentInfo = 5,
	wbLinking_InitFuncs   = 6,
	wbLinking_ComdatInfo  = 7,
	wbLinking_SymbolTable = 8,
};

struct wbObjSymbol {
	wbSymbolKind kind;
	u32    flags;
	String name;
	u32    index;   // function/global/table index in the object's index space
	u32    segment; // data symbols: segment index
	u32    offset;  // data symbols: offset within the segment
	u32    size;

	wbProcedure *proc; // resolved function
};

struct wbObjReloc {
	wbRelocType type;
	u32 offset; // relative to the start of the section contents
	u32 index;  // symbol index (type index for wbReloc_TypeIndexLEB)
	i32 addend;
};

struct wbObjSegment {
	String name;
	u32    align;      // log2
	u32    bytes_start; // offset of the payload within the data section contents
	u32    size;
	u32    addr;       // absolute address in the module's memory
};

struct wbObjFunction {
	wbProcedure *proc;
	u32 body_start; // offset of the body (local declarations) within the code section contents
	u32 body_end;
};

struct wbObjImport {
	String module;
	String name;
	u8     kind;
	u32    type_index; // functions
};

struct wbObject {
	String   path;
	u8 *     data;
	isize    size;

	Array<u32>           type_map; // object type index -> module type index
	Array<wbObjImport>   imports;
	Array<wbProcedure *> functions; // object function index space (imports first)
	u32                  import_function_count;
	Array<wbObjFunction> defined;   // functions with bodies, in order
	Array<wbObjSegment>  segments;
	Array<wbObjSymbol>   symbols;
	Array<wbObjReloc>    code_relocs;
	Array<wbObjReloc>    data_relocs;
	Array<u32>           init_funcs; // symbol indices

	u32 code_section_index; // section ordinal, for matching the reloc sections
	u32 data_section_index;
	isize code_contents;    // offset of the code section contents in the file
	isize data_contents;

	bool failed;
};

struct wbObjReader {
	u8 *  data;
	isize len;
	isize pos;
	bool  error;
};

gb_internal u8 wb_obj_byte(wbObjReader *r) {
	if (r->pos >= r->len) {
		r->error = true;
		return 0;
	}
	return r->data[r->pos++];
}

gb_internal u64 wb_obj_uleb(wbObjReader *r) {
	u64 result = 0;
	u32 shift = 0;
	for (;;) {
		u8 b = wb_obj_byte(r);
		result |= cast(u64)(b & 0x7f) << shift;
		if ((b & 0x80) == 0 || r->error) {
			break;
		}
		shift += 7;
		if (shift > 63) {
			r->error = true;
			break;
		}
	}
	return result;
}

gb_internal i64 wb_obj_sleb(wbObjReader *r) {
	i64 result = 0;
	u32 shift = 0;
	u8 b = 0;
	do {
		b = wb_obj_byte(r);
		result |= cast(i64)(b & 0x7f) << shift;
		shift += 7;
	} while ((b & 0x80) != 0 && !r->error && shift < 64);
	if (shift < 64 && (b & 0x40) != 0) {
		result |= -(cast(i64)1 << shift);
	}
	return result;
}

gb_internal String wb_obj_name(wbObjReader *r) {
	u64 len = wb_obj_uleb(r);
	if (r->pos + cast(isize)len > r->len) {
		r->error = true;
		return {};
	}
	String s = make_string(r->data + r->pos, cast(isize)len);
	r->pos += cast(isize)len;
	return s;
}

gb_internal void wb_obj_skip_limits(wbObjReader *r) {
	u8 flags = wb_obj_byte(r);
	wb_obj_uleb(r);
	if (flags & 0x01) {
		wb_obj_uleb(r);
	}
}

// Skips a constant expression (`i32.const N end` and friends)
gb_internal i64 wb_obj_const_expr(wbObjReader *r) {
	i64 value = 0;
	for (;;) {
		u8 op = wb_obj_byte(r);
		if (r->error) {
			return 0;
		}
		switch (op) {
		case 0x0b: return value;                                   // end
		case 0x41: value = wb_obj_sleb(r); break;                   // i32.const
		case 0x42: value = wb_obj_sleb(r); break;                   // i64.const
		case 0x43: r->pos += 4; break;                              // f32.const
		case 0x44: r->pos += 8; break;                              // f64.const
		case 0x23: wb_obj_uleb(r); break;                           // global.get
		case 0xd0: wb_obj_byte(r); break;                           // ref.null
		case 0xd2: wb_obj_uleb(r); break;                           // ref.func
		default:
			r->error = true;
			return 0;
		}
	}
}

gb_internal void wb_obj_error(wbModule *m, wbObject *obj, char const *msg) {
	if (!obj->failed) {
		gb_printf_err("wasm backend: %s: %s\n", msg, cast(char const *)obj->path.text);
		m->error_count += 1;
		obj->failed = true;
	}
}

gb_internal bool wb_obj_parse_type_section(wbModule *m, wbObject *obj, wbObjReader *r) {
	u64 count = wb_obj_uleb(r);
	for (u64 i = 0; i < count && !r->error; i++) {
		if (wb_obj_byte(r) != 0x60) {
			return false;
		}
		wbFuncType ft = {};
		array_init(&ft.params,  m->allocator);
		array_init(&ft.results, m->allocator);
		u64 pc = wb_obj_uleb(r);
		for (u64 j = 0; j < pc && !r->error; j++) {
			array_add(&ft.params, cast(wbValType)wb_obj_byte(r));
		}
		u64 rc = wb_obj_uleb(r);
		for (u64 j = 0; j < rc && !r->error; j++) {
			array_add(&ft.results, cast(wbValType)wb_obj_byte(r));
		}
		array_add(&obj->type_map, wb_add_functype(m, ft));
	}
	return !r->error;
}

gb_internal bool wb_obj_parse_import_section(wbModule *m, wbObject *obj, wbObjReader *r) {
	u64 count = wb_obj_uleb(r);
	for (u64 i = 0; i < count && !r->error; i++) {
		wbObjImport imp = {};
		imp.module = wb_obj_name(r);
		imp.name   = wb_obj_name(r);
		imp.kind   = wb_obj_byte(r);
		switch (imp.kind) {
		case 0x00: // function
			imp.type_index = cast(u32)wb_obj_uleb(r);
			array_add(&obj->functions, cast(wbProcedure *)nullptr);
			obj->import_function_count += 1;
			break;
		case 0x01: // table
			wb_obj_byte(r);
			wb_obj_skip_limits(r);
			break;
		case 0x02: // memory
			wb_obj_skip_limits(r);
			break;
		case 0x03: // global
			wb_obj_byte(r);
			wb_obj_byte(r);
			break;
		case 0x04: // tag
			wb_obj_byte(r);
			wb_obj_uleb(r);
			break;
		default:
			return false;
		}
		array_add(&obj->imports, imp);
	}
	return !r->error;
}

gb_internal bool wb_obj_parse_function_section(wbModule *m, wbObject *obj, wbObjReader *r) {
	u64 count = wb_obj_uleb(r);
	for (u64 i = 0; i < count && !r->error; i++) {
		u64 type_index = wb_obj_uleb(r);
		if (type_index >= cast(u64)obj->type_map.count) {
			return false;
		}
		wbProcedure *p = wb_alloc_procedure(m, {}); // named from the symbol table
		p->is_raw_body = true;
		p->type_index = obj->type_map[cast(isize)type_index];
		wbFuncType const &ft = m->types[p->type_index];
		for (wbValType vt : ft.results) {
			array_add(&p->results, vt);
		}
		wbObjFunction f = {};
		f.proc = p;
		array_add(&obj->defined, f);
		array_add(&obj->functions, p);
	}
	return !r->error;
}

gb_internal bool wb_obj_parse_code_section(wbModule *m, wbObject *obj, wbObjReader *r, isize contents_start) {
	u64 count = wb_obj_uleb(r);
	if (count != cast(u64)obj->defined.count) {
		return false;
	}
	for (u64 i = 0; i < count && !r->error; i++) {
		u64 size = wb_obj_uleb(r);
		if (r->pos + cast(isize)size > r->len) {
			return false;
		}
		wbObjFunction *f = &obj->defined[cast(isize)i];
		f->body_start = cast(u32)(r->pos - contents_start);
		f->body_end   = cast(u32)(f->body_start + size);
		wb_bytes(&f->proc->code, r->data + r->pos, cast(isize)size);
		r->pos += cast(isize)size;
	}
	return !r->error;
}

gb_internal bool wb_obj_parse_data_section(wbModule *m, wbObject *obj, wbObjReader *r, isize contents_start) {
	u64 count = wb_obj_uleb(r);
	for (u64 i = 0; i < count && !r->error; i++) {
		u64 flags = wb_obj_uleb(r);
		if (flags == 2) {
			wb_obj_uleb(r); // memory index
		}
		if (flags != 1) {
			wb_obj_const_expr(r); // offset, meaningless before linking
		}
		u64 size = wb_obj_uleb(r);
		if (r->pos + cast(isize)size > r->len) {
			return false;
		}
		wbObjSegment seg = {};
		seg.bytes_start = cast(u32)(r->pos - contents_start);
		seg.size        = cast(u32)size;
		array_add(&obj->segments, seg);
		r->pos += cast(isize)size;
	}
	return !r->error;
}

gb_internal bool wb_obj_parse_linking_section(wbModule *m, wbObject *obj, wbObjReader *r, isize end) {
	u64 version = wb_obj_uleb(r);
	if (version != 2) {
		wb_obj_error(m, obj, "unsupported object file linking metadata version");
		return false;
	}
	while (r->pos < end && !r->error) {
		u8 type = wb_obj_byte(r);
		u64 size = wb_obj_uleb(r);
		isize sub_end = r->pos + cast(isize)size;
		switch (type) {
		case wbLinking_SegmentInfo: {
			u64 count = wb_obj_uleb(r);
			for (u64 i = 0; i < count && !r->error; i++) {
				String name = wb_obj_name(r);
				u32 align = cast(u32)wb_obj_uleb(r);
				wb_obj_uleb(r); // flags
				if (i < cast(u64)obj->segments.count) {
					obj->segments[cast(isize)i].name  = name;
					obj->segments[cast(isize)i].align = align;
				}
			}
			break;
		}
		case wbLinking_InitFuncs: {
			u64 count = wb_obj_uleb(r);
			for (u64 i = 0; i < count && !r->error; i++) {
				wb_obj_uleb(r); // priority
				array_add(&obj->init_funcs, cast(u32)wb_obj_uleb(r));
			}
			break;
		}
		case wbLinking_SymbolTable: {
			u64 count = wb_obj_uleb(r);
			for (u64 i = 0; i < count && !r->error; i++) {
				wbObjSymbol sym = {};
				sym.kind  = cast(wbSymbolKind)wb_obj_byte(r);
				sym.flags = cast(u32)wb_obj_uleb(r);
				bool defined = (sym.flags & wbSymFlag_Undefined) == 0;
				switch (sym.kind) {
				case wbSym_Function:
				case wbSym_Global:
				case wbSym_Event:
				case wbSym_Table:
					sym.index = cast(u32)wb_obj_uleb(r);
					if (defined || (sym.flags & wbSymFlag_ExplicitName)) {
						sym.name = wb_obj_name(r);
					} else if (sym.kind == wbSym_Function && sym.index < cast(u32)obj->imports.count) {
						// imported: named by the import
						isize n = 0;
						for (wbObjImport const &imp : obj->imports) {
							if (imp.kind == 0x00 && n++ == cast(isize)sym.index) {
								sym.name = imp.name;
								break;
							}
						}
					} else {
						for (wbObjImport const &imp : obj->imports) {
							if ((sym.kind == wbSym_Global && imp.kind == 0x03) || (sym.kind == wbSym_Table && imp.kind == 0x01)) {
								sym.name = imp.name; // objects have at most one of each
							}
						}
					}
					break;
				case wbSym_Data:
					sym.name = wb_obj_name(r);
					if (defined) {
						sym.segment = cast(u32)wb_obj_uleb(r);
						sym.offset  = cast(u32)wb_obj_uleb(r);
						sym.size    = cast(u32)wb_obj_uleb(r);
					}
					break;
				case wbSym_Section:
					sym.index = cast(u32)wb_obj_uleb(r);
					break;
				default:
					wb_obj_error(m, obj, "unknown symbol kind in object file");
					return false;
				}
				array_add(&obj->symbols, sym);
			}
			break;
		}
		case wbLinking_ComdatInfo:
		default:
			break;
		}
		r->pos = sub_end;
	}
	return !r->error;
}

gb_internal bool wb_obj_parse_reloc_section(wbModule *m, wbObject *obj, wbObjReader *r, isize end) {
	u32 section = cast(u32)wb_obj_uleb(r);
	Array<wbObjReloc> *relocs = nullptr;
	if (section == obj->code_section_index) {
		relocs = &obj->code_relocs;
	} else if (section == obj->data_section_index) {
		relocs = &obj->data_relocs;
	} else {
		return true; // relocations for a section that is not linked (custom sections)
	}
	u64 count = wb_obj_uleb(r);
	for (u64 i = 0; i < count && !r->error; i++) {
		wbObjReloc rel = {};
		rel.type   = cast(wbRelocType)wb_obj_byte(r);
		rel.offset = cast(u32)wb_obj_uleb(r);
		rel.index  = cast(u32)wb_obj_uleb(r);
		switch (rel.type) {
		case wbReloc_MemoryAddrLEB:
		case wbReloc_MemoryAddrSLEB:
		case wbReloc_MemoryAddrI32:
		case wbReloc_FunctionOffsetI32:
		case wbReloc_SectionOffsetI32:
		case wbReloc_MemoryAddrRelSLEB:
			rel.addend = cast(i32)wb_obj_sleb(r);
			break;
		default:
			break;
		}
		array_add(relocs, rel);
	}
	return !r->error;
}

gb_internal bool wb_obj_parse(wbModule *m, wbObject *obj) {
	wbObjReader reader = {obj->data, obj->size, 0, false};
	wbObjReader *r = &reader;
	u8 const header[8] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
	if (obj->size < 8 || gb_memcompare(obj->data, header, 8) != 0) {
		wb_obj_error(m, obj, "not a WebAssembly object file");
		return false;
	}
	r->pos = 8;
	obj->code_section_index = ~cast(u32)0;
	obj->data_section_index = ~cast(u32)0;

	// The reloc sections refer to sections by ordinal and follow the sections
	// they relocate, so a single pass suffices
	u32 section_index = 0;
	while (r->pos < r->len && !r->error) {
		u8 id = wb_obj_byte(r);
		u64 size = wb_obj_uleb(r);
		isize start = r->pos;
		isize end = start + cast(isize)size;
		if (end > r->len) {
			r->error = true;
			break;
		}
		bool ok = true;
		switch (id) {
		case wbSection_Type:     ok = wb_obj_parse_type_section(m, obj, r); break;
		case wbSection_Import:   ok = wb_obj_parse_import_section(m, obj, r); break;
		case wbSection_Function: ok = wb_obj_parse_function_section(m, obj, r); break;
		case wbSection_Code:
			obj->code_section_index = section_index;
			obj->code_contents = start;
			ok = wb_obj_parse_code_section(m, obj, r, start);
			break;
		case wbSection_Data:
			obj->data_section_index = section_index;
			obj->data_contents = start;
			ok = wb_obj_parse_data_section(m, obj, r, start);
			break;
		case wbSection_Global:
			if (wb_obj_uleb(r) != 0) {
				wb_obj_error(m, obj, "object files defining globals are not supported");
				return false;
			}
			break;
		case wbSection_Custom: {
			String name = wb_obj_name(r);
			if (name == str_lit("linking")) {
				ok = wb_obj_parse_linking_section(m, obj, r, end);
			} else if (string_starts_with(name, str_lit("reloc."))) {
				ok = wb_obj_parse_reloc_section(m, obj, r, end);
			}
			break;
		}
		case wbSection_Table:
		case wbSection_Memory:
		case wbSection_Export:
		case wbSection_Start:
		case wbSection_Element: // function table contents are recreated from the relocations
		case wbSection_DataCount:
		default:
			break;
		}
		if (!ok || r->error) {
			wb_obj_error(m, obj, "malformed object file");
			return false;
		}
		r->pos = end;
		section_index += 1;
	}
	if (r->error) {
		wb_obj_error(m, obj, "malformed object file");
		return false;
	}
	return true;
}

gb_internal wbObject *wb_obj_load(wbModule *m, String path) {
	wbObject *obj = gb_alloc_item(m->allocator, wbObject);
	obj->path = path;
	array_init(&obj->type_map,    m->allocator);
	array_init(&obj->imports,     m->allocator);
	array_init(&obj->functions,   m->allocator);
	array_init(&obj->defined,     m->allocator);
	array_init(&obj->segments,    m->allocator);
	array_init(&obj->symbols,     m->allocator);
	array_init(&obj->code_relocs, m->allocator);
	array_init(&obj->data_relocs, m->allocator);
	array_init(&obj->init_funcs,  m->allocator);

	char *path_c = alloc_cstring(heap_allocator(), path);
	gbFileContents fc = gb_file_read_contents(permanent_allocator(), false, path_c);
	gb_free(heap_allocator(), path_c);
	if (fc.data == nullptr) {
		wb_obj_error(m, obj, "cannot read object file");
		return obj;
	}
	obj->data = cast(u8 *)fc.data;
	obj->size = fc.size;
	wb_obj_parse(m, obj);
	return obj;
}

// The module's defined procedures by link name
gb_internal void wb_link_collect_definitions(wbModule *m, StringMap<wbProcedure *> *defs) {
	for (wbProcedure *p : m->procedures) {
		if (p->name.len > 0 && string_map_get(defs, p->name) == nullptr) {
			string_map_set(defs, p->name, p);
		}
	}
}

gb_internal wbProcedure *wb_link_env_import(wbModule *m, StringMap<wbProcedure *> *env_imports, String name, u32 type_index) {
	wbProcedure **found = string_map_get(env_imports, name);
	if (found != nullptr) {
		return *found;
	}
	wbProcedure *p = wb_alloc_procedure(m, name);
	p->is_foreign = true;
	p->type_index = type_index;
	for (wbValType vt : m->types[type_index].results) {
		array_add(&p->results, vt);
	}
	p->import_module = str_lit("env");
	p->import_name   = name;
	p->link_created  = true;
	array_add(&m->imports, p);
	string_map_set(env_imports, name, p);
	return p;
}

gb_internal bool wb_link_symbol_is_global(wbObjSymbol const &sym) {
	return (sym.flags & wbSymFlag_Undefined) == 0 && (sym.flags & wbSymFlag_BindingLocal) == 0;
}

gb_internal void wb_write_padded_uleb(u8 *dst, u32 value) {
	for (isize i = 0; i < 5; i++) {
		u8 b = value & 0x7f;
		value >>= 7;
		dst[i] = cast(u8)(b | (i < 4 ? 0x80 : 0));
	}
}

gb_internal void wb_write_padded_sleb(u8 *dst, i32 value) {
	for (isize i = 0; i < 5; i++) {
		u8 b = cast(u8)(value & 0x7f);
		value >>= 7;
		dst[i] = cast(u8)(b | (i < 4 ? 0x80 : 0));
	}
}

gb_internal void wb_write_u32_le(u8 *dst, u32 value) {
	dst[0] = cast(u8)(value);
	dst[1] = cast(u8)(value >> 8);
	dst[2] = cast(u8)(value >> 16);
	dst[3] = cast(u8)(value >> 24);
}

// The table slot of a linked function (also for imports, which the Odin side
// cannot take the address of because of the calling convention differences)
gb_internal u32 wb_link_table_index(wbModule *m, wbProcedure *p) {
	if (p->table_index == 0) {
		array_add(&m->table, p);
		p->table_index = cast(u32)(m->table.count-1);
	}
	return p->table_index;
}

gb_internal bool wb_link_reloc_value(wbModule *m, wbObject *obj, wbObjReloc const &rel, u32 *value) {
	if (rel.type == wbReloc_TypeIndexLEB) {
		if (rel.index >= cast(u32)obj->type_map.count) {
			return false;
		}
		*value = obj->type_map[rel.index];
		return true;
	}
	if (rel.type == wbReloc_TableNumberLEB) {
		*value = 0;
		return true;
	}
	if (rel.index >= cast(u32)obj->symbols.count) {
		return false;
	}
	wbObjSymbol const &sym = obj->symbols[rel.index];
	switch (rel.type) {
	case wbReloc_FunctionIndexLEB:
		if (sym.kind != wbSym_Function || sym.proc == nullptr) {
			return false;
		}
		*value = sym.proc->func_index;
		return true;
	case wbReloc_TableIndexSLEB:
	case wbReloc_TableIndexI32:
		if (sym.kind != wbSym_Function || sym.proc == nullptr) {
			return false;
		}
		*value = wb_link_table_index(m, sym.proc);
		return true;
	case wbReloc_MemoryAddrLEB:
	case wbReloc_MemoryAddrSLEB:
	case wbReloc_MemoryAddrI32:
		if (sym.kind != wbSym_Data) {
			return false;
		}
		if (sym.flags & wbSymFlag_Undefined) {
			return false;
		}
		if (sym.segment >= cast(u32)obj->segments.count) {
			return false;
		}
		*value = obj->segments[sym.segment].addr + sym.offset + cast(u32)rel.addend;
		return true;
	case wbReloc_GlobalIndexLEB:
		if (sym.kind != wbSym_Global || sym.name != str_lit("__stack_pointer")) {
			return false;
		}
		*value = m->global_stack_pointer;
		return true;
	default:
		return false;
	}
}

gb_internal void wb_link_apply_reloc(u8 *at, wbRelocType type, u32 value) {
	switch (type) {
	case wbReloc_FunctionIndexLEB:
	case wbReloc_MemoryAddrLEB:
	case wbReloc_TypeIndexLEB:
	case wbReloc_GlobalIndexLEB:
	case wbReloc_TableNumberLEB:
		wb_write_padded_uleb(at, value);
		break;
	case wbReloc_TableIndexSLEB:
	case wbReloc_MemoryAddrSLEB:
		wb_write_padded_sleb(at, cast(i32)value);
		break;
	case wbReloc_TableIndexI32:
	case wbReloc_MemoryAddrI32:
		wb_write_u32_le(at, value);
		break;
	default:
		break;
	}
}

// Loads the object files referenced by the foreign procedures of the program,
// merges their functions and data into the module and resolves their imports.
// Called once the root procedures exist: symbols that name Odin procedures
// are resolved by link name, generating the procedure when needed.
gb_internal void wb_link_load_objects(wbModule *m) {
	CheckerInfo *info = m->info;
	StringMap<wbObject *> objects_by_path = {};
	string_map_init(&objects_by_path, 8);

	// The link names of the procedures that would end up in the program's
	// object with LLVM: everything with a dependency count
	StringMap<Entity *> entities_by_link_name = {};
	string_map_init(&entities_by_link_name, 256);

	for (Entity *e : info->entities) {
		if (e->kind != Entity_Procedure || e->min_dep_count.load(std::memory_order_relaxed) == 0) {
			continue;
		}
		if (!e->Procedure.is_foreign) {
			if ((e->scope->flags & ScopeFlag_File) != 0 && (e->Procedure.link_name.len > 0 || e->Procedure.is_export)) {
				String name = wb_procedure_name(e);
				if (string_map_get(&entities_by_link_name, name) == nullptr) {
					string_map_set(&entities_by_link_name, name, e);
				}
			}
			continue;
		}
		Entity *lib = e->Procedure.foreign_library;
		if (lib == nullptr || lib->kind != Entity_LibraryName || lib->LibraryName.paths.count != 1) {
			continue;
		}
		String path = lib->LibraryName.paths[0];
		if (!string_ends_with(path, str_lit(".o")) || string_map_get(&objects_by_path, path) != nullptr) {
			continue;
		}
		char *path_c = alloc_cstring(heap_allocator(), path);
		bool exists = gb_file_exists(path_c);
		gb_free(heap_allocator(), path_c);
		if (!exists) {
			continue;
		}
		wbObject *obj = wb_obj_load(m, path);
		string_map_set(&objects_by_path, path, obj);
		array_add(&m->objects, obj);
	}
	if (m->objects.count == 0) {
		return;
	}

	StringMap<wbProcedure *> defs = {};
	string_map_init(&defs, 256);
	wb_link_collect_definitions(m, &defs);

	// Name the functions, place the data, register the definitions
	StringMap<wbProcedure *> linked_defs = {};
	string_map_init(&linked_defs, 256);
	for (wbObject *obj : m->objects) {
		if (obj->failed) {
			continue;
		}
		for (wbObjSegment &seg : obj->segments) {
			seg.addr = wb_data_alloc(m, seg.size, cast(i64)1 << seg.align);
			wb_data_write(m, seg.addr, obj->data + obj->data_contents + seg.bytes_start, seg.size);
		}
		for (wbObjSymbol &sym : obj->symbols) {
			if (sym.kind != wbSym_Function || (sym.flags & wbSymFlag_Undefined)) {
				continue;
			}
			if (sym.index >= cast(u32)obj->functions.count || sym.index < obj->import_function_count) {
				wb_obj_error(m, obj, "symbol table refers to a function that does not exist");
				break;
			}
			sym.proc = obj->functions[sym.index];
			if (sym.proc->name.len == 0) {
				sym.proc->name = sym.name;
			}
			if (wb_link_symbol_is_global(sym) && string_map_get(&linked_defs, sym.name) == nullptr) {
				string_map_set(&linked_defs, sym.name, sym.proc);
			}
		}
		for_array(i, obj->defined) {
			wbProcedure *p = obj->defined[i].proc;
			if (p->name.len == 0) {
				p->name = str_lit("wasm_object_function");
			}
			array_add(&m->procedures, p);
		}
	}

	// Resolve the undefined function symbols: this module's procedures first
	// (vendor:libc-shim, the runtime's memcpy), then the other objects, else the host
	StringMap<wbProcedure *> env_imports = {};
	string_map_init(&env_imports, 64);
	for (wbProcedure *p : m->imports) {
		if (p->import_module == str_lit("env")) {
			string_map_set(&env_imports, p->import_name, p);
		}
	}
	for (wbObject *obj : m->objects) {
		if (obj->failed) {
			continue;
		}
		for (wbObjSymbol &sym : obj->symbols) {
			if (sym.kind != wbSym_Function || !(sym.flags & wbSymFlag_Undefined)) {
				continue;
			}
			if (sym.index >= obj->import_function_count) {
				wb_obj_error(m, obj, "undefined function symbol is not an import");
				break;
			}
			// the import's type
			u32 type_index = 0;
			isize n = 0;
			for (wbObjImport const &imp : obj->imports) {
				if (imp.kind == 0x00 && n++ == cast(isize)sym.index) {
					if (imp.type_index >= cast(u32)obj->type_map.count) {
						wb_obj_error(m, obj, "import refers to a type that does not exist");
						break;
					}
					type_index = obj->type_map[imp.type_index];
					break;
				}
			}
			wbProcedure *target = nullptr;
			wbProcedure **found = string_map_get(&defs, sym.name);
			if (found != nullptr) {
				target = *found;
			} else {
				Entity **found_entity = string_map_get(&entities_by_link_name, sym.name);
				if (found_entity != nullptr) {
					target = wb_procedure_for_entity(m, *found_entity);
					string_map_set(&defs, sym.name, target);
				} else {
					found = string_map_get(&linked_defs, sym.name);
					if (found != nullptr) {
						target = *found;
					}
				}
			}
			if (target != nullptr) {
				if (target->type_index != type_index) {
					gb_printf_err("wasm backend: the type of '%.*s' does not match the type expected by %.*s\n", LIT(sym.name), LIT(obj->path));
					m->error_count += 1;
				}
			} else {
				target = wb_link_env_import(m, &env_imports, sym.name, type_index);
			}
			sym.proc = target;
			obj->functions[sym.index] = target;
		}
		if (obj->init_funcs.count > 0) {
			wb_obj_error(m, obj, "object files with initialization functions are not supported");
		}
	}
}

// Once everything is lowered: the Odin foreign procedures declared against
// the objects call the linked functions directly, and host imports that the
// module defines itself (a libm call with vendor:libc-shim linked in) are
// resolved likewise.
gb_internal void wb_link_resolve_imports(wbModule *m) {
	StringMap<wbProcedure *> defs = {};
	string_map_init(&defs, 256);
	wb_link_collect_definitions(m, &defs);

	StringMap<wbObject *> objects_by_path = {};
	string_map_init(&objects_by_path, 8);
	for (wbObject *obj : m->objects) {
		string_map_set(&objects_by_path, obj->path, obj);
	}

	auto remaining = array_make<wbProcedure *>(m->allocator, 0, m->imports.count);
	for (wbProcedure *p : m->imports) {
		wbObject **found_obj = nullptr;
		if (!p->is_llvm_intrinsic) {
			found_obj = string_map_get(&objects_by_path, p->import_module);
		}
		if (found_obj == nullptr || (*found_obj)->failed) {
			array_add(&remaining, p);
			continue;
		}
		wbObject *obj = *found_obj;
		wbProcedure *target = nullptr;
		for (wbObjSymbol const &sym : obj->symbols) {
			if (sym.kind == wbSym_Function && wb_link_symbol_is_global(sym) && sym.name == p->import_name) {
				target = sym.proc;
				break;
			}
		}
		if (target == nullptr) {
			gb_printf_err("wasm backend: '%.*s' is not defined by %.*s\n", LIT(p->import_name), LIT(obj->path));
			m->error_count += 1;
			array_add(&remaining, p);
			continue;
		}
		if (target->type_index != p->type_index) {
			gb_printf_err("wasm backend: the foreign declaration of '%.*s' does not match its definition in %.*s\n", LIT(p->import_name), LIT(obj->path));
			m->error_count += 1;
		}
		p->alias = target;
		array_add(&m->aliased, p);
	}

	array_clear(&m->imports);
	for (wbProcedure *p : remaining) {
		if (p->alias == nullptr && p->import_module == str_lit("env")) {
			wbProcedure **found = string_map_get(&defs, p->import_name);
			if (found != nullptr && (*found)->type_index == p->type_index) {
				p->alias = *found;
				array_add(&m->aliased, p);
				continue;
			}
		}
		array_add(&m->imports, p);
	}
}

// The defined function of `obj` whose body contains the code offset
gb_internal wbObjFunction *wb_link_function_at(wbObject *obj, u32 offset) {
	isize lo = 0;
	isize hi = obj->defined.count;
	while (lo < hi) {
		isize mid = lo + (hi-lo)/2;
		wbObjFunction *g = &obj->defined[mid];
		if (offset < g->body_start) {
			hi = mid;
		} else if (offset >= g->body_end) {
			lo = mid+1;
		} else {
			return g;
		}
	}
	return nullptr;
}

// Drops the object functions nothing reaches (what --gc-sections does in
// wasm-ld): an object brings all its functions along, but only the ones the
// Odin foreign procedures resolve to, and whatever those name through their
// relocations, end up in the module. The data segments are kept whole, so the
// functions they hold the addresses of count as reached. Host imports that the
// objects declare (fputc, ...) are only kept while a reached function calls
// them, which is why a program that never prints does not need them.
// Flags the generated procedures an object's undefined symbols may resolve to
// (wb_link_resolve_imports), for the inliner: such a procedure is reachable
// from outside the generated code
gb_internal void wb_link_mark_object_refs(wbModule *m) {
	if (m->objects.count == 0) {
		return;
	}
	StringMap<wbProcedure *> defs = {};
	string_map_init(&defs, 256);
	wb_link_collect_definitions(m, &defs);
	for (wbObject *obj : m->objects) {
		for (wbObjSymbol const &sym : obj->symbols) {
			if (sym.kind != wbSym_Function || (sym.flags & wbSymFlag_Undefined) == 0) {
				continue;
			}
			wbProcedure **found = string_map_get(&defs, sym.name);
			if (found != nullptr) {
				(*found)->object_ref = true;
			}
		}
	}
	string_map_destroy(&defs);
}

gb_internal void wb_link_gc(wbModule *m) {
	if (m->objects.count == 0) {
		return;
	}
	auto worklist = array_make<wbProcedure *>(m->allocator, 0, 256);
	auto reach = [&](wbProcedure *p) {
		if (p != nullptr && !p->link_live) {
			p->link_live = true;
			if (p->is_raw_body) {
				array_add(&worklist, p);
			}
		}
	};
	for (wbObject *obj : m->objects) {
		if (obj->failed) {
			continue;
		}
		for (wbObjReloc const &rel : obj->code_relocs) {
			if (rel.type != wbReloc_FunctionIndexLEB && rel.type != wbReloc_TableIndexSLEB && rel.type != wbReloc_TableIndexI32) {
				continue;
			}
			wbObjFunction *f = wb_link_function_at(obj, rel.offset);
			if (f == nullptr || rel.index >= cast(u32)obj->symbols.count) {
				continue;
			}
			wbProcedure *target = obj->symbols[rel.index].proc;
			if (target != nullptr) {
				if (f->proc->link_refs.data == nullptr) {
					array_init(&f->proc->link_refs, m->allocator, 0, 8);
				}
				array_add(&f->proc->link_refs, target);
			}
		}
		for (wbObjReloc const &rel : obj->data_relocs) {
			if ((rel.type == wbReloc_TableIndexSLEB || rel.type == wbReloc_TableIndexI32) && rel.index < cast(u32)obj->symbols.count) {
				reach(obj->symbols[rel.index].proc);
			}
		}
	}
	for (wbProcedure *p : m->aliased) {
		reach(p->alias);
	}
	while (worklist.count > 0) {
		wbProcedure *p = array_pop(&worklist);
		for (wbProcedure *target : p->link_refs) {
			reach(target);
		}
	}

	isize kept = 0;
	for (wbProcedure *p : m->procedures) {
		if (!p->is_raw_body || p->link_live) {
			m->procedures[kept++] = p;
		}
	}
	m->procedures.count = kept;
	kept = 0;
	for (wbProcedure *p : m->imports) {
		if (!p->link_created || p->link_live) {
			m->imports[kept++] = p;
		}
	}
	m->imports.count = kept;
}

// Rewrites the function/table/type indices and data addresses in the linked
// code and data. Called once the function indices are final.
gb_internal void wb_link_apply_relocs(wbModule *m) {
	for (wbProcedure *p : m->aliased) {
		p->func_index = p->alias->func_index;
	}
	for (wbObject *obj : m->objects) {
		if (obj->failed) {
			continue;
		}
		for (wbObjReloc const &rel : obj->code_relocs) {
			wbObjFunction *f = wb_link_function_at(obj, rel.offset);
			if (f != nullptr && !f->proc->link_live) {
				continue; // dropped by wb_link_gc
			}
			u32 value = 0;
			if (f == nullptr || !wb_link_reloc_value(m, obj, rel, &value)) {
				wb_obj_error(m, obj, "unsupported relocation in object file");
				break;
			}
			isize at = cast(isize)(rel.offset - f->body_start);
			isize width = (rel.type == wbReloc_TableIndexI32 || rel.type == wbReloc_MemoryAddrI32) ? 4 : 5;
			if (at + width > f->proc->code.data.count) {
				wb_obj_error(m, obj, "relocation outside of the function body");
				break;
			}
			wb_link_apply_reloc(f->proc->code.data.data + at, rel.type, value);
		}
		for (wbObjReloc const &rel : obj->data_relocs) {
			wbObjSegment *seg = nullptr;
			for (wbObjSegment &s : obj->segments) {
				if (rel.offset >= s.bytes_start && rel.offset < s.bytes_start + s.size) {
					seg = &s;
					break;
				}
			}
			u32 value = 0;
			if (seg == nullptr || !wb_link_reloc_value(m, obj, rel, &value)) {
				wb_obj_error(m, obj, "unsupported relocation in object file data");
				break;
			}
			isize at = cast(isize)(seg->addr - m->data_base) + cast(isize)(rel.offset - seg->bytes_start);
			if (at + 4 > m->data.count) {
				wb_obj_error(m, obj, "relocation outside of the data segment");
				break;
			}
			wb_link_apply_reloc(m->data.data + at, rel.type, value);
		}
	}
}
