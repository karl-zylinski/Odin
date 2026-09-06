// Direct WebAssembly backend: procedures, values, memory and the module.
// See wasm_backend.hpp for an overview.
//
//   wasm_backend.cpp      - this file: types, procedures, values, frame, data, module
//   wasm_backend_expr.cpp - expressions
//   wasm_backend_stmt.cpp - statements

#include "wasm_backend.hpp"
#include "wasm_backend_emit.cpp"

gb_internal wbValue wb_build_expr(wbProcedure *p, Ast *expr);
gb_internal wbAddr  wb_build_addr(wbProcedure *p, Ast *expr);
gb_internal void    wb_build_stmt(wbProcedure *p, Ast *node);
gb_internal void    wb_open_scope(wbProcedure *p);
gb_internal void    wb_close_scope(wbProcedure *p);
gb_internal wbAddr  wb_add_variable(wbProcedure *p, Entity *e, Ast *init_expr = nullptr);
gb_internal void    wb_emit_epilogue(wbProcedure *p);
gb_internal wbValue wb_emit_conv(wbProcedure *p, wbValue v, Type *dst);
gb_internal wbProcedure *wb_procedure_for_entity(wbModule *m, Entity *e);
gb_internal u32     wb_table_index(wbModule *m, wbProcedure *p);
gb_internal void    wb_build_compound_lit(wbProcedure *p, Ast *expr, wbAddr dst);
gb_internal void    wb_prescan_addressed(wbProcedure *p, Ast *node);

// Diagnostics

gb_internal void wb_unsupported(wbProcedure *p, Ast *node, char const *what) {
	p->failed = true;
	p->module->error_count++;
	if (node != nullptr) {
		error(ast_token(node), "wasm backend: unsupported %s", what);
	} else if (p->entity != nullptr) {
		error(p->entity->token, "wasm backend: unsupported %s", what);
	} else {
		gb_printf_err("wasm backend: unsupported %s\n", what);
	}
}

gb_internal void wb_unsupported_type(wbProcedure *p, Ast *node, Type *t) {
	p->failed = true;
	p->module->error_count++;
	gbString s = type_to_string(t);
	Token token = node != nullptr ? ast_token(node) : (p->entity != nullptr ? p->entity->token : Token{});
	error(token, "wasm backend: unsupported type '%s'", s);
	gb_string_free(s);
}

// Types

// Returns the wasm value type used to represent a scalar Odin type, or
// wbValType_Invalid if the type is an aggregate (lives in memory).
gb_internal wbValType wb_valtype_of(Type *t) {
	if (t == nullptr) {
		return wbValType_Invalid;
	}
	if (is_type_untyped(t)) {
		t = default_type(t);
	}
	t = core_type(t);
	if (t == nullptr || t == t_invalid) {
		return wbValType_Invalid;
	}
	switch (t->kind) {
	case Type_Basic:
		if (t->Basic.flags & (BasicFlag_Boolean|BasicFlag_Integer|BasicFlag_Rune)) {
			switch (type_size_of(t)) {
			case 1: case 2: case 4: return wbValType_i32;
			case 8: return wbValType_i64;
			}
			return wbValType_Invalid;
		}
		if (t->Basic.flags & BasicFlag_Float) {
			switch (type_size_of(t)) {
			case 4: return wbValType_f32;
			case 8: return wbValType_f64;
			}
			return wbValType_Invalid;
		}
		if (t->Basic.kind == Basic_rawptr || t->Basic.kind == Basic_uintptr || t->Basic.kind == Basic_cstring || t->Basic.kind == Basic_typeid) {
			return wbValType_i32;
		}
		return wbValType_Invalid;
	case Type_Pointer:
	case Type_MultiPointer:
	case Type_Proc:
		return wbValType_i32;
	case Type_BitSet:
		switch (type_size_of(t)) {
		case 1: case 2: case 4: return wbValType_i32;
		case 8: return wbValType_i64;
		}
		return wbValType_Invalid;
	}
	return wbValType_Invalid;
}

gb_internal bool wb_is_scalar(Type *t) {
	return wb_valtype_of(t) != wbValType_Invalid;
}

gb_internal bool wb_type_is_signed(Type *t) {
	if (is_type_untyped(t)) {
		t = default_type(t);
	}
	t = core_type(t);
	return is_type_integer(t) && !is_type_unsigned(t);
}

gb_internal bool wb_is_odin_cc(Type *pt) {
	pt = base_type(pt);
	GB_ASSERT(pt->kind == Type_Proc);
	return pt->Proc.calling_convention == ProcCC_Odin;
}

gb_internal Type *wb_result_type(Type *pt) {
	pt = base_type(pt);
	if (pt->Proc.result_count == 0) {
		return nullptr;
	}
	return pt->Proc.results->Tuple.variables[0]->type;
}

// True if the results are returned through a pointer passed as the first parameter
gb_internal bool wb_uses_sret(Type *pt) {
	pt = base_type(pt);
	if (pt->Proc.result_count == 0) {
		return false;
	}
	if (pt->Proc.result_count > 1) {
		return true;
	}
	return !wb_is_scalar(wb_result_type(pt));
}

gb_internal u32 wb_add_functype(wbModule *m, wbFuncType const &ft) {
	for_array(i, m->types) {
		wbFuncType const &other = m->types[i];
		if (other.params.count != ft.params.count || other.results.count != ft.results.count) {
			continue;
		}
		bool same = true;
		for_array(j, ft.params) {
			if (other.params[j] != ft.params[j]) { same = false; break; }
		}
		for_array(j, ft.results) {
			if (other.results[j] != ft.results[j]) { same = false; break; }
		}
		if (same) {
			return cast(u32)i;
		}
	}
	array_add(&m->types, ft);
	return cast(u32)(m->types.count-1);
}

// Computes the wasm signature of an Odin procedure type (see the calling
// convention description in wasm_backend.hpp).
gb_internal void wb_functype_of_proc(wbModule *m, Type *pt, wbFuncType *ft) {
	pt = base_type(pt);
	GB_ASSERT(pt->kind == Type_Proc);
	array_init(&ft->params,  m->allocator);
	array_init(&ft->results, m->allocator);

	if (wb_uses_sret(pt)) {
		array_add(&ft->params, wbValType_i32);
	}
	if (pt->Proc.params != nullptr) {
		for (Entity *e : pt->Proc.params->Tuple.variables) {
			wbValType vt = wb_valtype_of(e->type);
			if (vt == wbValType_Invalid) {
				vt = wbValType_i32; // pointer to a copy
			}
			array_add(&ft->params, vt);
		}
	}
	if (wb_is_odin_cc(pt)) {
		array_add(&ft->params, wbValType_i32); // context pointer
	}
	if (pt->Proc.result_count == 1 && !wb_uses_sret(pt)) {
		array_add(&ft->results, wb_valtype_of(wb_result_type(pt)));
	}
}

