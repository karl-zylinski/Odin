// Direct WebAssembly backend: procedures, values, memory and the module.
// See wasm_backend.hpp for an overview.
//
//   wasm_backend.cpp      - this file: types, procedures, values, frame, data, module
//   wasm_backend_expr.cpp - expressions
//   wasm_backend_stmt.cpp - statements

#include "wasm_backend.hpp"
#include "wasm_backend_emit.cpp"
#include "wasm_backend_opt.cpp"

gb_internal wbValue wb_build_expr(wbProcedure *p, Ast *expr);
gb_internal wbAddr  wb_build_addr(wbProcedure *p, Ast *expr);
gb_internal void    wb_build_stmt(wbProcedure *p, Ast *node);
gb_internal void    wb_open_scope(wbProcedure *p);
gb_internal void    wb_close_scope(wbProcedure *p);
gb_internal wbAddr  wb_add_variable(wbProcedure *p, Entity *e, Ast *init_expr = nullptr);
gb_internal void    wb_emit_epilogue(wbProcedure *p);
gb_internal wbValue wb_emit_conv(wbProcedure *p, wbValue v, Type *dst);
gb_internal wbValue wb_emit_byte_swap(wbProcedure *p, wbValue x, Type *type);
gb_internal wbValue wb_emit_to_platform_endian(wbProcedure *p, wbValue x);
gb_internal wbValue wb_emit_from_platform_endian(wbProcedure *p, wbValue x, Type *type);
gb_internal wbValue wb_value_copy(wbProcedure *p, wbValue v);
gb_internal wbProcedure *wb_procedure_for_entity(wbModule *m, Entity *e);
gb_internal void wb_build_test_main_body(wbProcedure *p);
gb_internal void wb_emit_call_no_args(wbProcedure *p, Entity *e);
gb_internal u32 wb_data_alloc(wbModule *m, i64 size, i64 align);
gb_internal u32     wb_table_index(wbModule *m, wbProcedure *p);
gb_internal void    wb_gc_procedures(wbModule *m);
gb_internal wbProcedure *wb_hasher_proc_for_type(wbModule *m, Type *type);
gb_internal wbProcedure *wb_equal_proc_for_type(wbModule *m, Type *type);
gb_internal void    wb_build_compound_lit(wbProcedure *p, Ast *expr, wbAddr dst);
gb_internal void    wb_prescan_addressed(wbProcedure *p, Ast *node);
gb_internal void    wb_emit_global_inits(wbProcedure *p);
gb_internal void    wb_emit_call_no_args(wbProcedure *p, Entity *e);
gb_internal wbValue wb_emit_call(wbProcedure *p, Type *pt, wbProcedure *callee, wbValue proc_value, Array<wbValue> const &args);
gb_internal wbAddr  wb_context_addr(wbProcedure *p);
gb_internal wbAddr  wb_context_for_write(wbProcedure *p);
gb_internal void    wb_push_context_ptr(wbProcedure *p, wbAddr ctx);
gb_internal void    wb_build_hasher_body(wbProcedure *p);
gb_internal void    wb_build_equal_body(wbProcedure *p);
gb_internal void    wb_build_map_get_body(wbProcedure *p);
gb_internal void    wb_build_map_set_body(wbProcedure *p);
gb_internal wbValue wb_map_load(wbProcedure *p, wbAddr addr);
gb_internal void    wb_map_set(wbProcedure *p, wbAddr addr, wbValue v);
gb_internal wbValue wb_map_get_ptr(wbProcedure *p, wbAddr addr, Type *ptr_type);
gb_internal wbValue wb_soa_load(wbProcedure *p, wbAddr addr);
gb_internal void    wb_soa_store(wbProcedure *p, wbAddr addr, wbValue v);
gb_internal wbValue wb_soa_get_ptr(wbProcedure *p, wbAddr addr, Type *ptr_type);
gb_internal wbValue wb_soa_swizzle_load(wbProcedure *p, wbAddr addr);
gb_internal void    wb_soa_swizzle_store(wbProcedure *p, wbAddr addr, wbValue v);
gb_internal wbAddr  wb_addr_soa_variable(wbAddr soa, wbValue index, Ast *index_expr);
gb_internal wbAddr  wb_addr_soa_variable_from_soa_ptr(wbProcedure *p, wbValue soa_ptr);
gb_internal wbAddr  wb_soa_field_addr(wbProcedure *p, Ast *expr, wbAddr addr, Selection const &sel);
gb_internal wbAddr  wb_soa_swizzle_addr(wbProcedure *p, wbAddr addr, Type *type, u8 count, u8 const *indices);
gb_internal wbAddr  wb_soa_field_elem_addr(wbProcedure *p, wbAddr soa, wbValue field, wbValue index);
gb_internal wbAddr  wb_soa_container(wbAddr addr);
gb_internal isize   wb_soa_field_count(Type *soa_type);
gb_internal i64     wb_soa_elem_field_offset(Type *elem_type, isize i, Type **field_type);
gb_internal wbAddr  wb_soa_container_of_expr(wbProcedure *p, Ast *expr);
gb_internal wbAddr  wb_soa_elem_index_addr(wbProcedure *p, wbAddr addr, wbValue index, Ast *index_expr);
gb_internal wbValue wb_soa_len(wbProcedure *p, wbAddr soa);
gb_internal wbValue wb_soa_cap(wbProcedure *p, wbAddr soa);
gb_internal wbValue wb_build_soa_slice_expr(wbProcedure *p, Ast *expr);
gb_internal wbValue wb_build_soa_zip(wbProcedure *p, Ast *expr);
gb_internal wbValue wb_build_atomic_call(wbProcedure *p, Ast *expr, BuiltinProcId id);
gb_internal wbValue wb_build_soa_unzip(wbProcedure *p, Ast *expr);
gb_internal void    wb_build_soa_copy_from_slice(wbProcedure *p, Ast *expr);
gb_internal void    wb_build_range_soa(wbProcedure *p, AstRangeStmt *rs, Ast *node, Ast *val0, Ast *val1);

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
gb_internal bool wb_is_f16(Type *t) {
	t = core_type(t);
	return is_type_float(t) && type_size_of(t) == 2;
}

