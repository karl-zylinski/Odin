// Low level WebAssembly binary encoding: LEB128, instructions and the module
// writer. See https://webassembly.github.io/spec/core/binary/index.html

gb_internal void wb_buffer_init(wbBuffer *b, gbAllocator a, isize capacity = 256) {
	array_init(&b->data, a, 0, capacity);
}

gb_internal void wb_byte(wbBuffer *b, u8 x) {
	array_add(&b->data, x);
}

gb_internal void wb_bytes(wbBuffer *b, void const *data, isize len) {
	u8 const *bytes = cast(u8 const *)data;
	for (isize i = 0; i < len; i++) {
		array_add(&b->data, bytes[i]);
	}
}

gb_internal void wb_uleb(wbBuffer *b, u64 x) {
	do {
		u8 byte = cast(u8)(x & 0x7f);
		x >>= 7;
		if (x != 0) {
			byte |= 0x80;
		}
		wb_byte(b, byte);
	} while (x != 0);
}

gb_internal void wb_sleb(wbBuffer *b, i64 x) {
	for (;;) {
		u8 byte = cast(u8)(x & 0x7f);
		x >>= 7; // arithmetic shift
		bool done = (x == 0 && (byte & 0x40) == 0) || (x == -1 && (byte & 0x40) != 0);
		if (!done) {
			byte |= 0x80;
		}
		wb_byte(b, byte);
		if (done) {
			break;
		}
	}
}

gb_internal void wb_f32(wbBuffer *b, f32 x) {
	u32 bits = 0;
	gb_memmove(&bits, &x, 4);
	for (int i = 0; i < 4; i++) {
		wb_byte(b, cast(u8)(bits >> (8*i)));
	}
}

gb_internal void wb_f64(wbBuffer *b, f64 x) {
	u64 bits = 0;
	gb_memmove(&bits, &x, 8);
	for (int i = 0; i < 8; i++) {
		wb_byte(b, cast(u8)(bits >> (8*i)));
	}
}

gb_internal void wb_name(wbBuffer *b, String const &s) {
	wb_uleb(b, cast(u64)s.len);
	wb_bytes(b, s.text, s.len);
}

// Appends `src` with a ULEB length prefix
gb_internal void wb_sized(wbBuffer *b, wbBuffer const *src) {
	wb_uleb(b, cast(u64)src->data.count);
	wb_bytes(b, src->data.data, src->data.count);
}

gb_internal void wb_section(wbBuffer *out, wbSectionId id, wbBuffer const *contents) {
	wb_byte(out, cast(u8)id);
	wb_sized(out, contents);
}

// Instruction helpers

gb_internal void wb_op(wbProcedure *p, wbOp op) {
	wb_byte(&p->code, cast(u8)op);
}

gb_internal void wb_op_idx(wbProcedure *p, wbOp op, u32 idx) {
	wb_byte(&p->code, cast(u8)op);
	wb_uleb(&p->code, idx);
}

gb_internal void wb_i32_const(wbProcedure *p, i32 x) {
	wb_byte(&p->code, wbOp_i32_const);
	wb_sleb(&p->code, x);
}

gb_internal void wb_i64_const(wbProcedure *p, i64 x) {
	wb_byte(&p->code, wbOp_i64_const);
	wb_sleb(&p->code, x);
}

gb_internal void wb_f32_const(wbProcedure *p, f32 x) {
	wb_byte(&p->code, wbOp_f32_const);
	wb_f32(&p->code, x);
}

gb_internal void wb_f64_const(wbProcedure *p, f64 x) {
	wb_byte(&p->code, wbOp_f64_const);
	wb_f64(&p->code, x);
}