gb_internal u32 wb_type_index_of_proc(wbModule *m, Type *pt) {
	wbFuncType ft = {};
	wb_functype_of_proc(m, pt, &ft);
	return wb_add_functype(m, ft);
}

// Procedures

gb_internal String wb_procedure_name(Entity *e) {
	if (e->Procedure.link_name.len > 0) {
		return e->Procedure.link_name;
	}
	gbString name = string_canonical_entity_name(heap_allocator(), e);
	String s = copy_string(permanent_allocator(), make_string_c(name));
	gb_string_free(name);
	e->Procedure.link_name = s;
	return s;
}

gb_internal wbProcedure *wb_alloc_procedure(wbModule *m, String name) {
	wbProcedure *p = gb_alloc_item(m->allocator, wbProcedure);
	p->module = m;
	p->name   = name;
	p->context_local = -1;
	p->sret_local    = -1;
	array_init(&p->locals,       m->allocator);
	array_init(&p->results,      m->allocator);
	array_init(&p->result_addrs, m->allocator);
	array_init(&p->labels,       m->allocator);
	array_init(&p->call_relocs,  m->allocator);
	array_init(&p->defers,       m->allocator);
	array_init(&p->scopes,       m->allocator);
	map_init(&p->variables);
	ptr_set_init(&p->addressed);
	wb_buffer_init(&p->prologue, m->allocator, 16);
	wb_buffer_init(&p->code, m->allocator);
	return p;
}

gb_internal wbProcedure *wb_procedure_for_entity(wbModule *m, Entity *e) {
	GB_ASSERT(e->kind == Entity_Procedure);
	wbProcedure **found = map_get(&m->procedure_map, e);
	if (found) {
		return *found;
	}

	wbProcedure *p = wb_alloc_procedure(m, wb_procedure_name(e));
	p->entity = e;
	p->type   = e->type;
	p->is_foreign = e->Procedure.is_foreign;
	p->is_export  = e->Procedure.is_export;
	map_set(&m->procedure_map, e, p);

	wbFuncType ft = {};
	wb_functype_of_proc(m, e->type, &ft);
	p->type_index = wb_add_functype(m, ft);
	for (wbValType vt : ft.results) {
		array_add(&p->results, vt);
	}

	if (p->is_foreign) {
		// Mirrors lb_set_wasm_procedure_import_attributes: the module name is the
		// foreign import path and the link name has the form "module..name".
		String module_name = str_lit("env");
		String import_name = p->name;
		Entity *lib = e->Procedure.foreign_library;
		if (lib != nullptr && lib->kind == Entity_LibraryName && lib->LibraryName.paths.count == 1) {
			module_name = lib->LibraryName.paths[0];
			if (string_starts_with(import_name, module_name)) {
				import_name = substring(import_name, module_name.len+WASM_MODULE_NAME_SEPARATOR.len, import_name.len);
			}
		}
		p->import_module = module_name;
		p->import_name   = import_name;
		array_add(&m->imports, p);
	} else {
		DeclInfo *decl = e->decl_info;
		GB_ASSERT(decl != nullptr && decl->proc_lit != nullptr);
		GB_ASSERT(decl->proc_lit->kind == Ast_ProcLit);
		p->body = decl->proc_lit->ProcLit.body;
		array_add(&m->procedures, p);
		array_add(&m->work_queue, p);
	}
	return p;
}

// Anonymous procedure literals get an entity on first use (as in lb_generate_anonymous_proc_lit)
gb_internal wbProcedure *wb_procedure_for_proc_lit(wbModule *m, String const &prefix, Ast *expr) {
	ast_node(pl, ProcLit, expr);
	Entity *e = pl->decl->entity.load();
	if (e == nullptr) {
		isize name_len = prefix.len + 6 + 11;
		char *name_text = gb_alloc_array(permanent_allocator(), char, name_len);
		static i32 name_id = 0;
		name_len = gb_snprintf(name_text, name_len, "%.*s$anon-%d", LIT(prefix), ++name_id);
		String name = make_string((u8 *)name_text, name_len-1);

		Token token = {};
		token.pos = ast_token(expr).pos;
		token.kind = Token_Ident;
		token.string = name;
		e = alloc_entity_procedure(nullptr, token, type_of_expr(expr), pl->tags);
		e->file = expr->file();
		e->scope = e->file->scope;
		e->decl_info = pl->decl;
		e->parent_proc_decl = pl->decl->parent;
		e->Procedure.is_anonymous = true;
		e->Procedure.link_name = name;
		e->flags |= EntityFlag_ProcBodyChecked;
		pl->decl->entity.store(e);
	}
	return wb_procedure_for_entity(m, e);
}

gb_internal wbProcedure *wb_procedure_for_proc_lit(wbProcedure *parent, Ast *expr) {
	return wb_procedure_for_proc_lit(parent->module, parent->name, expr);
}

// Table index of a constant procedure value (a named procedure or a literal)
gb_internal bool wb_const_procedure_index(wbModule *m, String const &prefix, Ast *proc, u32 *index) {
	proc = unparen_expr(proc);
	if (proc->kind == Ast_ProcLit) {
		*index = wb_table_index(m, wb_procedure_for_proc_lit(m, prefix, proc));
		return true;
	}
	Entity *e = entity_of_node(proc);
	if (e == nullptr || e->kind != Entity_Procedure) {
		return false;
	}
	*index = wb_table_index(m, wb_procedure_for_entity(m, e));
	return true;
}

// Index of a procedure in the function table (used for procedure values)
gb_internal u32 wb_table_index(wbModule *m, wbProcedure *p) {
	if (p->table_index == 0) {
		array_add(&m->table, p);
		p->table_index = cast(u32)(m->table.count-1);
	}
	return p->table_index;
}

gb_internal wbProcedure *wb_lookup_runtime_procedure(wbModule *m, char const *name) {
	AstPackage *pkg = m->info->runtime_package;
	GB_ASSERT(pkg != nullptr);
	Entity *e = scope_lookup_current(pkg->scope, string_interner_insert(make_string_c(name)));
	GB_ASSERT_MSG(e != nullptr, "Runtime procedure not found: %s", name);
	return wb_procedure_for_entity(m, e);
}

