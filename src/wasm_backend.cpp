// Direct WebAssembly backend: lowering of procedures, statements and expressions.
// See wasm_backend.hpp for an overview.

#include "wasm_backend.hpp"
#include "wasm_backend_emit.cpp"

gb_internal wbValue wb_build_expr(wbProcedure *p, Ast *expr);
gb_internal void    wb_build_stmt(wbProcedure *p, Ast *node);
gb_internal wbValue wb_emit_conv(wbProcedure *p, wbValue v, Type *dst);
gb_internal wbProcedure *wb_procedure_for_entity(wbModule *m, Entity *e);

// Diagnostics

gb_internal void wb_unsupported(wbProcedure *p, Ast *node, char const *what) {
	p->failed = true;
	p->module->error_count++;
	error(ast_token(node), "wasm backend: unsupported %s", what);
}

gb_internal void wb_unsupported_type(wbProcedure *p, Ast *node, Type *t) {
	p->failed = true;
	p->module->error_count++;
	gbString s = type_to_string(t);
	error(ast_token(node), "wasm backend: unsupported type '%s'", s);
	gb_string_free(s);
}

// Types

// Returns the wasm value type used to represent a scalar Odin type, or
// wbValType_Invalid if the type is not (yet) representable as a scalar.
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
		if (t->Basic.kind == Basic_rawptr || t->Basic.kind == Basic_uintptr) {
			return wbValType_i32;
		}
		return wbValType_Invalid;
	case Type_Pointer:
	case Type_MultiPointer:
	case Type_Proc:
		return wbValType_i32;
	}
	return wbValType_Invalid;
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