gb_internal void wb_local_get(wbProcedure *p, u32 idx) { wb_op_idx(p, wbOp_local_get, idx); }
gb_internal void wb_local_set(wbProcedure *p, u32 idx) { wb_op_idx(p, wbOp_local_set, idx); }
gb_internal void wb_local_tee(wbProcedure *p, u32 idx) { wb_op_idx(p, wbOp_local_tee, idx); }
gb_internal void wb_global_get(wbProcedure *p, u32 idx) { wb_op_idx(p, wbOp_global_get, idx); }
gb_internal void wb_global_set(wbProcedure *p, u32 idx) { wb_op_idx(p, wbOp_global_set, idx); }
// `call` with a placeholder index, patched in `wb_patch_call_relocs`
gb_internal void wb_call(wbProcedure *p, wbProcedure *target) {
	wb_byte(&p->code, wbOp_call);
	wbCallReloc r = {p->code.data.count, target};
	array_add(&p->call_relocs, r);
	for (int i = 0; i < 5; i++) {
		wb_byte(&p->code, 0x80);
	}
	p->code.data[p->code.data.count-1] = 0x00;
}

gb_internal void wb_patch_call_relocs(wbProcedure *p) {
	for (wbCallReloc const &r : p->call_relocs) {
		u32 x = r.target->func_index;
		for (int i = 0; i < 5; i++) {
			u8 byte = cast(u8)(x & 0x7f);
			x >>= 7;
			if (i != 4) {
				byte |= 0x80;
			}
			p->code.data[r.offset + i] = byte;
		}
	}
}

// Memory instructions. The alignment immediate is only a hint in wasm, so
// possibly unaligned accesses (packed structs) are still correct.
gb_internal void wb_memarg(wbProcedure *p, wbOp op, u32 offset, u32 size) {
	u32 align = 0;
	while ((1u << align) < size && align < 3) {
		align++;
	}
	wb_byte(&p->code, cast(u8)op);
	wb_uleb(&p->code, align);
	wb_uleb(&p->code, offset);
}

// dst, src, size on the stack (memmove semantics)
gb_internal void wb_memory_copy(wbProcedure *p) {
	wb_byte(&p->code, 0xfc);
	wb_uleb(&p->code, 10);
	wb_byte(&p->code, 0x00);
	wb_byte(&p->code, 0x00);
}

// dst, byte value, size on the stack
gb_internal void wb_memory_fill(wbProcedure *p) {
	wb_byte(&p->code, 0xfc);
	wb_uleb(&p->code, 11);
	wb_byte(&p->code, 0x00);
}

gb_internal void wb_call_indirect(wbProcedure *p, u32 type_index) {
	wb_byte(&p->code, wbOp_call_indirect);
	wb_uleb(&p->code, type_index);
	wb_byte(&p->code, 0x00); // table 0
}

// Structured control flow. `wb_open_*` returns the absolute depth of the new label.
gb_internal u32 wb_open_block(wbProcedure *p, u8 block_type = 0x40) {
	wb_byte(&p->code, wbOp_block);
	wb_byte(&p->code, block_type);
	return p->depth++;
}
gb_internal u32 wb_open_loop(wbProcedure *p, u8 block_type = 0x40) {
	wb_byte(&p->code, wbOp_loop);
	wb_byte(&p->code, block_type);
	return p->depth++;
}
gb_internal u32 wb_open_if(wbProcedure *p, u8 block_type = 0x40) {
	wb_byte(&p->code, wbOp_if);
	wb_byte(&p->code, block_type);
	return p->depth++;
}
gb_internal void wb_else(wbProcedure *p) {
	wb_byte(&p->code, wbOp_else);
}
gb_internal void wb_close(wbProcedure *p) {
	GB_ASSERT(p->depth > 0);
	p->depth--;
	wb_byte(&p->code, wbOp_end);
}

// Branch to the label with the given absolute depth
gb_internal void wb_br(wbProcedure *p, u32 target_depth) {
	GB_ASSERT(target_depth < p->depth);
	wb_op_idx(p, wbOp_br, p->depth - 1 - target_depth);
}
gb_internal void wb_br_if(wbProcedure *p, u32 target_depth) {
	GB_ASSERT(target_depth < p->depth);
	wb_op_idx(p, wbOp_br_if, p->depth - 1 - target_depth);
}

// Module writer

gb_internal void wb_write_functype(wbBuffer *b, wbFuncType const &ft) {
	wb_byte(b, 0x60);
	wb_uleb(b, cast(u64)ft.params.count);
	for (wbValType vt : ft.params) {
		wb_byte(b, cast(u8)vt);
	}
	wb_uleb(b, cast(u64)ft.results.count);
	for (wbValType vt : ft.results) {
		wb_byte(b, cast(u8)vt);
	}
}