gb_internal u32 wb_add_local(wbProcedure *p, wbValType vt, String name = {}) {
	wbLocal l = {vt, name};
	array_add(&p->locals, l);
	return cast(u32)(p->locals.count-1);
}

// Values

gb_internal wbValue wb_value_local(u32 idx, wbValType vt, Type *type) {
	wbValue v = {};
	v.kind  = wbValue_Local;
	v.vt    = vt;
	v.type  = type;
	v.index = idx;
	return v;
}

gb_internal wbValue wb_value_memory(u32 base, i32 offset, Type *type) {
	wbValue v = {};
	v.kind   = wbValue_Memory;
	v.vt     = wbValType_i32;
	v.type   = type;
	v.index  = base;
	v.offset = offset;
	return v;
}

gb_internal wbValue wb_value_const_int(Type *type, i64 i) {
	wbValue v = {};
	v.kind = wbValue_Const;
	v.vt   = wb_valtype_of(type);
	v.type = type;
	v.i    = i;
	GB_ASSERT(v.vt == wbValType_i32 || v.vt == wbValType_i64);
	return v;
}

gb_internal wbValue wb_value_invalid(void) {
	wbValue v = {};
	return v;
}

// Pushes local[base] + offset (or the absolute address `offset`)
gb_internal void wb_push_address(wbProcedure *p, u32 base, i32 offset) {
	if (base == WB_NO_LOCAL) {
		wb_i32_const(p, offset);
		return;
	}
	wb_local_get(p, base);
	if (offset != 0) {
		wb_i32_const(p, offset);
		wb_op(p, wbOp_i32_add);
	}
}

// Pushes a value (for aggregates: its address) onto the wasm operand stack
gb_internal void wb_push(wbProcedure *p, wbValue v) {
	switch (v.kind) {
	case wbValue_Local:
		wb_local_get(p, v.index);
		break;
	case wbValue_Const:
		switch (v.vt) {
		case wbValType_i32: wb_i32_const(p, cast(i32)v.i); break;
		case wbValType_i64: wb_i64_const(p, v.i);          break;
		case wbValType_f32: wb_f32_const(p, cast(f32)v.f); break;
		case wbValType_f64: wb_f64_const(p, v.f);          break;
		default: GB_PANIC("invalid constant type"); break;
		}
		break;
	case wbValue_Memory:
		wb_push_address(p, v.index, v.offset);
		break;
	default:
		// Error already reported; keep the operand stack well-typed
		wb_op(p, wbOp_unreachable);
		break;
	}
}

// Pops the top of the operand stack into a fresh local
gb_internal wbValue wb_pop_to_local(wbProcedure *p, wbValType vt, Type *type) {
	u32 idx = wb_add_local(p, vt);
	wb_local_set(p, idx);
	return wb_value_local(idx, vt, type);
}

// Makes sure a scalar value is in a local (constants stay constants)
gb_internal wbValue wb_value_to_local(wbProcedure *p, wbValue v) {
	if (v.kind == wbValue_Local || v.kind == wbValue_Const || v.kind == wbValue_Invalid) {
		return v;
	}
	wb_push(p, v);
	return wb_pop_to_local(p, v.vt, v.type);
}

// Normalizes an integer held in an i32/i64 local so that the unused high bits
// hold the sign/zero extension of the Odin type's width.
gb_internal void wb_emit_normalize(wbProcedure *p, Type *type) {
	if (is_type_untyped(type)) {
		type = default_type(type);
	}
	Type *t = core_type(type);
	if (!is_type_integer(t) && !is_type_boolean(t) && !is_type_rune(t) && !is_type_bit_set(t)) {
		return;
	}
	i64 size = type_size_of(t);
	bool is_signed = wb_type_is_signed(t);
	wbValType vt = wb_valtype_of(t);
	if (vt == wbValType_i32) {
		switch (size) {
		case 1:
			if (is_signed) { wb_op(p, wbOp_i32_extend8_s); } else { wb_i32_const(p, 0xff); wb_op(p, wbOp_i32_and); }
			break;
		case 2:
			if (is_signed) { wb_op(p, wbOp_i32_extend16_s); } else { wb_i32_const(p, 0xffff); wb_op(p, wbOp_i32_and); }
			break;
		}
	}
}

// Stack frame

gb_internal i32 wb_alloc_slot(wbProcedure *p, i64 size, i64 align) {
	if (align < 1) align = 1;
	if (size < 0) size = 0;
	p->frame_size = cast(u32)((p->frame_size + align-1) & ~(align-1));
	i32 offset = cast(i32)p->frame_size;
	p->frame_size += cast(u32)size;
	return offset;
}

gb_internal wbAddr wb_addr_memory(u32 base, i32 offset, Type *type) {
	wbAddr a = {};
	a.kind   = wbAddr_Memory;
	a.type   = type;
	a.index  = base;
	a.offset = offset;
	return a;
}

gb_internal wbAddr wb_addr_local(u32 index, Type *type) {
	wbAddr a = {};
	a.kind  = wbAddr_Local;
	a.type  = type;
	a.index = index;
	return a;
}

// Allocates frame storage for a value of the given type
gb_internal wbAddr wb_add_temp(wbProcedure *p, Type *type) {
	i32 offset = wb_alloc_slot(p, type_size_of(type), type_align_of(type));
	return wb_addr_memory(p->fp_local, offset, type);
}

// Memory access

gb_internal void wb_emit_load_op(wbProcedure *p, Type *type, u32 offset) {
	Type *t = core_type(type);
	wbValType vt = wb_valtype_of(t);
	i64 size = type_size_of(t);
	bool is_signed = wb_type_is_signed(t);
	switch (vt) {
	case wbValType_i32:
		switch (size) {
		case 1: wb_memarg(p, is_signed ? wbOp_i32_load8_s  : wbOp_i32_load8_u,  offset, 1); break;
		case 2: wb_memarg(p, is_signed ? wbOp_i32_load16_s : wbOp_i32_load16_u, offset, 2); break;
		default: wb_memarg(p, wbOp_i32_load, offset, 4); break;
		}
		break;
	case wbValType_i64: wb_memarg(p, wbOp_i64_load, offset, 8); break;
	case wbValType_f32: wb_memarg(p, wbOp_f32_load, offset, 4); break;
	case wbValType_f64: wb_memarg(p, wbOp_f64_load, offset, 8); break;
	default: GB_PANIC("load of non-scalar type"); break;
	}
}