// IEEE 754 binary32 -> binary16 (round to nearest even), for constants
gb_internal u16 wb_f32_to_f16_bits(f32 value) {
	u32 i = 0;
	gb_memmove(&i, &value, 4);
	u32 sign = (i >> 16) & 0x8000;
	i32 exp  = cast(i32)((i >> 23) & 0xff) - 127 + 15;
	u32 mant = i & 0x7fffff;
	if (((i >> 23) & 0xff) == 0xff) {
		// inf or nan
		return cast(u16)(sign | 0x7c00 | (mant ? 0x200 | (mant >> 13) : 0));
	}
	if (exp >= 0x1f) {
		return cast(u16)(sign | 0x7c00);
	}
	if (exp <= 0) {
		if (exp < -10) {
			return cast(u16)sign;
		}
		mant |= 0x800000;
		u32 shift = cast(u32)(14 - exp);
		u32 half = mant >> shift;
		u32 rem  = mant & ((1u << shift) - 1);
		u32 mid  = 1u << (shift - 1);
		if (rem > mid || (rem == mid && (half & 1))) {
			half++;
		}
		return cast(u16)(sign | half);
	}
	u32 half = sign | (cast(u32)exp << 10) | (mant >> 13);
	u32 rem = mant & 0x1fff;
	if (rem > 0x1000 || (rem == 0x1000 && (half & 1))) {
		half++;
	}
	return cast(u16)half;
}

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
			case 2: return wbValType_i32; // f16: raw bits, converted through the runtime's helpers
			case 4: return wbValType_f32;
			case 8: return wbValType_f64;
			}
			return wbValType_Invalid;
		}
		if (t->Basic.kind == Basic_rawptr || t->Basic.kind == Basic_uintptr || t->Basic.kind == Basic_cstring || t->Basic.kind == Basic_cstring16) {
			return wbValType_i32;
		}
		if (t->Basic.kind == Basic_typeid) {
			return wbValType_i64; // typeid is always 64 bits
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

// Signatures follow the ABI the LLVM backend produces for wasm (lbAbiWasm in
// llvm_abi.cpp) so that imports, exports and the vendor objects linked in
// match: aggregates that consist of basic fields are flattened into one wasm
// parameter per field, anything else is passed as a pointer. The same rules
// apply to every procedure, whatever its calling convention, so procedure
// values of foreign and Odin procedures are interchangeable.

// A scalar piece of an aggregate passed directly
struct wbAbiLeaf {
	i64   offset;
	Type *type;
};

gb_internal bool wb_abi_is_basic(Type *t) {
	if (!wb_is_scalar(t)) {
		return false;
	}
	return type_size_of(t) <= 8;
}

gb_internal bool wb_abi_is_int128(Type *t) {
	t = core_type(t);
	return t->kind == Type_Basic && (t->Basic.flags & BasicFlag_Integer) != 0 && type_size_of(t) == 16;
}

// Whether `t` is passed directly on its own or as part of an aggregate
// (lbAbiWasm::is_basic_register_type): a basic scalar, or a 128 bit integer
// which is split into two i64.
gb_internal bool wb_abi_is_register(Type *t) {
	return wb_abi_is_basic(t) || wb_abi_is_int128(t);
}

gb_internal void wb_abi_add_register(Type *t, i64 base_offset, Array<wbAbiLeaf> *leaves) {
	if (wb_abi_is_int128(t)) {
		wbAbiLeaf lo = {base_offset,   t_u64};
		wbAbiLeaf hi = {base_offset+8, t_u64};
		array_add(leaves, lo);
		array_add(leaves, hi);
	} else {
		wbAbiLeaf leaf = {base_offset, t};
		array_add(leaves, leaf);
	}
}

// Appends the scalar leaves of `t` if it is passed directly, returns false if
// it is passed indirectly (through a pointer). Zero sized types have no leaves.
gb_internal bool wb_abi_flatten(Type *t, ProcCallingConvention cc, i64 base_offset, Array<wbAbiLeaf> *leaves) {
	Type *bt = base_type(t);
	i64 size = type_size_of(t);
	if (size == 0) {
		return true;
	}
	if (wb_abi_is_register(t)) {
		wb_abi_add_register(t, base_offset, leaves);
		return true;
	}
	if (bt->kind == Type_Union && is_type_union_maybe_pointer(bt)) {
		// represented as the pointer itself (nil when null)
		wbAbiLeaf leaf = {base_offset, t_rawptr};
		array_add(leaves, leaf);
		return true;
	}
	if (cc == ProcCC_CDecl) {
		// Basic C ABI: only single field structs are passed by value
		if (bt->kind == Type_Struct && !bt->Struct.is_raw_union && bt->Struct.fields.count == 1) {
			Type *ft = nullptr;
			i64 offset = type_offset_of(bt, 0, &ft);
			return wb_abi_flatten(ft, cc, base_offset+offset, leaves);
		}
		return false;
	}
	if (size > 32) {
		return false;
	}
	switch (bt->kind) {
	case Type_Array:
	case Type_Matrix: {
		// Matrices are arrays of their element type, padding included
		Type *elem = bt->kind == Type_Array ? bt->Array.elem : bt->Matrix.elem;
		if (!wb_abi_is_register(elem)) {
			return false;
		}
		i64 elem_size = type_size_of(elem);
		for (i64 i = 0; i < size/elem_size; i++) {
			wb_abi_add_register(elem, base_offset + i*elem_size, leaves);
		}
		return true;
	}
	case Type_Slice:
		{
			wbAbiLeaf data = {base_offset, t_rawptr};
			wbAbiLeaf len  = {base_offset + build_context.int_size, t_int};
			array_add(leaves, data);
			array_add(leaves, len);
		}
		return true;
	case Type_Basic:
		if (is_type_string(bt)) {
			wbAbiLeaf data = {base_offset, t_rawptr};
			wbAbiLeaf len  = {base_offset + build_context.int_size, t_int};
			array_add(leaves, data);
			array_add(leaves, len);
			return true;
		}
		if (is_type_any(bt)) {
			Type *ft = nullptr;
			wbAbiLeaf data = {base_offset, t_rawptr};
			wbAbiLeaf id   = {base_offset + type_offset_of(bt, 1, &ft), t_typeid};
			array_add(leaves, data);
			array_add(leaves, id);
			return true;
		}
		if (is_type_complex(bt) || is_type_quaternion(bt)) {
			Type *elem = base_complex_elem_type(bt);
			i64 elem_size = type_size_of(elem);
			for (i64 i = 0; i < size/elem_size; i++) {
				wbAbiLeaf leaf = {base_offset + i*elem_size, elem};
				array_add(leaves, leaf);
			}
			return true;
		}
		return false;
	case Type_Struct:
		if (bt->Struct.is_raw_union) {
			return false;
		}
		for_array(i, bt->Struct.fields) {
			if (!wb_abi_is_register(bt->Struct.fields[i]->type)) {
				return false;
			}
		}
		for_array(i, bt->Struct.fields) {
			Type *ft = nullptr;
			i64 offset = type_offset_of(bt, i, &ft);
			wb_abi_add_register(ft, base_offset+offset, leaves);
		}
		return true;
	}
	return false;
}

// Computes the wasm signature of a procedure type (see the calling convention
// description in wasm_backend.hpp)
gb_internal void wb_functype_of_proc(wbModule *m, Type *pt, wbFuncType *ft) {
	pt = base_type(pt);
	GB_ASSERT(pt->kind == Type_Proc);
	array_init(&ft->params,  m->allocator);
	array_init(&ft->results, m->allocator);
	ProcCallingConvention cc = pt->Proc.calling_convention;

	// Results that are not a single wasm value are returned through a pointer
	// passed as the first parameter (the C ABI's sret)
	if (wb_uses_sret(pt)) {
		array_add(&ft->params, wbValType_i32);
	}

	if (pt->Proc.params != nullptr) {
		for_array(i, pt->Proc.params->Tuple.variables) {
			Entity *e = pt->Proc.params->Tuple.variables[i];
			if (e->kind != Entity_Variable) {
				continue; // polymorphic parameters ($T, constants) take no space
			}
			if (pt->Proc.c_vararg && pt->Proc.variadic && i == pt->Proc.variadic_index) {
				continue; // the C variadic arguments are passed through the buffer pointer below
			}
			auto leaves = array_make<wbAbiLeaf>(temporary_allocator(), 0, 8);
			if (wb_abi_flatten(e->type, cc, 0, &leaves)) {
				for (wbAbiLeaf const &leaf : leaves) {
					array_add(&ft->params, wb_valtype_of(leaf.type));
				}
			} else {
				array_add(&ft->params, wbValType_i32);
			}
		}
	}
	if (pt->Proc.c_vararg) {
		// LLVM's wasm ABI passes the variadic arguments in a stack buffer whose
		// address is the final parameter
		array_add(&ft->params, wbValType_i32);
	}
	if (cc == ProcCC_Odin) {
		array_add(&ft->params, wbValType_i32); // context pointer
	}
	if (pt->Proc.result_count == 1 && !wb_uses_sret(pt)) {
		Type *rt = wb_result_type(pt);
		if (type_size_of(rt) > 0) {
			array_add(&ft->results, wb_valtype_of(rt));
		}
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
	array_init(&p->instrs,       m->allocator);
	array_init(&p->slots,        m->allocator);
	array_init(&p->defers,       m->allocator);
	array_init(&p->scopes,       m->allocator);
	array_init(&p->context_stack, m->allocator);
	map_init(&p->variables);
	map_init(&p->selector_values);
	map_init(&p->selector_addrs);
	ptr_set_init(&p->addressed);
	ptr_set_init(&p->aliased);
	ptr_set_init(&p->unchecked);
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

	// The runtime declares the startup/cleanup procedures as foreign and the
	// backend supplies them (lb_generate_startup_runtime and friends)
	if (p->is_foreign && p->name == str_lit("__$startup_runtime")) {
		p->is_foreign = false;
		p->gen = wbProcGen_StartupRuntime;
		m->startup_runtime = p;
	} else if (p->is_foreign && p->name == str_lit("__$cleanup_runtime")) {
		p->is_foreign = false;
		p->gen = wbProcGen_CleanupRuntime;
		m->cleanup_runtime = p;
	}

	wbFuncType ft = {};
	wb_functype_of_proc(m, e->type, &ft);
	p->type_index = wb_add_functype(m, ft);
	for (wbValType vt : ft.results) {
		array_add(&p->results, vt);
	}

	if (p->is_foreign && string_starts_with(p->name, str_lit("llvm."))) {
		// LLVM intrinsics are lowered at the call site (wb_build_llvm_intrinsic_call)
		p->is_llvm_intrinsic = true;
	} else if (p->is_foreign) {
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
	} else if (p->gen != wbProcGen_Body) {
		// generated last, when all global variables are known (see wb_generate_code)
		array_add(&m->procedures, p);
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
	if (p->is_llvm_intrinsic) {
		// LLVM intrinsics are lowered at the call site, there is no function to point to
		if (!p->failed) {
			gb_printf_err("wasm backend: taking the address of intrinsic '%.*s' is not supported\n", LIT(p->name));
			m->error_count += 1;
			p->failed = true;
		}
		return 0;
	}
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
	wbProcedure *p = wb_procedure_for_entity(m, e);
	if (string_ends_with(p->name, str_lit("_error")) || string_starts_with(p->name, str_lit("type_assertion_check"))) {
		// Only reached when a check the backend emitted inline has already failed
		p->never_inline = true;
	}
	return p;
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
	if (!is_type_integer(t) && !is_type_boolean(t) && !is_type_rune(t) && !is_type_bit_set(t) && !wb_is_f16(t)) {
		return;
	}
	i64 size = type_size_of(t);
	bool is_signed = wb_type_is_signed(t) && !wb_is_f16(t);
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
	wbFrameSlot slot = {cast(u32)offset, cast(u32)size};
	array_add(&p->slots, slot);
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

// True for the procedures that run the global variable initializers
gb_internal bool wb_is_startup_proc(wbProcedure *p) {
	return p == p->module->startup || p == p->module->startup_runtime;
}

// Frame storage, or static storage in a startup procedure (where a value
// referenced by a global initializer must outlive the procedure)
gb_internal wbAddr wb_add_temp_or_static(wbProcedure *p, Type *type) {
	if (wb_is_startup_proc(p)) {
		u32 addr = wb_data_alloc(p->module, type_size_of(type), type_align_of(type));
		return wb_addr_memory(WB_NO_LOCAL, cast(i32)addr, type);
	}
	return wb_add_temp(p, type);
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
	} else if (offset < 0) {
		// memarg offsets are unsigned, so negative offsets must be folded into the address
		wb_push_address(p, base, offset);
		wb_emit_load_op(p, type, 0);
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
	} else if (offset < 0) {
		wb_push_address(p, base, offset);
		offset = 0;
	} else {
		wb_local_get(p, base);
	}
	wb_push(p, v);
	wb_emit_store_op(p, type, cast(u32)offset);
}

// Engines implement `memory.copy`/`memory.fill` as out-of-line runtime calls, so
// copies of small aggregates (vectors, slices, strings, contexts...) are expanded
// into plain loads and stores up to this many bytes.
enum { WB_INLINE_COPY_MAX = 128 };

// Pushes the base address for a memory access at local[base] + offset and
// returns the (non-negative) offset left for the memarg immediate.
gb_internal u32 wb_push_memarg_base(wbProcedure *p, u32 base, i32 offset) {
	if (base == WB_NO_LOCAL) {
		wb_i32_const(p, 0);
		return cast(u32)offset;
	}
	if (offset < 0) {
		wb_push_address(p, base, offset);
		return 0;
	}
	wb_local_get(p, base);
	return cast(u32)offset;
}

gb_internal void wb_emit_copy(wbProcedure *p, u32 dst_base, i32 dst_offset, u32 src_base, i32 src_offset, i64 size) {
	if (size <= 0) {
		return;
	}
	if (dst_base == src_base && dst_offset == src_offset) {
		return;
	}
	// The inline expansion has memcpy semantics (chunks are copied in order), which is
	// what aggregate assignment through pointers gets on the other backends too. Copies
	// within the same frame have static offsets, so those can be checked for overlap.
	bool overlaps = dst_base == src_base && dst_base != WB_NO_LOCAL &&
	                dst_offset > src_offset && dst_offset < src_offset + size;
	if (size > WB_INLINE_COPY_MAX || overlaps) {
		wb_push_address(p, dst_base, dst_offset);
		wb_push_address(p, src_base, src_offset);
		wb_i32_const(p, cast(i32)size);
		wb_memory_copy(p);
		return;
	}
	for (i64 done = 0; done < size; ) {
		i64 rem = size - done;
		i64 chunk = rem >= 8 ? 8 : rem >= 4 ? 4 : rem >= 2 ? 2 : 1;
		u32 doff = wb_push_memarg_base(p, dst_base, dst_offset + cast(i32)done);
		u32 soff = wb_push_memarg_base(p, src_base, src_offset + cast(i32)done);
		switch (chunk) {
		case 8: wb_memarg(p, wbOp_i64_load,     soff, 1); wb_memarg(p, wbOp_i64_store,   doff, 1); break;
		case 4: wb_memarg(p, wbOp_i32_load,     soff, 1); wb_memarg(p, wbOp_i32_store,   doff, 1); break;
		case 2: wb_memarg(p, wbOp_i32_load16_u, soff, 1); wb_memarg(p, wbOp_i32_store16, doff, 1); break;
		case 1: wb_memarg(p, wbOp_i32_load8_u,  soff, 1); wb_memarg(p, wbOp_i32_store8,  doff, 1); break;
		}
		done += chunk;
	}
}

// Appends the scalar leaves (field/element offsets and types) of `type`, returns
// false for types that have to be copied as raw bytes (unions, huge arrays...)
gb_internal bool wb_copy_leaves(Type *type, i64 base_offset, Array<wbAbiLeaf> *leaves) {
	enum { MAX_LEAVES = 32 };
	if (leaves->count >= MAX_LEAVES) {
		return false;
	}
	if (wb_is_scalar(type)) {
		wbAbiLeaf leaf = {base_offset, type};
		array_add(leaves, leaf);
		return true;
	}
	Type *bt = base_type(type);
	switch (bt->kind) {
	case Type_Struct:
		if (bt->Struct.is_raw_union) {
			return false;
		}
		for_array(i, bt->Struct.fields) {
			Type *ft = nullptr;
			i64 offset = type_offset_of(bt, i, &ft);
			if (!wb_copy_leaves(ft, base_offset+offset, leaves)) {
				return false;
			}
		}
		return true;
	case Type_Array:
	case Type_EnumeratedArray:
	case Type_Matrix: {
		// Matrices are arrays of their element type (the padding is copied as a gap)
		Type *elem = bt->kind == Type_Array ? bt->Array.elem : bt->kind == Type_EnumeratedArray ? bt->EnumeratedArray.elem : bt->Matrix.elem;
		i64 elem_size = type_size_of(elem);
		i64 count = bt->kind == Type_Array ? bt->Array.count : bt->kind == Type_EnumeratedArray ? bt->EnumeratedArray.count : matrix_type_total_internal_elems(bt);
		if (elem_size == 0) {
			return true;
		}
		for (i64 i = 0; i < count; i++) {
			if (!wb_copy_leaves(elem, base_offset + i*elem_size, leaves)) {
				return false;
			}
		}
		return true;
	}
	case Type_Map:
		return t_raw_map != nullptr && wb_copy_leaves(t_raw_map, base_offset, leaves);
	case Type_DynamicArray: {
		// Raw_Dynamic_Array: data, len, cap, allocator{procedure, data}
		i64 ps = build_context.ptr_size;
		wbAbiLeaf data = {base_offset,        t_rawptr};
		wbAbiLeaf len  = {base_offset + ps,   t_int};
		wbAbiLeaf cap  = {base_offset + 2*ps, t_int};
		wbAbiLeaf ap   = {base_offset + 3*ps, t_rawptr};
		wbAbiLeaf ad   = {base_offset + 4*ps, t_rawptr};
		array_add(leaves, data);
		array_add(leaves, len);
		array_add(leaves, cap);
		array_add(leaves, ap);
		array_add(leaves, ad);
		return true;
	}
	case Type_Slice: case Type_Proc: case Type_Pointer: case Type_MultiPointer: case Type_SoaPointer:
	case Type_Basic:
		if (is_type_string(bt) || is_type_any(bt) || bt->kind == Type_Slice) {
			wbAbiLeaf data = {base_offset, t_rawptr};
			wbAbiLeaf len  = {base_offset + build_context.int_size, is_type_any(bt) ? t_typeid : t_int};
			array_add(leaves, data);
			array_add(leaves, len);
			return true;
		}
		if (is_type_complex(bt) || is_type_quaternion(bt)) {
			Type *elem = base_complex_elem_type(bt);
			i64 elem_size = type_size_of(elem);
			for (i64 i = 0; i < type_size_of(bt)/elem_size; i++) {
				wbAbiLeaf leaf = {base_offset + i*elem_size, elem};
				array_add(leaves, leaf);
			}
			return true;
		}
		return false;
	default:
		return false;
	}
}

// Copies a value of `type` leaf by leaf: loads and stores of the same width as the
// accesses of the surrounding code let the engine forward stores to later loads
// (and CPUs avoid store forwarding stalls), which the raw chunk copy defeats.
// The gaps between leaves (padding) are copied as raw bytes.
gb_internal void wb_emit_copy(wbProcedure *p, u32 dst_base, i32 dst_offset, u32 src_base, i32 src_offset, Type *type) {
	i64 size = type_size_of(type);
	if (size <= 0 || (dst_base == src_base && dst_offset == src_offset)) {
		return;
	}
	if (size > WB_INLINE_COPY_MAX) {
		wb_emit_copy(p, dst_base, dst_offset, src_base, src_offset, size);
		return;
	}
	auto leaves = array_make<wbAbiLeaf>(temporary_allocator(), 0, 16);
	if (!wb_copy_leaves(type, 0, &leaves)) {
		wb_emit_copy(p, dst_base, dst_offset, src_base, src_offset, size);
		return;
	}
	i64 done = 0;
	for (wbAbiLeaf const &leaf : leaves) {
		if (leaf.offset > done) {
			wb_emit_copy(p, dst_base, dst_offset + cast(i32)done, src_base, src_offset + cast(i32)done, leaf.offset - done);
		}
		i64 leaf_size = type_size_of(leaf.type);
		u32 doff = wb_push_memarg_base(p, dst_base, dst_offset + cast(i32)leaf.offset);
		u32 soff = wb_push_memarg_base(p, src_base, src_offset + cast(i32)leaf.offset);
		wb_emit_load_op(p, leaf.type, soff);
		wb_emit_store_op(p, leaf.type, doff);
		done = leaf.offset + leaf_size;
	}
	if (size > done) {
		wb_emit_copy(p, dst_base, dst_offset + cast(i32)done, src_base, src_offset + cast(i32)done, size - done);
	}
}

gb_internal void wb_emit_zero(wbProcedure *p, u32 base, i32 offset, i64 size) {
	if (size <= 0) {
		return;
	}
	if (size > WB_INLINE_COPY_MAX) {
		wb_push_address(p, base, offset);
		wb_i32_const(p, 0);
		wb_i32_const(p, cast(i32)size);
		wb_memory_fill(p);
		return;
	}
	for (i64 done = 0; done < size; ) {
		i64 rem = size - done;
		i64 chunk = rem >= 8 ? 8 : rem >= 4 ? 4 : rem >= 2 ? 2 : 1;
		u32 doff = wb_push_memarg_base(p, base, offset + cast(i32)done);
		if (chunk == 8) {
			wb_i64_const(p, 0);
		} else {
			wb_i32_const(p, 0);
		}
		switch (chunk) {
		case 8: wb_memarg(p, wbOp_i64_store,   doff, 1); break;
		case 4: wb_memarg(p, wbOp_i32_store,   doff, 1); break;
		case 2: wb_memarg(p, wbOp_i32_store16, doff, 1); break;
		case 1: wb_memarg(p, wbOp_i32_store8,  doff, 1); break;
		}
		done += chunk;
	}
}

// Zeroes a value of `type` leaf by leaf (see wb_emit_copy)
gb_internal void wb_emit_zero(wbProcedure *p, u32 base, i32 offset, Type *type) {
	i64 size = type_size_of(type);
	if (size <= 0) {
		return;
	}
	auto leaves = array_make<wbAbiLeaf>(temporary_allocator(), 0, 16);
	if (size > WB_INLINE_COPY_MAX || !wb_copy_leaves(type, 0, &leaves)) {
		wb_emit_zero(p, base, offset, size);
		return;
	}
	i64 done = 0;
	for (wbAbiLeaf const &leaf : leaves) {
		if (leaf.offset > done) {
			wb_emit_zero(p, base, offset + cast(i32)done, leaf.offset - done);
		}
		wbValue zero = {};
		zero.kind = wbValue_Const;
		zero.vt   = wb_valtype_of(leaf.type);
		zero.type = leaf.type;
		wb_emit_store(p, base, offset + cast(i32)leaf.offset, zero, leaf.type);
		done = leaf.offset + type_size_of(leaf.type);
	}
	if (size > done) {
		wb_emit_zero(p, base, offset + cast(i32)done, size - done);
	}
}

// Addresses

// Bit-field fields are accessed byte by byte: byte `b` of the bytes covering the field holds
// field bits [8*b - shift, 8*b - shift + 8) where shift = bit_offset % 8.
gb_internal void wb_emit_bit_field_shift(wbProcedure *p, i64 s) {
	if (s > 0) {
		wb_i64_const(p, s);
		wb_op(p, wbOp_i64_shl);
	} else if (s < 0) {
		wb_i64_const(p, -s);
		wb_op(p, wbOp_i64_shr_u);
	}
}

gb_internal wbValue wb_addr_load_bit_field(wbProcedure *p, wbAddr addr) {
	Type *ct = core_type(addr.type);
	wbValType vt = wb_valtype_of(ct);
	if (vt != wbValType_i32 && vt != wbValType_i64) {
		wb_unsupported_type(p, p->curr_stmt, addr.type);
		return wb_value_invalid();
	}
	i64 bit_size = addr.bit_size;
	i64 shift    = addr.bit_offset % 8;
	i64 start    = addr.bit_offset / 8;
	i64 nbytes   = (shift + bit_size + 7) / 8;

	if (addr.bit_field_in_local) {
		// backing value held in a register
		wb_local_get(p, addr.index);
		if (p->locals[addr.index].vt == wbValType_i32) {
			wb_op(p, wbOp_i64_extend_i32_u);
		}
		wb_emit_bit_field_shift(p, -addr.bit_offset);
	} else {
		u32 acc = wb_add_local(p, wbValType_i64);
		wb_i64_const(p, 0);
		wb_local_set(p, acc);
		for (i64 b = 0; b < nbytes; b++) {
			wb_local_get(p, acc);
			wb_push_address(p, addr.index, addr.offset + cast(i32)(start + b));
			wb_memarg(p, wbOp_i64_load8_u, 0, 1);
			wb_emit_bit_field_shift(p, 8*b - shift);
			wb_op(p, wbOp_i64_or);
			wb_local_set(p, acc);
		}
		wb_local_get(p, acc);
	}
	if (bit_size < 64) {
		if (is_type_unsigned(ct) || is_type_boolean(ct)) {
			wb_i64_const(p, cast(i64)((1ull << bit_size) - 1));
			wb_op(p, wbOp_i64_and);
		} else {
			// sign extension
			wb_i64_const(p, 64 - bit_size);
			wb_op(p, wbOp_i64_shl);
			wb_i64_const(p, 64 - bit_size);
			wb_op(p, wbOp_i64_shr_s);
		}
	}
	if (vt == wbValType_i32) {
		wb_op(p, wbOp_i32_wrap_i64);
	}
	return wb_pop_to_local(p, vt, addr.type);
}

gb_internal void wb_addr_store_bit_field(wbProcedure *p, wbAddr addr, wbValue v) {
	Type *ct = core_type(addr.type);
	wbValType vt = wb_valtype_of(ct);
	if (vt != wbValType_i32 && vt != wbValType_i64) {
		wb_unsupported_type(p, p->curr_stmt, addr.type);
		return;
	}
	i64 bit_size = addr.bit_size;
	i64 shift    = addr.bit_offset % 8;
	i64 start    = addr.bit_offset / 8;
	i64 nbytes   = (shift + bit_size + 7) / 8;

	wb_push(p, v);
	if (vt == wbValType_i32) {
		wb_op(p, wbOp_i64_extend_i32_u);
	}
	u32 val = wb_add_local(p, wbValType_i64);
	wb_local_set(p, val);

	if (addr.bit_field_in_local) {
		// backing = (backing & ~(mask << off)) | ((val & mask) << off)
		wbValType bvt = p->locals[addr.index].vt;
		u64 mask = bit_size >= 64 ? ~0ull : (1ull << bit_size) - 1;
		wb_local_get(p, addr.index);
		if (bvt == wbValType_i32) {
			wb_op(p, wbOp_i64_extend_i32_u);
		}
		wb_i64_const(p, cast(i64)~(mask << addr.bit_offset));
		wb_op(p, wbOp_i64_and);
		wb_local_get(p, val);
		wb_i64_const(p, cast(i64)mask);
		wb_op(p, wbOp_i64_and);
		wb_emit_bit_field_shift(p, addr.bit_offset);
		wb_op(p, wbOp_i64_or);
		if (bvt == wbValType_i32) {
			wb_op(p, wbOp_i32_wrap_i64);
		}
		wb_local_set(p, addr.index);
		return;
	}

	for (i64 b = 0; b < nbytes; b++) {
		i64 byte_bit = 8*(start + b);
		i64 lo = gb_max(cast(i64)addr.bit_offset, byte_bit);
		i64 hi = gb_min(cast(i64)addr.bit_offset + bit_size, byte_bit + 8);
		i32 mask = cast(i32)(((1 << (hi - lo)) - 1) << (lo - byte_bit));
		// byte = (byte & ~mask) | ((val >> (8*b - shift)) & mask)
		wb_push_address(p, addr.index, addr.offset + cast(i32)(start + b));
		wb_push_address(p, addr.index, addr.offset + cast(i32)(start + b));
		wb_memarg(p, wbOp_i32_load8_u, 0, 1);
		wb_i32_const(p, ~mask & 0xff);
		wb_op(p, wbOp_i32_and);
		wb_local_get(p, val);
		wb_emit_bit_field_shift(p, shift - 8*b);
		wb_op(p, wbOp_i32_wrap_i64);
		wb_i32_const(p, mask);
		wb_op(p, wbOp_i32_and);
		wb_op(p, wbOp_i32_or);
		wb_memarg(p, wbOp_i32_store8, 0, 1);
	}
}

gb_internal wbValue wb_addr_load(wbProcedure *p, wbAddr addr) {
	switch (addr.kind) {
	case wbAddr_Local:
		return wb_value_local(addr.index, p->locals[addr.index].vt, addr.type);
	case wbAddr_Memory:
		if (wb_is_scalar(addr.type)) {
			return wb_emit_load(p, addr.index, addr.offset, addr.type);
		}
		return wb_value_memory(addr.index, addr.offset, addr.type);
	case wbAddr_Map:
		return wb_map_load(p, addr);
	case wbAddr_SoaVariable:
		return wb_soa_load(p, addr);
	case wbAddr_Swizzle: {
		if (addr.soa_type != nullptr) {
			return wb_soa_swizzle_load(p, addr);
		}
		// gather the selected elements into a temporary
		Type *elem = base_type(addr.type)->Array.elem;
		i64 elem_size = type_size_of(elem);
		wbAddr tmp = wb_add_temp(p, addr.type);
		for (u8 i = 0; i < addr.swizzle_count; i++) {
			wb_emit_copy(p, tmp.index, tmp.offset + cast(i32)(i*elem_size), addr.index, addr.offset + cast(i32)(addr.swizzle_indices[i]*elem_size), elem);
		}
		return wb_value_memory(tmp.index, tmp.offset, addr.type);
	}
	case wbAddr_BitField:
		return wb_addr_load_bit_field(p, addr);
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
			wb_emit_copy(p, addr.index, addr.offset, v.index, v.offset, addr.type);
		} else {
			wb_emit_store(p, addr.index, addr.offset, v, addr.type);
		}
		break;
	case wbAddr_Map:
		wb_map_set(p, addr, v);
		break;
	case wbAddr_SoaVariable:
		wb_soa_store(p, addr, v);
		break;
	case wbAddr_Swizzle: {
		if (addr.soa_type != nullptr) {
			wb_soa_swizzle_store(p, addr, v);
			break;
		}
		// scatter the elements of `v` (copied first: `v.xy = v.yx` must work)
		Type *elem = base_type(addr.type)->Array.elem;
		i64 elem_size = type_size_of(elem);
		v = wb_value_copy(p, v);
		for (u8 i = 0; i < addr.swizzle_count; i++) {
			wb_emit_copy(p, addr.index, addr.offset + cast(i32)(addr.swizzle_indices[i]*elem_size), v.index, v.offset + cast(i32)(i*elem_size), elem);
		}
		break;
	}
	case wbAddr_BitField:
		wb_addr_store_bit_field(p, addr, v);
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
			wb_emit_zero(p, addr.index, addr.offset, addr.type);
		}
		break;
	case wbAddr_Map:
	case wbAddr_Swizzle:
	case wbAddr_BitField:
	case wbAddr_SoaVariable: {
		wbAddr zero = wb_add_temp(p, addr.type);
		wb_addr_zero(p, zero);
		wb_addr_store(p, addr, wb_addr_load(p, zero));
		break;
	}
	default:
		break;
	}
}

// The address of a memory location as a pointer value
gb_internal wbValue wb_addr_get_ptr(wbProcedure *p, wbAddr addr, Type *ptr_type = nullptr) {
	if (ptr_type == nullptr) {
		ptr_type = alloc_type_pointer(addr.type);
	}
	if (addr.kind == wbAddr_Map) {
		// nil when the key is absent
		return wb_map_get_ptr(p, addr, ptr_type);
	}
	if (addr.kind == wbAddr_SoaVariable) {
		return wb_soa_get_ptr(p, addr, ptr_type);
	}
	if (addr.kind == wbAddr_Swizzle || addr.kind == wbAddr_BitField) {
		// the swizzled elements are not contiguous / the bits are not addressable: the pointer is to a copy
		wbValue v = wb_addr_load(p, addr);
		return wb_addr_get_ptr(p, wb_addr_memory(v.index, v.offset, v.type), ptr_type);
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
	wb_emit_copy(p, tmp.index, tmp.offset, v.index, v.offset, v.type);
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

gb_internal void wb_write_le(u8 *dst, u64 bits, i64 size) {
	for (i64 i = 0; i < size; i++) {
		dst[i] = cast(u8)(bits >> (8*i));
	}
}

gb_internal void wb_write_const_data(wbModule *m, Type *type, ExactValue const &value, u8 *dst);
gb_internal i64  wb_compound_lit_elem_count(Slice<Ast *> const &elems, i64 min_value = 0);
gb_internal wbValue wb_value_copy(wbProcedure *p, wbValue v);
gb_internal i32  wb_union_tag_offset(Type *ut);
gb_internal bool wb_union_has_tag(Type *ut);

// A NUL-terminated UTF-16 copy of a string in the data segment, deduplicated
// by its UTF-8 source
gb_internal u32 wb_intern_string16_bytes(wbModule *m, String const &s) {
	u32 *found = string_map_get(&m->string16_bytes, s);
	if (found) {
		return *found;
	}
	String16 s16 = string_to_string16(temporary_allocator(), s);
	u32 addr = wb_data_alloc(m, (s16.len+1)*2, 2);
	u8 *bytes = gb_alloc_array(temporary_allocator(), u8, (s16.len+1)*2);
	for (isize i = 0; i < s16.len; i++) {
		wb_write_le(bytes + 2*i, s16.text[i], 2);
	}
	wb_data_write(m, addr, bytes, (s16.len+1)*2);
	string_map_set(&m->string16_bytes, s, addr);
	return addr;
}

gb_internal u32 wb_intern_string16_value(wbModule *m, String const &s) {
	u32 *found = string_map_get(&m->string16_values, s);
	if (found) {
		return *found;
	}
	u32 addr = wb_data_alloc(m, type_size_of(t_string16), type_align_of(t_string16));
	u8 *bytes = gb_alloc_array(temporary_allocator(), u8, type_size_of(t_string16));
	wb_write_const_data(m, t_string16, exact_value_string(s), bytes);
	wb_data_write(m, addr, bytes, type_size_of(t_string16));
	string_map_set(&m->string16_values, s, addr);
	return addr;
}

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


// The low 64 bits of an integer constant, two's complement for negative values
gb_internal u64 wb_big_int_bits(BigInt const *x) {
	if (x->sign) {
		return cast(u64)big_int_to_i64(x);
	}
	return big_int_to_u64(x);
}

// Writes an integer constant of `size` bytes (sign-extending 128-bit values)
gb_internal void wb_write_le_int(u8 *dst, BigInt const *x, i64 size) {
	if (size <= 8) {
		wb_write_le(dst, wb_big_int_bits(x), size);
		return;
	}
	// 128 bits: the magnitude's two words, negated in two's complement if needed
	BigInt mag = {};
	mp_init(&mag);
	mp_abs(x, &mag);
	u64 lo = mp_get_u64(&mag);
	u64 hi = 0;
	if (mp_count_bits(&mag) > 64) {
		BigInt tmp = {};
		mp_init(&tmp);
		mp_div_2d(&mag, 64, &tmp, nullptr);
		hi = mp_get_u64(&tmp);
		mp_clear(&tmp);
	}
	mp_clear(&mag);
	if (x->sign) {
		lo = ~lo + 1;
		hi = ~hi + (lo == 0 ? 1 : 0);
	}
	wb_write_le(dst, lo, 8);
	wb_write_le(dst + 8, hi, size - 8);
}

// Serializes a constant of `type` into `dst` (which is zeroed and type_size_of(type) bytes)
gb_internal void wb_write_const_data(wbModule *m, Type *type, ExactValue const &value, u8 *dst) {
	if (is_type_untyped(type)) {
		type = default_type(type);
	}
	Type *bt = base_type(type);
	i64 size = type_size_of(type);

	if ((bt->kind == Type_SimdVector || bt->kind == Type_Array || bt->kind == Type_EnumeratedArray) &&
	    value.kind != ExactValue_Compound && value.kind != ExactValue_Invalid && value.kind != ExactValue_String) {
		// a scalar constant of a vector/array type is splat across the elements
		Type *et = nullptr;
		i64 count = 0;
		switch (bt->kind) {
		case Type_SimdVector:      et = bt->SimdVector.elem;      count = bt->SimdVector.count;      break;
		case Type_Array:           et = bt->Array.elem;           count = bt->Array.count;           break;
		case Type_EnumeratedArray: et = bt->EnumeratedArray.elem; count = bt->EnumeratedArray.count; break;
		default: break;
		}
		i64 elem_size = type_size_of(et);
		for (i64 i = 0; i < count; i++) {
			wb_write_const_data(m, et, value, dst + i*elem_size);
		}
		return;
	}

	if (bt->kind == Type_Matrix && value.kind != ExactValue_Compound && value.kind != ExactValue_Invalid) {
		// a scalar constant of a matrix type is the scaled identity matrix
		Type *et = bt->Matrix.elem;
		i64 elem_size = type_size_of(et);
		i64 n = gb_min(bt->Matrix.row_count, bt->Matrix.column_count);
		for (i64 i = 0; i < n; i++) {
			wb_write_const_data(m, et, value, dst + matrix_indices_to_offset(bt, i, i)*elem_size);
		}
		return;
	}

	if (bt->kind == Type_Union && value.kind != ExactValue_Invalid) {
		// Constant union value: the variant's data followed by its tag
		ExactValue v = value;
		if (v.kind == ExactValue_Variant) {
			v = v.value_variant->tav.value;
		}
		Type *variant_type = v.variant_type;
		if (v.kind == ExactValue_Compound) {
			ast_node(cl, CompoundLit, v.value_compound);
			if (variant_type == nullptr || are_types_identical(variant_type, type)) {
				GB_ASSERT(cl->elems.count == 0);
				return; // nil
			}
		}
		if (variant_type == nullptr && bt->Union.variants.count == 1) {
			variant_type = bt->Union.variants[0];
		}
		GB_ASSERT_MSG(variant_type != nullptr, "%s :: %s", type_to_string(type), exact_value_to_string(value));
		if (variant_type == t_untyped_nil) {
			return;
		}
		wb_write_const_data(m, variant_type, v, dst);
		if (wb_union_has_tag(bt)) {
			i64 tag_index = union_variant_index_checked(bt, variant_type);
			GB_ASSERT(tag_index >= 0);
			wb_write_le(dst + wb_union_tag_offset(bt), cast(u64)tag_index, union_tag_size(bt));
		}
		return;
	}

	switch (value.kind) {
	case ExactValue_Invalid:
		return; // nil / zero
	case ExactValue_String16:
		wb_write_const_data(m, type, exact_value_string(string16_to_string(permanent_allocator(), value.value_string16)), dst);
		return;
	case ExactValue_Bool:
		dst[0] = value.value_bool ? 1 : 0;
		return;
	case ExactValue_Integer:
	case ExactValue_Float:
		if (is_type_different_to_arch_endianness(type)) {
			// endian-specific scalar: the platform bytes, reversed
			wb_write_const_data(m, integer_endian_type_to_platform_type(type), value, dst);
			for (i64 i = 0; i < size/2; i++) {
				u8 tmp = dst[i];
				dst[i] = dst[size-1-i];
				dst[size-1-i] = tmp;
			}
			return;
		}
		if (value.kind == ExactValue_Float) {
			goto write_float;
		}
		if (is_type_float(core_type(type)) || is_type_complex(type) || is_type_quaternion(type)) {
			ExactValue f = exact_value_to_float(value);
			wb_write_const_data(m, type, f, dst);
			return;
		}
		wb_write_le_int(dst, &value.value_integer, size);
		return;
	case ExactValue_Complex: {
		// {real, imag} (quaternions: {imag, jmag, kmag, real})
		Type *ft = base_complex_elem_type(type);
		i64 fs = type_size_of(ft);
		if (is_type_quaternion(type)) {
			wb_write_const_data(m, ft, exact_value_float(value.value_complex->imag), dst + 0*fs);
			wb_write_const_data(m, ft, exact_value_float(value.value_complex->real), dst + 3*fs);
		} else {
			wb_write_const_data(m, ft, exact_value_float(value.value_complex->real), dst + 0*fs);
			wb_write_const_data(m, ft, exact_value_float(value.value_complex->imag), dst + 1*fs);
		}
		return;
	}
	case ExactValue_Quaternion: {
		Type *ft = base_complex_elem_type(type);
		i64 fs = type_size_of(ft);
		wb_write_const_data(m, ft, exact_value_float(value.value_quaternion->imag), dst + 0*fs);
		wb_write_const_data(m, ft, exact_value_float(value.value_quaternion->jmag), dst + 1*fs);
		wb_write_const_data(m, ft, exact_value_float(value.value_quaternion->kmag), dst + 2*fs);
		wb_write_const_data(m, ft, exact_value_float(value.value_quaternion->real), dst + 3*fs);
		return;
	}
	write_float:
		if (is_type_complex(type) || is_type_quaternion(type)) {
			// real scalar promoted to a complex/quaternion constant
			Type *ft = base_complex_elem_type(type);
			i64 fs = type_size_of(ft);
			wb_write_const_data(m, ft, value, dst + (is_type_quaternion(type) ? 3 : 0)*fs);
			return;
		}
		if (size == 2 && is_type_float(core_type(type))) {
			wb_write_le(dst, wb_f32_to_f16_bits(cast(f32)value.value_float), 2);
		} else if (size == 4) {
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
	case ExactValue_Typeid:
		wb_write_le(dst, type_hash_canonical_type(default_type(value.value_typeid)), size);
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
		if (is_type_cstring16(type)) {
			wb_write_le(dst, wb_intern_string16_bytes(m, s), size);
		} else if (is_type_string16(type)) {
			isize len16 = string_to_string16(temporary_allocator(), s).len;
			u32 data = len16 > 0 ? wb_intern_string16_bytes(m, s) : 0;
			wb_write_le(dst, data, build_context.ptr_size);
			wb_write_le(dst + type_offset_of(bt, 1, nullptr), cast(u64)len16, build_context.int_size);
		} else if (is_type_cstring(type)) {
			wb_write_le(dst, wb_intern_string_bytes(m, s), size);
		} else if (is_type_string(type)) {
			u32 data = s.len > 0 ? wb_intern_string_bytes(m, s) : 0;
			i64 ptr_size = build_context.ptr_size;
			wb_write_le(dst, data, ptr_size);
			wb_write_le(dst + type_offset_of(bt, 1, nullptr), cast(u64)s.len, build_context.int_size);
		} else if (is_type_array(bt) && s.len == size) {
			gb_memmove(dst, s.text, s.len);
		} else if (is_type_slice(bt)) {
			// `#load` data: the bytes reinterpreted as the element type
			u32 data = s.len > 0 ? wb_intern_string_bytes(m, s) : 0;
			wb_write_le(dst, data, build_context.ptr_size);
			wb_write_le(dst + type_offset_of(bt, 1, nullptr), cast(u64)(s.len / type_size_of(bt->Slice.elem)), build_context.int_size);
		}
		return;
	}
	case ExactValue_Compound: {
		Ast *node = value.value_compound;
		GB_ASSERT(node->kind == Ast_CompoundLit);
		ast_node(cl, CompoundLit, node);
		if (cl->elems.count == 0) {
			return; // zero value
		}
		switch (bt->kind) {
		case Type_Struct: {
			if (is_type_soa_struct(bt)) {
				// #soa[N]T{...}: written as the [N]T literal, then transposed into the per-field arrays
				Type *elem_type = bt->Struct.soa_elem;
				i64 count = bt->Struct.soa_count;
				i64 elem_size = type_size_of(elem_type);
				u8 *aos = gb_alloc_array(temporary_allocator(), u8, count*elem_size);
				gb_zero_size(aos, count*elem_size);
				wb_write_const_data(m, alloc_type_array(elem_type, count), value, aos);
				isize n = wb_soa_field_count(bt);
				for (isize f = 0; f < n; f++) {
					Type *ft = nullptr;
					i64 field_offset = wb_soa_elem_field_offset(elem_type, f, &ft);
					i64 field_size   = type_size_of(ft);
					i64 array_offset = type_offset_of(bt, f, nullptr);
					for (i64 k = 0; k < count; k++) {
						gb_memmove(dst + array_offset + k*field_size, aos + k*elem_size + field_offset, field_size);
					}
				}
				return;
			}
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
		case Type_EnumeratedArray:
		case Type_SimdVector:
		case Type_Matrix:
		case Type_FixedCapacityDynamicArray: {
			Type *et = nullptr;
			switch (bt->kind) {
			case Type_Array:           et = bt->Array.elem;           break;
			case Type_SimdVector:      et = bt->SimdVector.elem;      break;
			case Type_EnumeratedArray: et = bt->EnumeratedArray.elem; break;
			case Type_Matrix:          et = bt->Matrix.elem;          break;
			case Type_FixedCapacityDynamicArray: et = bt->FixedCapacityDynamicArray.elem; break;
			default: break;
			}
			i64 elem_size = type_size_of(et);
			if (bt->kind == Type_FixedCapacityDynamicArray) {
				// [dynamic; N]T{...}: {data: [N]T, len: int}
				i64 capacity = bt->FixedCapacityDynamicArray.capacity;
				i64 count = 0;
				if (are_types_identical(node->tav.type, et)) {
					// a literal of the element type is splat across the array
					for (i64 k = 0; k < capacity; k++) {
						wb_write_const_data(m, et, value, dst + k*elem_size);
					}
					count = capacity;
				} else {
					wb_write_const_data(m, alloc_type_array(et, capacity), value, dst);
					count = wb_compound_lit_elem_count(cl->elems);
				}
				wb_write_le(dst + type_offset_of(bt, 1, nullptr), cast(u64)count, build_context.int_size);
				return;
			}
			i64 index = 0;
			i64 min_value = bt->kind == Type_EnumeratedArray ? exact_value_to_i64(*bt->EnumeratedArray.min_value) : 0;
			// matrix literals list their elements in row-major order
			auto elem_offset = [&](i64 k) -> i64 {
				if (bt->kind == Type_Matrix) {
					k = matrix_row_major_index_to_offset(bt, k);
				}
				return k*elem_size;
			};
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
							wb_write_const_data(m, et, tav.value, dst + elem_offset(k));
						}
						index = hi;
					} else {
						index = exact_value_to_i64(fv->field->tav.value) - min_value;
						wb_write_const_data(m, et, tav.value, dst + elem_offset(index));
						index++;
					}
				} else {
					TypeAndValue tav = type_and_value_of_expr(elem);
					wb_write_const_data(m, et, tav.value, dst + elem_offset(index));
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
	if (size > 0) {
		array_add(&m->const_data, (cast(u64)addr << 32) | cast(u64)size);
	}
	return addr;
}

// The bytes of the data segment at `addr` when they are those of a constant
// value (never written at runtime), so that loads of them can be folded
gb_internal u8 const *wb_const_data_bytes(wbModule *m, u32 addr, u32 size) {
	isize lo = 0, hi = m->const_data.count;
	while (lo < hi) {
		isize mid = (lo + hi) / 2;
		u32 a = cast(u32)(m->const_data[mid] >> 32);
		u32 n = cast(u32)m->const_data[mid];
		if (addr < a) {
			hi = mid;
		} else if (addr >= a + n) {
			lo = mid + 1;
		} else {
			if (addr + size > a + n) return nullptr;
			return m->data.data + (addr - m->data_base);
		}
	}
	return nullptr;
}

// Global variables

gb_internal bool wb_can_serialize_const(Type *type, ExactValue const &value) {
	if (value.kind == ExactValue_Invalid) {
		return true;
	}
	Type *bt = base_type(type);
	if (bt->kind == Type_Union) {
		ExactValue v = value;
		if (v.kind == ExactValue_Variant) {
			v = v.value_variant->tav.value;
		}
		if (v.kind == ExactValue_Compound && (v.variant_type == nullptr || are_types_identical(v.variant_type, type))) {
			return true; // nil
		}
		if (v.variant_type == nullptr) {
			return bt->Union.variants.count == 1 && wb_can_serialize_const(bt->Union.variants[0], v);
		}
		return wb_can_serialize_const(v.variant_type, v);
	}
	switch (value.kind) {
	case ExactValue_Bool:
	case ExactValue_Integer:
	case ExactValue_Float:
	case ExactValue_Pointer:
	case ExactValue_Typeid:
		return wb_is_scalar(type) || is_type_complex(bt) || is_type_quaternion(bt) || type_size_of(type) == 16 ||
		       (bt->kind == Type_SimdVector && wb_is_scalar(bt->SimdVector.elem)) ||
		       (bt->kind == Type_Array && wb_is_scalar(bt->Array.elem)) ||
		       (bt->kind == Type_EnumeratedArray && wb_is_scalar(bt->EnumeratedArray.elem)) ||
		       (bt->kind == Type_Matrix && wb_is_scalar(bt->Matrix.elem));
	case ExactValue_Complex:
		return is_type_complex(bt) || is_type_quaternion(bt);
	case ExactValue_Quaternion:
		return is_type_quaternion(bt);
	case ExactValue_String:
	case ExactValue_String16:
		return is_type_string(type) || is_type_cstring(type) || is_type_string16(type) || is_type_cstring16(type) || is_type_array(bt) || is_type_slice(bt);
	case ExactValue_Procedure:
		return is_type_proc(bt);
	case ExactValue_Compound:
		if (value.value_compound->CompoundLit.elems.count == 0) {
			return true; // `{}` is the zero value of any type
		}
		switch (bt->kind) {
		case Type_Struct: case Type_Array: case Type_EnumeratedArray: case Type_Slice: case Type_SimdVector: case Type_Matrix:
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
					} else if (bt->kind == Type_Matrix) {
						et = bt->Matrix.elem;
					} else if (is_type_soa_struct(bt)) {
						et = bt->Struct.soa_elem;
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

gb_internal wbValue wb_const(wbProcedure *p, Ast *node, Type *type, ExactValue const &value_) {
	if (is_type_untyped(type)) {
		type = default_type(type);
	}
	ExactValue value = value_;
	if (value.kind == ExactValue_String16) {
		// UTF-16 constants are interned by their UTF-8 form
		value = exact_value_string(string16_to_string(permanent_allocator(), value.value_string16));
	}
	wbValType vt = wb_valtype_of(type);
	if (vt == wbValType_Invalid) {
		// Aggregate constant: lives in the data segment
		if (value.kind == ExactValue_String && is_type_string16(type)) {
			return wb_value_memory(WB_NO_LOCAL, cast(i32)wb_intern_string16_value(p->module, value.value_string), type);
		}
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
	if (is_type_different_to_arch_endianness(type) && (value.kind == ExactValue_Integer || value.kind == ExactValue_Float)) {
		// endian-specific scalar: the platform constant, byte swapped
		wbValue platform = wb_const(p, node, integer_endian_type_to_platform_type(type), value);
		if (platform.kind == wbValue_Invalid) {
			return platform;
		}
		if (vt == wbValType_f32 || vt == wbValType_f64) {
			return wb_emit_byte_swap(p, platform, type);
		}
		i64 size = type_size_of(type);
		u64 bits = cast(u64)platform.i;
		u64 swapped = 0;
		for (i64 i = 0; i < size; i++) {
			swapped |= ((bits >> (8*i)) & 0xff) << (8*(size-1-i));
		}
		platform.i = cast(i64)swapped;
		platform.type = type;
		if (vt == wbValType_i32) {
			if (size == 2) {
				platform.i = wb_type_is_signed(type) ? cast(i16)platform.i : cast(u16)platform.i;
			} else {
				platform.i = cast(i32)platform.i;
			}
		}
		return platform;
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
		} else if (wb_is_f16(type)) {
			v.i = wb_f32_to_f16_bits(cast(f32)exact_value_to_f64(value));
		} else {
			v.i = cast(i64)wb_big_int_bits(&value.value_integer);
		}
		break;
	case ExactValue_Float:
		if (vt == wbValType_f32 || vt == wbValType_f64) {
			v.f = value.value_float;
		} else if (wb_is_f16(type)) {
			v.i = wb_f32_to_f16_bits(cast(f32)value.value_float);
		} else {
			v.i = cast(i64)value.value_float;
		}
		break;
	case ExactValue_Pointer:
		v.i = value.value_pointer;
		break;
	case ExactValue_Typeid:
		v.i = cast(i64)type_hash_canonical_type(default_type(value.value_typeid));
		break;
	case ExactValue_String:
		if (is_type_cstring(type)) {
			v.i = wb_intern_string_bytes(p->module, value.value_string);
			break;
		}
		if (is_type_cstring16(type)) {
			v.i = wb_intern_string16_bytes(p->module, value.value_string);
			break;
		}
		wb_unsupported(p, node, "string constant of this type");
		return wb_value_invalid();
	case ExactValue_Invalid:
		// nil
		v.i = 0;
		break;
	case ExactValue_Compound:
		if (value.value_compound->CompoundLit.elems.count == 0) {
			v.i = 0; // `{}`
			break;
		}
		wb_unsupported(p, node, "constant value kind");
		return wb_value_invalid();
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

// Context

// The address of the current `context`. Odin calling convention procedures
// start from the implicit parameter, others create a default context on first
// use (as lb_find_or_generate_context_ptr does).
gb_internal wbAddr wb_context_addr(wbProcedure *p) {
	if (p->context_stack.count == 0) {
		wbContextData cd = {};
		if (p->context_local >= 0) {
			cd.addr = wb_addr_memory(cast(u32)p->context_local, 0, t_context);
			cd.scope_index = -1;
			cd.uses = 1;
		} else {
			cd.addr = wb_add_temp(p, t_context);
			cd.scope_index = p->scopes.count;
			wb_addr_zero(p, cd.addr);
			wb_push_address(p, cd.addr.index, cd.addr.offset);
			wb_call(p, wb_lookup_runtime_procedure(p->module, "__init_context"));
		}
		array_add(&p->context_stack, cd);
	}
	wbContextData *cd = &p->context_stack[p->context_stack.count-1];
	cd->uses += 1;
	return cd->addr;
}

// The address to store into for an assignment to `context` (or a field of it).
// The context is copied first unless the current one is unused and belongs to
// the current scope, so that earlier uses (defers, calls) keep their value.
gb_internal wbAddr wb_context_for_write(wbProcedure *p) {
	wbAddr old = wb_context_addr(p);
	wbContextData *cd = &p->context_stack[p->context_stack.count-1];
	cd->uses -= 1;
	if (cd->uses == 0 && cd->scope_index == p->scopes.count) {
		return old;
	}
	wbContextData next = {};
	next.addr = wb_add_temp(p, t_context);
	next.scope_index = p->scopes.count;
	wb_emit_copy(p, next.addr.index, next.addr.offset, old.index, old.offset, t_context);
	array_add(&p->context_stack, next);
	return next.addr;
}

// Pushes the context pointer argument for a call to an Odin calling convention procedure
gb_internal void wb_push_context_ptr(wbProcedure *p, wbAddr ctx) {
	wb_push_address(p, ctx.index, ctx.offset);
}

#include "wasm_backend_expr.cpp"
#include "wasm_backend_rtti.cpp"
#include "wasm_backend_simd.cpp"
#include "wasm_backend_map.cpp"
#include "wasm_backend_stmt.cpp"
#include "wasm_backend_soa.cpp"
#include "wasm_backend_atomic.cpp"
#include "wasm_backend_link.cpp"

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

gb_internal void wb_finish_procedure(wbProcedure *p) {
	wb_optimize_procedure(p);

	// Frame setup: sp = old_sp - frame_size (kept 16 byte aligned). The
	// optimizer has removed the epilogues of procedures without a frame.
	// The prologue is not part of the instruction stream (the procedure may
	// be finished again after inlining).
	wbBuffer code = p->code;
	isize instr_count = p->instrs.count;
	p->code = p->prologue;
	p->code.data.count = 0;
	if (p->frame_size > 0) {
		u32 size = (p->frame_size + 15) & ~15u;
		wb_global_get(p, p->module->global_stack_pointer);
		wb_local_tee(p, p->old_sp_local);
		wb_i32_const(p, cast(i32)size);
		wb_op(p, wbOp_i32_sub);
		wb_local_tee(p, p->fp_local);
		wb_global_set(p, p->module->global_stack_pointer);
	} else if (p->uses_alloca) {
		wb_global_get(p, p->module->global_stack_pointer);
		wb_local_set(p, p->old_sp_local);
	}
	p->prologue = p->code;
	p->code = code;
	p->instrs.count = instr_count;
}

gb_internal void wb_build_procedure(wbProcedure *p) {
	if (p->failed) {
		wb_op(p, wbOp_unreachable);
		return;
	}
	Type *pt = base_type(p->type);

	// Parameters occupy the first wasm locals, laid out by wb_functype_of_proc
	if (wb_uses_sret(pt)) {
		p->sret_local = cast(i32)wb_add_local(p, wbValType_i32, str_lit("sret"));
	}
	struct wbParamLocals {
		u32 first; // index of the first local, WB_NO_LOCAL if the parameter takes none
		Array<wbAbiLeaf> leaves;
		bool direct;
	};
	auto param_locals = array_make<wbParamLocals>(temporary_allocator(), 0, 8);
	if (pt->Proc.params != nullptr) {
		for (Entity *e : pt->Proc.params->Tuple.variables) {
			wbParamLocals pl = {WB_NO_LOCAL};
			if (e->kind == Entity_Variable) {
				pl.leaves = array_make<wbAbiLeaf>(temporary_allocator(), 0, 8);
				pl.direct = wb_abi_flatten(e->type, pt->Proc.calling_convention, 0, &pl.leaves);
				if (!pl.direct) {
					pl.first = wb_add_local(p, wbValType_i32, e->token.string);
				} else if (pl.leaves.count > 0) {
					pl.first = wb_add_local(p, wb_valtype_of(pl.leaves[0].type), e->token.string);
					for (isize i = 1; i < pl.leaves.count; i++) {
						wb_add_local(p, wb_valtype_of(pl.leaves[i].type), e->token.string);
					}
				}
			}
			array_add(&param_locals, pl);
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
			wbParamLocals const &pl = param_locals[i];
			if (e->kind != Entity_Variable || e->token.string.len == 0 || is_blank_ident(e->token.string)) {
				continue;
			}
			if (!pl.direct) {
				// Passed as a pointer to a caller-owned copy
				map_set(&p->variables, e, wb_addr_memory(pl.first, 0, e->type));
			} else if (wb_is_scalar(e->type) && pl.leaves.count == 1 && !ptr_set_exists(&p->addressed, e)) {
				map_set(&p->variables, e, wb_addr_local(pl.first, e->type));
			} else {
				// Reassemble the flattened value in the frame
				wbAddr addr = wb_add_temp(p, e->type);
				for_array(j, pl.leaves) {
					wbAbiLeaf const &leaf = pl.leaves[j];
					u32 idx = pl.first + cast(u32)j;
					wb_emit_store(p, addr.index, addr.offset + cast(i32)leaf.offset, wb_value_local(idx, p->locals[idx].vt, leaf.type), leaf.type);
				}
				map_set(&p->variables, e, addr);
			}
		}
	}

	// Named results are ordinary variables, zero initialized unless they
	// have a default value (`-> (x: int = 1)`)
	if (pt->Proc.has_named_results && pt->Proc.results != nullptr) {
		for (Entity *e : pt->Proc.results->Tuple.variables) {
			wbAddr addr = wb_add_variable(p, e);
			if (e->kind == Entity_Variable && e->Variable.param_value.kind != ParameterValue_Invalid) {
				wb_addr_store(p, addr, wb_build_param_value(p, nullptr, e->type, e->Variable.param_value));
			} else {
				wb_addr_zero(p, addr);
			}
			array_add(&p->result_addrs, addr);
		}
	}

	wb_open_scope(p);
	switch (p->gen) {
	case wbProcGen_Body:
		wb_build_stmt(p, p->body);
		break;
	case wbProcGen_StartupRuntime:
		wb_emit_global_inits(p);
		for (Entity *e : p->module->info->init_procedures) {
			wb_emit_call_no_args(p, e);
		}
		break;
	case wbProcGen_CleanupRuntime:
		for (Entity *e : p->module->info->fini_procedures) {
			wb_emit_call_no_args(p, e);
		}
		break;
	case wbProcGen_Hasher:
		wb_build_hasher_body(p);
		break;
	case wbProcGen_Equal:
		wb_build_equal_body(p);
		break;
	case wbProcGen_MapGet:
		wb_build_map_get_body(p);
		break;
	case wbProcGen_MapSet:
		wb_build_map_set_body(p);
		break;
	case wbProcGen_TestMain:
		wb_build_test_main_body(p);
		break;
	}
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

// Calls a procedure without parameters or results (@(init)/@(fini) procedures)
gb_internal void wb_emit_call_no_args(wbProcedure *p, Entity *e) {
	wbProcedure *callee = wb_procedure_for_entity(p->module, e);
	if (wb_is_odin_cc(e->type)) {
		wb_push_context_ptr(p, wb_context_addr(p));
	}
	wb_call(p, callee);
}

// `odin test` has no user entry point (the runtime's `_start` is compiled out
// with ODIN_TEST), so the backend supplies one, as lb_create_main_procedure does:
// set up the arguments, run the runtime startup, hand the `@(test)` procedures
// to testing.runner and exit with its verdict.
gb_internal wbProcedure *wb_create_test_main(wbModule *m) {
	Type *params  = alloc_type_tuple();
	Type *results = alloc_type_tuple();
	Type *proc_type = alloc_type_proc(nullptr, params, 0, results, 0, false, ProcCC_CDecl);

	wbProcedure *p = wb_alloc_procedure(m, str_lit("_start"));
	p->type      = proc_type;
	p->gen       = wbProcGen_TestMain;
	p->is_export = true;
	wbFuncType ft = {};
	wb_functype_of_proc(m, proc_type, &ft);
	p->type_index = wb_add_functype(m, ft);
	array_add(&m->procedures, p);
	array_add(&m->work_queue, p);
	return p;
}

gb_internal void wb_build_test_main_body(wbProcedure *p) {
	wbModule *m = p->module;
	CheckerInfo *info = m->info;

	if (build_context.metrics.os == TargetOs_wasi) {
		wb_emit_call_no_args(p, find_entity_in_pkg(info, str_lit("runtime"), str_lit("_wasi_setup_args")));
	}
	wb_emit_call_no_args(p, find_entity_in_pkg(info, str_lit("runtime"), str_lit("_startup_runtime")));

	// []testing.Internal_Test{{pkg, name, p}, ...} in static storage
	Type *t_test  = find_type_in_pkg(info, str_lit("testing"), str_lit("Internal_Test"));
	Type *t_array = alloc_type_array(t_test, info->testing_procedures.count);
	Type *t_slice = alloc_type_slice(t_test);
	wbAddr tests = wb_addr_memory(WB_NO_LOCAL, cast(i32)wb_data_alloc(m, type_size_of(t_array), type_align_of(t_array)), t_array);
	i64 test_size = type_size_of(t_test);
	for_array(i, info->testing_procedures) {
		Entity *e = info->testing_procedures[i];
		String pkg_name = e->pkg != nullptr ? e->pkg->name : String{};
		wbAddr dst = wb_addr_offset(tests, i*test_size, t_test);
		for (isize field = 0; field < 3; field++) {
			Type *ft = nullptr;
			i64 offset = type_offset_of(t_test, field, &ft);
			wbAddr fa = wb_addr_offset(dst, offset, ft);
			switch (field) {
			case 0: wb_addr_store(p, fa, wb_const(p, nullptr, t_string, exact_value_string(pkg_name)));        break;
			case 1: wb_addr_store(p, fa, wb_const(p, nullptr, t_string, exact_value_string(e->token.string))); break;
			case 2: wb_addr_store(p, fa, wb_value_const_int(ft, wb_table_index(m, wb_procedure_for_entity(m, e)))); break;
			}
		}
	}
	wbAddr slice = wb_add_temp(p, t_slice);
	wb_addr_store(p, wb_addr_offset(slice, 0, t_rawptr), wb_addr_get_ptr(p, tests, t_rawptr));
	wb_addr_store(p, wb_addr_offset(slice, build_context.int_size, t_int), wb_value_const_int(t_int, info->testing_procedures.count));

	Entity *runner = find_entity_in_pkg(info, str_lit("testing"), str_lit("runner"));
	auto args = array_make<wbValue>(temporary_allocator(), 1);
	args[0] = wb_value_memory(slice.index, slice.offset, t_slice);
	wbValue ok = wb_emit_call(p, base_type(runner->type), wb_procedure_for_entity(m, runner), wb_value_invalid(), args);

	Entity *exit = find_entity_in_pkg(info, str_lit("runtime"), str_lit("exit"));
	auto exit_args = array_make<wbValue>(temporary_allocator(), 1);
	exit_args[0] = wb_emit_conv(p, wb_emit_arith(p, nullptr, Token_Xor, wb_emit_conv(p, ok, t_int), wb_value_const_int(t_int, 1), t_int, t_int), t_int);
	wb_emit_call(p, base_type(exit->type), wb_procedure_for_entity(m, exit), wb_value_invalid(), exit_args);
}

// Stores the pending non-constant global initializers, in declaration
// dependency order (see wb_generate_code)
gb_internal void wb_emit_global_inits(wbProcedure *p) {
	wbModule *m = p->module;
	for (; m->global_inits_emitted < m->global_init_queue.count; m->global_inits_emitted++) {
		wbGlobalInit gi = m->global_init_queue[m->global_inits_emitted];
		u32 addr = wb_global_addr(m, gi.entity);
		if (is_type_tuple(type_of_expr(gi.init_expr))) {
			wb_unsupported(p, gi.init_expr, "global variable initialized from multiple results");
			continue;
		}
		wbValue v = wb_build_expr(p, gi.init_expr);
		if (v.kind == wbValue_Invalid) {
			continue;
		}
		v = wb_emit_conv(p, v, gi.entity->type);
		wb_addr_store(p, wb_addr_memory(WB_NO_LOCAL, cast(i32)addr, gi.entity->type), v);
	}
}

// Without an entry point nothing calls `__$startup_runtime`, so the global
// initializers run from the wasm start function instead
gb_internal wbProcedure *wb_startup_function(wbModule *m) {
	if (m->startup != nullptr) {
		return m->startup;
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
	return p;
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
	array_init(&m->objects,    m->allocator);
	array_init(&m->aliased,    m->allocator);
	map_init(&m->procedure_map);
	map_init(&m->globals);
	string_map_init(&m->libm_imports, 16);
	string_map_init(&m->string_bytes, 64);
	string_map_init(&m->string_values, 64);
	string_map_init(&m->string16_values, 16);
	string_map_init(&m->string16_bytes, 16);
	string_map_init(&m->gen_procs, 16);
	string_map_init(&m->map_cell_infos, 16);
	string_map_init(&m->map_infos, 16);
	array_init(&m->data, heap_allocator(), 0, 4096);
	array_init(&m->const_data, heap_allocator(), 0, 256);
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
	wb_setup_type_info_data(m);

	// Global variables are allocated up front in initialization order so that
	// their initializers run in the order the checker determined
	for (DeclInfo *d : info->variable_init_order) {
		Entity *e = d->entity;
		if (e == nullptr || e->kind != Entity_Variable) {
			continue;
		}
		if (e->min_dep_count.load(std::memory_order_relaxed) == 0) {
			continue;
		}
		if (e->Variable.is_foreign || e->Variable.thread_local_model.len > 0) {
			wbProcedure dummy = {};
			dummy.module = m;
			dummy.entity = e;
			wb_unsupported(&dummy, nullptr, "foreign or thread local variable");
			continue;
		}
		wb_global_addr(m, e);
	}

	// Roots: exported and @(require) procedures. Everything else is generated
	// on demand when referenced, so unused code never has to be lowered.
	for (Entity *e : info->entities) {
		if (e->kind != Entity_Procedure) {
			continue;
		}
		if ((e->scope->flags & ScopeFlag_File) == 0) {
			continue;
		}
		if (e->Procedure.is_foreign) {
			continue;
		}
		if (build_context.wasm_lower_all && wasm_lower_all_root(info->init_package, e)) {
			wb_procedure_for_entity(m, e);
			continue;
		}
		if (!e->Procedure.is_export) {
			if ((e->flags & EntityFlag_Require) == 0) {
				continue;
			}
			// The runtime's required procedures are the compiler-rt style helpers
			// (__ashlti3, __truncsfhf2, memcpy, ...) that LLVM emits calls to;
			// this backend never does, so they are only generated when used.
			if (e->pkg == info->runtime_package) {
				continue;
			}
		}
		if (e->min_dep_count.load(std::memory_order_relaxed) == 0) {
			continue;
		}
		wb_procedure_for_entity(m, e);
	}

	if (build_context.command_kind == Command_test) {
		wb_create_test_main(m);
	}

	TIME_SECTION("wasm backend: link");
	wb_link_load_objects(m);
	if (m->error_count > 0) {
		return false;
	}

	TIME_SECTION("wasm backend: procedures");
	bool startup_runtime_built = false;
	bool cleanup_runtime_built = false;
	for (;;) {
		while (m->work_queue.count > 0) {
			wbProcedure *p = array_pop(&m->work_queue);
			wb_build_procedure(p);
		}
		// The startup procedures are built after everything else so that all
		// global initializers are known (lowering may still discover more
		// procedures, hence the loop)
		if (m->startup_runtime != nullptr && !startup_runtime_built) {
			startup_runtime_built = true;
			wb_build_procedure(m->startup_runtime);
			continue;
		}
		if (m->cleanup_runtime != nullptr && !cleanup_runtime_built) {
			cleanup_runtime_built = true;
			wb_build_procedure(m->cleanup_runtime);
			continue;
		}
		if (m->startup_runtime == nullptr && m->global_inits_emitted < m->global_init_queue.count) {
			wb_emit_global_inits(wb_startup_function(m));
			continue;
		}
		break;
	}
	if (m->startup != nullptr) {
		wbProcedure *p = m->startup;
		wb_close_scope(p);
		wb_emit_epilogue(p);
		wb_finish_procedure(p);
	}
	if (m->global_inits_emitted < m->global_init_queue.count) {
		Entity *e = m->global_init_queue[m->global_inits_emitted].entity;
		error(e->token, "wasm backend: global variable initializer discovered after the startup code was generated");
		m->error_count++;
	}

	if (m->error_count > 0) {
		return false;
	}

	TIME_SECTION("wasm backend: inline");
	wb_inline_procedures(m);
	wb_gc_procedures(m);

	wb_link_resolve_imports(m);
	if (m->error_count > 0) {
		return false;
	}
	wb_link_gc(m);

	TIME_SECTION("wasm backend: write");
	u32 index = 0;
	for (wbProcedure *p : m->imports) {
		p->func_index = index++;
	}
	for (wbProcedure *p : m->procedures) {
		p->func_index = index++;
	}
	wb_link_apply_relocs(m);
	if (m->error_count > 0) {
		return false;
	}
	for (wbProcedure *p : m->procedures) {
		wb_patch_call_relocs(p);
	}

	// Memory: stack, data and one spare page for the heap
	u64 data_end = cast(u64)m->data_base + cast(u64)m->data.count;
	m->memory_initial_pages = cast(u32)((data_end + 0xffff) / 0x10000) + 1;

	return wb_write_output(m);
}