gb_internal void wb_write_procedure_body(wbBuffer *b, wbProcedure *p) {
	wbBuffer body = {};
	wb_buffer_init(&body, heap_allocator(), p->code.data.count + 32);

	// Local declarations: run-length encoded groups of identical types
	// (params are not declared here)
	wbBuffer groups = {};
	wb_buffer_init(&groups, heap_allocator(), 32);
	u32 group_count = 0;
	for (isize i = p->param_count; i < p->locals.count; ) {
		wbValType vt = p->locals[i].vt;
		isize j = i;
		while (j < p->locals.count && p->locals[j].vt == vt) {
			j++;
		}
		wb_uleb(&groups, cast(u64)(j - i));
		wb_byte(&groups, cast(u8)vt);
		group_count++;
		i = j;
	}
	wb_uleb(&body, group_count);
	wb_bytes(&body, groups.data.data, groups.data.count);
	array_free(&groups.data);

	wb_bytes(&body, p->prologue.data.data, p->prologue.data.count);
	wb_bytes(&body, p->code.data.data, p->code.data.count);
	wb_byte(&body, wbOp_end);

	wb_sized(b, &body);
	array_free(&body.data);
}

gb_internal void wb_write_name_section(wbBuffer *out, wbModule *m) {
	wbBuffer sec = {};
	wb_buffer_init(&sec, heap_allocator(), 1024);
	wb_name(&sec, str_lit("name"));

	// subsection 1: function names
	{
		wbBuffer sub = {};
		wb_buffer_init(&sub, heap_allocator(), 1024);
		wb_uleb(&sub, cast(u64)(m->imports.count + m->procedures.count));
		for (wbProcedure *p : m->imports) {
			wb_uleb(&sub, p->func_index);
			wb_name(&sub, p->name);
		}
		for (wbProcedure *p : m->procedures) {
			wb_uleb(&sub, p->func_index);
			wb_name(&sub, p->name);
		}
		wb_byte(&sec, 1);
		wb_sized(&sec, &sub);
		array_free(&sub.data);
	}

	// subsection 2: local names
	{
		wbBuffer sub = {};
		wb_buffer_init(&sub, heap_allocator(), 1024);
		wb_uleb(&sub, cast(u64)m->procedures.count);
		for (wbProcedure *p : m->procedures) {
			wb_uleb(&sub, p->func_index);
			u32 named = 0;
			for (wbLocal const &l : p->locals) {
				if (l.name.len != 0) {
					named++;
				}
			}
			wb_uleb(&sub, named);
			for_array(i, p->locals) {
				wbLocal const &l = p->locals[i];
				if (l.name.len != 0) {
					wb_uleb(&sub, cast(u64)i);
					wb_name(&sub, l.name);
				}
			}
		}
		wb_byte(&sec, 2);
		wb_sized(&sec, &sub);
		array_free(&sub.data);
	}

	wb_section(out, wbSection_Custom, &sec);
	array_free(&sec.data);
}