gb_internal void wb_emit_store_op(wbProcedure *p, Type *type, u32 offset) {
	Type *t = core_type(type);
	wbValType vt = wb_valtype_of(t);
	i64 size = type_size_of(t);
	switch (vt) {
	case wbValType_i32:
		switch (size) {
		case 1: wb_memarg(p, wbOp_i32_store8,  offset, 1); break;
		case 2: wb_memarg(p, wbOp_i32_store16, offset, 2); break;
		default: wb_memarg(p, wbOp_i32_store, offset, 4); break;
		}
		break;
	case wbValType_i64: wb_memarg(p, wbOp_i64_store, offset, 8); break;
	case wbValType_f32: wb_memarg(p, wbOp_f32_store, offset, 4); break;
	case wbValType_f64: wb_memarg(p, wbOp_f64_store, offset, 8); break;
	default: GB_PANIC("store of non-scalar type"); break;
	}
}

// Loads a scalar from local[base] + offset into a fresh local
gb_internal wbValue wb_emit_load(wbProcedure *p, u32 base, i32 offset, Type *type) {
	wbValType vt = wb_valtype_of(type);
	GB_ASSERT(vt != wbValType_Invalid);
	if (base == WB_NO_LOCAL) {
		wb_i32_const(p, 0);
		wb_emit_load_op(p, type, cast(u32)offset);
	} else {
		wb_local_get(p, base);
		wb_emit_load_op(p, type, cast(u32)offset);
	}
	return wb_pop_to_local(p, vt, type);
}

// Stores a scalar value (already of `type`) to local[base] + offset
gb_internal void wb_emit_store(wbProcedure *p, u32 base, i32 offset, wbValue v, Type *type) {
	if (v.kind == wbValue_Invalid) {
		return;
	}
	if (base == WB_NO_LOCAL) {
		wb_i32_const(p, 0);
	} else {
		wb_local_get(p, base);
	}
	wb_push(p, v);
	wb_emit_store_op(p, type, cast(u32)offset);
}

gb_internal void wb_emit_copy(wbProcedure *p, u32 dst_base, i32 dst_offset, u32 src_base, i32 src_offset, i64 size) {
	if (size <= 0) {
		return;
	}
	if (dst_base == src_base && dst_offset == src_offset) {
		return;
	}
	wb_push_address(p, dst_base, dst_offset);
	wb_push_address(p, src_base, src_offset);
	wb_i32_const(p, cast(i32)size);
	wb_memory_copy(p);
}

gb_internal void wb_emit_zero(wbProcedure *p, u32 base, i32 offset, i64 size) {
	if (size <= 0) {
		return;
	}
	wb_push_address(p, base, offset);
	wb_i32_const(p, 0);
	wb_i32_const(p, cast(i32)size);
	wb_memory_fill(p);
}

// Addresses

gb_internal wbValue wb_addr_load(wbProcedure *p, wbAddr addr) {
	switch (addr.kind) {
	case wbAddr_Local:
		return wb_value_local(addr.index, p->locals[addr.index].vt, addr.type);
	case wbAddr_Memory:
		if (wb_is_scalar(addr.type)) {
			return wb_emit_load(p, addr.index, addr.offset, addr.type);
		}
		return wb_value_memory(addr.index, addr.offset, addr.type);
	default:
		return wb_value_invalid();
	}
}

gb_internal void wb_addr_store(wbProcedure *p, wbAddr addr, wbValue v) {
	if (addr.kind == wbAddr_Invalid || v.kind == wbValue_Invalid) {
		return;
	}
	v = wb_emit_conv(p, v, addr.type);
	if (v.kind == wbValue_Invalid) {
		return;
	}
	switch (addr.kind) {
	case wbAddr_Local:
		wb_push(p, v);
		wb_local_set(p, addr.index);
		break;
	case wbAddr_Memory:
		if (v.kind == wbValue_Memory) {
			wb_emit_copy(p, addr.index, addr.offset, v.index, v.offset, type_size_of(addr.type));
		} else {
			wb_emit_store(p, addr.index, addr.offset, v, addr.type);
		}
		break;
	default:
		break;
	}
}

gb_internal void wb_addr_zero(wbProcedure *p, wbAddr addr) {
	switch (addr.kind) {
	case wbAddr_Local: {
		switch (p->locals[addr.index].vt) {
		case wbValType_i32: wb_i32_const(p, 0); break;
		case wbValType_i64: wb_i64_const(p, 0); break;
		case wbValType_f32: wb_f32_const(p, 0); break;
		case wbValType_f64: wb_f64_const(p, 0); break;
		default: break;
		}
		wb_local_set(p, addr.index);
		break;
	}
	case wbAddr_Memory:
		if (wb_is_scalar(addr.type)) {
			wbValue zero = {};
			zero.kind = wbValue_Const;
			zero.vt   = wb_valtype_of(addr.type);
			zero.type = addr.type;
			wb_emit_store(p, addr.index, addr.offset, zero, addr.type);
		} else {
			wb_emit_zero(p, addr.index, addr.offset, type_size_of(addr.type));
		}
		break;
	default:
		break;
	}
}

// The address of a memory location as a pointer value
gb_internal wbValue wb_addr_get_ptr(wbProcedure *p, wbAddr addr, Type *ptr_type = nullptr) {
	if (ptr_type == nullptr) {
		ptr_type = alloc_type_pointer(addr.type);
	}
	if (addr.kind != wbAddr_Memory) {
		if (addr.kind == wbAddr_Local) {
			wb_unsupported(p, nullptr, "address of a variable held in a register (internal error)");
		}
		return wb_value_invalid();
	}
	if (addr.index == WB_NO_LOCAL) {
		return wb_value_const_int(ptr_type, addr.offset);
	}
	if (addr.offset == 0) {
		return wb_value_local(addr.index, wbValType_i32, ptr_type);
	}
	wb_push_address(p, addr.index, addr.offset);
	return wb_pop_to_local(p, wbValType_i32, ptr_type);
}

// Address of the i-th field/element (constant offset) of a memory location
gb_internal wbAddr wb_addr_offset(wbAddr addr, i64 offset, Type *type) {
	GB_ASSERT(addr.kind == wbAddr_Memory);
	return wb_addr_memory(addr.index, addr.offset + cast(i32)offset, type);
}