// Computes the wasm signature of an Odin procedure type.
// ABI: scalar parameters are passed directly in declaration order, followed
// by the context pointer for "odin" calling convention procedures. A single
// scalar result is returned directly.
gb_internal bool wb_functype_of_proc(wbModule *m, Type *pt, wbFuncType *ft, Ast *node) {
	pt = base_type(pt);
	GB_ASSERT(pt->kind == Type_Proc);
	array_init(&ft->params,  m->allocator);
	array_init(&ft->results, m->allocator);

	if (pt->Proc.params != nullptr) {
		for (Entity *e : pt->Proc.params->Tuple.variables) {
			wbValType vt = wb_valtype_of(e->type);
			if (vt == wbValType_Invalid) {
				return false;
			}
			array_add(&ft->params, vt);
		}
	}
	if (wb_is_odin_cc(pt)) {
		array_add(&ft->params, wbValType_i32); // context pointer
	}
	if (pt->Proc.result_count > 1) {
		return false;
	}
	if (pt->Proc.result_count == 1) {
		wbValType vt = wb_valtype_of(wb_result_type(pt));
		if (vt == wbValType_Invalid) {
			return false;
		}
		array_add(&ft->results, vt);
	}
	return true;
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

gb_internal wbProcedure *wb_procedure_for_entity(wbModule *m, Entity *e) {
	GB_ASSERT(e->kind == Entity_Procedure);
	wbProcedure **found = map_get(&m->procedure_map, e);
	if (found) {
		return *found;
	}

	wbProcedure *p = gb_alloc_item(m->allocator, wbProcedure);
	p->module = m;
	p->entity = e;
	p->type   = e->type;
	p->name   = wb_procedure_name(e);
	p->is_foreign = e->Procedure.is_foreign;
	p->is_export  = e->Procedure.is_export;
	p->context_local = -1;
	array_init(&p->locals,        m->allocator);
	array_init(&p->results,       m->allocator);
	array_init(&p->result_locals, m->allocator);
	array_init(&p->labels,        m->allocator);
	array_init(&p->call_relocs,   m->allocator);
	map_init(&p->entity_locals);
	wb_buffer_init(&p->code, m->allocator);
	map_set(&m->procedure_map, e, p);

	Ast *node = e->identifier.load();
	if (node == nullptr && e->decl_info != nullptr) {
		node = e->decl_info->proc_lit;
	}

	wbFuncType ft = {};
	if (!wb_functype_of_proc(m, e->type, &ft, node)) {
		p->failed = true;
		m->error_count++;
		gbString s = type_to_string(e->type);
		error(e->token, "wasm backend: unsupported procedure signature '%s'", s);
		gb_string_free(s);
	}
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

gb_internal u32 wb_add_local(wbProcedure *p, wbValType vt, String name = {}) {
	wbLocal l = {vt, name};
	array_add(&p->locals, l);
	return cast(u32)(p->locals.count-1);
}

gb_internal u32 wb_add_entity_local(wbProcedure *p, Entity *e) {
	wbValType vt = wb_valtype_of(e->type);
	GB_ASSERT(vt != wbValType_Invalid);
	u32 idx = wb_add_local(p, vt, e->token.string);
	map_set(&p->entity_locals, e, idx);
	return idx;
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

gb_internal wbValue wb_value_invalid(void) {
	wbValue v = {};
	return v;
}

// Pushes a value onto the wasm operand stack
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

// Normalizes an integer held in an i32/i64 local so that the unused high bits
// hold the sign/zero extension of the Odin type's width.
gb_internal void wb_emit_normalize(wbProcedure *p, Type *type) {
	if (is_type_untyped(type)) {
		type = default_type(type);
	}
	Type *t = core_type(type);
	if (!is_type_integer(t) && !is_type_boolean(t) && !is_type_rune(t)) {
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

gb_internal wbValue wb_const(wbProcedure *p, Ast *node, Type *type, ExactValue const &value) {
	if (is_type_untyped(type)) {
		type = default_type(type);
	}
	wbValType vt = wb_valtype_of(type);
	if (vt == wbValType_Invalid) {
		wb_unsupported_type(p, node, type);
		return wb_value_invalid();
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
	case ExactValue_Invalid:
		// nil
		v.i = 0;
		break;
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

// Conversions

gb_internal wbValue wb_emit_conv(wbProcedure *p, wbValue v, Type *dst) {
	if (v.kind == wbValue_Invalid) {
		return v;
	}
	if (is_type_untyped(dst)) {
		dst = default_type(dst);
	}
	Type *src = v.type;
	if (are_types_identical(src, dst)) {
		return v;
	}

	wbValType dvt = wb_valtype_of(dst);
	if (dvt == wbValType_Invalid) {
		wb_unsupported_type(p, p->body, dst);
		return wb_value_invalid();
	}

	Type *cs = core_type(src);
	Type *cd = core_type(dst);
	bool src_int   = is_type_integer(cs) || is_type_boolean(cs) || is_type_rune(cs) || is_type_pointer(cs) || is_type_multi_pointer(cs) || is_type_rawptr(cs) || is_type_uintptr(cs) || is_type_proc(cs);
	bool dst_int   = is_type_integer(cd) || is_type_boolean(cd) || is_type_rune(cd) || is_type_pointer(cd) || is_type_multi_pointer(cd) || is_type_rawptr(cd) || is_type_uintptr(cd) || is_type_proc(cd);
	bool src_float = is_type_float(cs);
	bool dst_float = is_type_float(cd);
	bool src_signed = wb_type_is_signed(cs);

	// Constant folding for the simple cases
	if (v.kind == wbValue_Const && dvt == v.vt && ((src_int && dst_int) || (src_float && dst_float))) {
		v.type = dst;
		if (dvt == wbValType_i32) {
			i64 size = type_size_of(cd);
			if (size == 1) {
				v.i = wb_type_is_signed(cd) ? cast(i8)v.i : cast(u8)v.i;
			} else if (size == 2) {
				v.i = wb_type_is_signed(cd) ? cast(i16)v.i : cast(u16)v.i;
			}
		}
		return v;
	}

	if (src_int && dst_int && v.vt == dvt && type_size_of(cd) >= type_size_of(cs)) {
		// Same representation and no narrowing: the bits are already correct
		v.type = dst;
		return v;
	}

	wb_push(p, v);

	if (src_int && dst_int) {
		if (v.vt == wbValType_i32 && dvt == wbValType_i64) {
			wb_op(p, src_signed ? wbOp_i64_extend_i32_s : wbOp_i64_extend_i32_u);
		} else if (v.vt == wbValType_i64 && dvt == wbValType_i32) {
			wb_op(p, wbOp_i32_wrap_i64);
			wb_emit_normalize(p, dst);
		} else if (v.vt == dvt) {
			// Same representation: only narrow if the destination is smaller
			if (type_size_of(cd) < type_size_of(cs)) {
				wb_emit_normalize(p, dst);
			}
		} else {
			goto unsupported;
		}
	} else if (src_int && dst_float) {
		if (v.vt == wbValType_i32 && dvt == wbValType_f32) {
			wb_op(p, src_signed ? wbOp_f32_convert_i32_s : wbOp_f32_convert_i32_u);
		} else if (v.vt == wbValType_i32 && dvt == wbValType_f64) {
			wb_op(p, src_signed ? wbOp_f64_convert_i32_s : wbOp_f64_convert_i32_u);
		} else if (v.vt == wbValType_i64 && dvt == wbValType_f32) {
			wb_op(p, src_signed ? wbOp_f32_convert_i64_s : wbOp_f32_convert_i64_u);
		} else if (v.vt == wbValType_i64 && dvt == wbValType_f64) {
			wb_op(p, src_signed ? wbOp_f64_convert_i64_s : wbOp_f64_convert_i64_u);
		} else {
			goto unsupported;
		}
	} else if (src_float && dst_int) {
		bool dst_signed = wb_type_is_signed(cd);
		if (v.vt == wbValType_f32 && dvt == wbValType_i32) {
			wb_op(p, dst_signed ? wbOp_i32_trunc_f32_s : wbOp_i32_trunc_f32_u);
		} else if (v.vt == wbValType_f64 && dvt == wbValType_i32) {
			wb_op(p, dst_signed ? wbOp_i32_trunc_f64_s : wbOp_i32_trunc_f64_u);
		} else if (v.vt == wbValType_f32 && dvt == wbValType_i64) {
			wb_op(p, dst_signed ? wbOp_i64_trunc_f32_s : wbOp_i64_trunc_f32_u);
		} else if (v.vt == wbValType_f64 && dvt == wbValType_i64) {
			wb_op(p, dst_signed ? wbOp_i64_trunc_f64_s : wbOp_i64_trunc_f64_u);
		} else {
			goto unsupported;
		}
		wb_emit_normalize(p, dst);
	} else if (src_float && dst_float) {
		if (v.vt == wbValType_f32 && dvt == wbValType_f64) {
			wb_op(p, wbOp_f64_promote_f32);
		} else if (v.vt == wbValType_f64 && dvt == wbValType_f32) {
			wb_op(p, wbOp_f32_demote_f64);
		}
	} else {
		goto unsupported;
	}

	return wb_pop_to_local(p, dvt, dst);

unsupported:
	{
		p->failed = true;
		p->module->error_count++;
		gbString s0 = type_to_string(src);
		gbString s1 = type_to_string(dst);
		error(ast_token(p->body), "wasm backend: unsupported conversion from '%s' to '%s'", s0, s1);
		gb_string_free(s0);
		gb_string_free(s1);
		wb_op(p, wbOp_drop);
		return wb_value_invalid();
	}
}

// Expressions

gb_internal wbValue wb_build_unary_expr(wbProcedure *p, Ast *expr) {
	ast_node(ue, UnaryExpr, expr);
	Type *type = expr->tav.type;
	if (is_type_untyped(type)) {
		type = default_type(type);
	}
	wbValType vt = wb_valtype_of(type);
	if (vt == wbValType_Invalid) {
		wb_unsupported_type(p, expr, type);
		return wb_value_invalid();
	}
	wbValue x = wb_build_expr(p, ue->expr);
	x = wb_emit_conv(p, x, type);
	if (x.kind == wbValue_Invalid) {
		return x;
	}

	switch (ue->op.kind) {
	case Token_Add:
		return x;
	case Token_Sub:
		switch (vt) {
		case wbValType_i32: wb_i32_const(p, 0); wb_push(p, x); wb_op(p, wbOp_i32_sub); wb_emit_normalize(p, type); break;
		case wbValType_i64: wb_i64_const(p, 0); wb_push(p, x); wb_op(p, wbOp_i64_sub); break;
		case wbValType_f32: wb_push(p, x); wb_op(p, wbOp_f32_neg); break;
		case wbValType_f64: wb_push(p, x); wb_op(p, wbOp_f64_neg); break;
		default: break;
		}
		return wb_pop_to_local(p, vt, type);
	case Token_Not:
		wb_push(p, x);
		wb_op(p, vt == wbValType_i64 ? wbOp_i64_eqz : wbOp_i32_eqz);
		if (vt == wbValType_i64) {
			wb_op(p, wbOp_i64_extend_i32_u);
		}
		return wb_pop_to_local(p, vt, type);
	case Token_Xor:
		wb_push(p, x);
		if (vt == wbValType_i64) {
			wb_i64_const(p, -1); wb_op(p, wbOp_i64_xor);
		} else {
			wb_i32_const(p, -1); wb_op(p, wbOp_i32_xor);
			wb_emit_normalize(p, type);
		}
		return wb_pop_to_local(p, vt, type);
	}

	wb_unsupported(p, expr, "unary operator");
	return wb_value_invalid();
}

// Emits the wasm opcode for a binary arithmetic/comparison operator whose
// operands are already on the stack. Returns false if unsupported.
gb_internal bool wb_emit_binary_op(wbProcedure *p, TokenKind op, wbValType vt, bool is_signed) {
	bool is_float = vt == wbValType_f32 || vt == wbValType_f64;
	bool is_64    = vt == wbValType_i64 || vt == wbValType_f64;

	#define WB_INT_OP(op32, op64)   do { wb_op(p, is_64 ? op64 : op32); return true; } while (0)
	#define WB_FLOAT_OP(op32, op64) do { wb_op(p, is_64 ? op64 : op32); return true; } while (0)

	if (is_float) {
		switch (op) {
		case Token_Add:   WB_FLOAT_OP(wbOp_f32_add, wbOp_f64_add);
		case Token_Sub:   WB_FLOAT_OP(wbOp_f32_sub, wbOp_f64_sub);
		case Token_Mul:   WB_FLOAT_OP(wbOp_f32_mul, wbOp_f64_mul);
		case Token_Quo:   WB_FLOAT_OP(wbOp_f32_div, wbOp_f64_div);
		case Token_CmpEq: WB_FLOAT_OP(wbOp_f32_eq,  wbOp_f64_eq);
		case Token_NotEq: WB_FLOAT_OP(wbOp_f32_ne,  wbOp_f64_ne);
		case Token_Lt:    WB_FLOAT_OP(wbOp_f32_lt,  wbOp_f64_lt);
		case Token_Gt:    WB_FLOAT_OP(wbOp_f32_gt,  wbOp_f64_gt);
		case Token_LtEq:  WB_FLOAT_OP(wbOp_f32_le,  wbOp_f64_le);
		case Token_GtEq:  WB_FLOAT_OP(wbOp_f32_ge,  wbOp_f64_ge);
		default: break;
		}
		return false;
	}

	switch (op) {
	case Token_Add:    WB_INT_OP(wbOp_i32_add, wbOp_i64_add);
	case Token_Sub:    WB_INT_OP(wbOp_i32_sub, wbOp_i64_sub);
	case Token_Mul:    WB_INT_OP(wbOp_i32_mul, wbOp_i64_mul);
	case Token_Quo:    if (is_signed) WB_INT_OP(wbOp_i32_div_s, wbOp_i64_div_s); else WB_INT_OP(wbOp_i32_div_u, wbOp_i64_div_u);
	case Token_Mod:    if (is_signed) WB_INT_OP(wbOp_i32_rem_s, wbOp_i64_rem_s); else WB_INT_OP(wbOp_i32_rem_u, wbOp_i64_rem_u);
	case Token_And:    WB_INT_OP(wbOp_i32_and, wbOp_i64_and);
	case Token_Or:     WB_INT_OP(wbOp_i32_or,  wbOp_i64_or);
	case Token_Xor:    WB_INT_OP(wbOp_i32_xor, wbOp_i64_xor);
	case Token_CmpEq:  WB_INT_OP(wbOp_i32_eq,  wbOp_i64_eq);
	case Token_NotEq:  WB_INT_OP(wbOp_i32_ne,  wbOp_i64_ne);
	case Token_Lt:     if (is_signed) WB_INT_OP(wbOp_i32_lt_s, wbOp_i64_lt_s); else WB_INT_OP(wbOp_i32_lt_u, wbOp_i64_lt_u);
	case Token_Gt:     if (is_signed) WB_INT_OP(wbOp_i32_gt_s, wbOp_i64_gt_s); else WB_INT_OP(wbOp_i32_gt_u, wbOp_i64_gt_u);
	case Token_LtEq:   if (is_signed) WB_INT_OP(wbOp_i32_le_s, wbOp_i64_le_s); else WB_INT_OP(wbOp_i32_le_u, wbOp_i64_le_u);
	case Token_GtEq:   if (is_signed) WB_INT_OP(wbOp_i32_ge_s, wbOp_i64_ge_s); else WB_INT_OP(wbOp_i32_ge_u, wbOp_i64_ge_u);
	default: break;
	}
	#undef WB_INT_OP
	#undef WB_FLOAT_OP
	return false;
}

gb_internal bool wb_is_comparison(TokenKind op) {
	switch (op) {
	case Token_CmpEq: case Token_NotEq:
	case Token_Lt: case Token_Gt: case Token_LtEq: case Token_GtEq:
		return true;
	}
	return false;
}

// Lowers `left op right` where both operands have been converted to `operand_type`
gb_internal wbValue wb_emit_arith(wbProcedure *p, Ast *node, TokenKind op, wbValue left, wbValue right, Type *operand_type, Type *result_type) {
	if (left.kind == wbValue_Invalid || right.kind == wbValue_Invalid) {
		return wb_value_invalid();
	}
	wbValType vt = wb_valtype_of(operand_type);
	wbValType rvt = wb_valtype_of(result_type);
	if (vt == wbValType_Invalid || rvt == wbValType_Invalid) {
		wb_unsupported_type(p, node, operand_type);
		return wb_value_invalid();
	}
	bool is_signed = wb_type_is_signed(operand_type);

	switch (op) {
	case Token_AndNot:
		// a &~ b == a & ~b
		wb_push(p, left);
		wb_push(p, right);
		if (vt == wbValType_i64) {
			wb_i64_const(p, -1); wb_op(p, wbOp_i64_xor); wb_op(p, wbOp_i64_and);
		} else {
			wb_i32_const(p, -1); wb_op(p, wbOp_i32_xor); wb_op(p, wbOp_i32_and);
		}
		return wb_pop_to_local(p, rvt, result_type);

	case Token_Shl:
	case Token_Shr: {
		// Odin: shifting by >= bit width gives 0 (or -1 for negative signed >>)
		i64 bits = 8*type_size_of(core_type(operand_type));
		wbValue amount = wb_emit_conv(p, right, operand_type);
		if (amount.kind == wbValue_Invalid) {
			return wb_value_invalid();
		}
		u32 res = wb_add_local(p, vt);
		wb_push(p, left);
		wb_push(p, amount);
		if (vt == wbValType_i64) {
			if (op == Token_Shl) wb_op(p, wbOp_i64_shl); else wb_op(p, is_signed ? wbOp_i64_shr_s : wbOp_i64_shr_u);
		} else {
			if (op == Token_Shl) wb_op(p, wbOp_i32_shl); else wb_op(p, is_signed ? wbOp_i32_shr_s : wbOp_i32_shr_u);
		}
		// select(shifted, saturated, amount < bits)
		if (op == Token_Shr && is_signed) {
			// arithmetic shift by (bits-1) gives 0 or -1
			wb_push(p, left);
			if (vt == wbValType_i64) { wb_i64_const(p, bits-1); wb_op(p, wbOp_i64_shr_s); }
			else                     { wb_i32_const(p, cast(i32)bits-1); wb_op(p, wbOp_i32_shr_s); }
		} else {
			if (vt == wbValType_i64) wb_i64_const(p, 0); else wb_i32_const(p, 0);
		}
		wb_push(p, amount);
		if (vt == wbValType_i64) { wb_i64_const(p, bits); wb_op(p, wbOp_i64_lt_u); }
		else                     { wb_i32_const(p, cast(i32)bits); wb_op(p, wbOp_i32_lt_u); }
		wb_op(p, wbOp_select);
		wb_emit_normalize(p, result_type);
		wb_local_set(p, res);
		return wb_value_local(res, vt, result_type);
	}

	case Token_ModMod: {
		// Floored modulo: ((a % b) + b) % b
		if (vt == wbValType_f32 || vt == wbValType_f64) {
			wb_unsupported(p, node, "floating point '%%'");
			return wb_value_invalid();
		}
		if (!is_signed) {
			return wb_emit_arith(p, node, Token_Mod, left, right, operand_type, result_type);
		}
		wbOp rem = vt == wbValType_i64 ? wbOp_i64_rem_s : wbOp_i32_rem_s;
		wbOp add = vt == wbValType_i64 ? wbOp_i64_add   : wbOp_i32_add;
		wb_push(p, left); wb_push(p, right); wb_op(p, rem);
		wb_push(p, right); wb_op(p, add);
		wb_push(p, right); wb_op(p, rem);
		wb_emit_normalize(p, result_type);
		return wb_pop_to_local(p, rvt, result_type);
	}
	default:
		break;
	}

	wb_push(p, left);
	wb_push(p, right);
	if (!wb_emit_binary_op(p, op, vt, is_signed)) {
		wb_unsupported(p, node, "binary operator");
		return wb_value_invalid();
	}
	if (wb_is_comparison(op)) {
		// comparisons always produce an i32
		if (rvt == wbValType_i64) {
			wb_op(p, wbOp_i64_extend_i32_u);
		}
		return wb_pop_to_local(p, rvt, result_type);
	}
	wb_emit_normalize(p, result_type);
	return wb_pop_to_local(p, rvt, result_type);
}

gb_internal wbValue wb_build_binary_expr(wbProcedure *p, Ast *expr) {
	ast_node(be, BinaryExpr, expr);
	Type *type = expr->tav.type;
	if (is_type_untyped(type)) {
		type = default_type(type);
	}

	switch (be->op.kind) {
	case Token_CmpAnd:
	case Token_CmpOr: {
		// Short circuit: res = left; if (res == cond) { res = right }
		wbValType vt = wb_valtype_of(type);
		if (vt == wbValType_Invalid) {
			wb_unsupported_type(p, expr, type);
			return wb_value_invalid();
		}
		u32 res = wb_add_local(p, vt);
		wbValue left = wb_emit_conv(p, wb_build_expr(p, be->left), type);
		wb_push(p, left);
		wb_local_set(p, res);
		wb_local_get(p, res);
		if (vt == wbValType_i64) {
			wb_op(p, wbOp_i64_eqz);
			if (be->op.kind == Token_CmpAnd) wb_op(p, wbOp_i32_eqz);
		} else if (be->op.kind == Token_CmpOr) {
			wb_op(p, wbOp_i32_eqz);
		}
		wb_open_if(p);
		wbValue right = wb_emit_conv(p, wb_build_expr(p, be->right), type);
		wb_push(p, right);
		wb_local_set(p, res);
		wb_close(p);
		return wb_value_local(res, vt, type);
	}
	default:
		break;
	}

	// Comparisons operate on the (common) operand type, not the bool result type
	Type *operand_type = type;
	if (wb_is_comparison(be->op.kind)) {
		operand_type = be->left->tav.type;
		if (is_type_untyped(operand_type)) {
			operand_type = be->right->tav.type;
		}
		if (is_type_untyped(operand_type)) {
			operand_type = default_type(operand_type);
		}
	}

	wbValue left  = wb_emit_conv(p, wb_build_expr(p, be->left),  operand_type);
	wbValue right;
	if (be->op.kind == Token_Shl || be->op.kind == Token_Shr) {
		right = wb_build_expr(p, be->right);
	} else {
		right = wb_emit_conv(p, wb_build_expr(p, be->right), operand_type);
	}
	return wb_emit_arith(p, expr, be->op.kind, left, right, operand_type, type);
}

gb_internal wbValue wb_build_call_expr(wbProcedure *p, Ast *expr) {
	ast_node(ce, CallExpr, expr);
	TypeAndValue proc_tv = type_and_value_of_expr(ce->proc);

	if (proc_tv.mode == Addressing_Type) {
		// Type conversion: T(x)
		GB_ASSERT(ce->args.count == 1);
		wbValue x = wb_build_expr(p, ce->args[0]);
		return wb_emit_conv(p, x, expr->tav.type);
	}
	if (proc_tv.mode == Addressing_Builtin) {
		wb_unsupported(p, expr, "builtin procedure call");
		return wb_value_invalid();
	}

	Entity *e = entity_of_node(ce->proc);
	if (e == nullptr || e->kind != Entity_Procedure) {
		wb_unsupported(p, expr, "indirect procedure call");
		return wb_value_invalid();
	}
	Type *pt = base_type(e->type);
	GB_ASSERT(pt->kind == Type_Proc);
	if (pt->Proc.variadic || pt->Proc.c_vararg) {
		wb_unsupported(p, expr, "variadic procedure call");
		return wb_value_invalid();
	}
	GB_ASSERT(ce->split_args != nullptr);
	if (ce->split_args->named.count > 0) {
		wb_unsupported(p, expr, "procedure call with named arguments");
		return wb_value_invalid();
	}
	Slice<Ast *> const &call_args = ce->split_args->positional;
	if (pt->Proc.result_count > 1) {
		wb_unsupported(p, expr, "procedure call with multiple results");
		return wb_value_invalid();
	}

	wbProcedure *callee = wb_procedure_for_entity(p->module, e);

	// Evaluate all arguments first (into locals), then push them in order
	auto args = array_make<wbValue>(temporary_allocator(), 0, call_args.count);
	isize param_count = pt->Proc.params ? pt->Proc.params->Tuple.variables.count : 0;
	if (call_args.count != param_count) {
		wb_unsupported(p, expr, "procedure call with default arguments");
		return wb_value_invalid();
	}
	for_array(i, call_args) {
		Type *param_type = pt->Proc.params->Tuple.variables[i]->type;
		wbValue v = wb_emit_conv(p, wb_build_expr(p, call_args[i]), param_type);
		array_add(&args, v);
	}
	for (wbValue const &v : args) {
		wb_push(p, v);
	}
	if (wb_is_odin_cc(pt)) {
		if (p->context_local >= 0) {
			wb_local_get(p, cast(u32)p->context_local);
		} else {
			// TODO(wasm): create a default context when calling from a "c" procedure
			wb_i32_const(p, 0);
		}
	}
	wb_call(p, callee);

	if (pt->Proc.result_count == 1) {
		Type *rt = wb_result_type(pt);
		wbValType vt = wb_valtype_of(rt);
		return wb_pop_to_local(p, vt, rt);
	}
	return wb_value_invalid();
}

gb_internal wbValue wb_build_expr(wbProcedure *p, Ast *expr) {
	expr = unparen_expr(expr);
	TypeAndValue tv = type_and_value_of_expr(expr);

	if (tv.value.kind != ExactValue_Invalid) {
		return wb_const(p, expr, tv.type, tv.value);
	}

	switch (expr->kind) {
	case_ast_node(i, Ident, expr);
		Entity *e = entity_of_node(expr);
		if (e == nullptr) {
			wb_unsupported(p, expr, "identifier");
			return wb_value_invalid();
		}
		if (e->kind == Entity_Nil) {
			ExactValue nil_value = {};
			return wb_const(p, expr, tv.type, nil_value);
		}
		if (e->kind == Entity_Variable) {
			u32 *idx = map_get(&p->entity_locals, e);
			if (idx != nullptr) {
				return wb_value_local(*idx, p->locals[*idx].vt, e->type);
			}
			wb_unsupported(p, expr, "global variable");
			return wb_value_invalid();
		}
		wb_unsupported(p, expr, "identifier kind");
		return wb_value_invalid();
	case_end;

	case_ast_node(ue, UnaryExpr, expr);
		return wb_build_unary_expr(p, expr);
	case_end;

	case_ast_node(be, BinaryExpr, expr);
		return wb_build_binary_expr(p, expr);
	case_end;

	case_ast_node(ce, CallExpr, expr);
		return wb_build_call_expr(p, expr);
	case_end;

	case_ast_node(tc, TypeCast, expr);
		wbValue x = wb_build_expr(p, tc->expr);
		return wb_emit_conv(p, x, tv.type);
	case_end;

	case_ast_node(te, TernaryIfExpr, expr);
		Type *type = tv.type;
		if (is_type_untyped(type)) {
			type = default_type(type);
		}
		wbValType vt = wb_valtype_of(type);
		if (vt == wbValType_Invalid) {
			wb_unsupported_type(p, expr, type);
			return wb_value_invalid();
		}
		u32 res = wb_add_local(p, vt);
		wbValue cond = wb_emit_conv(p, wb_build_expr(p, te->cond), t_bool);
		wb_push(p, cond);
		wb_open_if(p);
		wb_push(p, wb_emit_conv(p, wb_build_expr(p, te->x), type));
		wb_local_set(p, res);
		wb_else(p);
		wb_push(p, wb_emit_conv(p, wb_build_expr(p, te->y), type));
		wb_local_set(p, res);
		wb_close(p);
		return wb_value_local(res, vt, type);
	case_end;

	case_ast_node(te, TernaryWhenExpr, expr);
		TypeAndValue cond_tv = type_and_value_of_expr(te->cond);
		GB_ASSERT(cond_tv.value.kind == ExactValue_Bool);
		return wb_emit_conv(p, wb_build_expr(p, cond_tv.value.value_bool ? te->x : te->y), tv.type);
	case_end;
	}

	wb_unsupported(p, expr, "expression");
	return wb_value_invalid();
}

// Statements

gb_internal void wb_build_stmt_list(wbProcedure *p, Slice<Ast *> const &stmts) {
	for (Ast *stmt : stmts) {
		wb_build_stmt(p, stmt);
	}
}

gb_internal void wb_build_when_stmt(wbProcedure *p, AstWhenStmt *ws) {
	TypeAndValue tv = type_and_value_of_expr(ws->cond);
	GB_ASSERT(is_type_boolean(tv.type));
	GB_ASSERT(tv.value.kind == ExactValue_Bool);
	if (tv.value.value_bool) {
		wb_build_stmt_list(p, ws->body->BlockStmt.stmts);
	} else if (ws->else_stmt) {
		switch (ws->else_stmt->kind) {
		case Ast_BlockStmt:
			wb_build_stmt_list(p, ws->else_stmt->BlockStmt.stmts);
			break;
		case Ast_WhenStmt:
			wb_build_when_stmt(p, &ws->else_stmt->WhenStmt);
			break;
		default:
			GB_PANIC("Invalid 'else' statement in 'when' statement");
			break;
		}
	}
}

// Stores a value into an addressable expression (Phase 0: local variables only)
gb_internal void wb_build_store(wbProcedure *p, Ast *lhs, wbValue v) {
	lhs = unparen_expr(lhs);
	if (lhs->kind == Ast_Ident && is_blank_ident(lhs)) {
		return;
	}
	if (lhs->kind == Ast_Ident) {
		Entity *e = entity_of_node(lhs);
		if (e != nullptr && e->kind == Entity_Variable) {
			u32 *idx = map_get(&p->entity_locals, e);
			if (idx != nullptr) {
				v = wb_emit_conv(p, v, e->type);
				wb_push(p, v);
				wb_local_set(p, *idx);
				return;
			}
		}
	}
	wb_unsupported(p, lhs, "assignment target");
}

gb_internal void wb_build_assign_stmt(wbProcedure *p, AstAssignStmt *as, Ast *node) {
	if (as->op.kind == Token_Eq) {
		if (as->lhs.count != as->rhs.count) {
			wb_unsupported(p, node, "assignment with mismatched counts");
			return;
		}
		// Evaluate all right hand sides before storing (for `a, b = b, a`)
		auto values = array_make<wbValue>(temporary_allocator(), 0, as->rhs.count);
		for (Ast *rhs : as->rhs) {
			array_add(&values, wb_build_expr(p, rhs));
		}
		for_array(i, as->lhs) {
			wb_build_store(p, as->lhs[i], values[i]);
		}
		return;
	}

	// Operator assignment: x op= y
	GB_ASSERT(as->lhs.count == 1 && as->rhs.count == 1);
	Ast *lhs = as->lhs[0];
	TokenKind op = cast(TokenKind)(as->op.kind - Token_AddEq + Token_Add);
	if (as->op.kind == Token_CmpAndEq || as->op.kind == Token_CmpOrEq) {
		wb_unsupported(p, node, "'&&=' / '||=' assignment");
		return;
	}
	Type *type = type_of_expr(lhs);
	wbValue left  = wb_build_expr(p, lhs);
	wbValue right = wb_build_expr(p, as->rhs[0]);
	if (op != Token_Shl && op != Token_Shr) {
		right = wb_emit_conv(p, right, type);
	}
	wbValue res = wb_emit_arith(p, node, op, left, right, type, type);
	wb_build_store(p, lhs, res);
}

gb_internal void wb_build_return_stmt(wbProcedure *p, AstReturnStmt *rs, Ast *node) {
	Type *pt = base_type(p->type);
	if (pt->Proc.result_count == 0) {
		wb_op(p, wbOp_return);
		return;
	}
	if (rs->results.count == 0) {
		// bare return with named results
		GB_ASSERT(p->result_locals.count == 1);
		wb_local_get(p, p->result_locals[0]);
		wb_op(p, wbOp_return);
		return;
	}
	if (rs->results.count != 1) {
		wb_unsupported(p, node, "return with multiple values");
		return;
	}
	Type *rt = wb_result_type(pt);
	wbValue v = wb_emit_conv(p, wb_build_expr(p, rs->results[0]), rt);
	wb_push(p, v);
	wb_op(p, wbOp_return);
}

gb_internal void wb_push_label(wbProcedure *p, Ast *label, u32 break_depth, u32 continue_depth, bool is_loop) {
	wbLabel l = {};
	l.label = label;
	l.break_depth = break_depth;
	l.continue_depth = continue_depth;
	l.is_loop = is_loop;
	array_add(&p->labels, l);
}

gb_internal void wb_pop_label(wbProcedure *p) {
	array_pop(&p->labels);
}

gb_internal void wb_build_branch_stmt(wbProcedure *p, AstBranchStmt *bs, Ast *node) {
	bool is_break = bs->token.kind == Token_break;
	if (bs->token.kind != Token_break && bs->token.kind != Token_continue) {
		wb_unsupported(p, node, "'fallthrough'");
		return;
	}

	Ast *target_label = nullptr;
	if (bs->label != nullptr) {
		Entity *e = entity_of_node(bs->label);
		GB_ASSERT(e != nullptr && e->kind == Entity_Label);
		target_label = e->Label.node;
	}

	for (isize i = p->labels.count-1; i >= 0; i--) {
		wbLabel const &l = p->labels[i];
		if (target_label != nullptr) {
			if (l.label != target_label) {
				continue;
			}
		} else if (!l.is_loop) {
			// unlabelled break/continue only target loops (and switches)
			continue;
		}
		if (is_break) {
			wb_br(p, l.break_depth);
		} else if (l.is_loop) {
			wb_br(p, l.continue_depth);
		} else {
			wb_unsupported(p, node, "'continue' to a non-loop label");
		}
		return;
	}
	wb_unsupported(p, node, "branch target");
}

gb_internal void wb_build_if_stmt(wbProcedure *p, AstIfStmt *is, Ast *node) {
	if (is->init != nullptr) {
		wb_build_stmt(p, is->init);
	}

	u32 outer = 0;
	if (is->label != nullptr) {
		outer = wb_open_block(p);
		wb_push_label(p, is->label, outer, 0, false);
	}

	wbValue cond = wb_emit_conv(p, wb_build_expr(p, is->cond), t_bool);
	wb_push(p, cond);
	wb_open_if(p);
	wb_build_stmt(p, is->body);
	if (is->else_stmt != nullptr) {
		wb_else(p);
		wb_build_stmt(p, is->else_stmt);
	}
	wb_close(p);

	if (is->label != nullptr) {
		wb_pop_label(p);
		wb_close(p);
	}
}

gb_internal void wb_build_for_stmt(wbProcedure *p, AstForStmt *fs, Ast *node) {
	// block $break
	//   loop $loop
	//     if !cond br $break
	//     block $continue
	//       body
	//     end
	//     post
	//     br $loop
	//   end
	// end
	if (fs->init != nullptr) {
		wb_build_stmt(p, fs->init);
	}
	u32 break_depth = wb_open_block(p);
	u32 loop_depth  = wb_open_loop(p);
	if (fs->cond != nullptr) {
		wbValue cond = wb_emit_conv(p, wb_build_expr(p, fs->cond), t_bool);
		wb_push(p, cond);
		wb_op(p, wbOp_i32_eqz);
		wb_br_if(p, break_depth);
	}
	u32 continue_depth = wb_open_block(p);
	wb_push_label(p, fs->label, break_depth, continue_depth, true);
	wb_build_stmt(p, fs->body);
	wb_pop_label(p);
	wb_close(p); // continue
	if (fs->post != nullptr) {
		wb_build_stmt(p, fs->post);
	}
	wb_br(p, loop_depth);
	wb_close(p); // loop
	wb_close(p); // break
}

gb_internal void wb_build_value_decl(wbProcedure *p, AstValueDecl *vd, Ast *node) {
	if (!vd->is_mutable) {
		return;
	}
	if (vd->values.count != 0 && vd->values.count != vd->names.count) {
		wb_unsupported(p, node, "declaration with mismatched counts");
		return;
	}

	// Evaluate initializers before declaring the new locals
	auto values = array_make<wbValue>(temporary_allocator(), 0, vd->names.count);
	for (Ast *value : vd->values) {
		array_add(&values, wb_build_expr(p, value));
	}

	for_array(i, vd->names) {
		Ast *name = vd->names[i];
		if (is_blank_ident(name)) {
			continue;
		}
		Entity *e = entity_of_node(name);
		GB_ASSERT(e != nullptr && e->kind == Entity_Variable);
		wbValType vt = wb_valtype_of(e->type);
		if (vt == wbValType_Invalid) {
			wb_unsupported_type(p, name, e->type);
			continue;
		}
		u32 idx = wb_add_entity_local(p, e);
		if (values.count > 0) {
			wbValue v = wb_emit_conv(p, values[i], e->type);
			wb_push(p, v);
		} else {
			// Zero initialize (the declaration may be re-executed inside a loop)
			switch (vt) {
			case wbValType_i32: wb_i32_const(p, 0); break;
			case wbValType_i64: wb_i64_const(p, 0); break;
			case wbValType_f32: wb_f32_const(p, 0); break;
			case wbValType_f64: wb_f64_const(p, 0); break;
			default: break;
			}
		}
		wb_local_set(p, idx);
	}
}

gb_internal void wb_build_stmt(wbProcedure *p, Ast *node) {
	if (p->failed) {
		// Keep going to report more errors but do not bother about correctness
	}
	switch (node->kind) {
	case Ast_EmptyStmt:
		break;

	case_ast_node(bs, BlockStmt, node);
		if (bs->label != nullptr) {
			u32 depth = wb_open_block(p);
			wb_push_label(p, bs->label, depth, 0, false);
			wb_build_stmt_list(p, bs->stmts);
			wb_pop_label(p);
			wb_close(p);
		} else {
			wb_build_stmt_list(p, bs->stmts);
		}
	case_end;

	case_ast_node(ws, WhenStmt, node);
		wb_build_when_stmt(p, ws);
	case_end;

	case_ast_node(es, ExprStmt, node);
		wb_build_expr(p, es->expr);
	case_end;

	case_ast_node(vd, ValueDecl, node);
		wb_build_value_decl(p, vd, node);
	case_end;

	case_ast_node(as, AssignStmt, node);
		wb_build_assign_stmt(p, as, node);
	case_end;

	case_ast_node(rs, ReturnStmt, node);
		wb_build_return_stmt(p, rs, node);
	case_end;

	case_ast_node(is, IfStmt, node);
		wb_build_if_stmt(p, is, node);
	case_end;

	case_ast_node(fs, ForStmt, node);
		wb_build_for_stmt(p, fs, node);
	case_end;

	case_ast_node(bs, BranchStmt, node);
		wb_build_branch_stmt(p, bs, node);
	case_end;

	default:
		wb_unsupported(p, node, "statement");
		break;
	}
}

gb_internal void wb_build_procedure(wbProcedure *p) {
	if (p->failed) {
		wb_op(p, wbOp_unreachable);
		return;
	}
	Type *pt = base_type(p->type);

	// Parameters map 1:1 onto the first wasm locals
	if (pt->Proc.params != nullptr) {
		for (Entity *e : pt->Proc.params->Tuple.variables) {
			wb_add_entity_local(p, e);
		}
	}
	if (wb_is_odin_cc(pt)) {
		p->context_local = cast(i32)wb_add_local(p, wbValType_i32, str_lit("context"));
	}
	p->param_count = cast(u32)p->locals.count;

	// Named results get locals too
	if (pt->Proc.has_named_results && pt->Proc.results != nullptr) {
		for (Entity *e : pt->Proc.results->Tuple.variables) {
			u32 idx = wb_add_entity_local(p, e);
			array_add(&p->result_locals, idx);
		}
	}

	wb_build_stmt(p, p->body);

	// Falling off the end of a procedure with results is not allowed by the
	// checker, but wasm validation requires a well-typed end.
	if (p->results.count > 0) {
		wb_op(p, wbOp_unreachable);
	}
	GB_ASSERT(p->depth == 0);
}

// Module

gb_internal void wb_module_init(wbModule *m, CheckerInfo *info) {
	m->info = info;
	m->allocator = permanent_allocator();
	array_init(&m->types,      m->allocator);
	array_init(&m->imports,    m->allocator);
	array_init(&m->procedures, m->allocator);
	array_init(&m->work_queue, m->allocator);
	map_init(&m->procedure_map);

	// Same defaults as the wasm-ld invocation in linker.cpp: 1 MiB stack placed first
	m->stack_size = 1<<20;
	m->memory_initial_pages = (m->stack_size + 0xffff) / 0x10000;
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
	while (m->work_queue.count > 0) {
		wbProcedure *p = array_pop(&m->work_queue);
		wb_build_procedure(p);
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

	return wb_write_output(m);
}