gb_internal void wb_write_module(wbBuffer *out, wbModule *m) {
	gbAllocator ha = heap_allocator();

	u8 const header[8] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
	wb_bytes(out, header, 8);

	wbBuffer sec = {};
	wb_buffer_init(&sec, ha, 4096);

	// Type section
	{
		array_clear(&sec.data);
		wb_uleb(&sec, cast(u64)m->types.count);
		for (wbFuncType const &ft : m->types) {
			wb_write_functype(&sec, ft);
		}
		wb_section(out, wbSection_Type, &sec);
	}

	// Import section
	if (m->imports.count > 0) {
		array_clear(&sec.data);
		wb_uleb(&sec, cast(u64)m->imports.count);
		for (wbProcedure *p : m->imports) {
			wb_name(&sec, p->import_module);
			wb_name(&sec, p->import_name);
			wb_byte(&sec, wbExternal_Function);
			wb_uleb(&sec, p->type_index);
		}
		wb_section(out, wbSection_Import, &sec);
	}

	// Function section
	if (m->procedures.count > 0) {
		array_clear(&sec.data);
		wb_uleb(&sec, cast(u64)m->procedures.count);
		for (wbProcedure *p : m->procedures) {
			wb_uleb(&sec, p->type_index);
		}
		wb_section(out, wbSection_Function, &sec);
	}

	// Table section: the function table used for procedure values
	if (m->table.count > 1) {
		array_clear(&sec.data);
		wb_uleb(&sec, 1);
		wb_byte(&sec, wbValType_funcref);
		wb_byte(&sec, 0x01); // limits: min and max
		wb_uleb(&sec, cast(u64)m->table.count);
		wb_uleb(&sec, cast(u64)m->table.count);
		wb_section(out, wbSection_Table, &sec);
	}

	// Memory section: one memory, no maximum
	{
		array_clear(&sec.data);
		wb_uleb(&sec, 1);
		wb_byte(&sec, 0x00);
		wb_uleb(&sec, m->memory_initial_pages);
		wb_section(out, wbSection_Memory, &sec);
	}

	// Global section: __stack_pointer (mutable i32) initialized to the top of the stack area
	{
		array_clear(&sec.data);
		wb_uleb(&sec, 1);
		wb_byte(&sec, wbValType_i32);
		wb_byte(&sec, 0x01); // mutable
		wb_byte(&sec, wbOp_i32_const);
		wb_sleb(&sec, cast(i64)m->stack_size);
		wb_byte(&sec, wbOp_end);
		wb_section(out, wbSection_Global, &sec);
	}

	// Export section
	{
		array_clear(&sec.data);
		u32 export_count = 1; // memory
		for (wbProcedure *p : m->procedures) {
			if (p->is_export) {
				export_count++;
			}
		}
		wb_uleb(&sec, export_count);
		wb_name(&sec, str_lit("memory"));
		wb_byte(&sec, wbExternal_Memory);
		wb_uleb(&sec, 0);
		for (wbProcedure *p : m->procedures) {
			if (p->is_export) {
				wb_name(&sec, p->name);
				wb_byte(&sec, wbExternal_Function);
				wb_uleb(&sec, p->func_index);
			}
		}
		wb_section(out, wbSection_Export, &sec);
	}

	// Start section
	if (m->startup != nullptr) {
		array_clear(&sec.data);
		wb_uleb(&sec, m->startup->func_index);
		wb_section(out, wbSection_Start, &sec);
	}

	// Element section: fill the function table (index 0 stays null)
	if (m->table.count > 1) {
		array_clear(&sec.data);
		wb_uleb(&sec, 1);
		wb_byte(&sec, 0x00); // active segment, table 0, funcidx vector
		wb_byte(&sec, wbOp_i32_const);
		wb_sleb(&sec, 1);
		wb_byte(&sec, wbOp_end);
		wb_uleb(&sec, cast(u64)(m->table.count-1));
		for (isize i = 1; i < m->table.count; i++) {
			wb_uleb(&sec, m->table[i]->func_index);
		}
		wb_section(out, wbSection_Element, &sec);
	}

	// Code section
	if (m->procedures.count > 0) {
		array_clear(&sec.data);
		wb_uleb(&sec, cast(u64)m->procedures.count);
		for (wbProcedure *p : m->procedures) {
			wb_write_procedure_body(&sec, p);
		}
		wb_section(out, wbSection_Code, &sec);
	}

	// Data section: globals and constants
	if (m->data.count > 0) {
		array_clear(&sec.data);
		wb_uleb(&sec, 1);
		wb_byte(&sec, 0x00); // active segment in memory 0
		wb_byte(&sec, wbOp_i32_const);
		wb_sleb(&sec, cast(i64)m->data_base);
		wb_byte(&sec, wbOp_end);
		wb_uleb(&sec, cast(u64)m->data.count);
		wb_bytes(&sec, m->data.data, m->data.count);
		wb_section(out, wbSection_Data, &sec);
	}

	wb_write_name_section(out, m);

	array_free(&sec.data);
}