gb_internal wbAddr wb_addr_field(wbAddr addr, i64 index) {
	Type *ft = nullptr;
	i64 offset = type_offset_of(addr.type, index, &ft);
	return wb_addr_offset(addr, offset, ft);
}

// Address of a memory-resident value (aggregates), spilling scalars to the frame
gb_internal wbAddr wb_value_to_addr(wbProcedure *p, wbValue v) {
	if (v.kind == wbValue_Invalid) {
		wbAddr a = {};
		return a;
	}
	if (v.kind == wbValue_Memory) {
		return wb_addr_memory(v.index, v.offset, v.type);
	}
	wbAddr tmp = wb_add_temp(p, v.type);
	wb_emit_store(p, tmp.index, tmp.offset, v, v.type);
	return tmp;
}

// Copies a value into a fresh temporary (so later side effects cannot change it)
gb_internal wbValue wb_value_copy(wbProcedure *p, wbValue v) {
	if (v.kind != wbValue_Memory) {
		return wb_value_to_local(p, v);
	}
	wbAddr tmp = wb_add_temp(p, v.type);
	wb_emit_copy(p, tmp.index, tmp.offset, v.index, v.offset, type_size_of(v.type));
	return wb_value_memory(tmp.index, tmp.offset, v.type);
}

// Constant data

gb_internal u32 wb_data_alloc(wbModule *m, i64 size, i64 align) {
	if (align < 1) align = 1;
	isize offset = (m->data.count + align-1) & ~(align-1);
	array_resize(&m->data, offset + gb_max(size, 0));
	for (isize i = offset; i < m->data.count; i++) {
		m->data[i] = 0;
	}
	return cast(u32)(m->data_base + offset);
}

gb_internal void wb_data_write(wbModule *m, u32 addr, void const *bytes, i64 size) {
	isize offset = cast(isize)(addr - m->data_base);
	GB_ASSERT(offset + size <= m->data.count);
	gb_memmove(m->data.data + offset, bytes, size);
}

// NUL-terminated string bytes, deduplicated
gb_internal u32 wb_intern_string_bytes(wbModule *m, String const &s) {
	u32 *found = string_map_get(&m->string_bytes, s);
	if (found) {
		return *found;
	}
	u32 addr = wb_data_alloc(m, s.len+1, 1);
	wb_data_write(m, addr, s.text, s.len);
	string_map_set(&m->string_bytes, s, addr);
	return addr;
}

gb_internal void wb_write_const_data(wbModule *m, Type *type, ExactValue const &value, u8 *dst);

// A {data, len} `string` constant in the data segment, deduplicated
gb_internal u32 wb_intern_string_value(wbModule *m, String const &s) {
	u32 *found = string_map_get(&m->string_values, s);
	if (found) {
		return *found;
	}
	u32 addr = wb_data_alloc(m, type_size_of(t_string), type_align_of(t_string));
	u8 *bytes = gb_alloc_array(temporary_allocator(), u8, type_size_of(t_string));
	wb_write_const_data(m, t_string, exact_value_string(s), bytes);
	wb_data_write(m, addr, bytes, type_size_of(t_string));
	string_map_set(&m->string_values, s, addr);
	return addr;
}

gb_internal void wb_write_le(u8 *dst, u64 bits, i64 size) {
	for (i64 i = 0; i < size; i++) {
		dst[i] = cast(u8)(bits >> (8*i));
	}
}

// Serializes a constant of `type` into `dst` (which is zeroed and type_size_of(type) bytes)
gb_internal void wb_write_const_data(wbModule *m, Type *type, ExactValue const &value, u8 *dst) {
	if (is_type_untyped(type)) {
		type = default_type(type);
	}
	Type *bt = base_type(type);
	i64 size = type_size_of(type);

	switch (value.kind) {
	case ExactValue_Invalid:
		return; // nil / zero
	case ExactValue_Bool:
		dst[0] = value.value_bool ? 1 : 0;
		return;
	case ExactValue_Integer:
		if (is_type_float(core_type(type))) {
			ExactValue f = exact_value_to_float(value);
			wb_write_const_data(m, type, f, dst);
			return;
		}
		wb_write_le(dst, big_int_to_u64(&value.value_integer), gb_min(size, 8));
		return;
	case ExactValue_Float:
		if (size == 4) {
			f32 f = cast(f32)value.value_float;
			u32 bits = 0;
			gb_memmove(&bits, &f, 4);
			wb_write_le(dst, bits, 4);
		} else if (size == 8) {
			u64 bits = 0;
			gb_memmove(&bits, &value.value_float, 8);
			wb_write_le(dst, bits, 8);
		} else if (is_type_integer(core_type(type))) {
			wb_write_le(dst, cast(u64)cast(i64)value.value_float, gb_min(size, 8));
		}
		return;
	case ExactValue_Pointer:
		wb_write_le(dst, cast(u64)value.value_pointer, size);
		return;
	case ExactValue_Procedure: {
		u32 index = 0;
		if (wb_const_procedure_index(m, str_lit("global"), value.value_procedure, &index)) {
			wb_write_le(dst, index, size);
		}
		return;
	}
	case ExactValue_String: {
		String s = value.value_string;
		if (is_type_cstring(type)) {
			wb_write_le(dst, wb_intern_string_bytes(m, s), size);
		} else if (is_type_string(type)) {
			u32 data = s.len > 0 ? wb_intern_string_bytes(m, s) : 0;
			i64 ptr_size = build_context.ptr_size;
			wb_write_le(dst, data, ptr_size);
			wb_write_le(dst + type_offset_of(bt, 1, nullptr), cast(u64)s.len, build_context.int_size);
		} else if (is_type_array(bt) && s.len == size) {
			gb_memmove(dst, s.text, s.len);
		}
		return;
	}
	case ExactValue_Compound: {
		Ast *node = value.value_compound;
		GB_ASSERT(node->kind == Ast_CompoundLit);
		ast_node(cl, CompoundLit, node);
		switch (bt->kind) {
		case Type_Struct: {
			for_array(i, cl->elems) {
				Ast *elem = cl->elems[i];
				i64 index = cast(i64)i;
				Ast *value_expr = elem;
				if (elem->kind == Ast_FieldValue) {
					ast_node(fv, FieldValue, elem);
					Selection sel = lookup_field(bt, fv->field->Ident.interned, false);
					GB_ASSERT(sel.index.count == 1);
					index = sel.index[0];
					value_expr = fv->value;
				}
				Type *ft = nullptr;
				i64 offset = type_offset_of(bt, index, &ft);
				TypeAndValue tav = type_and_value_of_expr(value_expr);
				wb_write_const_data(m, ft, tav.value, dst + offset);
			}
			return;
		}
		case Type_Array:
		case Type_EnumeratedArray: {
			Type *et = bt->kind == Type_Array ? bt->Array.elem : bt->EnumeratedArray.elem;
			i64 elem_size = type_size_of(et);
			i64 index = 0;
			i64 min_value = bt->kind == Type_EnumeratedArray ? exact_value_to_i64(*bt->EnumeratedArray.min_value) : 0;
			for (Ast *elem : cl->elems) {
				if (elem->kind == Ast_FieldValue) {
					ast_node(fv, FieldValue, elem);
					TypeAndValue tav = type_and_value_of_expr(fv->value);
					if (is_ast_range(fv->field)) {
						ast_node(ie, BinaryExpr, fv->field);
						i64 lo = exact_value_to_i64(ie->left->tav.value) - min_value;
						i64 hi = exact_value_to_i64(ie->right->tav.value) - min_value;
						if (ie->op.kind != Token_RangeHalf) {
							hi += 1;
						}
						for (i64 k = lo; k < hi; k++) {
							wb_write_const_data(m, et, tav.value, dst + k*elem_size);
						}
						index = hi;
					} else {
						index = exact_value_to_i64(fv->field->tav.value) - min_value;
						wb_write_const_data(m, et, tav.value, dst + index*elem_size);
						index++;
					}
				} else {
					TypeAndValue tav = type_and_value_of_expr(elem);
					wb_write_const_data(m, et, tav.value, dst + index*elem_size);
					index++;
				}
			}
			return;
		}
		case Type_Slice: {
			// constant slice literal: elements in the data segment
			Type *et = bt->Slice.elem;
			i64 elem_size = type_size_of(et);
			i64 count = cast(i64)cl->elems.count;
			if (count == 0) {
				return;
			}
			u8 *elems = gb_alloc_array(temporary_allocator(), u8, count*elem_size);
			gb_zero_size(elems, count*elem_size);
			for_array(i, cl->elems) {
				Ast *elem = cl->elems[i];
				GB_ASSERT(elem->kind != Ast_FieldValue);
				TypeAndValue tav = type_and_value_of_expr(elem);
				wb_write_const_data(m, et, tav.value, elems + i*elem_size);
			}
			u32 addr = wb_data_alloc(m, count*elem_size, type_align_of(et));
			wb_data_write(m, addr, elems, count*elem_size);
			wb_write_le(dst, addr, build_context.ptr_size);
			wb_write_le(dst + type_offset_of(bt, 1, nullptr), cast(u64)count, build_context.int_size);
			return;
		}
		default:
			break;
		}
		break;
	}
	default:
		break;
	}
	GB_PANIC("wasm backend: cannot serialize constant of type %s", type_to_string(type));
}

// Places a constant in the data segment and returns its absolute address
gb_internal u32 wb_const_data_addr(wbModule *m, Type *type, ExactValue const &value) {
	if (is_type_untyped(type)) {
		type = default_type(type);
	}
	i64 size = type_size_of(type);
	u8 *bytes = gb_alloc_array(temporary_allocator(), u8, gb_max(size, 1));
	gb_zero_size(bytes, gb_max(size, 1));
	wb_write_const_data(m, type, value, bytes);
	u32 addr = wb_data_alloc(m, size, type_align_of(type));
	wb_data_write(m, addr, bytes, size);
	return addr;
}

// Global variables

gb_internal bool wb_can_serialize_const(Type *type, ExactValue const &value) {
	if (value.kind == ExactValue_Invalid) {
		return true;
	}
	Type *bt = base_type(type);
	switch (value.kind) {
	case ExactValue_Bool:
	case ExactValue_Integer:
	case ExactValue_Float:
	case ExactValue_Pointer:
		return wb_is_scalar(type);
	case ExactValue_String:
		return is_type_string(type) || is_type_cstring(type) || is_type_array(bt);
	case ExactValue_Procedure:
		return is_type_proc(bt);
	case ExactValue_Compound:
		switch (bt->kind) {
		case Type_Struct: case Type_Array: case Type_EnumeratedArray: case Type_Slice:
			break;
		default:
			return false;
		}
		{
			ast_node(cl, CompoundLit, value.value_compound);
			for (Ast *elem : cl->elems) {
				Ast *value_expr = elem;
				Type *et = nullptr;
				if (elem->kind == Ast_FieldValue) {
					value_expr = elem->FieldValue.value;
				}
				TypeAndValue tav = type_and_value_of_expr(value_expr);
				et = tav.type;
				if (is_type_untyped(et)) {
					if (bt->kind == Type_Array) {
						et = bt->Array.elem;
					} else if (bt->kind == Type_EnumeratedArray) {
						et = bt->EnumeratedArray.elem;
					} else if (bt->kind == Type_Slice) {
						et = bt->Slice.elem;
					} else {
						et = default_type(et);
					}
				}
				if (!wb_can_serialize_const(et, tav.value)) {
					return false;
				}
			}
		}
		return true;
	default:
		return false;
	}
}

// Absolute address of a global (or static local) variable, allocating it on first use
// (static locals do not have a DeclInfo, so their initializer is passed in)
gb_internal u32 wb_global_addr(wbModule *m, Entity *e, Ast *init_expr = nullptr) {
	u32 *found = map_get(&m->globals, e);
	if (found) {
		return *found;
	}
	Type *type = e->type;
	u32 addr = wb_data_alloc(m, type_size_of(type), type_align_of(type));
	map_set(&m->globals, e, addr);

	if (init_expr == nullptr && e->decl_info != nullptr) {
		init_expr = e->decl_info->init_expr;
	}
	if (init_expr != nullptr) {
		TypeAndValue tav = type_and_value_of_expr(init_expr);
		if (tav.mode != Addressing_Invalid && tav.value.kind != ExactValue_Invalid && wb_can_serialize_const(type, tav.value)) {
			i64 size = type_size_of(type);
			u8 *bytes = gb_alloc_array(temporary_allocator(), u8, gb_max(size, 1));
			gb_zero_size(bytes, gb_max(size, 1));
			wb_write_const_data(m, type, tav.value, bytes);
			wb_data_write(m, addr, bytes, size);
		} else {
			wbGlobalInit init = {e, init_expr};
			array_add(&m->global_init_queue, init);
		}
	}
	return addr;
}

// Constants

gb_internal wbValue wb_const(wbProcedure *p, Ast *node, Type *type, ExactValue const &value) {
	if (is_type_untyped(type)) {
		type = default_type(type);
	}
	wbValType vt = wb_valtype_of(type);
	if (vt == wbValType_Invalid) {
		// Aggregate constant: lives in the data segment
		if (value.kind == ExactValue_String && is_type_string(type)) {
			return wb_value_memory(WB_NO_LOCAL, cast(i32)wb_intern_string_value(p->module, value.value_string), type);
		}
		if (type_size_of(type) == 0) {
			return wb_value_memory(WB_NO_LOCAL, 0, type);
		}
		if (!wb_can_serialize_const(type, value)) {
			if (value.kind == ExactValue_Compound) {
				// Built at runtime (e.g. contains procedure literals or addresses)
				wbAddr tmp = wb_add_temp(p, type);
				wb_build_compound_lit(p, value.value_compound, tmp);
				return wb_value_memory(tmp.index, tmp.offset, type);
			}
			wb_unsupported(p, node, "constant value");
			return wb_value_invalid();
		}
		return wb_value_memory(WB_NO_LOCAL, cast(i32)wb_const_data_addr(p->module, type, value), type);
	}
	wbValue v = {};
	v.kind = wbValue_Const;
	v.vt   = vt;
	v.type = type;

	switch (value.kind) {
	case ExactValue_Bool:
		v.i = value.value_bool ? 1 : 0;
		break;
	case ExactValue_Integer:
		if (vt == wbValType_f32 || vt == wbValType_f64) {
			v.f = exact_value_to_f64(value);
		} else if (is_type_unsigned(core_type(type))) {
			v.i = cast(i64)big_int_to_u64(&value.value_integer);
		} else {
			v.i = big_int_to_i64(&value.value_integer);
		}
		break;
	case ExactValue_Float:
		if (vt == wbValType_f32 || vt == wbValType_f64) {
			v.f = value.value_float;
		} else {
			v.i = cast(i64)value.value_float;
		}
		break;
	case ExactValue_Pointer:
		v.i = value.value_pointer;
		break;
	case ExactValue_String:
		if (is_type_cstring(type)) {
			v.i = wb_intern_string_bytes(p->module, value.value_string);
			break;
		}
		wb_unsupported(p, node, "string constant of this type");
		return wb_value_invalid();
	case ExactValue_Invalid:
		// nil
		v.i = 0;
		break;
	case ExactValue_Procedure: {
		u32 index = 0;
		if (!wb_const_procedure_index(p->module, p->name, value.value_procedure, &index)) {
			wb_unsupported(p, node, "procedure constant");
			return wb_value_invalid();
		}
		v.i = index;
		break;
	}
	default:
		wb_unsupported(p, node, "constant value kind");
		return wb_value_invalid();
	}

	// Constants of narrow types are stored canonically (see wb_emit_normalize)
	if (vt == wbValType_i32) {
		i64 size = type_size_of(core_type(type));
		if (size == 1) {
			v.i = wb_type_is_signed(type) ? cast(i8)v.i : cast(u8)v.i;
		} else if (size == 2) {
			v.i = wb_type_is_signed(type) ? cast(i16)v.i : cast(u16)v.i;
		} else {
			v.i = cast(i32)v.i;
		}
	}
	return v;
}

#include "wasm_backend_expr.cpp"
#include "wasm_backend_stmt.cpp"

// Procedure bodies

// Storage for a new local variable
gb_internal wbAddr wb_add_variable(wbProcedure *p, Entity *e, Ast *init_expr) {
	wbAddr addr = {};
	if (e->flags & EntityFlag_Static) {
		addr = wb_addr_memory(WB_NO_LOCAL, cast(i32)wb_global_addr(p->module, e, init_expr), e->type);
	} else if (wb_is_scalar(e->type) && !ptr_set_exists(&p->addressed, e)) {
		u32 idx = wb_add_local(p, wb_valtype_of(e->type), e->token.string);
		addr = wb_addr_local(idx, e->type);
	} else {
		addr = wb_add_temp(p, e->type);
	}
	map_set(&p->variables, e, addr);
	return addr;
}

gb_internal void wb_emit_epilogue(wbProcedure *p) {
	wb_local_get(p, p->old_sp_local);
	wb_global_set(p, p->module->global_stack_pointer);
}

gb_internal void wb_finish_procedure(wbProcedure *p) {
	// Frame setup: sp = old_sp - frame_size (kept 16 byte aligned)
	wbBuffer *saved = &p->code;
	wbBuffer code = p->code;
	p->code = p->prologue;
	wb_global_get(p, p->module->global_stack_pointer);
	if (p->frame_size > 0) {
		u32 size = (p->frame_size + 15) & ~15u;
		wb_local_tee(p, p->old_sp_local);
		wb_i32_const(p, cast(i32)size);
		wb_op(p, wbOp_i32_sub);
		wb_local_tee(p, p->fp_local);
		wb_global_set(p, p->module->global_stack_pointer);
	} else {
		wb_local_set(p, p->old_sp_local);
	}
	p->prologue = p->code;
	p->code = code;
	gb_unused(saved);
}

gb_internal void wb_build_procedure(wbProcedure *p) {
	if (p->failed) {
		wb_op(p, wbOp_unreachable);
		return;
	}
	Type *pt = base_type(p->type);

	// Parameters map 1:1 onto the first wasm locals
	if (wb_uses_sret(pt)) {
		p->sret_local = cast(i32)wb_add_local(p, wbValType_i32, str_lit("sret"));
	}
	auto param_locals = array_make<u32>(temporary_allocator(), 0, 8);
	if (pt->Proc.params != nullptr) {
		for (Entity *e : pt->Proc.params->Tuple.variables) {
			wbValType vt = wb_valtype_of(e->type);
			if (vt == wbValType_Invalid) {
				vt = wbValType_i32;
			}
			array_add(&param_locals, wb_add_local(p, vt, e->token.string));
		}
	}
	if (wb_is_odin_cc(pt)) {
		p->context_local = cast(i32)wb_add_local(p, wbValType_i32, str_lit("context"));
	}
	p->param_count = cast(u32)p->locals.count;
	p->old_sp_local = wb_add_local(p, wbValType_i32, str_lit("old_sp"));
	p->fp_local     = wb_add_local(p, wbValType_i32, str_lit("fp"));

	wb_prescan_addressed(p, p->body);

	if (pt->Proc.params != nullptr) {
		for_array(i, pt->Proc.params->Tuple.variables) {
			Entity *e = pt->Proc.params->Tuple.variables[i];
			u32 idx = param_locals[i];
			if (e->token.string.len == 0 || is_blank_ident(e->token.string)) {
				continue;
			}
			if (wb_is_scalar(e->type)) {
				if (ptr_set_exists(&p->addressed, e)) {
					wbAddr addr = wb_add_temp(p, e->type);
					wb_emit_store(p, addr.index, addr.offset, wb_value_local(idx, p->locals[idx].vt, e->type), e->type);
					map_set(&p->variables, e, addr);
				} else {
					map_set(&p->variables, e, wb_addr_local(idx, e->type));
				}
			} else {
				// Aggregates are passed as a pointer to a caller-owned copy
				map_set(&p->variables, e, wb_addr_memory(idx, 0, e->type));
			}
		}
	}

	// Named results are ordinary variables, zero initialized
	if (pt->Proc.has_named_results && pt->Proc.results != nullptr) {
		for (Entity *e : pt->Proc.results->Tuple.variables) {
			wbAddr addr = wb_add_variable(p, e);
			wb_addr_zero(p, addr);
			array_add(&p->result_addrs, addr);
		}
	}

	wb_open_scope(p);
	wb_build_stmt(p, p->body);
	wb_close_scope(p);

	if (p->results.count > 0 || p->sret_local >= 0) {
		// Falling off the end of a procedure with results is not allowed by the
		// checker, but wasm validation requires a well-typed end.
		wb_op(p, wbOp_unreachable);
	} else {
		wb_emit_epilogue(p);
	}
	GB_ASSERT(p->depth == 0);
	wb_finish_procedure(p);
}

// Generates the start function that runs non-constant global initializers
gb_internal void wb_generate_startup(wbModule *m) {
	if (m->global_init_queue.count == 0) {
		return;
	}
	wbProcedure *p = wb_alloc_procedure(m, str_lit("__odin_wasm_startup"));
	wbFuncType ft = {};
	array_init(&ft.params,  m->allocator);
	array_init(&ft.results, m->allocator);
	p->type_index = wb_add_functype(m, ft);
	p->old_sp_local = wb_add_local(p, wbValType_i32, str_lit("old_sp"));
	p->fp_local     = wb_add_local(p, wbValType_i32, str_lit("fp"));
	array_add(&m->procedures, p);
	m->startup = p;

	wb_open_scope(p);
	// Lowering an initializer may reference further globals, growing the queue
	for (isize i = 0; i < m->global_init_queue.count; i++) {
		Entity *e = m->global_init_queue[i].entity;
		u32 addr = wb_global_addr(m, e);
		wbValue v = wb_build_expr(p, m->global_init_queue[i].init_expr);
		wb_addr_store(p, wb_addr_memory(WB_NO_LOCAL, cast(i32)addr, e->type), v);
	}
	wb_close_scope(p);
	wb_emit_epilogue(p);
	wb_finish_procedure(p);
}

// Module

gb_internal void wb_module_init(wbModule *m, CheckerInfo *info) {
	m->info = info;
	m->allocator = permanent_allocator();
	array_init(&m->types,      m->allocator);
	array_init(&m->imports,    m->allocator);
	array_init(&m->procedures, m->allocator);
	array_init(&m->work_queue, m->allocator);
	array_init(&m->table,      m->allocator);
	array_add(&m->table, cast(wbProcedure *)nullptr); // index 0 is nil
	map_init(&m->procedure_map);
	map_init(&m->globals);
	string_map_init(&m->string_bytes, 64);
	string_map_init(&m->string_values, 64);
	array_init(&m->data, heap_allocator(), 0, 4096);
	array_init(&m->global_init_queue, m->allocator);

	// Same defaults as the wasm-ld invocation in linker.cpp: 1 MiB stack placed first
	m->stack_size = 1<<20;
	m->data_base = m->stack_size;
	m->global_stack_pointer = 0;
}

gb_internal bool wb_write_output(wbModule *m) {
	wbBuffer out = {};
	wb_buffer_init(&out, heap_allocator(), 1<<16);
	wb_write_module(&out, m);

	String output_filename = path_to_string(heap_allocator(), build_context.build_paths[BuildPath_Output]);
	char const *path = alloc_cstring(temporary_allocator(), output_filename);

	gbFile f = {};
	if (gb_file_create(&f, path) != gbFileError_None) {
		gb_printf_err("wasm backend: unable to write output file '%s'\n", path);
		return false;
	}
	bool ok = gb_file_write(&f, out.data.data, out.data.count);
	gb_file_close(&f);
	if (!ok) {
		gb_printf_err("wasm backend: unable to write output file '%s'\n", path);
	}
	array_free(&out.data);
	return ok;
}

gb_internal bool wb_generate_code(CheckerInfo *info) {
	TIME_SECTION("wasm backend: setup");

	wbModule module = {};
	wbModule *m = &module;
	wb_module_init(m, info);

	// Roots: exported procedures. Everything else is generated on demand
	// when referenced, so unused code never has to be lowered.
	for (Entity *e : info->entities) {
		if (e->kind != Entity_Procedure) {
			continue;
		}
		if ((e->scope->flags & ScopeFlag_File) == 0) {
			continue;
		}
		if (!e->Procedure.is_export || e->Procedure.is_foreign) {
			continue;
		}
		if (e->min_dep_count.load(std::memory_order_relaxed) == 0) {
			continue;
		}
		wb_procedure_for_entity(m, e);
	}

	TIME_SECTION("wasm backend: procedures");
	for (;;) {
		while (m->work_queue.count > 0) {
			wbProcedure *p = array_pop(&m->work_queue);
			wb_build_procedure(p);
		}
		if (m->startup == nullptr && m->global_init_queue.count > 0) {
			wb_generate_startup(m); // may discover more procedures
			continue;
		}
		break;
	}

	if (m->error_count > 0) {
		return false;
	}

	TIME_SECTION("wasm backend: write");
	u32 index = 0;
	for (wbProcedure *p : m->imports) {
		p->func_index = index++;
	}
	for (wbProcedure *p : m->procedures) {
		p->func_index = index++;
	}
	for (wbProcedure *p : m->procedures) {
		wb_patch_call_relocs(p);
	}

	// Memory: stack, data and one spare page for the heap
	u64 data_end = cast(u64)m->data_base + cast(u64)m->data.count;
	m->memory_initial_pages = cast(u32)((data_end + 0xffff) / 0x10000) + 1;

	return wb_write_output(m);
}
