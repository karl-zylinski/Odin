// Direct WebAssembly backend: expressions

gb_internal wbValue wb_build_call_expr(wbProcedure *p, Ast *expr);
gb_internal wbValue wb_tuple_field(wbProcedure *p, wbValue tuple, isize index);
gb_internal void    wb_emit_return_values(wbProcedure *p, Array<wbValue> const &values, bool store_named);
gb_internal wbValue wb_emit_call(wbProcedure *p, Type *pt, wbProcedure *callee, wbValue proc_value, Array<wbValue> const &args);
gb_internal wbValue wb_build_slice_expr(wbProcedure *p, Ast *expr);

// Conservative check for whether evaluating an expression may run user code
// (and thereby change variables whose registers we have already picked up).
gb_internal bool wb_expr_has_call(Ast *expr) {
	if (expr == nullptr) {
		return false;
	}
	switch (expr->kind) {
	case Ast_CallExpr:
	case Ast_ProcLit:
	case Ast_OrElseExpr:
	case Ast_OrReturnExpr:
	case Ast_OrBranchExpr:
		return true;
	case Ast_ParenExpr:       return wb_expr_has_call(expr->ParenExpr.expr);
	case Ast_UnaryExpr:       return wb_expr_has_call(expr->UnaryExpr.expr);
	case Ast_BinaryExpr:      return wb_expr_has_call(expr->BinaryExpr.left) || wb_expr_has_call(expr->BinaryExpr.right);
	case Ast_SelectorExpr:    return wb_expr_has_call(expr->SelectorExpr.expr);
	case Ast_IndexExpr:       return wb_expr_has_call(expr->IndexExpr.expr) || wb_expr_has_call(expr->IndexExpr.index);
	case Ast_SliceExpr:       return wb_expr_has_call(expr->SliceExpr.expr) || wb_expr_has_call(expr->SliceExpr.low) || wb_expr_has_call(expr->SliceExpr.high);
	case Ast_DerefExpr:       return wb_expr_has_call(expr->DerefExpr.expr);
	case Ast_TypeCast:        return wb_expr_has_call(expr->TypeCast.expr);
	case Ast_AutoCast:        return wb_expr_has_call(expr->AutoCast.expr);
	case Ast_TernaryIfExpr:   return wb_expr_has_call(expr->TernaryIfExpr.cond) || wb_expr_has_call(expr->TernaryIfExpr.x) || wb_expr_has_call(expr->TernaryIfExpr.y);
	case Ast_TernaryWhenExpr: return wb_expr_has_call(expr->TernaryWhenExpr.x) || wb_expr_has_call(expr->TernaryWhenExpr.y);
	case Ast_TypeAssertion:   return true;
	case Ast_CompoundLit:
		for (Ast *elem : expr->CompoundLit.elems) {
			if (elem->kind == Ast_FieldValue) {
				if (wb_expr_has_call(elem->FieldValue.value)) return true;
			} else if (wb_expr_has_call(elem)) {
				return true;
			}
		}
		return false;
	default:
		return false;
	}
}

// Copies a value so that later side effects cannot change it
gb_internal wbValue wb_value_fresh(wbProcedure *p, wbValue v) {
	switch (v.kind) {
	case wbValue_Local:
		wb_push(p, v);
		return wb_pop_to_local(p, v.vt, v.type);
	case wbValue_Memory:
		return wb_value_copy(p, v);
	default:
		return v;
	}
}

gb_internal bool wb_is_int_class(Type *t) {
	t = core_type(t);
	return is_type_integer(t) || is_type_boolean(t) || is_type_rune(t) || is_type_pointer(t) ||
	       is_type_multi_pointer(t) || is_type_rawptr(t) || is_type_uintptr(t) || is_type_proc(t) ||
	       is_type_cstring(t) || is_type_cstring16(t) || is_type_bit_set(t) || is_type_typeid(t);
}

gb_internal wbValue wb_conv_unsupported(wbProcedure *p, Type *src, Type *dst) {
	p->failed = true;
	p->module->error_count++;
	gbString s0 = type_to_string(src);
	gbString s1 = type_to_string(dst);
	Token token = p->body != nullptr ? ast_token(p->body) : Token{};
	error(token, "wasm backend: unsupported conversion from '%s' to '%s'", s0, s1);
	gb_string_free(s0);
	gb_string_free(s1);
	return wb_value_invalid();
}

// Reinterprets a value as another type of the same size
gb_internal wbValue wb_emit_transmute(wbProcedure *p, wbValue v, Type *dst) {
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
	wbValType svt = v.vt;
	wbValType dvt = wb_valtype_of(dst);
	if (type_size_of(src) != type_size_of(dst)) {
		return wb_conv_unsupported(p, src, dst);
	}

	if (v.kind == wbValue_Memory) {
		if (dvt == wbValType_Invalid) {
			v.type = dst;
			return v;
		}
		return wb_emit_load(p, v.index, v.offset, dst);
	}
	if (dvt == wbValType_Invalid) {
		wbAddr tmp = wb_add_temp(p, dst);
		wb_emit_store(p, tmp.index, tmp.offset, v, src);
		return wb_value_memory(tmp.index, tmp.offset, dst);
	}

	wb_push(p, v);
	if (svt == dvt) {
		// same representation, but the canonical form of narrow integers may differ
	} else if (svt == wbValType_f32 && dvt == wbValType_i32) {
		wb_op(p, wbOp_i32_reinterpret_f32);
	} else if (svt == wbValType_i32 && dvt == wbValType_f32) {
		wb_op(p, wbOp_f32_reinterpret_i32);
	} else if (svt == wbValType_f64 && dvt == wbValType_i64) {
		wb_op(p, wbOp_i64_reinterpret_f64);
	} else if (svt == wbValType_i64 && dvt == wbValType_f64) {
		wb_op(p, wbOp_f64_reinterpret_i64);
	} else {
		wb_op(p, wbOp_drop);
		return wb_conv_unsupported(p, src, dst);
	}
	wb_emit_normalize(p, dst);
	return wb_pop_to_local(p, dvt, dst);
}

// Unions and `any`

gb_internal wbValue wb_emit_comp_against_nil(wbProcedure *p, Ast *node, wbValue v, bool is_not_eq);
gb_internal wbAddr  wb_addr_from_pointer(wbProcedure *p, wbValue ptr, Type *elem_type);
gb_internal wbValue wb_emit_arith(wbProcedure *p, Ast *node, TokenKind op, wbValue left, wbValue right, Type *operand_type, Type *result_type);
gb_internal wbValue wb_emit_conv_to_simd(wbProcedure *p, wbValue v, Type *dst);
gb_internal wbValue wb_emit_byte_swap(wbProcedure *p, wbValue x, Type *type);
gb_internal wbValue wb_emit_to_platform_endian(wbProcedure *p, wbValue x);
gb_internal wbValue wb_emit_from_platform_endian(wbProcedure *p, wbValue x, Type *type);
gb_internal wbValue wb_build_simd_builtin(wbProcedure *p, Ast *expr, BuiltinProcId id);
gb_internal wbValue wb_emit_runtime_call(wbProcedure *p, char const *name, Array<wbValue> const &args);
gb_internal wbAddr  wb_map_elem_addr(wbProcedure *p, wbAddr map, Type *map_type, wbValue key, Type *result_type);
gb_internal wbAddr  wb_addr_resolve_map(wbProcedure *p, wbAddr addr);
gb_internal wbValue wb_map_get_ptr(wbProcedure *p, wbAddr addr, Type *ptr_type);
gb_internal wbValue wb_emit_map_len(wbProcedure *p, wbValue m);
gb_internal wbValue wb_emit_map_cap(wbProcedure *p, wbValue m);
gb_internal u32     wb_map_info_addr(wbModule *m, Type *map_type);
gb_internal u32     wb_map_cell_info_addr(wbModule *m, Type *type);
gb_internal wbProcedure *wb_equal_proc_for_type(wbModule *m, Type *type);
gb_internal wbValue wb_emit_gen_call(wbProcedure *p, wbProcedure *callee, wbValue a, wbValue b);
gb_internal wbValue wb_emit_aggregate_compare(wbProcedure *p, Ast *node, TokenKind op, wbValue left, wbValue right, Type *operand_type, Type *result_type);
gb_internal wbValue wb_source_code_location(wbProcedure *p, String const &procedure, TokenPos const &pos);
gb_internal wbValue wb_type_info(wbProcedure *p, Type *type);
gb_internal bool wb_is_int128(Type *t);
gb_internal wbAddr wb_addr_deep_field(wbProcedure *p, Ast *expr, wbAddr addr, Type *type, Selection const &sel);
gb_internal wbValue wb_emit_arith128(wbProcedure *p, Ast *node, TokenKind op, wbValue left, wbValue right, Type *operand_type, Type *result_type);
gb_internal void wb_emit_branch(wbProcedure *p, TokenKind kind, Ast *label, Ast *node);

gb_internal wbValue wb_typeid(Type *type) {
	type = default_type(type);
	u64 id = type_hash_canonical_type(type);
	GB_ASSERT(id != 0);
	return wb_value_const_int(t_typeid, cast(i64)id);
}

// Offset of the tag within a union (the variants share offset 0)
gb_internal i32 wb_union_tag_offset(Type *ut) {
	ut = base_type(ut);
	GB_ASSERT(ut->kind == Type_Union);
	type_size_of(ut); // makes sure the layout has been computed
	return cast(i32)ut->Union.variant_block_size;
}

gb_internal bool wb_union_has_tag(Type *ut) {
	ut = base_type(ut);
	return !is_type_union_maybe_pointer(ut) && type_size_of(ut) > 0 && union_tag_size(ut) > 0;
}

// Stores the tag for `variant_type` into the union at `parent`
gb_internal void wb_emit_store_union_tag(wbProcedure *p, wbAddr parent, Type *variant_type) {
	Type *ut = base_type(parent.type);
	if (!wb_union_has_tag(ut)) {
		return;
	}
	Type *tag_type = union_tag_type(ut);
	i64 index = union_variant_index_checked(ut, variant_type);
	wb_emit_store(p, parent.index, parent.offset + wb_union_tag_offset(ut), wb_value_const_int(tag_type, index), tag_type);
}

// The tag of a union value (for maybe-pointer unions: 1 when non-nil)
gb_internal wbValue wb_emit_union_tag(wbProcedure *p, Ast *node, wbValue u) {
	Type *ut = base_type(u.type);
	if (is_type_union_maybe_pointer(ut)) {
		return wb_emit_conv(p, wb_emit_comp_against_nil(p, node, u, true), t_int);
	}
	if (!wb_union_has_tag(ut)) {
		return wb_value_const_int(t_int, 0);
	}
	wbAddr a = wb_value_to_addr(p, u);
	return wb_emit_load(p, a.index, a.offset + wb_union_tag_offset(ut), union_tag_type(ut));
}

// The tag value that selects `variant_type` (nil selects 0)
gb_internal wbValue wb_const_union_tag(Type *ut, Type *variant_type) {
	ut = base_type(ut);
	Type *tag_type = wb_union_has_tag(ut) ? union_tag_type(ut) : t_int;
	if (is_type_untyped_nil(variant_type)) {
		return wb_value_const_int(tag_type, 0);
	}
	if (is_type_union_maybe_pointer(ut)) {
		return wb_value_const_int(tag_type, 1);
	}
	return wb_value_const_int(tag_type, union_variant_index_checked(ut, variant_type));
}

// Picks the variant of `ut` that `src` converts to (lb_emit_conv)
gb_internal Type *wb_union_variant_for(Type *ut, Type *src) {
	ut = base_type(ut);
	if (ut->Union.variants.count == 1 && internal_check_is_assignable_to(src, ut->Union.variants[0])) {
		return ut->Union.variants[0];
	}
	for (Type *vt : ut->Union.variants) {
		if (are_types_identical(src, vt)) {
			return vt;
		}
	}
	Type *found = nullptr;
	isize count = 0;
	for (Type *vt : ut->Union.variants) {
		if (internal_check_is_assignable_to(src, vt)) {
			found = vt;
			count += 1;
		}
	}
	return count == 1 ? found : nullptr;
}

gb_internal wbValue wb_emit_conv_to_union(wbProcedure *p, wbValue v, Type *dst) {
	Type *src = v.type;
	Type *ut = base_type(dst);
	Type *vt = wb_union_variant_for(ut, src);
	if (vt == nullptr) {
		return wb_conv_unsupported(p, src, dst);
	}
	v = wb_emit_conv(p, v, vt);
	if (v.kind == wbValue_Invalid) {
		return v;
	}
	wbAddr res = wb_add_temp(p, dst);
	wb_addr_zero(p, res);
	wbAddr slot = wb_addr_memory(res.index, res.offset, vt);
	if (ut->Union.kind == UnionType_shared_nil) {
		// a nil variant is stored as the union's nil
		v = wb_value_fresh(p, v);
		wb_push(p, wb_emit_comp_against_nil(p, nullptr, v, true));
		wb_open_if(p);
		wb_addr_store(p, slot, v);
		wb_emit_store_union_tag(p, res, vt);
		wb_close(p);
	} else {
		wb_addr_store(p, slot, v);
		wb_emit_store_union_tag(p, res, vt);
	}
	return wb_value_memory(res.index, res.offset, dst);
}

gb_internal wbValue wb_emit_conv_to_any(wbProcedure *p, wbValue v, Type *dst) {
	Type *src = default_type(v.type);
	wbAddr res = wb_add_temp(p, dst);
	if (is_type_untyped_nil(src)) {
		wb_addr_zero(p, res);
		return wb_value_memory(res.index, res.offset, dst);
	}
	v.type = src;
	wbAddr data = wb_value_to_addr(p, v);
	if (data.kind != wbAddr_Memory) {
		return wb_conv_unsupported(p, src, dst);
	}
	wb_addr_store(p, wb_addr_field(res, 0), wb_addr_get_ptr(p, data, t_rawptr));
	wb_addr_store(p, wb_addr_field(res, 1), wb_typeid(src));
	return wb_value_memory(res.index, res.offset, dst);
}

// Panics through the runtime when a type assertion failed (`ok` is false)
gb_internal void wb_emit_type_assertion_check(wbProcedure *p, wbValue ok, TokenPos pos, wbValue from_id, wbValue to_id, wbValue from_data) {
	if (build_context.no_type_assert || (p->state_flags & StateFlag_no_type_assert) != 0) {
		return;
	}
	auto args = array_make<wbValue>(temporary_allocator(), 0, 7);
	array_add(&args, ok);
	String file = get_file_path_string(pos.file_id);
	array_add(&args, wb_const(p, nullptr, t_string, exact_value_string(file)));
	array_add(&args, wb_value_const_int(t_i32, pos.line));
	array_add(&args, wb_value_const_int(t_i32, pos.column));
	if (!build_context.no_rtti) {
		array_add(&args, from_id);
		array_add(&args, to_id);
		array_add(&args, from_data);
	}
	char const *name = "type_assertion_check2_contextless";
	if (wb_is_odin_cc(p->type) || p->context_stack.count > 0) {
		name = "type_assertion_check2_with_context";
	}
	wb_emit_runtime_call(p, name, args);
}

// `u.(T)` / `u.(T), ok` for unions (lb_emit_union_cast)
gb_internal wbValue wb_emit_union_cast(wbProcedure *p, Ast *node, wbValue value, Type *type, TokenPos pos) {
	if (is_type_pointer(value.type)) {
		value = wb_addr_load(p, wb_addr_from_pointer(p, value, type_deref(value.type)));
	}
	if (value.kind == wbValue_Invalid) {
		return value;
	}
	bool is_tuple = type->kind == Type_Tuple;
	Type *tuple = is_tuple ? type : make_optional_ok_type(type);
	Type *dst = tuple->Tuple.variables[0]->type;
	Type *src = base_type(value.type);
	GB_ASSERT(src->kind == Type_Union);

	wbAddr res = wb_add_temp(p, tuple);
	wb_addr_zero(p, res);
	wbAddr value_addr = wb_value_to_addr(p, value);

	wbValue cond = {};
	if (is_type_union_maybe_pointer(src)) {
		cond = wb_emit_comp_against_nil(p, node, value, true);
	} else if (!wb_union_has_tag(src)) {
		cond = wb_value_const_int(t_bool, 1);
	} else {
		wbValue tag = wb_emit_union_tag(p, node, value);
		cond = wb_emit_arith(p, node, Token_CmpEq, tag, wb_const_union_tag(src, dst), tag.type, t_bool);
	}
	if (cond.kind == wbValue_Invalid) {
		return cond;
	}
	wb_push(p, cond);
	wb_open_if(p);
	wbAddr slot = wb_addr_field(res, 0);
	wb_emit_copy(p, slot.index, slot.offset, value_addr.index, value_addr.offset, type_size_of(dst));
	wb_addr_store(p, wb_addr_field(res, 1), wb_value_const_int(t_bool, 1));
	wb_close(p);

	if (!is_tuple) {
		wbValue ok = wb_addr_load(p, wb_addr_field(res, 1));
		wb_emit_type_assertion_check(p, ok, pos, wb_typeid(value.type), wb_typeid(dst), wb_addr_get_ptr(p, value_addr, t_rawptr));
		return wb_addr_load(p, wb_addr_field(res, 0));
	}
	return wb_value_memory(res.index, res.offset, tuple);
}

// `a.(T)` / `a.(T), ok` for `any` (lb_emit_any_cast)
gb_internal wbValue wb_emit_any_cast(wbProcedure *p, Ast *node, wbValue value, Type *type, TokenPos pos) {
	if (is_type_pointer(value.type)) {
		value = wb_addr_load(p, wb_addr_from_pointer(p, value, type_deref(value.type)));
	}
	if (value.kind == wbValue_Invalid) {
		return value;
	}
	bool is_tuple = type->kind == Type_Tuple;
	Type *tuple = is_tuple ? type : make_optional_ok_type(type);
	Type *dst = tuple->Tuple.variables[0]->type;

	wbAddr res = wb_add_temp(p, tuple);
	wb_addr_zero(p, res);
	wbAddr value_addr = wb_value_to_addr(p, value);
	wbValue any_data = wb_addr_load(p, wb_addr_field(value_addr, 0));
	wbValue any_id   = wb_addr_load(p, wb_addr_field(value_addr, 1));
	wbValue dst_id   = wb_typeid(dst);

	wbValue cond = wb_emit_arith(p, node, Token_CmpEq, any_id, dst_id, t_typeid, t_bool);
	if (cond.kind == wbValue_Invalid) {
		return cond;
	}
	wb_push(p, cond);
	wb_open_if(p);
	wbAddr slot = wb_addr_field(res, 0);
	wbAddr src_addr = wb_addr_from_pointer(p, any_data, dst);
	wb_emit_copy(p, slot.index, slot.offset, src_addr.index, src_addr.offset, type_size_of(dst));
	wb_addr_store(p, wb_addr_field(res, 1), wb_value_const_int(t_bool, 1));
	wb_close(p);

	if (!is_tuple) {
		wbValue ok = wb_addr_load(p, wb_addr_field(res, 1));
		wb_emit_type_assertion_check(p, ok, pos, any_id, dst_id, any_data);
		return wb_addr_load(p, wb_addr_field(res, 0));
	}
	return wb_value_memory(res.index, res.offset, tuple);
}

// `&x.(T)` / `&x.(T), ok`: a pointer into the union or any's data rather than
// a copy (lb_build_unary_and)
gb_internal wbValue wb_build_unary_and_type_assertion(wbProcedure *p, Ast *expr, Ast *inner) {
	ast_node(ta, TypeAssertion, inner);
	TokenPos pos = ast_token(expr).pos;
	Type *tv_type = type_of_expr(expr);
	Type *type = type_of_expr(inner);
	bool is_tuple = is_type_tuple(tv_type);
	Type *ptr_type = is_tuple ? tv_type->Tuple.variables[0]->type : tv_type;
	Type *ok_type  = is_tuple ? tv_type->Tuple.variables[1]->type : t_bool;

	Type *et = type_of_expr(ta->expr);
	wbAddr addr = {};
	if (is_type_pointer(et)) {
		wbValue ptr = wb_build_expr(p, ta->expr);
		if (ptr.kind == wbValue_Invalid) {
			return wb_value_invalid();
		}
		addr = wb_addr_from_pointer(p, ptr, type_deref(et));
	} else {
		addr = wb_build_addr(p, ta->expr);
	}
	if (addr.kind == wbAddr_Invalid) {
		return wb_value_invalid();
	}
	addr = wb_value_to_addr(p, wb_addr_load(p, addr));
	Type *t = type_deref(et);
	wbValue value = wb_addr_load(p, addr);

	wbValue cond = {};
	wbValue data_ptr = {};
	wbValue from_id = {};
	if (is_type_union(t)) {
		Type *src = base_type(t);
		if (is_type_union_maybe_pointer(src)) {
			cond = wb_emit_comp_against_nil(p, expr, value, true);
		} else if (!wb_union_has_tag(src)) {
			cond = wb_value_const_int(t_bool, 1);
		} else {
			wbValue tag = wb_emit_union_tag(p, expr, value);
			cond = wb_emit_arith(p, expr, Token_CmpEq, tag, wb_const_union_tag(src, type), tag.type, t_bool);
		}
		data_ptr = wb_addr_get_ptr(p, addr, ptr_type);
		from_id = wb_typeid(t);
	} else if (is_type_any(t)) {
		wbValue any_data = wb_addr_load(p, wb_addr_field(addr, 0));
		from_id = wb_addr_load(p, wb_addr_field(addr, 1));
		cond = wb_emit_arith(p, expr, Token_CmpEq, from_id, wb_typeid(type), t_typeid, t_bool);
		data_ptr = wb_emit_conv(p, any_data, ptr_type);
	} else {
		wb_unsupported_type(p, expr, et);
		return wb_value_invalid();
	}
	if (cond.kind == wbValue_Invalid || data_ptr.kind == wbValue_Invalid) {
		return wb_value_invalid();
	}
	if (!is_tuple) {
		wb_emit_type_assertion_check(p, cond, pos, from_id, wb_typeid(type), wb_emit_conv(p, data_ptr, t_rawptr));
		return data_ptr;
	}
	// (ok ? ptr : nil, ok)
	wbAddr res = wb_add_temp(p, tv_type);
	wb_push(p, data_ptr);
	wb_i32_const(p, 0);
	wb_push(p, cond);
	wb_op(p, wbOp_select);
	wb_addr_store(p, wb_addr_field(res, 0), wb_pop_to_local(p, wbValType_i32, ptr_type));
	wb_addr_store(p, wb_addr_field(res, 1), wb_emit_conv(p, cond, ok_type));
	return wb_value_memory(res.index, res.offset, tv_type);
}

gb_internal wbValue wb_build_type_assertion(wbProcedure *p, Ast *expr) {
	ast_node(ta, TypeAssertion, expr);
	TokenPos pos = ast_token(expr).pos;
	Type *type = type_of_expr(expr);
	wbValue e = wb_build_expr(p, ta->expr);
	if (e.kind == wbValue_Invalid) {
		return e;
	}
	Type *t = type_deref(e.type);
	if (is_type_union(t)) {
		return wb_emit_union_cast(p, expr, e, type, pos);
	} else if (is_type_any(t)) {
		return wb_emit_any_cast(p, expr, e, type, pos);
	}
	wb_unsupported_type(p, expr, e.type);
	return wb_value_invalid();
}

gb_internal wbValue wb_matrix_elem(wbProcedure *p, wbValue m, i64 row, i64 col);
gb_internal void    wb_matrix_store_elem(wbProcedure *p, wbAddr res, i64 row, i64 col, wbValue v);

// matrix -> matrix: same shape converts element-wise; square matrices embed into the
// top-left corner of a larger identity; otherwise the element counts must match
gb_internal wbValue wb_emit_conv_matrix(wbProcedure *p, wbValue v, Type *dst) {
	Type *st = base_type(v.type);
	Type *dt = base_type(dst);
	if (v.kind != wbValue_Memory) {
		return wb_value_invalid();
	}
	wbAddr res = wb_add_temp(p, dst);
	if (dt->Matrix.row_count == st->Matrix.row_count && dt->Matrix.column_count == st->Matrix.column_count) {
		for (i64 j = 0; j < dt->Matrix.column_count; j++) {
			for (i64 i = 0; i < dt->Matrix.row_count; i++) {
				wb_matrix_store_elem(p, res, i, j, wb_matrix_elem(p, v, i, j));
			}
		}
	} else if (is_matrix_square(dt) && is_matrix_square(st)) {
		wb_addr_zero(p, res);
		for (i64 j = 0; j < dt->Matrix.column_count; j++) {
			for (i64 i = 0; i < dt->Matrix.row_count; i++) {
				if (i < st->Matrix.row_count && j < st->Matrix.column_count) {
					wb_matrix_store_elem(p, res, i, j, wb_matrix_elem(p, v, i, j));
				} else if (i == j) {
					wb_matrix_store_elem(p, res, i, j, wb_const(p, nullptr, dt->Matrix.elem, exact_value_i64(1)));
				}
			}
		}
	} else {
		i64 count = st->Matrix.row_count*st->Matrix.column_count;
		GB_ASSERT(count == dt->Matrix.row_count*dt->Matrix.column_count);
		Type *se = st->Matrix.elem;
		Type *de = dt->Matrix.elem;
		for (i64 k = 0; k < count; k++) {
			wbValue e = wb_emit_load(p, v.index, v.offset + cast(i32)(matrix_column_major_index_to_offset(st, k)*type_size_of(se)), se);
			wb_addr_store(p, wb_addr_offset(res, matrix_column_major_index_to_offset(dt, k)*type_size_of(de), de), e);
		}
	}
	return wb_value_memory(res.index, res.offset, dst);
}

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
	if (is_type_untyped_nil(src)) {
		// nil: a zero scalar, or zeroed memory for aggregates
		if (dvt == wbValType_Invalid) {
			wbAddr tmp = wb_add_temp(p, dst);
			wb_addr_zero(p, tmp);
			return wb_value_memory(tmp.index, tmp.offset, dst);
		}
		wbValue z = {};
		z.kind = wbValue_Const;
		z.vt   = dvt;
		z.type = dst;
		return z;
	}
	wbValType svt = wb_valtype_of(src);

	// Endian-specific scalars convert through their platform types
	if (is_type_different_to_arch_endianness(src) || is_type_different_to_arch_endianness(dst)) {
		if ((wb_is_int_class(core_type(src)) || is_type_float(core_type(src)) || is_type_bit_set(src)) &&
		    (wb_is_int_class(core_type(dst)) || is_type_float(core_type(dst)) || is_type_bit_set(dst))) {
			Type *platform_dst = is_type_different_to_arch_endianness(dst) ? integer_endian_type_to_platform_type(dst) : dst;
			wbValue res = wb_emit_to_platform_endian(p, v);
			res = wb_emit_conv(p, res, platform_dst);
			return wb_emit_from_platform_endian(p, res, dst);
		}
	}

	if (is_type_array_like(dst) && !is_type_array_like(src) && svt != wbValType_Invalid) {
		// scalar -> [N]T splat (array arithmetic with a scalar operand)
		Type *bt = base_type(dst);
		Type *elem = bt->kind == Type_Array ? bt->Array.elem : bt->EnumeratedArray.elem;
		i64 count  = bt->kind == Type_Array ? bt->Array.count : bt->EnumeratedArray.count;
		wbValue e = wb_value_to_local(p, wb_emit_conv(p, v, elem));
		wbAddr tmp = wb_add_temp(p, dst);
		i64 elem_size = type_size_of(elem);
		for (i64 i = 0; i < count; i++) {
			wb_addr_store(p, wb_addr_offset(tmp, i * elem_size, elem), e);
		}
		return wb_value_memory(tmp.index, tmp.offset, dst);
	}
	if (is_type_array(dst) && is_type_array(src)) {
		// [N]A -> [N]B: element by element
		Type *bs = base_type(src);
		Type *bd = base_type(dst);
		if (bs->Array.count == bd->Array.count && !are_types_identical(bs->Array.elem, bd->Array.elem)) {
			wbAddr sa = wb_value_to_addr(p, v);
			wbAddr tmp = wb_add_temp(p, dst);
			i64 ses = type_size_of(bs->Array.elem);
			i64 des = type_size_of(bd->Array.elem);
			for (i64 i = 0; i < bd->Array.count; i++) {
				wbValue e = wb_addr_load(p, wb_addr_offset(sa, i * ses, bs->Array.elem));
				wb_addr_store(p, wb_addr_offset(tmp, i * des, bd->Array.elem), wb_emit_conv(p, e, bd->Array.elem));
			}
			return wb_value_memory(tmp.index, tmp.offset, dst);
		}
	}
	if (is_type_matrix(dst) && !is_type_matrix(src) && svt != wbValType_Invalid) {
		// scalar -> square matrix: the scaled identity
		Type *bt = base_type(dst);
		Type *elem = bt->Matrix.elem;
		wbValue e = wb_value_to_local(p, wb_emit_conv(p, v, elem));
		wbAddr tmp = wb_add_temp(p, dst);
		wb_addr_zero(p, tmp);
		i64 n = gb_min(bt->Matrix.row_count, bt->Matrix.column_count);
		for (i64 i = 0; i < n; i++) {
			wb_matrix_store_elem(p, tmp, i, i, e);
		}
		return wb_value_memory(tmp.index, tmp.offset, dst);
	}
	if (is_type_matrix(dst) && is_type_matrix(src)) {
		return wb_emit_conv_matrix(p, v, dst);
	}
	if (is_type_union(dst) && !are_types_identical(base_type(src), base_type(dst))) {
		return wb_emit_conv_to_union(p, v, dst);
	}
	if (is_type_any(dst)) {
		return wb_emit_conv_to_any(p, v, dst);
	}

	// f16 is held as raw bits; all conversions go through f32 using the runtime helpers
	if (wb_is_f16(src) && !wb_is_f16(dst)) {
		auto args = array_make<wbValue>(temporary_allocator(), 1);
		args[0] = v;
		args[0].type = t_u16;
		wbValue f = wb_emit_runtime_call(p, "extendhfsf2", args);
		f.type = t_f32;
		return wb_emit_conv(p, f, dst);
	}
	if (wb_is_f16(dst) && !wb_is_f16(src)) {
		wbValue f = wb_emit_conv(p, v, t_f32);
		if (f.kind == wbValue_Invalid) {
			return f;
		}
		auto args = array_make<wbValue>(temporary_allocator(), 1);
		args[0] = f;
		wbValue h = wb_emit_runtime_call(p, "truncsfhf2", args);
		h.type = dst;
		return h;
	}
	if (is_type_cstring(src) && is_type_string(dst) && !is_type_cstring(dst)) {
		auto args = array_make<wbValue>(temporary_allocator(), 1);
		args[0] = v;
		wbValue str = wb_emit_runtime_call(p, "cstring_to_string", args);
		str.type = dst;
		return str;
	}
	if (is_type_cstring16(src) && is_type_string16(dst)) {
		auto args = array_make<wbValue>(temporary_allocator(), 1);
		args[0] = v;
		wbValue str = wb_emit_runtime_call(p, "cstring16_to_string16", args);
		str.type = dst;
		return str;
	}

	// Subtype polymorphism: a struct (or pointer to one) converts to the type of a `using` field
	if (check_is_assignable_to_using_subtype(src, dst)) {
		TEMPORARY_ALLOCATOR_GUARD();
		Selection sel = {};
		sel.index.allocator = temporary_allocator();
		if (lookup_subtype_polymorphic_selection(dst, src, &sel) && sel.entity != nullptr) {
			wbAddr addr = {};
			Type *st = src;
			if (is_type_pointer(src)) {
				st = type_deref(src);
				addr = wb_addr_from_pointer(p, v, st);
			} else {
				addr = wb_value_to_addr(p, v);
			}
			addr = wb_addr_deep_field(p, nullptr, addr, st, sel);
			if (addr.kind == wbAddr_Invalid) {
				return wb_value_invalid();
			}
			if (is_type_pointer(dst) && !is_type_pointer(addr.type)) {
				return wb_addr_get_ptr(p, addr, dst);
			}
			wbValue res = wb_addr_load(p, addr);
			res.type = dst;
			return res;
		}
	}

	// SIMD vectors: a scalar is splat across the lanes, vectors convert lane by lane
	if (is_type_simd_vector(dst)) {
		return wb_emit_conv_to_simd(p, v, dst);
	}

	// Complex numbers and quaternions: elementwise, a real scalar becomes the real part
	if (is_type_complex(dst) || is_type_quaternion(dst)) {
		Type *dft = base_complex_elem_type(dst);
		i32 dfs = cast(i32)type_size_of(dft);
		wbAddr res = wb_add_temp(p, dst);
		wb_addr_zero(p, res);
		bool dst_quat = is_type_quaternion(dst);
		if (is_type_complex(src) || is_type_quaternion(src)) {
			Type *sft = base_complex_elem_type(src);
			i32 sfs = cast(i32)type_size_of(sft);
			wbAddr sa = wb_value_to_addr(p, v);
			bool src_quat = is_type_quaternion(src);
			// complex: {real, imag}; quaternion: {imag, jmag, kmag, real}
			i32 src_real = src_quat ? 3 : 0, src_imag = src_quat ? 0 : 1;
			i32 dst_real = dst_quat ? 3 : 0, dst_imag = dst_quat ? 0 : 1;
			wb_emit_store(p, res.index, res.offset + dst_real*dfs, wb_emit_conv(p, wb_emit_load(p, sa.index, sa.offset + src_real*sfs, sft), dft), dft);
			wb_emit_store(p, res.index, res.offset + dst_imag*dfs, wb_emit_conv(p, wb_emit_load(p, sa.index, sa.offset + src_imag*sfs, sft), dft), dft);
			if (src_quat && dst_quat) {
				for (i32 i = 1; i <= 2; i++) {
					wb_emit_store(p, res.index, res.offset + i*dfs, wb_emit_conv(p, wb_emit_load(p, sa.index, sa.offset + i*sfs, sft), dft), dft);
				}
			}
		} else {
			wbValue re = wb_emit_conv(p, v, dft);
			if (re.kind == wbValue_Invalid) {
				return re;
			}
			wb_emit_store(p, res.index, res.offset + (dst_quat ? 3 : 0)*dfs, re, dft);
		}
		return wb_value_memory(res.index, res.offset, dst);
	}

	// 128-bit integers <-> floats go through the runtime's f64 helpers
	if (wb_is_int128(src) && is_type_float(core_type(dst))) {
		auto args = array_make<wbValue>(temporary_allocator(), 1);
		args[0] = v;
		wbValue f = wb_emit_runtime_call(p, is_type_unsigned(core_type(src)) ? "floattidf_unsigned" : "floattidf", args);
		return wb_emit_conv(p, f, dst);
	}
	if (is_type_float(core_type(src)) && wb_is_int128(dst)) {
		auto args = array_make<wbValue>(temporary_allocator(), 1);
		args[0] = wb_emit_conv(p, v, t_f64);
		wbValue i = wb_emit_runtime_call(p, is_type_unsigned(core_type(dst)) ? "fixunsdfti" : "fixdfti", args);
		i.type = dst;
		return i;
	}

	// 128-bit integers live in memory as two 64-bit words (little endian)
	if (wb_is_int_class(core_type(src)) && wb_is_int_class(core_type(dst)) && (type_size_of(src) == 16) != (type_size_of(dst) == 16)) {
		if (type_size_of(src) == 16) {
			// narrowing: the low word holds the value
			GB_ASSERT(v.kind == wbValue_Memory);
			wbValue low = wb_emit_load(p, v.index, v.offset, t_u64);
			return wb_emit_conv(p, low, dst);
		}
		wbAddr tmp = wb_add_temp(p, dst);
		wbValue low = wb_emit_conv(p, v, wb_type_is_signed(core_type(src)) ? t_i64 : t_u64);
		if (low.kind == wbValue_Invalid) {
			return wb_value_invalid();
		}
		wb_emit_store(p, tmp.index, tmp.offset, low, t_u64);
		// high word: sign extension of the low word for signed sources, else zero
		wb_push(p, low);
		if (wb_type_is_signed(core_type(src))) {
			wb_i64_const(p, 63);
			wb_op(p, wbOp_i64_shr_s);
		} else {
			wb_op(p, wbOp_drop);
			wb_i64_const(p, 0);
		}
		wbValue high = wb_pop_to_local(p, wbValType_i64, t_u64);
		wb_emit_store(p, tmp.index, tmp.offset + 8, high, t_u64);
		return wb_value_memory(tmp.index, tmp.offset, dst);
	}

	if (dvt == wbValType_Invalid || svt == wbValType_Invalid) {
		// Aggregates: only representation-preserving conversions are supported
		// (distinct types, named/unnamed, unions of the same layout, ...)
		if (v.kind == wbValue_Memory && dvt == wbValType_Invalid && type_size_of(src) == type_size_of(dst)) {
			Type *bs = base_type(src);
			Type *bd = base_type(dst);
			if (bs->kind == bd->kind || is_type_untyped_nil(src)) {
				v.type = dst;
				return v;
			}
			// []u8 <-> string and []u16 <-> string16 share the {data, len} layout
			if ((is_type_slice(bs) && is_type_string(bd)) || (is_type_string(bs) && is_type_slice(bd))) {
				v.type = dst;
				return v;
			}
		}
		return wb_conv_unsupported(p, src, dst);
	}

	Type *cs = core_type(src);
	Type *cd = core_type(dst);
	bool src_int   = wb_is_int_class(cs);
	bool dst_int   = wb_is_int_class(cd);
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
	if (v.kind == wbValue_Const && src_int && dst_float) {
		v.type = dst;
		v.vt = dvt;
		v.f = is_type_unsigned(cs) ? cast(f64)cast(u64)v.i : cast(f64)v.i;
		return v;
	}
	if (v.kind == wbValue_Const && src_int && dst_int) {
		v.type = dst;
		v.vt = dvt;
		i64 size = type_size_of(cd);
		if (dvt == wbValType_i32) {
			if (size == 1) {
				v.i = wb_type_is_signed(cd) ? cast(i8)v.i : cast(u8)v.i;
			} else if (size == 2) {
				v.i = wb_type_is_signed(cd) ? cast(i16)v.i : cast(u16)v.i;
			} else {
				v.i = cast(i32)v.i;
			}
		}
		return v;
	}

	if (src_int && dst_int && v.vt == dvt) {
		// Same representation. Values narrower than the wasm type are held
		// sign/zero extended, so they need to be renormalized when narrowing
		// or when the signedness changes (u8 128 -> i8 -128)
		i64 ss = type_size_of(cs);
		i64 ds = type_size_of(cd);
		bool sub_width = ds < (dvt == wbValType_i64 ? 8 : 4);
		if (ds >= ss && !(sub_width && wb_type_is_signed(cd) != wb_type_is_signed(cs))) {
			v.type = dst;
			return v;
		}
	}

	wb_push(p, v);

	if (src_int && dst_int) {
		if (v.vt == wbValType_i32 && dvt == wbValType_i64) {
			wb_op(p, src_signed ? wbOp_i64_extend_i32_s : wbOp_i64_extend_i32_u);
		} else if (v.vt == wbValType_i64 && dvt == wbValType_i32) {
			wb_op(p, wbOp_i32_wrap_i64);
			wb_emit_normalize(p, dst);
		} else if (v.vt == dvt) {
			// Same representation: narrowing or signedness change
			wb_emit_normalize(p, dst);
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
		if ((v.vt == wbValType_f32 || v.vt == wbValType_f64) && (dvt == wbValType_i32 || dvt == wbValType_i64)) {
			wb_trunc_sat(p, dvt == wbValType_i64, v.vt == wbValType_f64, dst_signed);
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
	wb_op(p, wbOp_drop);
	return wb_conv_unsupported(p, src, dst);
}

// Element addressing

// Pushes an index converted to a 32 bit integer
gb_internal void wb_push_index(wbProcedure *p, wbValue index) {
	index = wb_emit_conv(p, index, t_int);
	wb_push(p, index);
}

// Address of element `index` (of `elem_size` bytes) starting at local[base]+offset
gb_internal wbAddr wb_emit_elem_addr(wbProcedure *p, u32 base, i32 offset, wbValue index, Type *elem_type) {
	i64 elem_size = type_size_of(elem_type);
	if (index.kind == wbValue_Const) {
		return wb_addr_memory(base, offset + cast(i32)(index.i * elem_size), elem_type);
	}
	if (index.kind == wbValue_Invalid) {
		return wb_addr_memory(base, offset, elem_type);
	}
	wb_push_address(p, base, offset);
	wb_push_index(p, index);
	if (elem_size != 1) {
		wb_i32_const(p, cast(i32)elem_size);
		wb_op(p, wbOp_i32_mul);
	}
	wb_op(p, wbOp_i32_add);
	u32 local = wb_add_local(p, wbValType_i32);
	wb_local_set(p, local);
	return wb_addr_memory(local, 0, elem_type);
}

// Memory location a pointer value points at
gb_internal wbAddr wb_addr_from_pointer(wbProcedure *p, wbValue ptr, Type *elem_type) {
	if (ptr.kind == wbValue_Invalid) {
		wbAddr a = {};
		return a;
	}
	if (ptr.kind == wbValue_Const) {
		return wb_addr_memory(WB_NO_LOCAL, cast(i32)ptr.i, elem_type);
	}
	ptr = wb_value_to_local(p, ptr);
	return wb_addr_memory(ptr.index, 0, elem_type);
}

// The `data` pointer of a slice/string/dynamic array held in memory
gb_internal wbValue wb_emit_slice_data(wbProcedure *p, wbValue s) {
	Type *ptr_type = t_rawptr;
	Type *bt = base_type(s.type);
	if (bt->kind == Type_Slice) {
		ptr_type = alloc_type_multi_pointer(bt->Slice.elem);
	} else if (bt->kind == Type_DynamicArray) {
		ptr_type = alloc_type_multi_pointer(bt->DynamicArray.elem);
	} else if (is_type_string(bt)) {
		ptr_type = alloc_type_multi_pointer(t_u8);
	}
	if (s.kind != wbValue_Memory) {
		return wb_value_invalid();
	}
	return wb_emit_load(p, s.index, s.offset, ptr_type);
}

gb_internal wbValue wb_emit_slice_len(wbProcedure *p, wbValue s) {
	if (s.kind != wbValue_Memory) {
		return wb_value_invalid();
	}
	return wb_emit_load(p, s.index, s.offset + cast(i32)build_context.int_size, t_int);
}

gb_internal wbValue wb_emit_dynamic_array_cap(wbProcedure *p, wbValue s) {
	if (s.kind != wbValue_Memory) {
		return wb_value_invalid();
	}
	return wb_emit_load(p, s.index, s.offset + cast(i32)(2*build_context.int_size), t_int);
}

// Addresses

gb_internal wbAddr wb_addr_of_entity(wbProcedure *p, Entity *e, Ast *node) {
	wbAddr *found = map_get(&p->variables, e);
	if (found) {
		return *found;
	}
	if (e->kind == Entity_Variable) {
		if ((e->scope->flags & (ScopeFlag_Global|ScopeFlag_File|ScopeFlag_Pkg)) != 0 || (e->flags & EntityFlag_Static) != 0) {
			return wb_addr_memory(WB_NO_LOCAL, cast(i32)wb_global_addr(p->module, e), e->type);
		}
		if (e->flags & EntityFlag_Using) {
			// `using x` variable (or parameter): the entity names a field of the parent
			Entity *parent = e->using_parent;
			GB_ASSERT(parent != nullptr);
			wbAddr parent_addr = {};
			wbAddr *pv = map_get(&p->variables, parent);
			if (pv != nullptr) {
				parent_addr = *pv;
			} else if (e->using_expr != nullptr) {
				parent_addr = wb_build_addr(p, e->using_expr);
			} else {
				parent_addr = wb_addr_of_entity(p, parent, node);
			}
			if (parent_addr.kind == wbAddr_Invalid) {
				return parent_addr;
			}
			Selection sel = lookup_field(parent->type, entity_interned_name(e), false);
			GB_ASSERT(sel.entity != nullptr);
			wbAddr addr = wb_addr_deep_field(p, node, wb_addr_resolve_map(p, parent_addr), parent->type, sel);
			addr.type = e->type;
			return addr;
		}
		// A local declared in an enclosing procedure (closure capture) or a
		// variable we have not seen a declaration for
		wb_unsupported(p, node, "reference to a variable of an enclosing procedure");
		wbAddr a = {};
		return a;
	}
	if (e->kind == Entity_Constant) {
		// `CONST_ARRAY[i].x`: the constant is materialized (in a frame temporary, so that it
		// can be indexed dynamically without touching the data segment)
		wbValue v = wb_const(p, node, e->type, e->Constant.value);
		return wb_value_to_addr(p, wb_value_copy(p, v));
	}
	wb_unsupported(p, node, "addressable entity");
	wbAddr a = {};
	return a;
}

gb_internal wbAddr wb_build_addr_selector(wbProcedure *p, Ast *expr) {
	ast_node(se, SelectorExpr, expr);
	wbAddr invalid = {};
	Ast *sel_node = unparen_expr(se->selector);
	if (sel_node->kind != Ast_Ident) {
		wb_unsupported(p, expr, "selector expression");
		return invalid;
	}
	TypeAndValue tav = type_and_value_of_expr(se->expr);
	if (tav.mode == Addressing_Invalid) {
		// pkg.name
		return wb_build_addr(p, sel_node);
	}
	if (tav.mode == Addressing_Type) {
		wb_unsupported(p, expr, "selector on a type");
		return invalid;
	}
	if (se->swizzle_count > 0) {
		// v.xyz on an array (through a pointer as well)
		wbAddr base = {};
		if (is_type_pointer(tav.type)) {
			base = wb_addr_from_pointer(p, wb_build_expr(p, se->expr), type_deref(tav.type));
		} else if (is_type_soa_pointer(tav.type)) {
			base = wb_addr_soa_variable_from_soa_ptr(p, wb_build_expr(p, se->expr));
		} else {
			base = wb_build_addr(p, se->expr);
		}
		if (base.kind == wbAddr_SoaVariable) {
			// soa[i].xy
			u8 indices[4] = {};
			for (u8 i = 0; i < se->swizzle_count; i++) {
				indices[i] = (se->swizzle_indices >> (i*2)) & 3;
			}
			return wb_soa_swizzle_addr(p, base, type_of_expr(expr), se->swizzle_count, indices);
		}
		if (base.kind == wbAddr_Swizzle) {
			// v.xyzw.yx: compose the two selections
			wbAddr composed = base;
			composed.type = type_of_expr(expr);
			composed.swizzle_count = se->swizzle_count;
			for (u8 i = 0; i < se->swizzle_count; i++) {
				composed.swizzle_indices[i] = base.swizzle_indices[(se->swizzle_indices >> (i*2)) & 3];
			}
			return composed;
		}
		if (base.kind != wbAddr_Memory) {
			if (base.kind != wbAddr_Invalid) {
				wb_unsupported(p, expr, "swizzle of a register value");
			}
			return invalid;
		}
		Type *array_type = base_type(type_deref(tav.type));
		if (array_type->kind != Type_Array) {
			wb_unsupported_type(p, expr, tav.type);
			return invalid;
		}
		wbAddr addr = base;
		addr.kind = wbAddr_Swizzle;
		addr.type = type_of_expr(expr);
		addr.swizzle_count = se->swizzle_count;
		for (u8 i = 0; i < se->swizzle_count; i++) {
			addr.swizzle_indices[i] = (se->swizzle_indices >> (i*2)) & 3;
		}
		return addr;
	}
	Selection sel = lookup_field(tav.type, sel_node->Ident.interned, false);
	if (sel.entity == nullptr || sel.pseudo_field) {
		wb_unsupported(p, expr, "selector kind");
		return invalid;
	}
	if (sel.is_bit_field) {
		// `bf.x`: the last selection index is a field of a bit_field
		Selection sub_sel = sel;
		sub_sel.index.count -= 1;
		wbAddr addr = wb_addr_resolve_map(p, wb_build_addr(p, se->expr));
		if (addr.kind == wbAddr_Invalid) {
			return invalid;
		}
		Type *bf_type = tav.type;
		if (addr.kind == wbAddr_SoaVariable) {
			if (sub_sel.index.count == 0) {
				wb_unsupported(p, expr, "bit_field #soa element");
				return invalid;
			}
			addr = wb_soa_field_addr(p, expr, addr, sub_sel);
			if (addr.kind == wbAddr_Invalid) {
				return invalid;
			}
			bf_type = addr.type;
		} else if (sub_sel.index.count > 0) {
			addr = wb_addr_deep_field(p, expr, addr, tav.type, sub_sel);
			if (addr.kind == wbAddr_Invalid) {
				return invalid;
			}
			bf_type = addr.type;
		}
		if (is_type_pointer(bf_type)) {
			wbValue ptr = wb_addr_load(p, addr);
			bf_type = type_deref(bf_type);
			addr = wb_addr_from_pointer(p, ptr, bf_type);
		}
		if (addr.kind != wbAddr_Memory && addr.kind != wbAddr_Local) {
			wb_unsupported(p, expr, "bit_field access");
			return invalid;
		}
		bf_type = base_type(bf_type);
		GB_ASSERT(bf_type->kind == Type_BitField);
		i32 index = sel.index[sel.index.count-1];
		addr.bit_field_in_local = addr.kind == wbAddr_Local;
		addr.kind       = wbAddr_BitField;
		addr.type       = bf_type->BitField.fields[index]->type;
		addr.bit_size   = bf_type->BitField.bit_sizes[index];
		addr.bit_offset = cast(i32)bf_type->BitField.bit_offsets[index];
		return addr;
	}

	wbAddr addr = {};
	if (is_type_soa_pointer(tav.type)) {
		// p.x with `p` an #soa pointer (auto dereference)
		addr = wb_addr_soa_variable_from_soa_ptr(p, wb_build_expr(p, se->expr));
	} else {
		addr = wb_addr_resolve_map(p, wb_build_addr(p, se->expr));
	}
	if (addr.kind == wbAddr_Invalid) {
		return invalid;
	}
	if (addr.kind == wbAddr_SoaVariable) {
		// soa[i].x: one component of the element
		addr = wb_soa_field_addr(p, expr, addr, sel);
	} else {
		addr = wb_addr_deep_field(p, expr, addr, tav.type, sel);
	}
	if (addr.kind == wbAddr_Invalid) {
		return invalid;
	}
	addr.type = type_of_expr(expr);
	return addr;
}

// Follows a field selection path (auto-dereferencing pointers along the way)
gb_internal wbAddr wb_addr_deep_field(wbProcedure *p, Ast *expr, wbAddr addr, Type *type, Selection const &sel) {
	wbAddr invalid = {};
	for_array(i, sel.index) {
		i32 index = sel.index[i];
		if (is_type_pointer(type)) {
			// auto dereference
			wbValue ptr = wb_addr_load(p, addr);
			type = type_deref(type);
			addr = wb_addr_from_pointer(p, ptr, type);
			if (addr.kind == wbAddr_Invalid) {
				return invalid;
			}
		}
		if (addr.kind != wbAddr_Memory) {
			wb_unsupported(p, expr, "field of a register value (internal error)");
			return invalid;
		}
		Type *bt = base_type(type);
		if (bt->kind == Type_Struct && bt->Struct.is_raw_union) {
			type = bt->Struct.fields[index]->type;
			addr.type = type;
			continue;
		}
		switch (bt->kind) {
		case Type_Struct:
		case Type_Tuple:
		case Type_Array:
		case Type_Basic:
		case Type_Slice:
		case Type_DynamicArray:
			break;
		case Type_Map:
			// `m.allocator`: a map is laid out as a Raw_Map
			bt = base_type(t_raw_map);
			break;
		default:
			wb_unsupported_type(p, expr, type);
			return invalid;
		}
		Type *ft = nullptr;
		i64 offset = type_offset_of(bt, index, &ft);
		if (ft == nullptr && bt->kind == Type_Array) {
			// `v.x` of an array (the implicit x/y/z/w and r/g/b/a fields)
			ft = bt->Array.elem;
		}
		if (ft == nullptr) {
			wb_unsupported(p, expr, "field selection");
			return invalid;
		}
		addr = wb_addr_offset(addr, offset, ft);
		type = ft;
	}
	addr.type = type;
	return addr;
}

// Bounds checking (lb_emit_bounds_check and friends)

gb_internal bool wb_bounds_check_disabled(wbProcedure *p) {
	if (build_context.no_bounds_check) {
		return true;
	}
	return (p->state_flags & StateFlag_no_bounds_check) != 0;
}

gb_internal void wb_set_file_line_col(wbProcedure *p, Array<wbValue> *args, TokenPos pos) {
	String file = get_file_path_string(pos.file_id);
	i32 line    = pos.line;
	i32 col     = pos.column;
	switch (build_context.source_code_location_info) {
	case SourceCodeLocationInfo_Normal:
		break;
	case SourceCodeLocationInfo_Obfuscated:
		file = obfuscate_string(file, "F");
		line = obfuscate_i32(line);
		col  = obfuscate_i32(col);
		break;
	case SourceCodeLocationInfo_Filename:
		file = last_path_element(file);
		break;
	case SourceCodeLocationInfo_None:
		file = str_lit("");
		line = 0;
		col  = 0;
		break;
	}
	array_add(args, wb_const(p, nullptr, t_string, exact_value_string(file)));
	array_add(args, wb_value_const_int(t_i32, line));
	array_add(args, wb_value_const_int(t_i32, col));
}

// `index` must be in 0..<len; the runtime call is guarded by an inline
// unsigned comparison so that the common case is a couple of instructions
gb_internal void wb_emit_bounds_check(wbProcedure *p, Token token, wbValue index, wbValue len) {
	if (wb_bounds_check_disabled(p)) {
		return;
	}
	if (index.kind == wbValue_Invalid || len.kind == wbValue_Invalid) {
		return;
	}
	index = wb_emit_conv(p, index, t_int);
	len   = wb_emit_conv(p, len, t_int);
	if (index.kind == wbValue_Const && len.kind == wbValue_Const) {
		if (0 <= index.i && index.i < len.i) {
			return;
		}
	}
	index = wb_value_to_local(p, index);
	if (len.kind != wbValue_Const) {
		len = wb_value_to_local(p, len);
	}
	wb_push(p, index);
	wb_push(p, len);
	wb_op(p, wbOp_i32_ge_u);
	wb_open_if(p);
	auto args = array_make<wbValue>(temporary_allocator(), 0, 5);
	wb_set_file_line_col(p, &args, token.pos);
	array_add(&args, index);
	array_add(&args, len);
	wb_emit_runtime_call(p, "bounds_check_error", args);
	wb_close(p);
}

gb_internal void wb_emit_slice_bounds_check(wbProcedure *p, Token token, wbValue low, wbValue high, wbValue len, bool lower_value_used) {
	if (wb_bounds_check_disabled(p)) {
		return;
	}
	if (low.kind == wbValue_Invalid || high.kind == wbValue_Invalid || len.kind == wbValue_Invalid) {
		return;
	}
	low  = wb_emit_conv(p, low, t_int);
	high = wb_emit_conv(p, high, t_int);
	len  = wb_emit_conv(p, len, t_int);
	if (!lower_value_used) {
		if (high.kind == wbValue_Const && len.kind == wbValue_Const && 0 <= high.i && high.i <= len.i) {
			return;
		}
		auto args = array_make<wbValue>(temporary_allocator(), 0, 5);
		wb_set_file_line_col(p, &args, token.pos);
		array_add(&args, high);
		array_add(&args, len);
		wb_emit_runtime_call(p, "slice_expr_error_hi", args);
	} else {
		if (low.kind == wbValue_Const && high.kind == wbValue_Const && len.kind == wbValue_Const &&
		    0 <= low.i && low.i <= high.i && high.i <= len.i) {
			return;
		}
		auto args = array_make<wbValue>(temporary_allocator(), 0, 6);
		wb_set_file_line_col(p, &args, token.pos);
		array_add(&args, low);
		array_add(&args, high);
		array_add(&args, len);
		wb_emit_runtime_call(p, "slice_expr_error_lo_hi", args);
	}
}

gb_internal void wb_emit_multi_pointer_slice_bounds_check(wbProcedure *p, Token token, wbValue low, wbValue high) {
	if (wb_bounds_check_disabled(p)) {
		return;
	}
	if (low.kind == wbValue_Invalid || high.kind == wbValue_Invalid) {
		return;
	}
	low  = wb_emit_conv(p, low, t_int);
	high = wb_emit_conv(p, high, t_int);
	if (low.kind == wbValue_Const && high.kind == wbValue_Const && low.i < high.i) {
		return;
	}
	auto args = array_make<wbValue>(temporary_allocator(), 0, 5);
	wb_set_file_line_col(p, &args, token.pos);
	array_add(&args, low);
	array_add(&args, high);
	wb_emit_runtime_call(p, "multi_pointer_slice_expr_error", args);
}

// m[row, column]
gb_internal wbAddr wb_build_addr_matrix_index(wbProcedure *p, Ast *expr) {
	ast_node(ie, MatrixIndexExpr, expr);
	wbAddr invalid = {};
	Type *base_t = type_of_expr(ie->expr);
	Type *bt = base_type(base_t);
	Type *elem_type = type_of_expr(expr);

	wbAddr base = {};
	if (bt->kind == Type_Pointer) {
		wbValue ptr = wb_build_expr(p, ie->expr);
		bt = base_type(type_deref(bt));
		base = wb_addr_from_pointer(p, ptr, type_deref(base_t));
	} else {
		base = wb_addr_resolve_map(p, wb_build_addr(p, ie->expr));
	}
	if (base.kind == wbAddr_Invalid) {
		return invalid;
	}
	GB_ASSERT(bt->kind == Type_Matrix);
	wbValue row = wb_emit_conv(p, wb_build_expr(p, ie->row_index), t_int);
	wbValue col = wb_emit_conv(p, wb_build_expr(p, ie->column_index), t_int);
	if (row.kind == wbValue_Invalid || col.kind == wbValue_Invalid) {
		return invalid;
	}
	i64 elem_size = type_size_of(elem_type);
	if (row.kind == wbValue_Const && col.kind == wbValue_Const &&
	    0 <= row.i && row.i < bt->Matrix.row_count && 0 <= col.i && col.i < bt->Matrix.column_count) {
		return wb_addr_offset(base, matrix_indices_to_offset(bt, row.i, col.i)*elem_size, elem_type);
	}
	row = wb_value_to_local(p, row);
	col = wb_value_to_local(p, col);
	if (!wb_bounds_check_disabled(p)) {
		// if row >= row_count || col >= column_count { matrix_bounds_check_error(...) }
		wb_push(p, row);
		wb_i32_const(p, cast(i32)bt->Matrix.row_count);
		wb_op(p, wbOp_i32_ge_u);
		wb_push(p, col);
		wb_i32_const(p, cast(i32)bt->Matrix.column_count);
		wb_op(p, wbOp_i32_ge_u);
		wb_op(p, wbOp_i32_or);
		wb_open_if(p);
		auto args = array_make<wbValue>(temporary_allocator(), 0, 7);
		wb_set_file_line_col(p, &args, ast_token(ie->row_index).pos);
		array_add(&args, row);
		array_add(&args, col);
		array_add(&args, wb_value_const_int(t_int, bt->Matrix.row_count));
		array_add(&args, wb_value_const_int(t_int, bt->Matrix.column_count));
		wb_emit_runtime_call(p, "matrix_bounds_check_error", args);
		wb_close(p);
	}
	// element index = minor + stride*major
	i64 stride = matrix_type_stride_in_elems(bt);
	wbValue minor = bt->Matrix.is_row_major ? col : row;
	wbValue major = bt->Matrix.is_row_major ? row : col;
	wb_push(p, major);
	wb_i32_const(p, cast(i32)stride);
	wb_op(p, wbOp_i32_mul);
	wb_push(p, minor);
	wb_op(p, wbOp_i32_add);
	wbValue index = wb_pop_to_local(p, wbValType_i32, t_int);
	return wb_emit_elem_addr(p, base.index, base.offset, index, elem_type);
}

gb_internal wbAddr wb_build_addr_index(wbProcedure *p, Ast *expr) {
	ast_node(ie, IndexExpr, expr);
	wbAddr invalid = {};
	Type *base_t = type_of_expr(ie->expr);
	Type *bt = base_type(base_t);
	Type *elem_type = type_of_expr(expr);

	// ^[N]T and ^[]T etc. automatically dereference
	wbAddr base = {};
	if (bt->kind == Type_Pointer) {
		wbValue ptr = wb_build_expr(p, ie->expr);
		bt = base_type(type_deref(bt));
		base = wb_addr_from_pointer(p, ptr, type_deref(base_t));
	} else if (bt->kind == Type_MultiPointer) {
		wbValue ptr = wb_build_expr(p, ie->expr);
		base = wb_addr_from_pointer(p, ptr, bt->MultiPointer.elem);
	} else if (bt->kind == Type_SoaPointer) {
		// p[j] with `p` an #soa pointer to an array element
		base = wb_addr_soa_variable_from_soa_ptr(p, wb_build_expr(p, ie->expr));
		if (base.kind == wbAddr_Invalid) {
			return invalid;
		}
		bt = base_type(base.type);
	} else {
		base = wb_addr_resolve_map(p, wb_build_addr(p, ie->expr));
		if (base.kind == wbAddr_Swizzle) {
			// v.xyz[i]: index a copy of the swizzled elements
			wbValue v = wb_addr_load(p, base);
			base = wb_addr_memory(v.index, v.offset, v.type);
		}
	}
	if (base.kind == wbAddr_Invalid) {
		return invalid;
	}

	wbValue index = wb_build_expr(p, ie->index);
	if (index.kind == wbValue_Invalid) {
		return invalid;
	}

	if (is_type_soa_struct(bt)) {
		// soa[i]
		if (base.kind != wbAddr_Memory) {
			wb_unsupported(p, expr, "#soa container access");
			return invalid;
		}
		return wb_addr_soa_variable(base, index, ie->index);
	}
	if (base.kind == wbAddr_SoaVariable) {
		// soa[i][j] (or v[j] with `v` ranging over an #soa container): one component of an array element
		return wb_soa_elem_index_addr(p, base, index, ie->index);
	}

	switch (bt->kind) {
	case Type_Map:
		return wb_map_elem_addr(p, base, base.type, index, elem_type);
	case Type_Array:
		wb_emit_bounds_check(p, ast_token(ie->index), index, wb_value_const_int(t_int, bt->Array.count));
		return wb_emit_elem_addr(p, base.index, base.offset, index, elem_type);
	case Type_EnumeratedArray: {
		// index - min_value
		wbValue min = wb_const(p, expr, bt->EnumeratedArray.index, *bt->EnumeratedArray.min_value);
		index = wb_emit_conv(p, index, t_int);
		if (min.kind == wbValue_Const && min.i != 0) {
			if (index.kind == wbValue_Const) {
				index.i -= min.i;
			} else {
				wb_push(p, index);
				wb_i32_const(p, cast(i32)min.i);
				wb_op(p, wbOp_i32_sub);
				index = wb_pop_to_local(p, wbValType_i32, t_int);
			}
		}
		wb_emit_bounds_check(p, ast_token(ie->index), index, wb_value_const_int(t_int, bt->EnumeratedArray.count));
		return wb_emit_elem_addr(p, base.index, base.offset, index, elem_type);
	}
	case Type_MultiPointer:
		if (ie->expr->tav.mode == Addressing_SoaVariable && !wb_bounds_check_disabled(p)) {
			// soa.x[i]: checked against the container's length
			Ast *se_expr = unparen_expr(ie->expr);
			if (se_expr->kind == Ast_SelectorExpr && !wb_expr_has_call(se_expr->SelectorExpr.expr)) {
				wbAddr soa = wb_soa_container_of_expr(p, se_expr->SelectorExpr.expr);
				if (soa.kind == wbAddr_Memory) {
					wb_emit_bounds_check(p, ast_token(ie->index), index, wb_soa_len(p, soa));
				}
			}
		}
		return wb_emit_elem_addr(p, base.index, base.offset, index, elem_type);
	case Type_Matrix: {
		// m[i]: the i-th stored column (or row for row-major matrices), which is padding-free
		i64 count = bt->Matrix.is_row_major ? bt->Matrix.row_count : bt->Matrix.column_count;
		GB_ASSERT(type_size_of(elem_type) == matrix_type_stride_in_bytes(bt, nullptr));
		wb_emit_bounds_check(p, ast_token(ie->index), index, wb_value_const_int(t_int, count));
		return wb_emit_elem_addr(p, base.index, base.offset, index, elem_type);
	}
	case Type_FixedCapacityDynamicArray: {
		// the elements are in place, checked against the current length
		wbValue len = wb_emit_load(p, base.index, base.offset + cast(i32)type_offset_of(bt, 1, nullptr), t_int);
		wb_emit_bounds_check(p, ast_token(ie->index), index, len);
		return wb_emit_elem_addr(p, base.index, base.offset, index, elem_type);
	}
	case Type_Slice:
	case Type_DynamicArray:
	case Type_Basic: {
		if (bt->kind == Type_Basic && !is_type_string(bt)) {
			break;
		}
		wbValue s = wb_value_memory(base.index, base.offset, base.type);
		wbValue data = wb_emit_slice_data(p, s);
		if (data.kind == wbValue_Invalid) {
			return invalid;
		}
		wb_emit_bounds_check(p, ast_token(ie->index), index, wb_emit_slice_len(p, s));
		return wb_emit_elem_addr(p, data.index, 0, index, elem_type);
	}
	default:
		break;
	}
	wb_unsupported_type(p, expr, base_t);
	return invalid;
}

gb_internal wbAddr wb_build_addr_internal(wbProcedure *p, Ast *expr);

gb_internal wbAddr wb_build_addr(wbProcedure *p, Ast *expr) {
	expr = unparen_expr(expr);
	if (expr->state_flags & StateFlag_SelectorCallExpr) {
		// see wb_build_expr
		wbAddr *pa = map_get(&p->selector_addrs, expr);
		if (pa != nullptr) {
			wbAddr res = *pa;
			map_remove(&p->selector_addrs, expr);
			return res;
		}
	}
	wbAddr addr = wb_build_addr_internal(p, expr);
	if (expr->state_flags & StateFlag_SelectorCallExpr) {
		map_set(&p->selector_addrs, expr, addr);
	}
	return addr;
}

gb_internal wbAddr wb_build_addr_internal(wbProcedure *p, Ast *expr) {
	wbAddr invalid = {};

	switch (expr->kind) {
	case_ast_node(i, Ident, expr);
		if (is_blank_ident(expr)) {
			return invalid;
		}
		Entity *e = entity_of_node(expr);
		if (e == nullptr) {
			wb_unsupported(p, expr, "identifier");
			return invalid;
		}
		return wb_addr_of_entity(p, e, expr);
	case_end;

	case_ast_node(im, Implicit, expr);
		if (im->kind == Token_context) {
			return wb_context_addr(p);
		}
		wb_unsupported(p, expr, "implicit value");
		return invalid;
	case_end;

	case_ast_node(se, SelectorExpr, expr);
		return wb_build_addr_selector(p, expr);
	case_end;

	case_ast_node(ie, IndexExpr, expr);
		return wb_build_addr_index(p, expr);
	case_end;

	case_ast_node(ie, MatrixIndexExpr, expr);
		return wb_build_addr_matrix_index(p, expr);
	case_end;

	case_ast_node(de, DerefExpr, expr);
		wbValue ptr = wb_build_expr(p, de->expr);
		Type *type = type_of_expr(expr);
		if (is_type_soa_pointer(type_of_expr(de->expr))) {
			return wb_addr_soa_variable_from_soa_ptr(p, ptr);
		}
		return wb_addr_from_pointer(p, ptr, type);
	case_end;

	case_ast_node(cl, CompoundLit, expr);
		Type *type = type_of_expr(expr);
		TypeAndValue tav = type_and_value_of_expr(expr);
		wbAddr tmp = wb_add_temp(p, type);
		if (tav.value.kind != ExactValue_Invalid) {
			wb_addr_store(p, tmp, wb_const(p, expr, type, tav.value));
		} else {
			wb_build_compound_lit(p, expr, tmp);
		}
		return tmp;
	case_end;
	}

	// Anything else: evaluate into a temporary
	wbValue v = wb_build_expr(p, expr);
	if (v.kind == wbValue_Invalid) {
		return invalid;
	}
	if (v.kind == wbValue_Memory) {
		return wb_addr_memory(v.index, v.offset, v.type);
	}
	return wb_value_to_addr(p, v);
}

// Compound literals

gb_internal void wb_build_compound_lit_array_elems(wbProcedure *p, Ast *expr, Slice<Ast *> const &elems, wbAddr dst, Type *elem_type, i64 min_value = 0, Type *matrix_type = nullptr) {
	i64 elem_size = type_size_of(elem_type);
	i64 index = 0;
	// matrix literals list their elements in row-major order
	auto elem_addr = [&](i64 k) -> wbAddr {
		if (is_type_soa_struct(dst.type)) {
			// #soa[N]T{...}: the elements are scattered into the per-field arrays
			return wb_addr_soa_variable(dst, wb_value_const_int(t_int, k), nullptr);
		}
		if (matrix_type != nullptr) {
			k = matrix_row_major_index_to_offset(matrix_type, k);
		}
		return wb_addr_offset(dst, k*elem_size, elem_type);
	};
	for (Ast *elem : elems) {
		if (elem->kind == Ast_FieldValue) {
			ast_node(fv, FieldValue, elem);
			if (is_ast_range(fv->field)) {
				ast_node(ie, BinaryExpr, fv->field);
				i64 lo = exact_value_to_i64(ie->left->tav.value) - min_value;
				i64 hi = exact_value_to_i64(ie->right->tav.value) - min_value;
				if (ie->op.kind != Token_RangeHalf) {
					hi += 1;
				}
				wbValue v = wb_emit_conv(p, wb_build_expr(p, fv->value), elem_type);
				v = wb_value_fresh(p, v);
				for (i64 k = lo; k < hi; k++) {
					wb_addr_store(p, elem_addr(k), v);
				}
				index = hi;
			} else {
				index = exact_value_to_i64(fv->field->tav.value) - min_value;
				wbValue v = wb_emit_conv(p, wb_build_expr(p, fv->value), elem_type);
				wb_addr_store(p, elem_addr(index), v);
				index++;
			}
		} else {
			wbValue v = wb_emit_conv(p, wb_build_expr(p, elem), elem_type);
			wb_addr_store(p, elem_addr(index), v);
			index++;
		}
	}
}

// The number of elements an array-like literal initializes (its highest index + 1)
gb_internal i64 wb_compound_lit_elem_count(Slice<Ast *> const &elems, i64 min_value) {
	i64 index = 0;
	i64 max_index = 0;
	for (Ast *elem : elems) {
		if (elem->kind == Ast_FieldValue) {
			ast_node(fv, FieldValue, elem);
			if (is_ast_range(fv->field)) {
				ast_node(ie, BinaryExpr, fv->field);
				i64 hi = exact_value_to_i64(ie->right->tav.value) - min_value;
				if (ie->op.kind != Token_RangeHalf) {
					hi += 1;
				}
				index = hi;
			} else {
				index = exact_value_to_i64(fv->field->tav.value) - min_value + 1;
			}
		} else {
			index++;
		}
		max_index = gb_max(max_index, index);
	}
	return max_index;
}

// Populates `dst` (of the literal's type) from a non-constant compound literal
gb_internal void wb_build_compound_lit(wbProcedure *p, Ast *expr, wbAddr dst) {
	ast_node(cl, CompoundLit, expr);
	Type *type = type_of_expr(expr);
	Type *bt = base_type(type);
	if (dst.kind != wbAddr_Memory) {
		wb_unsupported(p, expr, "compound literal destination");
		return;
	}

	wb_addr_zero(p, dst);
	if (cl->elems.count == 0) {
		return;
	}

	if (is_type_any(bt)) {
		// any{data, id}
		for_array(i, cl->elems) {
			Ast *elem = cl->elems[i];
			Ast *value_expr = elem;
			i64 index = i;
			if (elem->kind == Ast_FieldValue) {
				ast_node(fv, FieldValue, elem);
				index = fv->field->Ident.interned.string() == str_lit("data") ? 0 : 1;
				value_expr = fv->value;
			}
			wbAddr field_addr = wb_addr_field(dst, index);
			wb_addr_store(p, field_addr, wb_emit_conv(p, wb_build_expr(p, value_expr), field_addr.type));
		}
		return;
	}

	switch (bt->kind) {
	case Type_Struct: {
		if (bt->Struct.is_raw_union) {
			wb_unsupported(p, expr, "raw union literal");
			return;
		}
		if (is_type_soa_struct(bt)) {
			wb_build_compound_lit_array_elems(p, expr, cl->elems, dst, bt->Struct.soa_elem);
			return;
		}
		for_array(i, cl->elems) {
			Ast *elem = cl->elems[i];
			wbAddr field_addr = dst;
			Ast *value_expr = elem;
			if (elem->kind == Ast_FieldValue) {
				ast_node(fv, FieldValue, elem);
				Selection sel = lookup_field(bt, fv->field->Ident.interned, false);
				GB_ASSERT(sel.entity != nullptr);
				Type *ct = bt;
				for (i32 index : sel.index) {
					Type *ft = nullptr;
					i64 offset = type_offset_of(ct, index, &ft);
					field_addr = wb_addr_offset(field_addr, offset, ft);
					ct = base_type(ft);
				}
				value_expr = fv->value;
			} else {
				field_addr = wb_addr_field(dst, cast(i64)i);
			}
			wbValue v = wb_emit_conv(p, wb_build_expr(p, value_expr), field_addr.type);
			wb_addr_store(p, field_addr, v);
		}
		return;
	}
	case Type_BitField: {
		// Fields are named or positional, stored through bit field addresses
		for_array(i, cl->elems) {
			Ast *elem = cl->elems[i];
			Ast *value_expr = elem;
			i32 index = cast(i32)i;
			if (elem->kind == Ast_FieldValue) {
				ast_node(fv, FieldValue, elem);
				Selection sel = lookup_field(bt, fv->field->Ident.interned, false);
				GB_ASSERT(sel.entity != nullptr && sel.index.count == 1);
				index = sel.index[0];
				value_expr = fv->value;
			}
			wbAddr field_addr = dst;
			field_addr.kind       = wbAddr_BitField;
			field_addr.type       = bt->BitField.fields[index]->type;
			field_addr.bit_size   = bt->BitField.bit_sizes[index];
			field_addr.bit_offset = cast(i32)bt->BitField.bit_offsets[index];
			wb_addr_store(p, field_addr, wb_emit_conv(p, wb_build_expr(p, value_expr), field_addr.type));
		}
		return;
	}
	case Type_Array:
		wb_build_compound_lit_array_elems(p, expr, cl->elems, dst, bt->Array.elem);
		return;
	case Type_SimdVector:
		// laid out like `[N]T` (see wasm_backend_simd.cpp)
		wb_build_compound_lit_array_elems(p, expr, cl->elems, dst, bt->SimdVector.elem);
		return;
	case Type_FixedCapacityDynamicArray:
		// [dynamic; N]T: {data: [N]T, len: int}
		wb_build_compound_lit_array_elems(p, expr, cl->elems, dst, bt->FixedCapacityDynamicArray.elem);
		wb_addr_store(p, wb_addr_offset(dst, type_offset_of(bt, 1, nullptr), t_int), wb_value_const_int(t_int, wb_compound_lit_elem_count(cl->elems)));
		return;
	case Type_EnumeratedArray:
		wb_build_compound_lit_array_elems(p, expr, cl->elems, dst, bt->EnumeratedArray.elem, exact_value_to_i64(*bt->EnumeratedArray.min_value));
		return;
	case Type_Matrix:
		wb_build_compound_lit_array_elems(p, expr, cl->elems, dst, bt->Matrix.elem, 0, bt);
		return;
	case Type_Slice: {
		// Backing array on the stack
		Type *et = bt->Slice.elem;
		i64 count = gb_max(cl->max_count, cast(i64)cl->elems.count);
		Type *array_type = alloc_type_array(et, count);
		wbAddr backing = wb_add_temp_or_static(p, array_type);
		wb_addr_zero(p, backing);
		wb_build_compound_lit_array_elems(p, expr, cl->elems, backing, et);
		wbValue data = wb_addr_get_ptr(p, backing, alloc_type_multi_pointer(et));
		wb_emit_store(p, dst.index, dst.offset, data, t_rawptr);
		wb_emit_store(p, dst.index, dst.offset + cast(i32)build_context.int_size, wb_value_const_int(t_int, count), t_int);
		return;
	}
	case Type_DynamicArray: {
		// reserve, fill a backing array on the stack, then append it in one go
		String proc_name = p->entity != nullptr ? p->entity->token.string : p->name;
		TokenPos pos = ast_token(expr).pos;
		Type *et = bt->DynamicArray.elem;
		wbValue size  = wb_value_const_int(t_int, type_size_of(et));
		wbValue align = wb_value_const_int(t_int, type_align_of(et));
		i64 count = gb_max(cl->max_count, cast(i64)cl->elems.count);
		{
			auto args = array_make<wbValue>(temporary_allocator(), 5);
			args[0] = wb_addr_get_ptr(p, dst, t_rawptr);
			args[1] = size;
			args[2] = align;
			args[3] = wb_value_const_int(t_int, count);
			args[4] = wb_source_code_location(p, proc_name, pos);
			wb_emit_runtime_call(p, "__dynamic_array_reserve", args);
		}

		wbAddr items = wb_add_temp(p, alloc_type_array(et, count));
		wb_addr_zero(p, items);
		wb_build_compound_lit_array_elems(p, expr, cl->elems, items, et);

		{
			auto args = array_make<wbValue>(temporary_allocator(), 6);
			args[0] = wb_addr_get_ptr(p, dst, t_rawptr);
			args[1] = size;
			args[2] = align;
			args[3] = wb_addr_get_ptr(p, items, t_rawptr);
			args[4] = wb_value_const_int(t_int, count);
			args[5] = wb_source_code_location(p, proc_name, pos);
			wb_emit_runtime_call(p, "__dynamic_array_append", args);
		}
		return;
	}
	case Type_Map: {
		// reserve, then insert each element
		String proc_name = p->entity != nullptr ? p->entity->token.string : p->name;
		auto args = array_make<wbValue>(temporary_allocator(), 4);
		args[0] = wb_addr_get_ptr(p, dst, t_raw_map_ptr);
		args[1] = wb_value_const_int(t_map_info_ptr, wb_map_info_addr(p->module, bt));
		args[2] = wb_value_const_int(t_uint, cl->elems.count);
		args[3] = wb_source_code_location(p, proc_name, ast_token(expr).pos);
		wb_emit_runtime_call(p, "__dynamic_map_reserve", args);
		for (Ast *elem : cl->elems) {
			if (elem->kind != Ast_FieldValue) {
				wb_unsupported(p, elem, "map literal element");
				return;
			}
			ast_node(fv, FieldValue, elem);
			wbValue key = wb_build_expr(p, fv->field);
			wbAddr elem_addr = wb_map_elem_addr(p, dst, type, key, bt->Map.value);
			if (elem_addr.kind == wbAddr_Invalid) {
				return;
			}
			wb_addr_store(p, elem_addr, wb_build_expr(p, fv->value));
		}
		return;
	}
	case Type_BitSet: {
		// res |= 1 << (elem - lower) for each (runtime) element
		Type *it = bit_set_to_int(bt);
		Type *set_type = it;
		it = integer_endian_type_to_platform_type(it);
		wbValType vt = wb_valtype_of(it);
		if (vt != wbValType_i32 && vt != wbValType_i64) {
			wb_unsupported_type(p, expr, type);
			return;
		}
		u32 res = wb_add_local(p, vt);
		if (vt == wbValType_i64) wb_i64_const(p, 0); else wb_i32_const(p, 0);
		wb_local_set(p, res);
		for (Ast *elem : cl->elems) {
			GB_ASSERT(elem->kind != Ast_FieldValue);
			wbValue e = wb_emit_conv(p, wb_build_expr(p, elem), it);
			if (e.kind == wbValue_Invalid) {
				return;
			}
			if (vt == wbValType_i64) {
				wb_i64_const(p, 1);
				wb_push(p, e);
				if (bt->BitSet.lower != 0) {
					wb_i64_const(p, bt->BitSet.lower);
					wb_op(p, wbOp_i64_sub);
				}
				wb_op(p, wbOp_i64_shl);
				wb_local_get(p, res);
				wb_op(p, wbOp_i64_or);
			} else {
				wb_i32_const(p, 1);
				wb_push(p, e);
				if (bt->BitSet.lower != 0) {
					wb_i32_const(p, cast(i32)bt->BitSet.lower);
					wb_op(p, wbOp_i32_sub);
				}
				wb_op(p, wbOp_i32_shl);
				wb_local_get(p, res);
				wb_op(p, wbOp_i32_or);
			}
			wb_local_set(p, res);
		}
		wb_emit_store(p, dst.index, dst.offset, wb_emit_from_platform_endian(p, wb_value_local(res, vt, it), set_type), set_type);
		return;
	}
	default:
		break;
	}
	wb_unsupported(p, expr, "compound literal type");
}

// Unary and binary expressions

gb_internal wbValue wb_build_unary_expr(wbProcedure *p, Ast *expr) {
	ast_node(ue, UnaryExpr, expr);
	Type *type = expr->tav.type;
	if (is_type_untyped(type)) {
		type = default_type(type);
	}

	if (ue->op.kind == Token_And) {
		// &x
		Ast *inner = unparen_expr(ue->expr);
		wbAddr addr = {};
		if (inner->kind == Ast_TypeAssertion) {
			return wb_build_unary_and_type_assertion(p, expr, inner);
		}
		if (inner->kind == Ast_CompoundLit && wb_is_startup_proc(p)) {
			// `x := &T{...}` at file scope: the literal must outlive the
			// initializer, so it gets static storage (lb_add_global_generated_from_procedure)
			Type *lit_type = type_of_expr(inner);
			addr = wb_add_temp_or_static(p, lit_type);
			TypeAndValue tav = type_and_value_of_expr(inner);
			if (tav.value.kind != ExactValue_Invalid) {
				wb_addr_store(p, addr, wb_const(p, inner, lit_type, tav.value));
			} else {
				wb_build_compound_lit(p, inner, addr);
			}
		} else {
			addr = wb_build_addr(p, inner);
		}
		if (addr.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		return wb_addr_get_ptr(p, addr, type);
	}

	wbValType vt = wb_valtype_of(type);
	if (wb_is_int128(type)) {
		// -x == 0 - x, ~x == x ~ -1
		wbValue x = wb_emit_conv(p, wb_build_expr(p, ue->expr), type);
		ExactValue zero = exact_value_i64(0);
		ExactValue all  = exact_value_i64(-1);
		if (is_type_bit_set(type)) {
			// complement within the set's valid element range
			all = exact_bit_set_all_set_mask(type);
		}
		switch (ue->op.kind) {
		case Token_Add: return x;
		case Token_Sub: return wb_emit_arith128(p, expr, Token_Sub, wb_const(p, expr, type, zero), x, type, type);
		case Token_Xor: return wb_emit_arith128(p, expr, Token_Xor, x, wb_const(p, expr, type, all), type, type);
		default: break;
		}
		wb_unsupported(p, expr, "128-bit unary operator");
		return wb_value_invalid();
	}
	if ((is_type_complex(type) || is_type_quaternion(type)) && ue->op.kind != Token_Add) {
		// negate every component
		wbValue x = wb_emit_conv(p, wb_build_expr(p, ue->expr), type);
		if (x.kind != wbValue_Memory) {
			return wb_value_invalid();
		}
		Type *ft = base_complex_elem_type(type);
		i64 fs = type_size_of(ft);
		i64 n = is_type_quaternion(type) ? 4 : 2;
		wbAddr res = wb_add_temp(p, type);
		for (i64 i = 0; i < n; i++) {
			wbValue c = wb_emit_conv(p, wb_emit_load(p, x.index, x.offset + cast(i32)(i*fs), ft), t_f64);
			c = wb_emit_arith(p, expr, Token_Sub, wb_const(p, expr, t_f64, exact_value_float(0)), c, t_f64, t_f64);
			wb_addr_store(p, wb_addr_memory(res.index, res.offset + cast(i32)(i*fs), ft), c);
		}
		return wb_value_memory(res.index, res.offset, type);
	}
	if (is_type_array_like(type)) {
		// element-wise: -x == 0 - x, ~x == x ~ -1
		wbValue x = wb_emit_conv(p, wb_build_expr(p, ue->expr), type);
		Type *elem = base_array_type(type);
		switch (ue->op.kind) {
		case Token_Add: return x;
		case Token_Sub: return wb_emit_arith(p, expr, Token_Sub, wb_const(p, expr, elem, exact_value_i64(0)), x, type, type);
		case Token_Xor: return wb_emit_arith(p, expr, Token_Xor, x, wb_const(p, expr, elem, exact_value_i64(-1)), type, type);
		default: break;
		}
		wb_unsupported(p, expr, "array unary operator");
		return wb_value_invalid();
	}
	if (vt == wbValType_Invalid) {
		wb_unsupported_type(p, expr, type);
		return wb_value_invalid();
	}
	wbValue x = wb_build_expr(p, ue->expr);
	x = wb_emit_conv(p, x, type);
	if (x.kind == wbValue_Invalid) {
		return x;
	}
	if (ue->op.kind == Token_Sub && is_type_different_to_arch_endianness(type)) {
		// negate as the platform type
		return wb_emit_arith(p, expr, Token_Sub, wb_const(p, expr, type, exact_value_i64(0)), x, type, type);
	}

	switch (ue->op.kind) {
	case Token_Add:
		return x;
	case Token_Sub:
		if (wb_is_f16(type)) {
			wb_push(p, x); wb_i32_const(p, 0x8000); wb_op(p, wbOp_i32_xor);
			return wb_pop_to_local(p, vt, type);
		}
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
		if (is_type_bit_set(type)) {
			// complement within the set's valid element range
			ExactValue mask = exact_bit_set_all_set_mask(type);
			GB_ASSERT(mask.kind == ExactValue_Integer);
			if (vt == wbValType_i64) {
				wb_i64_const(p, cast(i64)wb_big_int_bits(&mask.value_integer)); wb_op(p, wbOp_i64_xor);
			} else {
				wb_i32_const(p, cast(i32)wb_big_int_bits(&mask.value_integer)); wb_op(p, wbOp_i32_xor);
				wb_emit_normalize(p, type);
			}
		} else if (vt == wbValType_i64) {
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
// 128-bit integers
//
// They live in memory as two little endian 64-bit words. The simple operations
// are done inline on the words, multiplication and division call the runtime's
// compiler-rt style helpers.

gb_internal bool wb_is_int128(Type *t) {
	t = core_type(t);
	if (is_type_bit_set(t)) {
		// wide bit_sets share the two-word representation of 128-bit integers
		return type_size_of(t) == 16;
	}
	return is_type_integer(t) && type_size_of(t) == 16;
}

// Loads both words of a 128-bit value into i64 locals
gb_internal void wb_load_int128(wbProcedure *p, wbValue v, u32 *lo, u32 *hi) {
	wbAddr a = wb_value_to_addr(p, v);
	*lo = wb_emit_load(p, a.index, a.offset,     t_u64).index;
	*hi = wb_emit_load(p, a.index, a.offset + 8, t_u64).index;
}

// x << k for k in [0, 64] with Odin semantics (k >= 64 gives 0); x and k on the stack
gb_internal void wb_emit_shl64_sat(wbProcedure *p, u32 x, u32 k) {
	wb_local_get(p, x); wb_local_get(p, k); wb_op(p, wbOp_i64_shl);
	wb_i64_const(p, 0);
	wb_local_get(p, k); wb_i64_const(p, 64); wb_op(p, wbOp_i64_lt_u);
	wb_op(p, wbOp_select);
}
gb_internal void wb_emit_shr64u_sat(wbProcedure *p, u32 x, u32 k) {
	wb_local_get(p, x); wb_local_get(p, k); wb_op(p, wbOp_i64_shr_u);
	wb_i64_const(p, 0);
	wb_local_get(p, k); wb_i64_const(p, 64); wb_op(p, wbOp_i64_lt_u);
	wb_op(p, wbOp_select);
}
gb_internal void wb_emit_shr64s_sat(wbProcedure *p, u32 x, u32 k) {
	wb_local_get(p, x); wb_local_get(p, k); wb_op(p, wbOp_i64_shr_s);
	wb_local_get(p, x); wb_i64_const(p, 63); wb_op(p, wbOp_i64_shr_s);
	wb_local_get(p, k); wb_i64_const(p, 64); wb_op(p, wbOp_i64_lt_u);
	wb_op(p, wbOp_select);
}

gb_internal wbValue wb_emit_arith128(wbProcedure *p, Ast *node, TokenKind op, wbValue left, wbValue right, Type *operand_type, Type *result_type) {
	bool is_signed = wb_type_is_signed(operand_type);

	// Multiplication and division use the runtime helpers
	char const *helper = nullptr;
	bool helper_has_rem_ptr = false;
	switch (op) {
	case Token_Mul: helper = "__multi3"; break;
	case Token_Quo: helper = is_signed ? "divmodti4" : "udivmodti4"; helper_has_rem_ptr = true; break;
	case Token_Mod: helper = is_signed ? "modti3" : "umodti3"; break;
	case Token_ModMod:
		if (!is_signed) {
			helper = "umodti3";
		}
		break;
	default:
		break;
	}
	if (helper != nullptr) {
		Type *ht = is_signed ? t_i128 : t_u128;
		if (op == Token_Mul) {
			ht = t_i128;
		}
		wbValue l = left;  l.type = ht;
		wbValue r = right; r.type = ht;
		auto args = array_make<wbValue>(temporary_allocator(), 0, 3);
		array_add(&args, l);
		array_add(&args, r);
		if (helper_has_rem_ptr) {
			array_add(&args, wb_value_const_int(t_rawptr, 0));
		}
		wbValue res = wb_emit_runtime_call(p, helper, args);
		res.type = result_type;
		return res;
	}
	if (op == Token_ModMod) {
		// ((a % b) + b) % b
		wbValue r = wb_emit_arith128(p, node, Token_Mod, left, right, operand_type, operand_type);
		r = wb_emit_arith128(p, node, Token_Add, r, right, operand_type, operand_type);
		return wb_emit_arith128(p, node, Token_Mod, r, right, operand_type, result_type);
	}

	u32 alo, ahi;
	wb_load_int128(p, left, &alo, &ahi);

	if (op == Token_Shl || op == Token_Shr) {
		// amount clamped to [0, 128]
		wbValue amount = wb_emit_conv(p, right, t_u64);
		if (amount.kind == wbValue_Invalid) {
			return wb_value_invalid();
		}
		u32 n = wb_add_local(p, wbValType_i64);
		wb_push(p, amount);
		wb_i64_const(p, 128);
		wb_push(p, amount); wb_i64_const(p, 128); wb_op(p, wbOp_i64_lt_u);
		wb_op(p, wbOp_select);
		wb_local_set(p, n);
		u32 n_minus_64 = wb_add_local(p, wbValType_i64); // (as unsigned: huge when n < 64)
		u32 sixty4_minus_n = wb_add_local(p, wbValType_i64);
		wb_local_get(p, n); wb_i64_const(p, 64); wb_op(p, wbOp_i64_sub); wb_local_set(p, n_minus_64);
		wb_i64_const(p, 64); wb_local_get(p, n); wb_op(p, wbOp_i64_sub); wb_local_set(p, sixty4_minus_n);

		wbAddr res = wb_add_temp(p, result_type);
		if (op == Token_Shl) {
			// lo' = lo << n
			wb_push_address(p, res.index, res.offset);
			wb_emit_shl64_sat(p, alo, n);
			wb_memarg(p, wbOp_i64_store, 0, 8);
			// hi' = (hi << n) | (lo >> (64-n)) | (lo << (n-64))
			wb_push_address(p, res.index, res.offset + 8);
			wb_emit_shl64_sat(p, ahi, n);
			wb_emit_shr64u_sat(p, alo, sixty4_minus_n);
			wb_op(p, wbOp_i64_or);
			wb_emit_shl64_sat(p, alo, n_minus_64);
			wb_op(p, wbOp_i64_or);
			wb_memarg(p, wbOp_i64_store, 0, 8);
		} else {
			// lo' = n < 64 ? (lo >> n) | (hi << (64-n)) : hi >> (n-64)
			wb_push_address(p, res.index, res.offset);
			wb_emit_shr64u_sat(p, alo, n);
			wb_emit_shl64_sat(p, ahi, sixty4_minus_n);
			wb_op(p, wbOp_i64_or);
			if (is_signed) wb_emit_shr64s_sat(p, ahi, n_minus_64); else wb_emit_shr64u_sat(p, ahi, n_minus_64);
			wb_local_get(p, n); wb_i64_const(p, 64); wb_op(p, wbOp_i64_lt_u);
			wb_op(p, wbOp_select);
			wb_memarg(p, wbOp_i64_store, 0, 8);
			// hi' = hi >> n
			wb_push_address(p, res.index, res.offset + 8);
			if (is_signed) wb_emit_shr64s_sat(p, ahi, n); else wb_emit_shr64u_sat(p, ahi, n);
			wb_memarg(p, wbOp_i64_store, 0, 8);
		}
		return wb_value_memory(res.index, res.offset, result_type);
	}

	wbValue r = wb_emit_conv(p, right, operand_type);
	if (r.kind == wbValue_Invalid) {
		return wb_value_invalid();
	}
	u32 blo, bhi;
	wb_load_int128(p, r, &blo, &bhi);

	switch (op) {
	case Token_CmpEq:
	case Token_NotEq:
		wb_local_get(p, alo); wb_local_get(p, blo); wb_op(p, wbOp_i64_eq);
		wb_local_get(p, ahi); wb_local_get(p, bhi); wb_op(p, wbOp_i64_eq);
		wb_op(p, wbOp_i32_and);
		if (op == Token_NotEq) {
			wb_op(p, wbOp_i32_eqz);
		}
		return wb_pop_to_local(p, wbValType_i32, result_type);
	case Token_Lt:
	case Token_LtEq:
	case Token_Gt:
	case Token_GtEq: {
		// a < b: (a.hi < b.hi) | (a.hi == b.hi & a.lo <u b.lo)
		// a > b == b < a, a <= b == !(b < a), a >= b == !(a < b)
		bool swap = op == Token_Gt || op == Token_LtEq;
		bool negate = op == Token_LtEq || op == Token_GtEq;
		u32 xlo = swap ? blo : alo, xhi = swap ? bhi : ahi;
		u32 ylo = swap ? alo : blo, yhi = swap ? ahi : bhi;
		wb_local_get(p, xhi); wb_local_get(p, yhi); wb_op(p, is_signed ? wbOp_i64_lt_s : wbOp_i64_lt_u);
		wb_local_get(p, xhi); wb_local_get(p, yhi); wb_op(p, wbOp_i64_eq);
		wb_local_get(p, xlo); wb_local_get(p, ylo); wb_op(p, wbOp_i64_lt_u);
		wb_op(p, wbOp_i32_and);
		wb_op(p, wbOp_i32_or);
		if (negate) {
			// a <= b == !(b < a)
			wb_op(p, wbOp_i32_eqz);
		}
		return wb_pop_to_local(p, wbValType_i32, result_type);
	}
	default:
		break;
	}

	wbAddr res = wb_add_temp(p, result_type);
	switch (op) {
	case Token_Add: {
		// lo = a.lo + b.lo; hi = a.hi + b.hi + (lo <u a.lo)
		u32 lo = wb_add_local(p, wbValType_i64);
		wb_local_get(p, alo); wb_local_get(p, blo); wb_op(p, wbOp_i64_add); wb_local_set(p, lo);
		wb_push_address(p, res.index, res.offset); wb_local_get(p, lo); wb_memarg(p, wbOp_i64_store, 0, 8);
		wb_push_address(p, res.index, res.offset + 8);
		wb_local_get(p, ahi); wb_local_get(p, bhi); wb_op(p, wbOp_i64_add);
		wb_local_get(p, lo); wb_local_get(p, alo); wb_op(p, wbOp_i64_lt_u); wb_op(p, wbOp_i64_extend_i32_u);
		wb_op(p, wbOp_i64_add);
		wb_memarg(p, wbOp_i64_store, 0, 8);
		break;
	}
	case Token_Sub: {
		// lo = a.lo - b.lo; hi = a.hi - b.hi - (a.lo <u b.lo)
		wb_push_address(p, res.index, res.offset);
		wb_local_get(p, alo); wb_local_get(p, blo); wb_op(p, wbOp_i64_sub);
		wb_memarg(p, wbOp_i64_store, 0, 8);
		wb_push_address(p, res.index, res.offset + 8);
		wb_local_get(p, ahi); wb_local_get(p, bhi); wb_op(p, wbOp_i64_sub);
		wb_local_get(p, alo); wb_local_get(p, blo); wb_op(p, wbOp_i64_lt_u); wb_op(p, wbOp_i64_extend_i32_u);
		wb_op(p, wbOp_i64_sub);
		wb_memarg(p, wbOp_i64_store, 0, 8);
		break;
	}
	case Token_And:
	case Token_Or:
	case Token_Xor:
	case Token_AndNot: {
		wbOp wop = wbOp_i64_and;
		switch (op) {
		case Token_Or:  wop = wbOp_i64_or;  break;
		case Token_Xor: wop = wbOp_i64_xor; break;
		default: break;
		}
		for (i32 i = 0; i < 2; i++) {
			wb_push_address(p, res.index, res.offset + 8*i);
			wb_local_get(p, i == 0 ? alo : ahi);
			wb_local_get(p, i == 0 ? blo : bhi);
			if (op == Token_AndNot) {
				wb_i64_const(p, -1); wb_op(p, wbOp_i64_xor);
			}
			wb_op(p, wop);
			wb_memarg(p, wbOp_i64_store, 0, 8);
		}
		break;
	}
	default:
		wb_unsupported(p, node, "128-bit integer operator");
		return wb_value_invalid();
	}
	return wb_value_memory(res.index, res.offset, result_type);
}

// Element-wise arithmetic on fixed arrays / enumerated arrays (`[3]f32 + [3]f32`, `v * 2`).
// Scalar operands are splatted by wb_emit_conv. Small arrays are unrolled, larger ones loop.
gb_internal wbValue wb_emit_arith_array(wbProcedure *p, Ast *node, TokenKind op, wbValue left, wbValue right, Type *type) {
	Type *bt = base_type(type);
	Type *elem = nullptr;
	i64 count = 0;
	if (bt->kind == Type_Array) {
		elem  = bt->Array.elem;
		count = bt->Array.count;
	} else {
		GB_ASSERT(bt->kind == Type_EnumeratedArray);
		elem  = bt->EnumeratedArray.elem;
		count = bt->EnumeratedArray.count;
	}
	left  = wb_emit_conv(p, left,  type);
	right = wb_emit_conv(p, right, type);
	if (left.kind != wbValue_Memory || right.kind != wbValue_Memory) {
		return wb_value_invalid();
	}
	i64 elem_size = type_size_of(elem);
	wbAddr res = wb_add_temp(p, type);

	if (count <= 8) {
		for (i64 i = 0; i < count; i++) {
			i32 off = cast(i32)(i * elem_size);
			wbValue l = wb_addr_load(p, wb_addr_memory(left.index,  left.offset  + off, elem));
			wbValue r = wb_addr_load(p, wb_addr_memory(right.index, right.offset + off, elem));
			wbValue v = wb_emit_arith(p, node, op, l, r, elem, elem);
			if (v.kind == wbValue_Invalid) {
				return v;
			}
			wb_addr_store(p, wb_addr_offset(res, off, elem), v);
		}
		return wb_value_memory(res.index, res.offset, type);
	}

	u32 idx = wb_add_local(p, wbValType_i32);
	wb_i32_const(p, 0);
	wb_local_set(p, idx);
	u32 block = wb_open_block(p);
	u32 loop  = wb_open_loop(p);
	wb_local_get(p, idx);
	wb_i32_const(p, cast(i32)count);
	wb_op(p, wbOp_i32_ge_u);
	wb_br_if(p, block);
	wbValue index = wb_value_local(idx, wbValType_i32, t_i32);
	wbValue l = wb_addr_load(p, wb_emit_elem_addr(p, left.index,  left.offset,  index, elem));
	wbValue r = wb_addr_load(p, wb_emit_elem_addr(p, right.index, right.offset, index, elem));
	wbValue v = wb_emit_arith(p, node, op, l, r, elem, elem);
	if (v.kind == wbValue_Invalid) {
		return v;
	}
	wb_addr_store(p, wb_emit_elem_addr(p, res.index, res.offset, index, elem), v);
	wb_local_get(p, idx);
	wb_i32_const(p, 1);
	wb_op(p, wbOp_i32_add);
	wb_local_set(p, idx);
	wb_br(p, loop);
	wb_close(p);
	wb_close(p);
	return wb_value_memory(res.index, res.offset, type);
}

// complex / quaternion arithmetic: add/sub are component-wise, complex mul is inlined,
// everything else goes through the runtime (quo_complex*, mul_quaternion*, quo_quaternion*)
gb_internal wbValue wb_emit_arith_complex(wbProcedure *p, Ast *node, TokenKind op, wbValue left, wbValue right, Type *type) {
	Type *ft = base_complex_elem_type(type);
	bool is_quat = is_type_quaternion(type);
	i64 fs = type_size_of(ft);
	i64 n = is_quat ? 4 : 2;

	left  = wb_emit_conv(p, left,  type);
	right = wb_emit_conv(p, right, type);
	if (left.kind != wbValue_Memory || right.kind != wbValue_Memory) {
		return wb_value_invalid();
	}

	char const *name = nullptr;
	if (op == Token_Quo) {
		if (is_quat) {
			switch (fs) { case 2: name = "quo_quaternion64"; break; case 4: name = "quo_quaternion128"; break; case 8: name = "quo_quaternion256"; break; }
		} else {
			switch (fs) { case 2: name = "quo_complex32"; break; case 4: name = "quo_complex64"; break; case 8: name = "quo_complex128"; break; }
		}
	} else if (op == Token_Mul && is_quat) {
		switch (fs) { case 2: name = "mul_quaternion64"; break; case 4: name = "mul_quaternion128"; break; case 8: name = "mul_quaternion256"; break; }
	}
	if (name != nullptr) {
		auto args = array_make<wbValue>(temporary_allocator(), 2);
		args[0] = left;
		args[1] = right;
		wbValue res = wb_emit_runtime_call(p, name, args);
		res.type = type;
		return res;
	}

	// f16 components are computed in f32
	Type *it = fs == 2 ? t_f32 : ft;
	wbValue l[4] = {};
	wbValue r[4] = {};
	for (i64 i = 0; i < n; i++) {
		l[i] = wb_emit_conv(p, wb_emit_load(p, left.index,  left.offset  + cast(i32)(i*fs), ft), it);
		r[i] = wb_emit_conv(p, wb_emit_load(p, right.index, right.offset + cast(i32)(i*fs), ft), it);
	}
	wbValue z[4] = {};
	switch (op) {
	case Token_Add:
	case Token_Sub:
		for (i64 i = 0; i < n; i++) {
			z[i] = wb_emit_arith(p, node, op, l[i], r[i], it, it);
		}
		break;
	case Token_Mul: {
		// (a+bi)(c+di) = (ac - bd) + (bc + ad)i
		wbValue a = wb_value_to_local(p, l[0]), b = wb_value_to_local(p, l[1]);
		wbValue c = wb_value_to_local(p, r[0]), d = wb_value_to_local(p, r[1]);
		wbValue ac = wb_emit_arith(p, node, Token_Mul, a, c, it, it);
		wbValue bd = wb_emit_arith(p, node, Token_Mul, b, d, it, it);
		z[0] = wb_emit_arith(p, node, Token_Sub, ac, bd, it, it);
		wbValue bc = wb_emit_arith(p, node, Token_Mul, b, c, it, it);
		wbValue ad = wb_emit_arith(p, node, Token_Mul, a, d, it, it);
		z[1] = wb_emit_arith(p, node, Token_Add, bc, ad, it, it);
		break;
	}
	default:
		wb_unsupported(p, node, "complex operator");
		return wb_value_invalid();
	}
	wbAddr res = wb_add_temp(p, type);
	for (i64 i = 0; i < n; i++) {
		wb_addr_store(p, wb_addr_memory(res.index, res.offset + cast(i32)(i*fs), ft), wb_emit_conv(p, z[i], ft));
	}
	return wb_value_memory(res.index, res.offset, type);
}

// Matrices are stored without padding: a sequence of columns (or rows when row-major).
gb_internal wbValue wb_matrix_elem(wbProcedure *p, wbValue m, i64 row, i64 col) {
	Type *mt = base_type(m.type);
	Type *elem = mt->Matrix.elem;
	i64 offset = matrix_indices_to_offset(mt, row, col)*type_size_of(elem);
	return wb_emit_load(p, m.index, m.offset + cast(i32)offset, elem);
}

gb_internal void wb_matrix_store_elem(wbProcedure *p, wbAddr res, i64 row, i64 col, wbValue v) {
	Type *mt = base_type(res.type);
	Type *elem = mt->Matrix.elem;
	i64 offset = matrix_indices_to_offset(mt, row, col)*type_size_of(elem);
	wb_addr_store(p, wb_addr_offset(res, offset, elem), v);
}

// The i-th element of a fixed array value in memory
gb_internal wbValue wb_array_elem(wbProcedure *p, wbValue a, i64 i) {
	Type *elem = base_array_type(a.type);
	return wb_emit_load(p, a.index, a.offset + cast(i32)(i*type_size_of(elem)), elem);
}

// sum_k a_k * b_k, where the terms are produced by `term(k)`
template <typename F>
gb_internal wbValue wb_emit_dot(wbProcedure *p, Ast *node, Type *elem, i64 count, F term) {
	wbValue acc = wb_value_invalid();
	for (i64 k = 0; k < count; k++) {
		wbValue t = term(k);
		if (t.kind == wbValue_Invalid) {
			return t;
		}
		acc = k == 0 ? wb_value_to_local(p, t) : wb_emit_arith(p, node, Token_Add, acc, t, elem, elem);
	}
	return acc;
}

gb_internal wbValue wb_emit_matrix_mul(wbProcedure *p, Ast *node, wbValue lhs, wbValue rhs, Type *type) {
	Type *xt = base_type(lhs.type);
	Type *yt = base_type(rhs.type);
	GB_ASSERT(xt->Matrix.column_count == yt->Matrix.row_count);
	Type *elem = xt->Matrix.elem;
	i64 inner = xt->Matrix.column_count;
	wbAddr res = wb_add_temp(p, type);
	for (i64 i = 0; i < xt->Matrix.row_count; i++) {
		for (i64 j = 0; j < yt->Matrix.column_count; j++) {
			wbValue v = wb_emit_dot(p, node, elem, inner, [&](i64 k) {
				return wb_emit_arith(p, node, Token_Mul, wb_matrix_elem(p, lhs, i, k), wb_matrix_elem(p, rhs, k, j), elem, elem);
			});
			wb_matrix_store_elem(p, res, i, j, v);
		}
	}
	return wb_value_memory(res.index, res.offset, type);
}

// matrix * vector -> vector of row_count
gb_internal wbValue wb_emit_matrix_mul_vector(wbProcedure *p, Ast *node, wbValue lhs, wbValue rhs, Type *type) {
	Type *mt = base_type(lhs.type);
	Type *elem = mt->Matrix.elem;
	i64 elem_size = type_size_of(elem);
	wbAddr res = wb_add_temp(p, type);
	for (i64 i = 0; i < mt->Matrix.row_count; i++) {
		wbValue v = wb_emit_dot(p, node, elem, mt->Matrix.column_count, [&](i64 j) {
			return wb_emit_arith(p, node, Token_Mul, wb_matrix_elem(p, lhs, i, j), wb_array_elem(p, rhs, j), elem, elem);
		});
		wb_addr_store(p, wb_addr_offset(res, i*elem_size, elem), v);
	}
	return wb_value_memory(res.index, res.offset, type);
}

// vector * matrix -> vector of column_count
gb_internal wbValue wb_emit_vector_mul_matrix(wbProcedure *p, Ast *node, wbValue lhs, wbValue rhs, Type *type) {
	Type *mt = base_type(rhs.type);
	Type *elem = mt->Matrix.elem;
	i64 elem_size = type_size_of(elem);
	wbAddr res = wb_add_temp(p, type);
	for (i64 j = 0; j < mt->Matrix.column_count; j++) {
		wbValue v = wb_emit_dot(p, node, elem, mt->Matrix.row_count, [&](i64 i) {
			return wb_emit_arith(p, node, Token_Mul, wb_array_elem(p, lhs, i), wb_matrix_elem(p, rhs, i, j), elem, elem);
		});
		wb_addr_store(p, wb_addr_offset(res, j*elem_size, elem), v);
	}
	return wb_value_memory(res.index, res.offset, type);
}

gb_internal wbValue wb_emit_arith_matrix(wbProcedure *p, Ast *node, TokenKind op, wbValue left, wbValue right, Type *type, bool component_wise) {
	if (left.kind == wbValue_Invalid || right.kind == wbValue_Invalid) {
		return wb_value_invalid();
	}
	Type *xt = base_type(left.type);
	Type *yt = base_type(right.type);
	GB_ASSERT(xt->kind == Type_Matrix || yt->kind == Type_Matrix);
	if (op == Token_Mul && !component_wise) {
		if (xt->kind == Type_Matrix && yt->kind == Type_Matrix) {
			return wb_emit_matrix_mul(p, node, left, right, type);
		} else if (xt->kind == Type_Matrix && is_type_array_like(yt)) {
			return wb_emit_matrix_mul_vector(p, node, left, right, type);
		} else if (is_type_array_like(xt) && yt->kind == Type_Matrix) {
			return wb_emit_vector_mul_matrix(p, node, left, right, type);
		}
	}
	// element-wise: pretend the matrices are arrays of their stored elements
	Type *mt = xt->kind == Type_Matrix ? left.type : right.type;
	left  = wb_emit_conv(p, left,  mt);
	right = wb_emit_conv(p, right, mt);
	if (left.kind != wbValue_Memory || right.kind != wbValue_Memory) {
		return wb_value_invalid();
	}
	Type *bmt = base_type(mt);
	Type *array_type = alloc_type_array(bmt->Matrix.elem, matrix_type_total_internal_elems(bmt));
	GB_ASSERT(type_size_of(array_type) == type_size_of(bmt));
	left.type  = array_type;
	right.type = array_type;
	if (wb_is_comparison(op)) {
		return wb_emit_aggregate_compare(p, node, op, left, right, array_type, type);
	}
	wbValue res = wb_emit_arith_array(p, node, op, left, right, array_type);
	res.type = type;
	return res;
}

gb_internal wbValue wb_emit_arith(wbProcedure *p, Ast *node, TokenKind op, wbValue left, wbValue right, Type *operand_type, Type *result_type) {
	if (left.kind == wbValue_Invalid || right.kind == wbValue_Invalid) {
		return wb_value_invalid();
	}
	// Endian-specific operands are computed on as their platform types; the
	// bitwise operations (and bit_sets, which only use them) are byte order agnostic
	if (is_type_different_to_arch_endianness(operand_type) && !is_type_bit_set(operand_type)) {
		switch (op) {
		case Token_And: case Token_Or: case Token_Xor: case Token_AndNot:
			break;
		default: {
			Type *platform_operand = integer_endian_type_to_platform_type(operand_type);
			Type *platform_result = is_type_different_to_arch_endianness(result_type) ? integer_endian_type_to_platform_type(result_type) : result_type;
			left  = wb_emit_to_platform_endian(p, left);
			right = wb_emit_to_platform_endian(p, right);
			wbValue res = wb_emit_arith(p, node, op, left, right, platform_operand, platform_result);
			return wb_emit_from_platform_endian(p, res, result_type);
		}
		}
	}
	if (is_type_matrix(left.type) || is_type_matrix(right.type)) {
		return wb_emit_arith_matrix(p, node, op, left, right, result_type, false);
	}
	if (wb_is_comparison(op) && (is_type_cstring(operand_type) || is_type_cstring16(operand_type))) {
		// C strings compare by content, except against nil (a plain pointer compare)
		bool against_nil = (left.kind == wbValue_Const && left.i == 0) || (right.kind == wbValue_Const && right.i == 0);
		if (!against_nil) {
			char const *name = nullptr;
			bool is16 = is_type_cstring16(operand_type);
			switch (op) {
			case Token_CmpEq: name = is16 ? "cstring16_eq" : "cstring_eq"; break;
			case Token_NotEq: name = is16 ? "cstring16_ne" : "cstring_ne"; break;
			case Token_Lt:    name = is16 ? "cstring16_lt" : "cstring_lt"; break;
			case Token_Gt:    name = is16 ? "cstring16_gt" : "cstring_gt"; break;
			case Token_LtEq:  name = is16 ? "cstring16_le" : "cstring_le"; break;
			case Token_GtEq:  name = is16 ? "cstring16_ge" : "cstring_ge"; break;
			}
			GB_ASSERT(name != nullptr);
			auto args = array_make<wbValue>(temporary_allocator(), 0, 2);
			array_add(&args, wb_emit_conv(p, left,  is16 ? t_cstring16 : t_cstring));
			array_add(&args, wb_emit_conv(p, right, is16 ? t_cstring16 : t_cstring));
			wbValue res = wb_emit_runtime_call(p, name, args);
			return wb_emit_conv(p, res, result_type);
		}
	}
	if (is_type_array_like(operand_type) && !wb_is_comparison(op)) {
		return wb_emit_conv(p, wb_emit_arith_array(p, node, op, left, right, operand_type), result_type);
	}
	if ((is_type_complex(operand_type) || is_type_quaternion(operand_type)) && !wb_is_comparison(op)) {
		return wb_emit_conv(p, wb_emit_arith_complex(p, node, op, left, right, operand_type), result_type);
	}
	if (wb_is_int128(operand_type)) {
		if (is_type_bit_set(operand_type)) {
			switch (op) {
			case Token_Add: op = Token_Or;     break;
			case Token_Sub: op = Token_AndNot; break;
			case Token_Lt: case Token_LtEq:
			case Token_Gt: case Token_GtEq: {
				// subset tests, see the scalar case below
				bool lt = op == Token_Lt || op == Token_LtEq;
				left  = wb_value_copy(p, left);
				right = wb_value_copy(p, right);
				wbValue both = wb_emit_arith128(p, node, Token_And, left, right, operand_type, operand_type);
				wbValue res = wb_emit_arith128(p, node, Token_CmpEq, both, lt ? left : right, operand_type, t_bool);
				if (op == Token_Lt || op == Token_Gt) {
					wbValue ne = wb_emit_arith128(p, node, Token_NotEq, left, right, operand_type, t_bool);
					wb_push(p, res);
					wb_push(p, ne);
					wb_op(p, wbOp_i32_and);
					res = wb_pop_to_local(p, wbValType_i32, t_bool);
				}
				return wb_emit_conv(p, res, result_type);
			}
			default:
				break;
			}
		}
		return wb_emit_arith128(p, node, op, left, right, operand_type, result_type);
	}
	wbValType vt = wb_valtype_of(operand_type);
	wbValType rvt = wb_valtype_of(result_type);
	if (vt == wbValType_Invalid || rvt == wbValType_Invalid) {
		wb_unsupported_type(p, node, operand_type);
		return wb_value_invalid();
	}
	bool is_signed = wb_type_is_signed(operand_type);

	if (wb_is_f16(operand_type)) {
		wbValue l = wb_emit_conv(p, left, t_f32);
		wbValue r = wb_emit_conv(p, right, t_f32);
		wbValue res = wb_emit_arith(p, node, op, l, r, t_f32, wb_is_comparison(op) ? result_type : t_f32);
		if (wb_is_comparison(op)) {
			return res;
		}
		return wb_emit_conv(p, res, result_type);
	}

	if (is_type_bit_set(operand_type)) {
		switch (op) {
		case Token_Add: op = Token_Or;     break;
		case Token_Sub: op = Token_AndNot; break;
		case Token_Lt: case Token_LtEq:
		case Token_Gt: case Token_GtEq: {
			// subset tests: a <= b is (a & b) == a, a >= b is (a & b) == b;
			// the strict forms additionally require a != b
			bool lt = op == Token_Lt || op == Token_LtEq;
			wb_push(p, left);
			wb_push(p, right);
			wb_op(p, vt == wbValType_i64 ? wbOp_i64_and : wbOp_i32_and);
			wb_push(p, lt ? left : right);
			wb_op(p, vt == wbValType_i64 ? wbOp_i64_eq : wbOp_i32_eq);
			if (op == Token_Lt || op == Token_Gt) {
				wb_push(p, left);
				wb_push(p, right);
				wb_op(p, vt == wbValType_i64 ? wbOp_i64_ne : wbOp_i32_ne);
				wb_op(p, wbOp_i32_and);
			}
			if (rvt == wbValType_i64) {
				wb_op(p, wbOp_i64_extend_i32_u);
			}
			return wb_pop_to_local(p, rvt, result_type);
		}
		default:
			break;
		}
	}

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

// Calls a runtime procedure with already converted arguments (aggregates are copied)
gb_internal wbValue wb_emit_runtime_call(wbProcedure *p, char const *name, Array<wbValue> const &args) {
	wbProcedure *callee = wb_lookup_runtime_procedure(p->module, name);
	auto copies = array_make<wbValue>(temporary_allocator(), 0, args.count);
	for (wbValue v : args) {
		if (v.kind == wbValue_Memory) {
			v = wb_value_copy(p, v);
		}
		array_add(&copies, v);
	}
	return wb_emit_call(p, callee->type, callee, wb_value_invalid(), copies);
}

// `x == nil` / `x != nil` (lb_emit_comp_against_nil)
gb_internal wbValue wb_emit_comp_against_nil(wbProcedure *p, Ast *node, wbValue v, bool is_not_eq) {
	Type *bt = base_type(v.type);
	if (v.kind == wbValue_Invalid) {
		return v;
	}
	if (v.kind != wbValue_Memory) {
		// scalars (pointers, procedures, enums, typeid, ...) are nil when zero
		wb_push(p, v);
		wb_op(p, v.vt == wbValType_i64 ? wbOp_i64_eqz : wbOp_i32_eqz);
		if (is_not_eq) {
			wb_op(p, wbOp_i32_eqz);
		}
		return wb_pop_to_local(p, wbValType_i32, t_bool);
	}

	i32 offset = 0;
	Type *word_type = t_rawptr;
	switch (bt->kind) {
	case Type_Slice:
	case Type_DynamicArray:
		break;
	case Type_Basic:
		if (is_type_string(bt)) {
			break;
		}
		if (bt->Basic.kind == Basic_any) {
			// nil if either the data pointer or the type id is nil
			Type *id_type = nullptr;
			i64 id_offset = type_offset_of(bt, 1, &id_type);
			wbValue data = wb_emit_load(p, v.index, v.offset, t_rawptr);
			wbValue id   = wb_emit_load(p, v.index, v.offset + cast(i32)id_offset, id_type);
			wb_push(p, data);
			wb_op(p, wbOp_i32_eqz);
			wb_push(p, id);
			wb_op(p, id.vt == wbValType_i64 ? wbOp_i64_eqz : wbOp_i32_eqz);
			wb_op(p, wbOp_i32_or);
			if (is_not_eq) {
				wb_op(p, wbOp_i32_eqz);
			}
			return wb_pop_to_local(p, wbValType_i32, t_bool);
		}
		wb_unsupported(p, node, "nil comparison");
		return wb_value_invalid();
	case Type_Union:
		if (type_size_of(bt) == 0) {
			return wb_value_const_int(t_bool, is_not_eq ? 0 : 1);
		}
		if (is_type_union_maybe_pointer(bt)) {
			break; // just the pointer
		}
		offset = cast(i32)bt->Union.variant_block_size;
		word_type = union_tag_type(bt);
		break;
	case Type_Map:
		// the data pointer of the raw map
		break;
	default:
		wb_unsupported(p, node, "nil comparison");
		return wb_value_invalid();
	}
	wbValue word = wb_emit_load(p, v.index, v.offset + offset, word_type);
	wb_push(p, word);
	wb_op(p, word.vt == wbValType_i64 ? wbOp_i64_eqz : wbOp_i32_eqz);
	if (is_not_eq) {
		wb_op(p, wbOp_i32_eqz);
	}
	return wb_pop_to_local(p, wbValType_i32, t_bool);
}

// Comparison of two already built aggregate values (strings via the runtime
// string_* procedures, everything else that is simply comparable via memory_equal)
gb_internal wbValue wb_emit_aggregate_compare(wbProcedure *p, Ast *node, TokenKind op, wbValue left, wbValue right, Type *operand_type, Type *result_type) {
	if (left.kind == wbValue_Invalid || right.kind == wbValue_Invalid) {
		return wb_value_invalid();
	}
	Type *bt = base_type(operand_type);
	if (is_type_string(bt) && !is_type_cstring(bt)) {
		char const *name = nullptr;
		switch (op) {
		case Token_CmpEq: name = "string_eq"; break;
		case Token_NotEq: name = "string_ne"; break;
		case Token_Lt:    name = "string_lt"; break;
		case Token_Gt:    name = "string_gt"; break;
		case Token_LtEq:  name = "string_le"; break;
		case Token_GtEq:  name = "string_ge"; break;
		}
		if (name != nullptr) {
			auto args = array_make<wbValue>(temporary_allocator(), 0, 2);
			array_add(&args, left);
			array_add(&args, right);
			wbValue res = wb_emit_runtime_call(p, name, args);
			res.type = result_type;
			return res;
		}
	}
	if ((op == Token_CmpEq || op == Token_NotEq) && (is_type_complex(bt) || is_type_quaternion(bt))) {
		// component-wise float comparison (so that -0 == 0 and NaN != NaN hold)
		left  = wb_emit_conv(p, left,  operand_type);
		right = wb_emit_conv(p, right, operand_type);
		if (left.kind != wbValue_Memory || right.kind != wbValue_Memory) {
			return wb_value_invalid();
		}
		Type *ft = base_complex_elem_type(bt);
		i64 fs = type_size_of(ft);
		i64 n = is_type_quaternion(bt) ? 4 : 2;
		for (i64 i = 0; i < n; i++) {
			wbValue l = wb_emit_load(p, left.index,  left.offset  + cast(i32)(i*fs), ft);
			wbValue r = wb_emit_load(p, right.index, right.offset + cast(i32)(i*fs), ft);
			wb_push(p, wb_emit_arith(p, node, op, l, r, ft, t_bool));
			if (i > 0) {
				wb_op(p, op == Token_CmpEq ? wbOp_i32_and : wbOp_i32_or);
			}
		}
		return wb_pop_to_local(p, wbValType_i32, result_type);
	}
	if (op == Token_CmpEq || op == Token_NotEq) {
		// bitwise comparison of the whole value (structs/arrays without padding, slices, ...)
		if (is_type_simple_compare(operand_type)) {
			left = wb_value_copy(p, left);
			right = wb_value_copy(p, right);
			auto args = array_make<wbValue>(temporary_allocator(), 0, 3);
			array_add(&args, wb_addr_get_ptr(p, wb_value_to_addr(p, left)));
			array_add(&args, wb_addr_get_ptr(p, wb_value_to_addr(p, right)));
			array_add(&args, wb_value_const_int(t_int, type_size_of(operand_type)));
			wbValue res = wb_emit_runtime_call(p, "memory_equal", args);
			if (op == Token_NotEq) {
				wb_push(p, res);
				wb_op(p, wbOp_i32_eqz);
				res = wb_pop_to_local(p, wbValType_i32, result_type);
			}
			res.type = result_type;
			return res;
		}
		// Field by field comparison through a generated procedure
		left = wb_value_copy(p, left);
		right = wb_value_copy(p, right);
		wbValue lptr = wb_addr_get_ptr(p, wb_value_to_addr(p, left));
		wbValue rptr = wb_addr_get_ptr(p, wb_value_to_addr(p, right));
		wbValue res = wb_emit_gen_call(p, wb_equal_proc_for_type(p->module, operand_type), lptr, rptr);
		if (op == Token_NotEq) {
			wb_push(p, res);
			wb_op(p, wbOp_i32_eqz);
			res = wb_pop_to_local(p, wbValType_i32, result_type);
		}
		res.type = result_type;
		return res;
	}
	wb_unsupported(p, node, "comparison of non-scalar values");
	return wb_value_invalid();
}

// Comparison of two built values of any type
gb_internal wbValue wb_emit_compare(wbProcedure *p, Ast *node, TokenKind op, wbValue left, wbValue right, Type *operand_type, Type *result_type) {
	if (wb_is_scalar(operand_type) || wb_is_int128(operand_type)) {
		return wb_emit_arith(p, node, op, left, right, operand_type, result_type);
	}
	return wb_emit_aggregate_compare(p, node, op, left, right, operand_type, result_type);
}

// Comparison of aggregate operands
gb_internal wbValue wb_build_aggregate_compare(wbProcedure *p, Ast *expr, AstBinaryExpr *be, Type *operand_type, Type *result_type) {
	bool left_nil  = is_type_untyped_nil(be->left->tav.type);
	bool right_nil = is_type_untyped_nil(be->right->tav.type);
	TokenKind op = be->op.kind;
	if ((op == Token_CmpEq || op == Token_NotEq) && (left_nil || right_nil)) {
		wbValue v = wb_build_expr(p, left_nil ? be->right : be->left);
		v = wb_emit_comp_against_nil(p, expr, v, op == Token_NotEq);
		if (v.kind != wbValue_Invalid) {
			v.type = result_type;
		}
		return v;
	}
	wbValue left = wb_value_copy(p, wb_emit_conv(p, wb_build_expr(p, be->left), operand_type));
	wbValue right = wb_emit_conv(p, wb_build_expr(p, be->right), operand_type);
	return wb_emit_aggregate_compare(p, expr, op, left, right, operand_type, result_type);
}

// `elem in set` / `elem not_in set` for bit_sets: (set & (1 << (elem-lower))) != 0
gb_internal wbValue wb_build_in_expr(wbProcedure *p, Ast *expr) {
	ast_node(be, BinaryExpr, expr);
	Type *result_type = default_type(expr->tav.type);
	wbValue right = wb_build_expr(p, be->right);
	if (right.kind == wbValue_Invalid) {
		return wb_value_invalid();
	}
	Type *rt = base_type(right.type);
	if (is_type_pointer(rt)) {
		right = wb_addr_load(p, wb_addr_from_pointer(p, right, type_deref(right.type)));
		rt = base_type(type_deref(rt));
	}
	if (rt->kind == Type_Map) {
		wbValue key = wb_build_expr(p, be->left);
		wbAddr elem = wb_map_elem_addr(p, wb_value_to_addr(p, right), right.type, key, rt->Map.value);
		if (elem.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		wbValue ptr = wb_map_get_ptr(p, elem, t_rawptr);
		wbValue res = wb_emit_comp_against_nil(p, expr, ptr, be->op.kind == Token_in);
		if (res.kind != wbValue_Invalid) {
			res.type = result_type;
		}
		return res;
	}
	if (rt->kind != Type_BitSet) {
		wb_unsupported(p, expr, "'in' operator on non bit_set");
		return wb_value_invalid();
	}
	Type *it = bit_set_to_int(rt);
	// endian-specific sets are tested as their platform integer
	right.type = it;
	right = wb_emit_to_platform_endian(p, right);
	it = right.type;
	wbValType vt = wb_valtype_of(it);
	if (wb_is_int128(it)) {
		// ((set >> (elem-lower)) & 1) != 0, on the low word after the shift
		right = wb_value_copy(p, right);
		wbValue elem = wb_emit_conv(p, wb_build_expr(p, be->left), t_i64);
		if (elem.kind == wbValue_Invalid) {
			return wb_value_invalid();
		}
		wb_push(p, elem);
		wb_i64_const(p, rt->BitSet.lower);
		wb_op(p, wbOp_i64_sub);
		wbValue amount = wb_pop_to_local(p, wbValType_i64, t_u64);
		wbValue shifted = wb_emit_arith128(p, expr, Token_Shr, right, amount, it, it);
		if (shifted.kind == wbValue_Invalid) {
			return wb_value_invalid();
		}
		wb_push(p, wb_emit_load(p, shifted.index, shifted.offset, t_u64));
		wb_i64_const(p, 1);
		wb_op(p, wbOp_i64_and);
		wb_op(p, wbOp_i64_eqz);
		if (be->op.kind == Token_in) {
			wb_op(p, wbOp_i32_eqz);
		}
		return wb_pop_to_local(p, wbValType_i32, result_type);
	}
	if (vt == wbValType_Invalid) {
		wb_unsupported_type(p, expr, rt);
		return wb_value_invalid();
	}
	wbValue elem = wb_emit_conv(p, wb_emit_conv(p, wb_build_expr(p, be->left), rt->BitSet.elem), it);
	if (elem.kind == wbValue_Invalid) {
		return wb_value_invalid();
	}
	right.type = it;
	wb_push(p, right);
	if (vt == wbValType_i64) {
		wb_i64_const(p, 1);
		wb_push(p, elem);
		wb_i64_const(p, rt->BitSet.lower);
		wb_op(p, wbOp_i64_sub);
		wb_op(p, wbOp_i64_shl);
		wb_op(p, wbOp_i64_and);
		wb_op(p, wbOp_i64_eqz);
	} else {
		wb_i32_const(p, 1);
		wb_push(p, elem);
		wb_i32_const(p, cast(i32)rt->BitSet.lower);
		wb_op(p, wbOp_i32_sub);
		wb_op(p, wbOp_i32_shl);
		wb_op(p, wbOp_i32_and);
		wb_op(p, wbOp_i32_eqz);
	}
	if (be->op.kind == Token_in) {
		wb_op(p, wbOp_i32_eqz);
	}
	return wb_pop_to_local(p, wbValType_i32, result_type);
}

// Short-circuiting `&&` / `||`: res = left; if (res == cond) { res = right }
gb_internal wbValue wb_emit_logical_binary(wbProcedure *p, Ast *expr, TokenKind op, Ast *left_expr, Ast *right_expr, Type *type) {
	wbValType vt = wb_valtype_of(type);
	if (vt == wbValType_Invalid) {
		wb_unsupported_type(p, expr, type);
		return wb_value_invalid();
	}
	u32 res = wb_add_local(p, vt);
	wbValue left = wb_emit_conv(p, wb_build_expr(p, left_expr), type);
	wb_push(p, left);
	wb_local_set(p, res);
	wb_local_get(p, res);
	if (vt == wbValType_i64) {
		wb_op(p, wbOp_i64_eqz);
		if (op == Token_CmpAnd) wb_op(p, wbOp_i32_eqz);
	} else if (op == Token_CmpOr) {
		wb_op(p, wbOp_i32_eqz);
	}
	wb_open_if(p);
	wbValue right = wb_emit_conv(p, wb_build_expr(p, right_expr), type);
	wb_push(p, right);
	wb_local_set(p, res);
	wb_close(p);
	return wb_value_local(res, vt, type);
}

gb_internal bool wb_is_empty_string_constant(Ast *expr) {
	return expr->tav.value.kind == ExactValue_String && is_type_string(expr->tav.type) && expr->tav.value.value_string.len == 0;
}

gb_internal wbValue wb_build_binary_expr(wbProcedure *p, Ast *expr) {
	ast_node(be, BinaryExpr, expr);
	Type *type = expr->tav.type;
	if (is_type_untyped(type)) {
		type = default_type(type);
	}

	switch (be->op.kind) {
	case Token_CmpAnd:
	case Token_CmpOr:
		return wb_emit_logical_binary(p, expr, be->op.kind, be->left, be->right, type);
	case Token_in:
	case Token_not_in:
		return wb_build_in_expr(p, expr);
	default:
		break;
	}

	if (is_type_matrix(be->left->tav.type) || is_type_matrix(be->right->tav.type)) {
		wbValue left = wb_build_expr(p, be->left);
		if (wb_expr_has_call(be->right)) {
			left = wb_value_fresh(p, left);
		}
		wbValue right = wb_build_expr(p, be->right);
		return wb_emit_arith_matrix(p, expr, be->op.kind, left, right, type, false);
	}

	// `x == ""` / `"" != x`: a length test (so that a nil cstring is "")
	if (be->op.kind == Token_CmpEq || be->op.kind == Token_NotEq) {
		Ast *other = nullptr;
		if (wb_is_empty_string_constant(be->right) && !is_type_union(be->left->tav.type)) {
			other = be->left;
		} else if (wb_is_empty_string_constant(be->left) && !is_type_union(be->right->tav.type)) {
			other = be->right;
		}
		if (other != nullptr && is_type_string(other->tav.type)) {
			bool is16 = is_type_string16(other->tav.type) || is_type_cstring16(other->tav.type);
			wbValue s = wb_emit_conv(p, wb_build_expr(p, other), is16 ? t_string16 : t_string);
			wbValue len = wb_emit_slice_len(p, s);
			wbValue res = wb_emit_arith(p, expr, be->op.kind, len, wb_value_const_int(t_int, 0), t_int, t_bool);
			return wb_emit_conv(p, res, type);
		}
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
		if (!wb_is_scalar(operand_type) && !wb_is_int128(operand_type)) {
			return wb_build_aggregate_compare(p, expr, be, operand_type, type);
		}
	}

	wbValue left  = wb_emit_conv(p, wb_build_expr(p, be->left),  operand_type);
	if (wb_expr_has_call(be->right)) {
		left = wb_value_fresh(p, left);
	}
	wbValue right;
	if (be->op.kind == Token_Shl || be->op.kind == Token_Shr) {
		right = wb_build_expr(p, be->right);
	} else {
		right = wb_emit_conv(p, wb_build_expr(p, be->right), operand_type);
	}
	if (wb_is_comparison(be->op.kind) && !is_type_boolean(core_type(type))) {
		// `int(a == b)`: the checker types the untyped bool as the target type
		wbValue res = wb_emit_arith(p, expr, be->op.kind, left, right, operand_type, t_bool);
		return wb_emit_conv(p, res, type);
	}
	return wb_emit_arith(p, expr, be->op.kind, left, right, operand_type, type);
}

// Calls

// A host math procedure imported from the `env` module (as LLVM does for the
// libm calls it cannot lower to wasm instructions)
gb_internal wbProcedure *wb_libm_import(wbModule *m, String const &name, wbValType vt, isize arg_count) {
	wbProcedure **found = string_map_get(&m->libm_imports, name);
	if (found != nullptr) {
		return *found;
	}
	wbProcedure *proc = wb_alloc_procedure(m, name);
	proc->is_foreign = true;
	wbFuncType ft = {};
	array_init(&ft.params,  m->allocator);
	array_init(&ft.results, m->allocator);
	for (isize i = 0; i < arg_count; i++) {
		array_add(&ft.params, vt);
	}
	array_add(&ft.results, vt);
	proc->type_index = wb_add_functype(m, ft);
	array_add(&proc->results, vt);
	proc->import_module = str_lit("env");
	proc->import_name   = name;
	array_add(&m->imports, proc);
	string_map_set(&m->libm_imports, name, proc);
	return proc;
}

// Foreign procedures with an `llvm.<op>.<type>` link name (core:math binds
// `llvm.sin.f64` and friends). The ones with wasm instructions are inlined,
// the rest call libm procedures imported from the host as LLVM would.
// f16 operands are computed in f32.
gb_internal wbValue wb_build_llvm_intrinsic_call(wbProcedure *p, Ast *expr, wbProcedure *callee) {
	ast_node(ce, CallExpr, expr);
	Type *pt = base_type(callee->type);
	Type *result_type = pt->Proc.result_count == 1 ? pt->Proc.results->Tuple.variables[0]->type : nullptr;
	isize param_count = pt->Proc.params ? pt->Proc.params->Tuple.variables.count : 0;

	String name = substring(callee->name, 5, callee->name.len); // after "llvm."
	String op = name;
	for (isize i = name.len-1; i >= 0; i--) {
		if (name.text[i] == '.') {
			op = substring(name, 0, i);
			break;
		}
	}

	Type *ft = result_type;
	if (ft == nullptr || !is_type_float(ft) || param_count == 0 || ce->args.count != param_count) {
		wb_unsupported(p, expr, "LLVM intrinsic");
		return wb_value_invalid();
	}
	Type *ct = wb_is_f16(ft) ? t_f32 : ft;
	wbValType vt = wb_valtype_of(ct);
	bool f64 = vt == wbValType_f64;

	auto args = array_make<wbValue>(temporary_allocator(), 0, param_count);
	for (Ast *arg : ce->args) {
		Type *param_type = pt->Proc.params->Tuple.variables[args.count]->type;
		wbValue v = wb_emit_conv(p, wb_build_expr(p, arg), param_type);
		if (is_type_float(param_type)) {
			v = wb_emit_conv(p, v, ct);
		}
		if (v.kind == wbValue_Invalid) {
			return v;
		}
		array_add(&args, v);
	}
	if (param_count > 1) {
		for_array(i, args) {
			args[i] = wb_value_fresh(p, args[i]);
		}
	}

	wbOp unary = wbOp_unreachable, binary = wbOp_unreachable;
	if      (op == "sqrt")      unary = f64 ? wbOp_f64_sqrt     : wbOp_f32_sqrt;
	else if (op == "fabs")      unary = f64 ? wbOp_f64_abs      : wbOp_f32_abs;
	else if (op == "floor")     unary = f64 ? wbOp_f64_floor    : wbOp_f32_floor;
	else if (op == "ceil")      unary = f64 ? wbOp_f64_ceil     : wbOp_f32_ceil;
	else if (op == "trunc")     unary = f64 ? wbOp_f64_trunc    : wbOp_f32_trunc;
	else if (op == "rint" || op == "nearbyint" || op == "roundeven") unary = f64 ? wbOp_f64_nearest : wbOp_f32_nearest;
	else if (op == "copysign")  binary = f64 ? wbOp_f64_copysign : wbOp_f32_copysign;
	else if (op == "minnum")    binary = f64 ? wbOp_f64_min      : wbOp_f32_min;
	else if (op == "maxnum")    binary = f64 ? wbOp_f64_max      : wbOp_f32_max;

	if (unary != wbOp_unreachable && args.count == 1) {
		wb_push(p, args[0]);
		wb_op(p, unary);
	} else if (binary != wbOp_unreachable && args.count == 2) {
		wb_push(p, args[0]);
		wb_push(p, args[1]);
		wb_op(p, binary);
	} else if ((op == "fmuladd" || op == "fma") && args.count == 3) {
		wb_push(p, args[0]);
		wb_push(p, args[1]);
		wb_op(p, f64 ? wbOp_f64_mul : wbOp_f32_mul);
		wb_push(p, args[2]);
		wb_op(p, f64 ? wbOp_f64_add : wbOp_f32_add);
	} else {
		// libm: `sin`/`sinf`, `pow`/`powf`, ...
		static char const *libm_names[] = {
			"sin", "cos", "tan", "asin", "acos", "atan", "atan2", "sinh", "cosh", "tanh",
			"exp", "exp2", "exp10", "log", "log2", "log10", "pow", "round", "cbrt", "fmod",
		};
		bool known = false;
		for (char const *n : libm_names) {
			if (op == make_string_c(n)) {
				known = true;
				break;
			}
		}
		if (!known) {
			wb_unsupported(p, expr, "LLVM intrinsic");
			return wb_value_invalid();
		}
		gbString import_name = gb_string_make_length(temporary_allocator(), op.text, op.len);
		if (!f64) {
			import_name = gb_string_appendc(import_name, "f");
		}
		wbProcedure *libm = wb_libm_import(p->module, copy_string(permanent_allocator(), make_string_c(import_name)), vt, args.count);
		for (wbValue const &v : args) {
			wb_push(p, v);
		}
		wb_call(p, libm);
	}
	wbValue res = wb_pop_to_local(p, vt, ct);
	return wb_emit_conv(p, res, result_type);
}

gb_internal wbValue wb_source_code_location(wbProcedure *p, String const &procedure, TokenPos const &pos) {
	Type *type = t_source_code_location;
	Type *bt = base_type(type);
	i64 size = type_size_of(type);
	u8 *bytes = gb_alloc_array(temporary_allocator(), u8, size);
	gb_zero_size(bytes, size);

	String file = get_file_path_string(pos.file_id);
	Type *ft = nullptr;
	i64 offset = 0;
	offset = type_offset_of(bt, 0, &ft); wb_write_const_data(p->module, ft, exact_value_string(file), bytes + offset);
	offset = type_offset_of(bt, 1, &ft); wb_write_const_data(p->module, ft, exact_value_i64(pos.line), bytes + offset);
	offset = type_offset_of(bt, 2, &ft); wb_write_const_data(p->module, ft, exact_value_i64(pos.column), bytes + offset);
	offset = type_offset_of(bt, 3, &ft); wb_write_const_data(p->module, ft, exact_value_string(procedure), bytes + offset);

	u32 addr = wb_data_alloc(p->module, size, type_align_of(type));
	wb_data_write(p->module, addr, bytes, size);
	return wb_value_memory(WB_NO_LOCAL, cast(i32)addr, type);
}

gb_internal wbValue wb_build_param_value(wbProcedure *p, Ast *call, Type *param_type, ParameterValue const &pv) {
	switch (pv.kind) {
	case ParameterValue_Constant: {
		if (pv.proc_entity != nullptr && is_type_proc(param_type)) {
			return wb_value_const_int(param_type, wb_table_index(p->module, wb_procedure_for_entity(p->module, pv.proc_entity)));
		}
		Type *type = pv.original_ast_expr != nullptr ? type_of_expr(pv.original_ast_expr) : nullptr;
		if (type == nullptr || is_type_untyped(type)) {
			type = param_type;
		}
		return wb_emit_conv(p, wb_const(p, call, type, pv.value), param_type);
	}
	case ParameterValue_Nil: {
		ExactValue nil_value = {};
		return wb_const(p, call, param_type, nil_value);
	}
	case ParameterValue_Value:
		return wb_emit_conv(p, wb_build_expr(p, pv.ast_value), param_type);
	case ParameterValue_Location: {
		String proc_name = {};
		if (p->entity != nullptr) {
			proc_name = p->entity->token.string;
		}
		ast_node(ce, CallExpr, call);
		return wb_source_code_location(p, proc_name, ast_token(ce->proc).pos);
	}
	case ParameterValue_Expression: {
		// #caller_expression / #caller_expression(param): the source text of the call or argument
		ast_node(ce, CallExpr, call);
		Ast *orig = pv.original_ast_expr;
		Ast *target_expr = call;
		if (orig->kind != Ast_BasicDirective) {
			Ast *directive_call = unparen_expr(orig);
			GB_ASSERT(directive_call->kind == Ast_CallExpr && directive_call->CallExpr.args.count == 1);
			Ast *target = directive_call->CallExpr.args[0];
			GB_ASSERT(target->kind == Ast_Ident);
			String target_str = target->Ident.token.string;
			Type *pt = base_type(type_of_expr(ce->proc));
			isize param_idx = lookup_procedure_parameter(pt, target_str);
			GB_ASSERT(param_idx >= 0);
			target_expr = nullptr;
			if (ce->split_args->positional.count > param_idx) {
				target_expr = ce->split_args->positional[param_idx];
			}
			for (Ast *arg : ce->split_args->named) {
				ast_node(fv, FieldValue, arg);
				if (fv->field->Ident.token.string == target_str) {
					target_expr = fv->value;
					break;
				}
			}
			GB_ASSERT(target_expr != nullptr);
		}
		gbString str = expr_to_string(target_expr, temporary_allocator());
		String text = copy_string(permanent_allocator(), make_string_c(str));
		return wb_emit_conv(p, wb_const(p, call, t_string, exact_value_string(text)), param_type);
	}
	default:
		wb_unsupported(p, call, "default parameter value kind");
		return wb_value_invalid();
	}
}

// Reverses the byte order of a 2, 4, 8 or 16 byte integer or float and gives
// the result `type` (the same size as the input). Integers held in a wasm local:
// (x >> 8*i & 0xff) << 8*(n-1-i) for each byte, or'd together; floats go
// through their integer bit pattern; 128-bit values swap the two words in memory.
gb_internal wbValue wb_emit_byte_swap(wbProcedure *p, wbValue x, Type *type) {
	if (x.kind == wbValue_Invalid) {
		return x;
	}
	i64 size = type_size_of(type);
	if (size == 1) {
		x.type = type;
		return x;
	}
	if (size == 16) {
		GB_ASSERT(x.kind == wbValue_Memory);
		wbAddr res = wb_add_temp(p, type);
		for (i32 w = 0; w < 2; w++) {
			wbValue word = wb_emit_load(p, x.index, x.offset + 8*w, t_u64);
			wb_emit_store(p, res.index, res.offset + 8*(1-w), wb_emit_byte_swap(p, word, t_u64), t_u64);
		}
		return wb_value_memory(res.index, res.offset, type);
	}
	wbValType vt = wb_valtype_of(type);
	bool is_float = vt == wbValType_f32 || vt == wbValType_f64;
	Type *it = type;
	if (is_float) {
		it = size == 4 ? t_u32 : t_u64;
	}
	wbValType ivt = wb_valtype_of(it);
	wb_push(p, x);
	if (is_float) {
		wb_op(p, size == 4 ? wbOp_i32_reinterpret_f32 : wbOp_i64_reinterpret_f64);
	}
	wbValue bits = wb_pop_to_local(p, ivt, it);
	for (i64 i = 0; i < size; i++) {
		wb_push(p, bits);
		if (ivt == wbValType_i64) {
			if (8*i != 0)  { wb_i64_const(p, 8*i); wb_op(p, wbOp_i64_shr_u); }
			wb_i64_const(p, 0xff); wb_op(p, wbOp_i64_and);
			i64 sh = 8*(size-1-i);
			if (sh != 0) { wb_i64_const(p, sh); wb_op(p, wbOp_i64_shl); }
			if (i != 0)  { wb_op(p, wbOp_i64_or); }
		} else {
			if (8*i != 0)  { wb_i32_const(p, cast(i32)(8*i)); wb_op(p, wbOp_i32_shr_u); }
			wb_i32_const(p, 0xff); wb_op(p, wbOp_i32_and);
			i32 sh = cast(i32)(8*(size-1-i));
			if (sh != 0) { wb_i32_const(p, sh); wb_op(p, wbOp_i32_shl); }
			if (i != 0)  { wb_op(p, wbOp_i32_or); }
		}
	}
	if (is_float) {
		wb_op(p, size == 4 ? wbOp_f32_reinterpret_i32 : wbOp_f64_reinterpret_i64);
	} else {
		wb_emit_normalize(p, type);
	}
	return wb_pop_to_local(p, vt, type);
}

// Endian-specific types (u32be, f64le, ...) hold their bytes in the declared
// order, so on a target of the other endianness the platform value is the byte
// swapped one. `wb_emit_to_platform_endian` yields the value as its platform
// type, `wb_emit_from_platform_endian` goes back.
gb_internal wbValue wb_emit_to_platform_endian(wbProcedure *p, wbValue x) {
	if (x.kind == wbValue_Invalid || !is_type_different_to_arch_endianness(x.type)) {
		return x;
	}
	return wb_emit_byte_swap(p, x, integer_endian_type_to_platform_type(x.type));
}
gb_internal wbValue wb_emit_from_platform_endian(wbProcedure *p, wbValue x, Type *type) {
	if (x.kind == wbValue_Invalid) {
		return x;
	}
	if (!is_type_different_to_arch_endianness(type)) {
		x.type = type;
		return x;
	}
	return wb_emit_byte_swap(p, x, type);
}

// `alloca`: memory below the frame, freed by the epilogue's stack pointer restore
// sp = (sp - size) & -align
gb_internal wbValue wb_emit_alloca(wbProcedure *p, wbValue size, i64 align, Type *type) {
	wb_global_get(p, p->module->global_stack_pointer);
	wb_push(p, size);
	wb_op(p, wbOp_i32_sub);
	if (align > 1) {
		wb_i32_const(p, cast(i32)-align);
		wb_op(p, wbOp_i32_and);
	}
	wbValue res = wb_pop_to_local(p, wbValType_i32, type);
	wb_push(p, res);
	wb_global_set(p, p->module->global_stack_pointer);
	return res;
}

// Reverses the bits of a 1, 2, 4 or 8 byte integer: byte swap, then swap
// nibbles, bit pairs and single bits within each byte using masks
gb_internal wbValue wb_emit_reverse_bits(wbProcedure *p, wbValue x, Type *type) {
	wbValType vt = wb_valtype_of(type);
	i64 size = type_size_of(type);
	Type *ut = size == 8 ? t_u64 : size == 4 ? t_u32 : size == 2 ? t_u16 : t_u8;
	x.type = ut;
	x = wb_emit_byte_swap(p, x, ut);
	struct { u64 mask; i64 shift; } steps[] = {
		{0x0f0f0f0f0f0f0f0full, 4},
		{0x3333333333333333ull, 2},
		{0x5555555555555555ull, 1},
	};
	for (auto const &step : steps) {
		u64 mask = size == 8 ? step.mask : step.mask & ((1ull << (8*size)) - 1);
		// ((x >> s) & m) | ((x & m) << s)
		wb_push(p, x);
		if (vt == wbValType_i64) {
			wb_i64_const(p, step.shift); wb_op(p, wbOp_i64_shr_u);
			wb_i64_const(p, cast(i64)mask); wb_op(p, wbOp_i64_and);
			wb_push(p, x);
			wb_i64_const(p, cast(i64)mask); wb_op(p, wbOp_i64_and);
			wb_i64_const(p, step.shift); wb_op(p, wbOp_i64_shl);
			wb_op(p, wbOp_i64_or);
		} else {
			wb_i32_const(p, cast(i32)step.shift); wb_op(p, wbOp_i32_shr_u);
			wb_i32_const(p, cast(i32)mask); wb_op(p, wbOp_i32_and);
			wb_push(p, x);
			wb_i32_const(p, cast(i32)mask); wb_op(p, wbOp_i32_and);
			wb_i32_const(p, cast(i32)step.shift); wb_op(p, wbOp_i32_shl);
			wb_op(p, wbOp_i32_or);
		}
		x = wb_pop_to_local(p, vt, ut);
	}
	wb_push(p, x);
	wb_emit_normalize(p, type);
	return wb_pop_to_local(p, vt, type);
}

gb_internal wbValue wb_build_builtin_call_internal(wbProcedure *p, Ast *expr, BuiltinProcId id, Type *type);

gb_internal wbValue wb_build_builtin_call(wbProcedure *p, Ast *expr, BuiltinProcId id) {
	Type *type = expr->tav.type;
	if (type != nullptr && is_type_untyped(type)) {
		type = default_type(type);
	}
	if (type != nullptr && wb_is_f16(type)) {
		// f16 is held as raw bits: the float builtins compute in f32
		switch (id) {
		case BuiltinProc_min:
		case BuiltinProc_max:
		case BuiltinProc_abs:
		case BuiltinProc_clamp:
		case BuiltinProc_sqrt:
		case BuiltinProc_fused_mul_add:
			return wb_emit_conv(p, wb_build_builtin_call_internal(p, expr, id, t_f32), type);
		default:
			break;
		}
	}
	return wb_build_builtin_call_internal(p, expr, id, type);
}

gb_internal wbValue wb_build_builtin_call_internal(wbProcedure *p, Ast *expr, BuiltinProcId id, Type *type) {
	ast_node(ce, CallExpr, expr);
	wbValType vt = type != nullptr ? wb_valtype_of(type) : wbValType_Invalid;
	Slice<Ast *> const &args = ce->args;

	if (id >= BuiltinProc_atomic_type_is_lock_free && id <= BuiltinProc_atomic_compare_exchange_weak_explicit) {
		return wb_build_atomic_call(p, expr, id);
	}

	switch (id) {
	case BuiltinProc_cpu_relax:
		return wb_value_invalid();
	case BuiltinProc_len:
	case BuiltinProc_cap: {
		Type *at = base_type(type_of_expr(args[0]));
		if (is_type_pointer(at)) {
			at = base_type(type_deref(at));
		}
		if (at->kind == Type_Array) {
			return wb_value_const_int(t_int, at->Array.count);
		}
		if (at->kind == Type_FixedCapacityDynamicArray && id == BuiltinProc_cap) {
			return wb_value_const_int(t_int, at->FixedCapacityDynamicArray.capacity);
		}
		wbValue s = wb_build_expr(p, args[0]);
		if (s.kind == wbValue_Invalid) {
			return s;
		}
		if (at->kind == Type_FixedCapacityDynamicArray) {
			if (s.kind == wbValue_Local) {
				s = wb_value_memory(s.index, 0, type_deref(s.type));
			}
			return wb_emit_load(p, s.index, s.offset + cast(i32)type_offset_of(at, 1, nullptr), t_int);
		}
		if (is_type_cstring(at) || is_type_cstring16(at)) {
			// NUL terminated: counted by the runtime
			auto cargs = array_make<wbValue>(temporary_allocator(), 1);
			cargs[0] = s;
			return wb_emit_runtime_call(p, is_type_cstring(at) ? "cstring_len" : "cstring16_len", cargs);
		}
		if (s.kind == wbValue_Local) {
			// pointer to slice/array (auto dereference)
			s = wb_value_memory(s.index, 0, type_deref(s.type));
		}
		if (is_type_soa_struct(at)) {
			wbAddr soa = wb_addr_memory(s.index, s.offset, at);
			return id == BuiltinProc_len ? wb_soa_len(p, soa) : wb_soa_cap(p, soa);
		}
		if (at->kind == Type_Slice || is_type_string(at) || (at->kind == Type_DynamicArray && id == BuiltinProc_len)) {
			return wb_emit_slice_len(p, s);
		}
		if (at->kind == Type_DynamicArray) {
			return wb_emit_dynamic_array_cap(p, s);
		}
		if (at->kind == Type_Map) {
			return id == BuiltinProc_len ? wb_emit_map_len(p, s) : wb_emit_map_cap(p, s);
		}
		break;
	}
	case BuiltinProc_swizzle: {
		// swizzle(a, i, j, ...): copy the selected elements into a temporary.
		// `#simd[N]T` is laid out like `[N]T` (see wasm_backend_simd.cpp), so
		// both kinds of vector are handled the same way.
		wbValue v = wb_build_expr(p, args[0]);
		if (v.kind == wbValue_Invalid || args.count == 1) {
			return v;
		}
		wbAddr src = wb_value_to_addr(p, v);
		Type *at = base_type(src.type);
		Type *elem = at->kind == Type_SimdVector ? at->SimdVector.elem : at->Array.elem;
		i64 elem_size = type_size_of(elem);
		wbAddr res = wb_add_temp(p, type);
		for (isize i = 1; i < args.count; i++) {
			i64 index = exact_value_to_i64(args[i]->tav.value);
			wb_emit_copy(p, res.index, res.offset + cast(i32)((i-1)*elem_size),
			             src.index, src.offset + cast(i32)(index*elem_size), elem_size);
		}
		return wb_value_memory(res.index, res.offset, type);
	}

	case BuiltinProc_soa_zip:
		return wb_build_soa_zip(p, expr);
	case BuiltinProc_soa_unzip:
		return wb_build_soa_unzip(p, expr);
	case BuiltinProc_type_equal_proc:
		return wb_value_const_int(type, wb_table_index(p->module, wb_equal_proc_for_type(p->module, args[0]->tav.type)));
	case BuiltinProc_type_hasher_proc:
		return wb_value_const_int(type, wb_table_index(p->module, wb_hasher_proc_for_type(p->module, args[0]->tav.type)));
	case BuiltinProc_type_map_info:
		return wb_value_const_int(type, wb_map_info_addr(p->module, args[0]->tav.type));
	case BuiltinProc_type_map_cell_info:
		return wb_value_const_int(type, wb_map_cell_info_addr(p->module, args[0]->tav.type));
	case BuiltinProc_raw_data: {
		Type *at = base_type(type_of_expr(args[0]));
		if (at->kind == Type_Slice || is_type_string(at) || at->kind == Type_DynamicArray) {
			wbValue s = wb_build_expr(p, args[0]);
			if (s.kind == wbValue_Invalid) return s;
			wbValue data = wb_emit_slice_data(p, s);
			data.type = type;
			return data;
		}
		if (at->kind == Type_Pointer && (is_type_array(type_deref(at)) || is_type_fixed_capacity_dynamic_array(type_deref(at)))) {
			wbValue ptr = wb_build_expr(p, args[0]);
			ptr.type = type;
			return ptr;
		}
		if (at->kind == Type_FixedCapacityDynamicArray) {
			wbAddr addr = wb_build_addr(p, args[0]);
			if (addr.kind == wbAddr_Invalid) return wb_value_invalid();
			return wb_addr_get_ptr(p, addr, type);
		}
		break;
	}
	case BuiltinProc_min:
	case BuiltinProc_max: {
		wbValue acc = wb_emit_conv(p, wb_build_expr(p, args[0]), type);
		for (isize i = 1; i < args.count; i++) {
			if (wb_expr_has_call(args[i])) acc = wb_value_fresh(p, acc);
			wbValue x = wb_emit_conv(p, wb_build_expr(p, args[i]), type);
			if (acc.kind == wbValue_Invalid || x.kind == wbValue_Invalid) return wb_value_invalid();
			if (vt == wbValType_f32 || vt == wbValType_f64) {
				wb_push(p, acc); wb_push(p, x);
				if (id == BuiltinProc_min) wb_op(p, vt == wbValType_f32 ? wbOp_f32_min : wbOp_f64_min);
				else                       wb_op(p, vt == wbValType_f32 ? wbOp_f32_max : wbOp_f64_max);
			} else {
				bool is_signed = wb_type_is_signed(type);
				wb_push(p, acc); wb_push(p, x);
				wb_push(p, acc); wb_push(p, x);
				wb_emit_binary_op(p, id == BuiltinProc_min ? Token_Lt : Token_Gt, vt, is_signed);
				wb_op(p, wbOp_select);
			}
			acc = wb_pop_to_local(p, vt, type);
		}
		return acc;
	}
	case BuiltinProc_abs: {
		Type *at = type_of_expr(args[0]);
		if (is_type_complex(at) || is_type_quaternion(at)) {
			char const *name = nullptr;
			switch (type_size_of(at)) {
			case 4:  name = "abs_complex32";     break;
			case 8:  name = is_type_quaternion(at) ? "abs_quaternion64"  : "abs_complex64";  break;
			case 16: name = is_type_quaternion(at) ? "abs_quaternion128" : "abs_complex128"; break;
			case 32: name = "abs_quaternion256"; break;
			}
			auto cargs = array_make<wbValue>(temporary_allocator(), 1);
			cargs[0] = wb_emit_conv(p, wb_build_expr(p, args[0]), at);
			return wb_emit_conv(p, wb_emit_runtime_call(p, name, cargs), type);
		}
		wbValue x = wb_emit_conv(p, wb_build_expr(p, args[0]), type);
		if (x.kind == wbValue_Invalid) return x;
		if (wb_is_int128(type)) {
			if (!wb_type_is_signed(type)) {
				return x;
			}
			// res = x; if high word < 0 { res = 0 - x }
			x = wb_value_copy(p, x);
			wbAddr res = wb_value_to_addr(p, x);
			wb_push(p, wb_emit_load(p, res.index, res.offset + 8, t_i64));
			wb_i64_const(p, 0);
			wb_op(p, wbOp_i64_lt_s);
			wb_open_if(p);
			wbValue neg = wb_emit_arith128(p, expr, Token_Sub, wb_emit_conv(p, wb_value_const_int(t_int, 0), type), x, type, type);
			wb_addr_store(p, res, neg);
			wb_close(p);
			return x;
		}
		if (vt == wbValType_f32 || vt == wbValType_f64) {
			wb_push(p, x);
			wb_op(p, vt == wbValType_f32 ? wbOp_f32_abs : wbOp_f64_abs);
		} else if (wb_type_is_signed(type)) {
			// select(x, -x, x >= 0)
			x = wb_value_to_local(p, x);
			wb_push(p, x);
			if (vt == wbValType_i64) { wb_i64_const(p, 0); wb_push(p, x); wb_op(p, wbOp_i64_sub); }
			else                     { wb_i32_const(p, 0); wb_push(p, x); wb_op(p, wbOp_i32_sub); wb_emit_normalize(p, type); }
			wb_push(p, x);
			if (vt == wbValType_i64) { wb_i64_const(p, 0); wb_op(p, wbOp_i64_ge_s); }
			else                     { wb_i32_const(p, 0); wb_op(p, wbOp_i32_ge_s); }
			wb_op(p, wbOp_select);
		} else {
			return x;
		}
		return wb_pop_to_local(p, vt, type);
	}
	case BuiltinProc_clamp: {
		wbValue x  = wb_emit_conv(p, wb_build_expr(p, args[0]), type);
		if (wb_expr_has_call(args[1]) || wb_expr_has_call(args[2])) x = wb_value_fresh(p, x);
		wbValue lo = wb_emit_conv(p, wb_build_expr(p, args[1]), type);
		if (wb_expr_has_call(args[2])) lo = wb_value_fresh(p, lo);
		wbValue hi = wb_emit_conv(p, wb_build_expr(p, args[2]), type);
		if (x.kind == wbValue_Invalid || lo.kind == wbValue_Invalid || hi.kind == wbValue_Invalid) return wb_value_invalid();
		if (vt == wbValType_f32 || vt == wbValType_f64) {
			wb_push(p, x); wb_push(p, lo);
			wb_op(p, vt == wbValType_f32 ? wbOp_f32_max : wbOp_f64_max);
			wb_push(p, hi);
			wb_op(p, vt == wbValType_f32 ? wbOp_f32_min : wbOp_f64_min);
			return wb_pop_to_local(p, vt, type);
		}
		bool is_signed = wb_type_is_signed(type);
		wb_push(p, x); wb_push(p, lo); wb_push(p, x); wb_push(p, lo);
		wb_emit_binary_op(p, Token_Gt, vt, is_signed);
		wb_op(p, wbOp_select);
		wbValue m = wb_pop_to_local(p, vt, type);
		wb_push(p, m); wb_push(p, hi); wb_push(p, m); wb_push(p, hi);
		wb_emit_binary_op(p, Token_Lt, vt, is_signed);
		wb_op(p, wbOp_select);
		return wb_pop_to_local(p, vt, type);
	}
	case BuiltinProc_mem_copy:
	case BuiltinProc_mem_copy_non_overlapping: {
		wbValue dst = wb_emit_conv(p, wb_build_expr(p, args[0]), t_rawptr);
		if (wb_expr_has_call(args[1]) || wb_expr_has_call(args[2])) dst = wb_value_fresh(p, dst);
		wbValue src = wb_emit_conv(p, wb_build_expr(p, args[1]), t_rawptr);
		if (wb_expr_has_call(args[2])) src = wb_value_fresh(p, src);
		wbValue len = wb_emit_conv(p, wb_build_expr(p, args[2]), t_int);
		if (dst.kind == wbValue_Invalid || src.kind == wbValue_Invalid || len.kind == wbValue_Invalid) return wb_value_invalid();
		wb_push(p, dst); wb_push(p, src); wb_push(p, len);
		wb_memory_copy(p);
		return wb_value_invalid();
	}
	case BuiltinProc_volatile_load:
	case BuiltinProc_unaligned_load:
	case BuiltinProc_non_temporal_load: {
		// wasm memory accesses are always unaligned-tolerant and never elided
		wbValue ptr = wb_build_expr(p, args[0]);
		if (ptr.kind == wbValue_Invalid) return ptr;
		Type *elem = type_deref(type_of_expr(args[0]));
		wbAddr addr = wb_addr_from_pointer(p, ptr, elem);
		wbValue v = wb_addr_load(p, addr);
		if (v.kind == wbValue_Memory) {
			v = wb_value_copy(p, v);
		}
		return v;
	}
	case BuiltinProc_volatile_store:
	case BuiltinProc_unaligned_store:
	case BuiltinProc_non_temporal_store: {
		wbValue ptr = wb_build_expr(p, args[0]);
		if (wb_expr_has_call(args[1])) ptr = wb_value_fresh(p, ptr);
		Type *elem = type_deref(type_of_expr(args[0]));
		wbValue v = wb_emit_conv(p, wb_build_expr(p, args[1]), elem);
		if (ptr.kind == wbValue_Invalid || v.kind == wbValue_Invalid) return wb_value_invalid();
		wb_addr_store(p, wb_addr_from_pointer(p, ptr, elem), v);
		return wb_value_invalid();
	}
	case BuiltinProc_overflow_add:
	case BuiltinProc_overflow_sub:
	case BuiltinProc_overflow_mul: {
		// result is the tuple (T, bool)
		Type *tuple = type;
		Type *et = nullptr;
		type_offset_of(tuple, 0, &et);
		wbValType evt = wb_valtype_of(et);
		if (evt != wbValType_i32 && evt != wbValType_i64) {
			wb_unsupported_type(p, expr, et);
			return wb_value_invalid();
		}
		bool is_signed = wb_type_is_signed(et);
		i64 bits = 8*type_size_of(et);
		wbValue a = wb_emit_conv(p, wb_build_expr(p, args[0]), et);
		if (wb_expr_has_call(args[1])) a = wb_value_fresh(p, a);
		wbValue b = wb_emit_conv(p, wb_build_expr(p, args[1]), et);
		if (a.kind == wbValue_Invalid || b.kind == wbValue_Invalid) return wb_value_invalid();
		a = wb_value_to_local(p, a);
		b = wb_value_to_local(p, b);

		wbOp op = wbOp_i32_add;
		switch (id) {
		case BuiltinProc_overflow_add: op = evt == wbValType_i64 ? wbOp_i64_add : wbOp_i32_add; break;
		case BuiltinProc_overflow_sub: op = evt == wbValType_i64 ? wbOp_i64_sub : wbOp_i32_sub; break;
		case BuiltinProc_overflow_mul: op = evt == wbValType_i64 ? wbOp_i64_mul : wbOp_i32_mul; break;
		}

		wbAddr res = wb_add_temp(p, tuple);
		Type *bt = nullptr;
		i64 bool_offset = type_offset_of(tuple, 1, &bt);
		u32 r = wb_add_local(p, evt);
		u32 overflow = wb_add_local(p, wbValType_i32);

		if (bits < 32) {
			// computed in 32 bits: overflow when the result does not fit the narrow type
			wb_push(p, a); wb_push(p, b); wb_op(p, op);
			wb_local_tee(p, r);
			wb_emit_normalize(p, et);
			wb_local_get(p, r);
			wb_op(p, wbOp_i32_ne);
			wb_local_set(p, overflow);
			wb_local_get(p, r);
			wb_emit_normalize(p, et);
			wb_local_set(p, r);
		} else if (id == BuiltinProc_overflow_mul && bits == 32) {
			// computed in 64 bits
			wb_push(p, a); wb_op(p, is_signed ? wbOp_i64_extend_i32_s : wbOp_i64_extend_i32_u);
			wb_push(p, b); wb_op(p, is_signed ? wbOp_i64_extend_i32_s : wbOp_i64_extend_i32_u);
			wb_op(p, wbOp_i64_mul);
			u32 wide = wb_add_local(p, wbValType_i64);
			wb_local_tee(p, wide);
			wb_op(p, wbOp_i32_wrap_i64);
			wb_local_tee(p, r);
			wb_op(p, is_signed ? wbOp_i64_extend_i32_s : wbOp_i64_extend_i32_u);
			wb_local_get(p, wide);
			wb_op(p, wbOp_i64_ne);
			wb_local_set(p, overflow);
		} else if (id == BuiltinProc_overflow_mul) {
			// 64 bit multiply: check by dividing back (a != 0 && (r / a != b || (a == -1 && b == min)))
			wb_push(p, a); wb_push(p, b); wb_op(p, wbOp_i64_mul);
			wb_local_set(p, r);
			wb_push(p, a); wb_op(p, wbOp_i64_eqz);
			wb_open_if(p, wbValType_i32);
			wb_i32_const(p, 0);
			wb_else(p);
			if (is_signed) {
				// avoid trapping on min / -1
				wb_push(p, a); wb_i64_const(p, -1); wb_op(p, wbOp_i64_eq);
				wb_push(p, b); wb_i64_const(p, INT64_MIN); wb_op(p, wbOp_i64_eq);
				wb_op(p, wbOp_i32_and);
				wb_open_if(p, wbValType_i32);
				wb_i32_const(p, 1);
				wb_else(p);
				wb_local_get(p, r); wb_push(p, a); wb_op(p, wbOp_i64_div_s);
				wb_push(p, b); wb_op(p, wbOp_i64_ne);
				wb_close(p);
			} else {
				wb_local_get(p, r); wb_push(p, a); wb_op(p, wbOp_i64_div_u);
				wb_push(p, b); wb_op(p, wbOp_i64_ne);
			}
			wb_close(p);
			wb_local_set(p, overflow);
		} else {
			bool is_add = id == BuiltinProc_overflow_add;
			wb_push(p, a); wb_push(p, b); wb_op(p, op);
			wb_local_set(p, r);
			if (is_signed) {
				// add: overflow if a and b have the same sign and r differs; sub: a and b differ and r differs from a
				wbOp xor_op = evt == wbValType_i64 ? wbOp_i64_xor : wbOp_i32_xor;
				wbOp and_op = evt == wbValType_i64 ? wbOp_i64_and : wbOp_i32_and;
				wbOp lt_op  = evt == wbValType_i64 ? wbOp_i64_lt_s : wbOp_i32_lt_s;
				wb_push(p, a); wb_local_get(p, r); wb_op(p, xor_op);
				if (is_add) {
					wb_push(p, b); wb_local_get(p, r); wb_op(p, xor_op);
				} else {
					wb_push(p, a); wb_push(p, b); wb_op(p, xor_op);
				}
				wb_op(p, and_op);
				if (evt == wbValType_i64) wb_i64_const(p, 0); else wb_i32_const(p, 0);
				wb_op(p, lt_op);
			} else {
				wbOp lt_op = evt == wbValType_i64 ? wbOp_i64_lt_u : wbOp_i32_lt_u;
				if (is_add) {
					wb_local_get(p, r); wb_push(p, a); wb_op(p, lt_op); // r < a
				} else {
					wb_push(p, a); wb_push(p, b); wb_op(p, lt_op); // a < b
				}
			}
			wb_local_set(p, overflow);
		}
		wb_emit_store(p, res.index, res.offset, wb_value_local(r, evt, et), et);
		wb_emit_store(p, res.index, res.offset + cast(i32)bool_offset, wb_value_local(overflow, wbValType_i32, t_bool), t_bool);
		return wb_value_memory(res.index, res.offset, tuple);
	}
	case BuiltinProc_likely:
	case BuiltinProc_unlikely:
		return wb_emit_conv(p, wb_build_expr(p, args[0]), type);
	case BuiltinProc_reverse_bits: {
		// bit i of x moves to bit n-1-i; byte swap first, then reverse the bits within each byte
		// with masked swaps of 4, 2 and 1 bit groups
		wbValue x = wb_emit_conv(p, wb_build_expr(p, args[0]), type);
		if (x.kind == wbValue_Invalid) return x;
		if (wb_is_int128(type)) {
			wbAddr res = wb_add_temp(p, type);
			for (i32 w = 0; w < 2; w++) {
				wbValue word = wb_emit_load(p, x.index, x.offset + 8*w, t_u64);
				wbValue r = wb_emit_reverse_bits(p, word, t_u64);
				wb_emit_store(p, res.index, res.offset + 8*(1-w), r, t_u64);
			}
			return wb_value_memory(res.index, res.offset, type);
		}
		return wb_emit_reverse_bits(p, x, type);
	}
	case BuiltinProc_alloca: {
		// stack memory of a runtime size: bump the stack pointer
		wbValue size = wb_emit_conv(p, wb_build_expr(p, args[0]), t_int);
		i64 align = exact_value_to_i64(type_and_value_of_expr(args[1]).value);
		if (size.kind == wbValue_Invalid) return size;
		return wb_emit_alloca(p, size, align, type);
	}
	case BuiltinProc_expect: {
		wbValue v = wb_build_expr(p, args[0]);
		if (wb_expr_has_call(args[1])) v = wb_value_fresh(p, v);
		wb_build_expr(p, args[1]);
		return v;
	}
	case BuiltinProc_mem_zero_volatile:
	case BuiltinProc_mem_zero: {
		wbValue dst = wb_emit_conv(p, wb_build_expr(p, args[0]), t_rawptr);
		if (wb_expr_has_call(args[1])) dst = wb_value_fresh(p, dst);
		wbValue len = wb_emit_conv(p, wb_build_expr(p, args[1]), t_int);
		if (dst.kind == wbValue_Invalid || len.kind == wbValue_Invalid) return wb_value_invalid();
		wb_push(p, dst); wb_i32_const(p, 0); wb_push(p, len);
		wb_memory_fill(p);
		return wb_value_invalid();
	}
	case BuiltinProc_trap:
	case BuiltinProc_debug_trap:
	case BuiltinProc_unreachable:
		wb_op(p, wbOp_unreachable);
		return wb_value_invalid();
	case BuiltinProc_wasm_memory_grow: {
		wbValue delta = wb_emit_conv(p, wb_build_expr(p, args[1]), t_int);
		if (delta.kind == wbValue_Invalid) return delta;
		wb_push(p, delta);
		wb_op_idx(p, wbOp_memory_grow, 0);
		return wb_pop_to_local(p, wbValType_i32, type);
	}
	case BuiltinProc_wasm_memory_size:
		wb_op_idx(p, wbOp_memory_size, 0);
		return wb_pop_to_local(p, wbValType_i32, type);
	case BuiltinProc_wasm_memory_atomic_wait32: {
		// Single threaded: nobody can change the value or notify, so the wait
		// either fails the comparison (1) or times out (2)
		wbValue ptr = wb_build_expr(p, args[0]);
		if (ptr.kind == wbValue_Invalid) return ptr;
		if (wb_expr_has_call(args[1]) || wb_expr_has_call(args[2])) ptr = wb_value_fresh(p, ptr);
		wbValue expected = wb_emit_conv(p, wb_build_expr(p, args[1]), t_u32);
		if (expected.kind == wbValue_Invalid) return expected;
		if (wb_expr_has_call(args[2])) expected = wb_value_fresh(p, expected);
		wbValue timeout = wb_build_expr(p, args[2]); // evaluated for its side effects only
		if (timeout.kind == wbValue_Invalid) return timeout;
		wb_push(p, wb_value_const_int(t_u32, 2));
		wb_push(p, wb_value_const_int(t_u32, 1));
		wb_push(p, wb_addr_load(p, wb_addr_from_pointer(p, ptr, t_u32)));
		wb_push(p, expected);
		wb_op(p, wbOp_i32_eq);
		wb_op(p, wbOp_select);
		return wb_pop_to_local(p, wbValType_i32, type);
	}
	case BuiltinProc_wasm_memory_atomic_notify32: {
		// Single threaded: there are never any waiters to wake
		for (Ast *arg : args) {
			wbValue v = wb_build_expr(p, arg);
			if (v.kind == wbValue_Invalid) return v;
		}
		return wb_const(p, expr, type, exact_value_u64(0));
	}
	case BuiltinProc_count_ones:
	case BuiltinProc_count_trailing_zeros:
	case BuiltinProc_count_leading_zeros: {
		wbValue x = wb_emit_conv(p, wb_build_expr(p, args[0]), type);
		if (x.kind == wbValue_Invalid) return x;
		if (wb_is_int128(type)) {
			// on the two words: popcnt(lo) + popcnt(hi); lo != 0 ? ctz(lo) : 64 + ctz(hi); hi != 0 ? clz(hi) : 64 + clz(lo)
			u32 lo, hi;
			wb_load_int128(p, x, &lo, &hi);
			wbAddr res = wb_add_temp(p, type);
			wb_push_address(p, res.index, res.offset);
			switch (id) {
			case BuiltinProc_count_ones:
				wb_local_get(p, lo); wb_op(p, wbOp_i64_popcnt);
				wb_local_get(p, hi); wb_op(p, wbOp_i64_popcnt);
				wb_op(p, wbOp_i64_add);
				break;
			case BuiltinProc_count_trailing_zeros:
				wb_local_get(p, lo); wb_op(p, wbOp_i64_ctz);
				wb_local_get(p, hi); wb_op(p, wbOp_i64_ctz); wb_i64_const(p, 64); wb_op(p, wbOp_i64_add);
				wb_local_get(p, lo); wb_op(p, wbOp_i64_eqz); wb_op(p, wbOp_i32_eqz);
				wb_op(p, wbOp_select);
				break;
			default:
				wb_local_get(p, hi); wb_op(p, wbOp_i64_clz);
				wb_local_get(p, lo); wb_op(p, wbOp_i64_clz); wb_i64_const(p, 64); wb_op(p, wbOp_i64_add);
				wb_local_get(p, hi); wb_op(p, wbOp_i64_eqz); wb_op(p, wbOp_i32_eqz);
				wb_op(p, wbOp_select);
				break;
			}
			wb_memarg(p, wbOp_i64_store, 0, 8);
			wb_push_address(p, res.index, res.offset + 8);
			wb_i64_const(p, 0);
			wb_memarg(p, wbOp_i64_store, 0, 8);
			return wb_value_memory(res.index, res.offset, type);
		}
		i64 bits = 8*type_size_of(type);
		wb_push(p, x);
		if (vt == wbValType_i32 && bits < 32) {
			// operate on the zero extended value
			wb_i32_const(p, cast(i32)((1ll<<bits)-1));
			wb_op(p, wbOp_i32_and);
			if (id == BuiltinProc_count_trailing_zeros) {
				// set the bit just past the width so ctz never exceeds it
				wb_i32_const(p, cast(i32)(1ll<<bits));
				wb_op(p, wbOp_i32_or);
			}
		}
		switch (id) {
		case BuiltinProc_count_ones:           wb_op(p, vt == wbValType_i64 ? wbOp_i64_popcnt : wbOp_i32_popcnt); break;
		case BuiltinProc_count_trailing_zeros: wb_op(p, vt == wbValType_i64 ? wbOp_i64_ctz    : wbOp_i32_ctz);    break;
		default:                               wb_op(p, vt == wbValType_i64 ? wbOp_i64_clz    : wbOp_i32_clz);    break;
		}
		if (vt == wbValType_i32 && bits < 32 && id == BuiltinProc_count_leading_zeros) {
			wb_i32_const(p, cast(i32)(32-bits));
			wb_op(p, wbOp_i32_sub);
		}
		wb_emit_normalize(p, type);
		return wb_pop_to_local(p, vt, type);
	}
	case BuiltinProc_byte_swap: {
		wbValue x = wb_emit_conv(p, wb_build_expr(p, args[0]), type);
		return wb_emit_byte_swap(p, x, type);
	}
	case BuiltinProc_sqrt: {
		wbValue x = wb_emit_conv(p, wb_build_expr(p, args[0]), type);
		if (x.kind == wbValue_Invalid) return x;
		wb_push(p, x);
		wb_op(p, vt == wbValType_f32 ? wbOp_f32_sqrt : wbOp_f64_sqrt);
		return wb_pop_to_local(p, vt, type);
	}
	case BuiltinProc_fused_mul_add: {
		// a*b + c (wasm has no fma instruction; LLVM's fmuladd allows the unfused form)
		wbValue a = wb_emit_conv(p, wb_build_expr(p, args[0]), type);
		if (wb_expr_has_call(args[1]) || wb_expr_has_call(args[2])) a = wb_value_fresh(p, a);
		wbValue b = wb_emit_conv(p, wb_build_expr(p, args[1]), type);
		if (wb_expr_has_call(args[2])) b = wb_value_fresh(p, b);
		wbValue c = wb_emit_conv(p, wb_build_expr(p, args[2]), type);
		if (a.kind == wbValue_Invalid || b.kind == wbValue_Invalid || c.kind == wbValue_Invalid) return wb_value_invalid();
		if (vt != wbValType_f32 && vt != wbValType_f64) break;
		wb_push(p, a); wb_push(p, b);
		wb_op(p, vt == wbValType_f32 ? wbOp_f32_mul : wbOp_f64_mul);
		wb_push(p, c);
		wb_op(p, vt == wbValType_f32 ? wbOp_f32_add : wbOp_f64_add);
		return wb_pop_to_local(p, vt, type);
	}
	case BuiltinProc_ptr_offset: {
		Type *pt = type_of_expr(args[0]);
		Type *elem = type_deref(pt);
		if (is_type_multi_pointer(base_type(pt))) {
			elem = base_type(pt)->MultiPointer.elem;
		}
		wbValue ptr = wb_build_expr(p, args[0]);
		if (wb_expr_has_call(args[1])) ptr = wb_value_fresh(p, ptr);
		wbValue n = wb_build_expr(p, args[1]);
		if (ptr.kind == wbValue_Invalid || n.kind == wbValue_Invalid) return wb_value_invalid();
		wb_push(p, ptr);
		wb_push_index(p, n);
		i64 size = type_size_of(elem);
		if (size != 1) { wb_i32_const(p, cast(i32)size); wb_op(p, wbOp_i32_mul); }
		wb_op(p, wbOp_i32_add);
		return wb_pop_to_local(p, wbValType_i32, type);
	}
	case BuiltinProc_ptr_sub: {
		Type *pt = type_of_expr(args[0]);
		Type *elem = type_deref(pt);
		if (is_type_multi_pointer(base_type(pt))) {
			elem = base_type(pt)->MultiPointer.elem;
		}
		wbValue a = wb_build_expr(p, args[0]);
		if (wb_expr_has_call(args[1])) a = wb_value_fresh(p, a);
		wbValue b = wb_build_expr(p, args[1]);
		if (a.kind == wbValue_Invalid || b.kind == wbValue_Invalid) return wb_value_invalid();
		wb_push(p, a); wb_push(p, b); wb_op(p, wbOp_i32_sub);
		i64 size = type_size_of(elem);
		if (size != 1) { wb_i32_const(p, cast(i32)size); wb_op(p, wbOp_i32_div_s); }
		return wb_pop_to_local(p, wbValType_i32, type);
	}
	case BuiltinProc_real:
	case BuiltinProc_imag:
	case BuiltinProc_jmag:
	case BuiltinProc_kmag: {
		wbValue v = wb_build_expr(p, args[0]);
		if (v.kind == wbValue_Invalid) {
			return v;
		}
		Type *ft = base_complex_elem_type(v.type);
		i64 index = 0;
		// @QuaternionLayout: {imag, jmag, kmag, real}; complex: {real, imag}
		if (is_type_quaternion(v.type)) {
			switch (id) {
			case BuiltinProc_real: index = 3; break;
			case BuiltinProc_imag: index = 0; break;
			case BuiltinProc_jmag: index = 1; break;
			case BuiltinProc_kmag: index = 2; break;
			}
		} else {
			index = id == BuiltinProc_imag ? 1 : 0;
		}
		wbAddr addr = wb_value_to_addr(p, v);
		return wb_emit_conv(p, wb_emit_load(p, addr.index, addr.offset + cast(i32)(index*type_size_of(ft)), ft), type);
	}

	case BuiltinProc_read_cycle_counter:
	case BuiltinProc_read_cycle_counter_frequency:
		// wasm has no cycle counter
		return wb_value_const_int(type, 0);

	case BuiltinProc_transpose: {
		wbValue m = wb_build_expr(p, args[0]);
		if (m.kind != wbValue_Memory) {
			return wb_value_invalid();
		}
		if (is_type_array(m.type)) {
			// arrays transpose to themselves
			m.type = type;
			return m;
		}
		Type *mt = base_type(m.type);
		wbAddr res = wb_add_temp(p, type);
		for (i64 j = 0; j < mt->Matrix.column_count; j++) {
			for (i64 i = 0; i < mt->Matrix.row_count; i++) {
				wb_matrix_store_elem(p, res, j, i, wb_matrix_elem(p, m, i, j));
			}
		}
		return wb_value_memory(res.index, res.offset, type);
	}
	case BuiltinProc_hadamard_product: {
		wbValue a = wb_build_expr(p, args[0]);
		if (wb_expr_has_call(args[1])) {
			a = wb_value_fresh(p, a);
		}
		wbValue b = wb_build_expr(p, args[1]);
		if (is_type_matrix(type)) {
			return wb_emit_arith_matrix(p, expr, Token_Mul, a, b, type, true);
		}
		return wb_emit_arith(p, expr, Token_Mul, a, b, type, type);
	}
	case BuiltinProc_matrix_flatten: {
		// the stored elements as a flat array (same size, no padding)
		wbValue m = wb_build_expr(p, args[0]);
		if (m.kind != wbValue_Memory) {
			return wb_value_invalid();
		}
		GB_ASSERT(type_size_of(type) == type_size_of(m.type));
		m = wb_value_copy(p, m);
		m.type = type;
		return m;
	}
	case BuiltinProc_outer_product: {
		wbValue a = wb_build_expr(p, args[0]);
		if (wb_expr_has_call(args[1])) {
			a = wb_value_fresh(p, a);
		}
		wbValue b = wb_build_expr(p, args[1]);
		if (a.kind != wbValue_Memory || b.kind != wbValue_Memory) {
			return wb_value_invalid();
		}
		Type *mt = base_type(type);
		Type *elem = mt->Matrix.elem;
		wbAddr res = wb_add_temp(p, type);
		for (i64 j = 0; j < mt->Matrix.column_count; j++) {
			for (i64 i = 0; i < mt->Matrix.row_count; i++) {
				wbValue v = wb_emit_arith(p, expr, Token_Mul, wb_array_elem(p, a, i), wb_array_elem(p, b, j), elem, elem);
				wb_matrix_store_elem(p, res, i, j, v);
			}
		}
		return wb_value_memory(res.index, res.offset, type);
	}

	case BuiltinProc_conj: {
		// negate the imaginary components (quaternion layout: {imag, jmag, kmag, real})
		wbValue v = wb_emit_conv(p, wb_build_expr(p, args[0]), type);
		if (v.kind != wbValue_Memory) {
			return wb_value_invalid();
		}
		Type *ft = base_complex_elem_type(type);
		i64 fs = type_size_of(ft);
		bool is_quat = is_type_quaternion(type);
		wbAddr res = wb_add_temp(p, type);
		for (i64 i = 0; i < (is_quat ? 4 : 2); i++) {
			bool negate = is_quat ? i < 3 : i == 1;
			wbValue c = wb_emit_load(p, v.index, v.offset + cast(i32)(i*fs), ft);
			if (negate) {
				c = wb_emit_conv(p, c, t_f64);
				c = wb_emit_arith(p, expr, Token_Sub, wb_const(p, expr, t_f64, exact_value_float(0)), c, t_f64, t_f64);
			}
			wb_addr_store(p, wb_addr_memory(res.index, res.offset + cast(i32)(i*fs), ft), c);
		}
		return wb_value_memory(res.index, res.offset, type);
	}

	case BuiltinProc_complex: {
		Type *ft = base_complex_elem_type(type);
		wbAddr dst = wb_add_temp(p, type);
		i32 fs = cast(i32)type_size_of(ft);
		wbValue re = wb_emit_conv(p, wb_build_expr(p, args[0]), ft);
		wbValue im = wb_emit_conv(p, wb_build_expr(p, args[1]), ft);
		wb_emit_store(p, dst.index, dst.offset, re, ft);
		wb_emit_store(p, dst.index, dst.offset + fs, im, ft);
		return wb_value_memory(dst.index, dst.offset, type);
	}

	case BuiltinProc_quaternion: {
		Type *ft = base_complex_elem_type(type);
		wbAddr dst = wb_add_temp(p, type);
		i32 fs = cast(i32)type_size_of(ft);
		for (i32 i = 0; i < 4; i++) {
			ast_node(f, FieldValue, args[i]);
			GB_ASSERT(f->field->kind == Ast_Ident);
			String name = f->field->Ident.token.string;
			i32 index = -1;
			if (name == "x" || name == "imag") {
				index = 0;
			} else if (name == "y" || name == "jmag") {
				index = 1;
			} else if (name == "z" || name == "kmag") {
				index = 2;
			} else if (name == "w" || name == "real") {
				index = 3;
			}
			GB_ASSERT(index >= 0);
			wbValue v = wb_emit_conv(p, wb_build_expr(p, f->value), ft);
			wb_emit_store(p, dst.index, dst.offset + index*fs, v, ft);
		}
		return wb_value_memory(dst.index, dst.offset, type);
	}

	case BuiltinProc_type_info_of: {
		Ast *arg = args[0];
		TypeAndValue tav = type_and_value_of_expr(arg);
		if (tav.mode == Addressing_Type) {
			return wb_type_info(p, type_of_expr(arg));
		}
		GB_ASSERT(is_type_typeid(tav.type));
		auto call_args = array_make<wbValue>(temporary_allocator(), 1);
		call_args[0] = wb_build_expr(p, arg);
		return wb_emit_runtime_call(p, "__type_info_of", call_args);
	}

	case BuiltinProc_typeid_of: {
		Ast *arg = args[0];
		TypeAndValue tav = type_and_value_of_expr(arg);
		GB_ASSERT(tav.mode == Addressing_Type);
		return wb_typeid(type_of_expr(arg));
	}

	case BuiltinProc___entry_point: {
		// intrinsics.__entry_point(): calls main
		Entity *entry = p->module->info->entry_point;
		if (entry == nullptr) {
			return wb_value_invalid();
		}
		auto no_args = array_make<wbValue>(temporary_allocator(), 0, 0);
		return wb_emit_call(p, entry->type, wb_procedure_for_entity(p->module, entry), wb_value_invalid(), no_args);
	}
	default:
		if (BuiltinProc_simd_add <= id && id <= BuiltinProc_simd_x86__MM_SHUFFLE) {
			return wb_build_simd_builtin(p, expr, id);
		}
		break;
	}
	wb_unsupported(p, expr, "builtin procedure");
	return wb_value_invalid();
}

// Pushes one argument of a call to a foreign procedure using the LLVM wasm ABI
gb_internal void wb_push_foreign_arg(wbProcedure *p, wbValue v, Type *param_type, ProcCallingConvention cc) {
	auto leaves = array_make<wbAbiLeaf>(temporary_allocator(), 0, 8);
	if (!wb_abi_flatten(param_type, cc, 0, &leaves)) {
		// indirect: pointer to the caller owned copy
		GB_ASSERT(v.kind == wbValue_Memory);
		wb_push_address(p, v.index, v.offset);
		return;
	}
	if (v.kind != wbValue_Memory) {
		GB_ASSERT(leaves.count == 1);
		wb_push(p, v);
		return;
	}
	for (wbAbiLeaf const &leaf : leaves) {
		wb_push(p, wb_emit_load(p, v.index, v.offset + cast(i32)leaf.offset, leaf.type));
	}
}

// Emits a call of a procedure of type `pt`, either directly to `callee` or
// indirectly through the table index in `proc_value`. `args` hold one value per
// non-polymorphic parameter, already converted; aggregates must be in caller
// owned storage. Returns the result (a Memory value for sret results).
gb_internal wbValue wb_emit_call(wbProcedure *p, Type *pt, wbProcedure *callee, wbValue proc_value, Array<wbValue> const &args) {
	pt = base_type(pt);
	bool foreign = callee != nullptr && callee->is_foreign;
	bool sret = wb_uses_sret(pt);

	wbAddr ctx = {};
	if (wb_is_odin_cc(pt)) {
		ctx = wb_context_addr(p); // may emit code, so before any operand is pushed
	}

	wbAddr result_addr = {};
	if (sret) {
		result_addr = wb_add_temp(p, pt->Proc.results);
		wb_push_address(p, result_addr.index, result_addr.offset);
	}
	if (foreign && pt->Proc.params != nullptr) {
		isize arg_index = 0;
		for_array(i, pt->Proc.params->Tuple.variables) {
			Entity *param = pt->Proc.params->Tuple.variables[i];
			if (param->kind != Entity_Variable) {
				continue;
			}
			if (pt->Proc.c_vararg && pt->Proc.variadic && i == pt->Proc.variadic_index) {
				wb_push(p, args[arg_index++]); // pointer to the C variadic argument buffer
				continue;
			}
			wb_push_foreign_arg(p, args[arg_index++], param->type, pt->Proc.calling_convention);
		}
	} else if (!foreign) {
		for (wbValue const &v : args) {
			wb_push(p, v);
		}
	}
	if (wb_is_odin_cc(pt)) {
		wb_push_context_ptr(p, ctx);
	}
	if (callee != nullptr) {
		wb_call(p, callee);
	} else {
		wb_push(p, proc_value);
		wb_call_indirect(p, wb_type_index_of_proc(p->module, pt));
	}

	if (sret) {
		if (pt->Proc.result_count == 1) {
			return wb_value_memory(result_addr.index, result_addr.offset, wb_result_type(pt));
		}
		return wb_value_memory(result_addr.index, result_addr.offset, pt->Proc.results);
	}
	if (pt->Proc.result_count == 1) {
		Type *rt = wb_result_type(pt);
		if (type_size_of(rt) == 0) {
			return wb_value_invalid();
		}
		wbValType vt = wb_valtype_of(rt);
		return wb_pop_to_local(p, vt, rt);
	}
	return wb_value_invalid();
}

// Converts an argument of a `#c_vararg` procedure to the type C's default
// argument promotions give it (lb_emit_c_vararg). `type` is the variadic
// element type, or the argument's own type for `..any`.
gb_internal wbValue wb_emit_c_vararg(wbProcedure *p, wbValue v, Type *type) {
	if (is_type_untyped_nil(type)) {
		return wb_value_const_int(t_rawptr, 0);
	}
	Type *core = core_type(type);
	if (core->kind == Type_BitSet) {
		core = core_type(bit_set_to_int(core));
		v = wb_emit_transmute(p, v, core);
	}
	return wb_emit_conv(p, v, c_vararg_promote_type(core));
}

// Stores the extra arguments of a `#c_vararg` call in a stack buffer, as
// LLVM's wasm ABI does, and returns its address (null when there are none).
// The callee reads them back with `va_arg`: each at its natural alignment,
// but at least 4 bytes wide and 4 byte aligned.
gb_internal wbValue wb_build_c_vararg_buffer(wbProcedure *p, Ast *expr, Slice<wbValue> const &values) {
	if (values.count == 0) {
		return wb_value_const_int(t_rawptr, 0);
	}
	auto offsets = array_make<i64>(temporary_allocator(), values.count);
	i64 size = 0;
	for_array(i, values) {
		Type *t = values[i].type;
		if (!wb_abi_is_basic(t)) {
			wb_unsupported(p, expr, "aggregate C variadic argument");
			return wb_value_invalid();
		}
		i64 align = gb_max(type_align_of(t), 4);
		size = align_formula(size, align);
		offsets[i] = size;
		size += gb_max(type_size_of(t), 4);
	}
	i32 base = wb_alloc_slot(p, size, 16);
	for_array(i, values) {
		wb_emit_store(p, p->fp_local, base + cast(i32)offsets[i], values[i], values[i].type);
	}
	return wb_addr_get_ptr(p, wb_addr_memory(p->fp_local, base, t_u8), t_rawptr);
}

// Builds a slice over a stack array holding the extra arguments of a variadic call
gb_internal wbValue wb_build_variadic_slice(wbProcedure *p, Type *slice_type, Slice<wbValue> const &values) {
	Type *elem_type = base_type(slice_type)->Slice.elem;
	wbAddr backing = wb_add_temp(p, alloc_type_array(elem_type, values.count));
	i64 elem_size = type_size_of(elem_type);
	for_array(i, values) {
		wbAddr elem = wb_addr_offset(backing, i*elem_size, elem_type);
		wb_addr_store(p, elem, values[i]);
	}
	wbAddr result = wb_add_temp(p, slice_type);
	wb_emit_store(p, result.index, result.offset, wb_addr_get_ptr(p, backing), t_rawptr);
	wb_emit_store(p, result.index, result.offset + cast(i32)build_context.int_size, wb_value_const_int(t_int, values.count), t_int);
	return wb_value_memory(result.index, result.offset, slice_type);
}

gb_internal wbValue wb_build_call_expr_internal(wbProcedure *p, Ast *expr);

// Registers the call to the `@(deferred_*)` procedure of `e` for the end of
// the current scope. `in_args` are the call's arguments (one per parameter),
// `result` its value (lb_add_defer_proc).
gb_internal void wb_add_defer_proc(wbProcedure *p, Ast *expr, Entity *e, Array<wbValue> const &in_args, wbValue result) {
	ast_node(ce, CallExpr, expr);
	DeferredProcedureKind kind = e->Procedure.deferred_procedure.kind;
	Entity *deferred_entity = e->Procedure.deferred_procedure.entity;
	wbProcedure *deferred = wb_procedure_for_entity(p->module, deferred_entity);
	if (deferred == nullptr) {
		return;
	}
	Type *pt = base_type(deferred_entity->type);

	bool by_ptr = false;
	bool use_in = false, use_out = false;
	switch (kind) {
	case DeferredProcedure_none:                             break;
	case DeferredProcedure_in_by_ptr:     by_ptr = true;     /*fallthrough*/
	case DeferredProcedure_in:            use_in = true;     break;
	case DeferredProcedure_out_by_ptr:    by_ptr = true;     /*fallthrough*/
	case DeferredProcedure_out:           use_out = true;    break;
	case DeferredProcedure_in_out_by_ptr: by_ptr = true;     /*fallthrough*/
	case DeferredProcedure_in_out:        use_in = use_out = true; break;
	}

	// The expressions the in-arguments came from, for `by_ptr` (nullptr if unknown)
	auto values = array_make<wbValue>(permanent_allocator(), 0, in_args.count+4);
	auto exprs  = array_make<Ast *>(temporary_allocator(), 0, in_args.count+4);
	if (use_in) {
		Type *ct = base_type(e->type);
		isize variadic_index = ct->Proc.variadic ? ct->Proc.variadic_index : (ct->Proc.params ? ct->Proc.params->Tuple.variables.count : 0);
		Slice<Ast *> const &positional = ce->split_args->positional;
		bool positional_map = true; // positional[i] corresponds to parameter i
		for (Ast *arg : positional) {
			if (is_type_tuple(type_of_expr(arg))) {
				positional_map = false;
			}
		}
		for_array(i, in_args) {
			wbValue const &v = in_args[i];
			if (v.kind == wbValue_Invalid) {
				continue;
			}
			Ast *arg = nullptr;
			if (positional_map && i < variadic_index && i < positional.count) {
				arg = positional[i];
			}
			array_add(&values, v);
			array_add(&exprs, arg);
		}
	}
	if (use_out && result.kind != wbValue_Invalid) {
		if (is_type_tuple(result.type)) {
			for_array(i, result.type->Tuple.variables) {
				array_add(&values, wb_tuple_field(p, result, i));
			}
		} else {
			array_add(&values, result);
		}
	}

	// The values must survive until the end of the scope, so they are copied
	// into storage nothing else writes to.
	isize param_index = 0;
	for_array(i, values) {
		wbValue v = values[i];
		Type *param_type = nullptr;
		while (param_index < pt->Proc.params->Tuple.variables.count) {
			Entity *param = pt->Proc.params->Tuple.variables[param_index++];
			if (param->kind == Entity_Variable) {
				param_type = param->type;
				break;
			}
		}
		if (param_type == nullptr) {
			break;
		}
		if (by_ptr) {
			// An addressable argument is passed by its own address, so that
			// the deferred procedure sees later changes to it (lb_address_from_load_or_generate_local)
			Ast *arg = i < exprs.count ? exprs[i] : nullptr;
			wbAddr src = {};
			if (arg != nullptr && arg->tav.mode == Addressing_Variable && !wb_expr_has_call(arg) &&
			    are_types_identical(type_of_expr(arg), v.type)) {
				src = wb_build_addr(p, arg);
				if (src.kind != wbAddr_Local && src.kind != wbAddr_Memory) {
					src = {};
				}
			}
			if (src.kind == wbAddr_Invalid) {
				src = wb_add_temp(p, v.type);
				wb_addr_store(p, src, v);
			}
			values[i] = wb_value_fresh(p, wb_emit_conv(p, wb_addr_get_ptr(p, src, alloc_type_pointer(v.type)), param_type));
		} else {
			values[i] = wb_value_fresh(p, wb_emit_conv(p, v, param_type));
		}
	}

	wbDefer d = {};
	d.scope_index = p->scopes.count;
	d.context_stack_count = p->context_stack.count;
	d.proc = deferred;
	d.proc_type = pt;
	d.proc_args = values;
	array_add(&p->defers, d);
}

gb_internal wbValue wb_build_call_expr(wbProcedure *p, Ast *expr) {
	ast_node(ce, CallExpr, expr);
	wbValue res = wb_build_call_expr_internal(p, expr);
	if (ce->optional_ok_one && res.kind == wbValue_Memory && is_type_tuple(res.type)) {
		// `x := f()` where f returns (T, bool): only the value is used
		return wb_tuple_field(p, res, 0);
	}
	return res;
}

gb_internal wbValue wb_build_call_expr_internal(wbProcedure *p, Ast *expr) {
	ast_node(ce, CallExpr, expr);
	TypeAndValue proc_tv = type_and_value_of_expr(ce->proc);

	if (proc_tv.mode == Addressing_Type) {
		// Type conversion: T(x)
		GB_ASSERT(ce->args.count == 1);
		wbValue x = wb_build_expr(p, ce->args[0]);
		return wb_emit_conv(p, x, expr->tav.type);
	}
	if (proc_tv.mode == Addressing_Builtin) {
		Entity *e = entity_of_node(ce->proc);
		if (e == nullptr || e->kind != Entity_Builtin) {
			wb_unsupported(p, expr, "builtin procedure call");
			return wb_value_invalid();
		}
		return wb_build_builtin_call(p, expr, cast(BuiltinProcId)e->Builtin.id);
	}

	Type *pt = base_type(proc_tv.type);
	if (pt == nullptr || pt->kind != Type_Proc) {
		wb_unsupported(p, expr, "call of non-procedure");
		return wb_value_invalid();
	}
	bool c_vararg = pt->Proc.c_vararg;

	// Direct call if the callee is a known procedure, otherwise through the table
	Entity *e = entity_of_node(ce->proc);
	wbProcedure *callee = nullptr;
	if (e != nullptr && (e->flags & EntityFlag_Disabled)) {
		// `@(disabled=true)` procedure: the call is removed
		return wb_value_invalid();
	}
	wbValue proc_value = {};
	if (e != nullptr && e->kind == Entity_Procedure) {
		callee = wb_procedure_for_entity(p->module, e);
		if (callee->is_llvm_intrinsic) {
			return wb_build_llvm_intrinsic_call(p, expr, callee);
		}
	} else {
		proc_value = wb_build_expr(p, ce->proc);
		if (proc_value.kind == wbValue_Invalid) {
			return proc_value;
		}
		proc_value = wb_value_fresh(p, proc_value);
	}
	if (c_vararg && (callee == nullptr || !callee->is_foreign)) {
		wb_unsupported(p, expr, "indirect C variadic procedure call");
		return wb_value_invalid();
	}

	GB_ASSERT(ce->split_args != nullptr);
	isize param_count = pt->Proc.params ? pt->Proc.params->Tuple.variables.count : 0;
	Slice<Ast *> const &positional = ce->split_args->positional;
	bool variadic = pt->Proc.variadic;
	bool vari_expand = ce->ellipsis.pos.line != 0;
	isize variadic_index = variadic ? pt->Proc.variadic_index : param_count;
	Type *slice_type = variadic ? pt->Proc.params->Tuple.variables[variadic_index]->type : nullptr;
	Type *elem_type  = variadic ? base_type(slice_type)->Slice.elem : nullptr;

	// Positional arguments are evaluated first, in order. A value must be kept
	// in fresh storage if a later argument contains a call.
	auto later_call = array_make<bool>(temporary_allocator(), positional.count);
	{
		bool seen = false;
		for (isize i = positional.count-1; i >= 0; i--) {
			later_call[i] = seen;
			if (wb_expr_has_call(positional[i])) {
				seen = true;
			}
		}
	}
	auto values = array_make<wbValue>(temporary_allocator(), 0, positional.count+1);
	for_array(i, positional) {
		Type *arg_type = type_of_expr(positional[i]);
		if (is_type_tuple(arg_type)) {
			// f(g()) where g has multiple results
			wbValue tuple = wb_build_expr(p, positional[i]);
			for_array(j, arg_type->Tuple.variables) {
				isize index = values.count;
				Type *param_type = index < variadic_index ? pt->Proc.params->Tuple.variables[index]->type : elem_type;
				if (param_type == nullptr) {
					wb_unsupported(p, positional[i], "argument count");
					return wb_value_invalid();
				}
				wbValue v = wb_tuple_field(p, tuple, j);
				if (c_vararg && index >= variadic_index) {
					v = wb_emit_c_vararg(p, v, is_type_any(elem_type) ? v.type : elem_type);
				} else {
					v = wb_emit_conv(p, v, param_type);
				}
				array_add(&values, later_call[i] ? wb_value_fresh(p, v) : v);
			}
			continue;
		}
		isize index = values.count;
		Type *param_type = nullptr;
		if (index < variadic_index) {
			Entity *param = pt->Proc.params->Tuple.variables[index];
			if (param->kind != Entity_Variable) {
				array_add(&values, wb_value_invalid()); // polymorphic parameter, takes no space
				continue;
			}
			param_type = param->type;
		} else if (variadic) {
			param_type = vari_expand ? slice_type : elem_type;
		} else {
			wb_unsupported(p, positional[i], "argument count");
			return wb_value_invalid();
		}
		wbValue v = wb_build_expr(p, positional[i]);
		if (v.kind == wbValue_Invalid) {
			return v;
		}
		if (c_vararg && index >= variadic_index) {
			v = wb_emit_c_vararg(p, v, is_type_any(elem_type) ? type_of_expr(positional[i]) : elem_type);
		} else {
			v = wb_emit_conv(p, v, param_type);
		}
		if (v.kind == wbValue_Invalid) {
			return v;
		}
		array_add(&values, later_call[i] ? wb_value_fresh(p, v) : v);
	}
	// Map onto parameters: positional, then named, then defaults
	auto args = array_make<wbValue>(temporary_allocator(), 0, param_count);
	auto arg_set = array_make<bool>(temporary_allocator(), param_count);
	array_resize(&args, param_count);
	for (isize i = 0; i < param_count; i++) {
		arg_set[i] = false;
	}
	for (isize i = 0; i < gb_min(values.count, variadic_index); i++) {
		args[i] = values[i];
		arg_set[i] = true;
	}
	if (variadic && c_vararg) {
		if (vari_expand) {
			wb_unsupported(p, expr, "expanded C variadic arguments");
			return wb_value_invalid();
		}
		wbValue buffer = wb_build_c_vararg_buffer(p, expr, slice(slice_from_array(values), variadic_index, values.count));
		if (buffer.kind == wbValue_Invalid) {
			return buffer;
		}
		args[variadic_index] = buffer;
		arg_set[variadic_index] = true;
	} else if (variadic) {
		wbValue slice_value = {};
		if (values.count <= variadic_index) {
			ExactValue nil_value = {};
			slice_value = wb_const(p, expr, slice_type, nil_value);
		} else if (vari_expand) {
			GB_ASSERT(values.count == variadic_index+1);
			slice_value = values[variadic_index];
		} else {
			slice_value = wb_build_variadic_slice(p, slice_type, slice(slice_from_array(values), variadic_index, values.count));
		}
		args[variadic_index] = slice_value;
		arg_set[variadic_index] = true;
	}
	for (Ast *arg : ce->split_args->named) {
		ast_node(fv, FieldValue, arg);
		String name = fv->field->Ident.token.string;
		isize index = lookup_procedure_parameter(pt, name);
		if (index < 0) {
			wb_unsupported(p, arg, "named argument");
			return wb_value_invalid();
		}
		if (c_vararg && variadic && index == variadic_index) {
			wb_unsupported(p, arg, "named C variadic argument");
			return wb_value_invalid();
		}
		Type *param_type = pt->Proc.params->Tuple.variables[index]->type;
		wbValue v = wb_emit_conv(p, wb_build_expr(p, fv->value), param_type);
		if (v.kind == wbValue_Invalid) {
			return v;
		}
		args[index] = wb_value_fresh(p, v);
		arg_set[index] = true;
	}
	for (isize i = 0; i < param_count; i++) {
		if (arg_set[i]) {
			continue;
		}
		Entity *param = pt->Proc.params->Tuple.variables[i];
		if (param->kind == Entity_TypeName || param->kind == Entity_Constant) {
			continue; // polymorphic parameters take no space
		}
		if (has_parameter_value(param->Variable.param_value)) {
			args[i] = wb_build_param_value(p, expr, param->type, param->Variable.param_value);
		} else {
			wb_unsupported(p, expr, "missing argument");
			return wb_value_invalid();
		}
		if (args[i].kind == wbValue_Invalid) {
			return args[i];
		}
		arg_set[i] = true;
	}

	// Aggregates are passed by pointer to a copy owned by the caller
	auto final_args = array_make<wbValue>(temporary_allocator(), 0, param_count);
	for (isize i = 0; i < param_count; i++) {
		if (pt->Proc.params->Tuple.variables[i]->kind != Entity_Variable) {
			continue;
		}
		GB_ASSERT(arg_set[i]);
		wbValue v = args[i];
		if (v.kind == wbValue_Memory) {
			v = wb_value_copy(p, v);
		}
		array_add(&final_args, v);
	}
	wbValue result = wb_emit_call(p, pt, callee, proc_value, final_args);

	if (e != nullptr && e->kind == Entity_Procedure && entity_has_deferred_procedure(e)) {
		wb_add_defer_proc(p, expr, e, args, result);
	}
	return result;
}

// or_else / or_return

// Splits `x` (a value or a multiple-result call) into the value part and the
// trailing ok/error part (lb_emit_try_lhs_rhs)
gb_internal void wb_emit_try_lhs_rhs(wbProcedure *p, Ast *arg, TypeAndValue const &tv, wbValue *lhs_, wbValue *rhs_) {
	wbValue lhs = wb_value_invalid();
	wbValue rhs = wb_value_invalid();
	wbValue value = wb_build_expr(p, arg);
	if (value.kind != wbValue_Invalid && is_type_tuple(value.type)) {
		isize n = value.type->Tuple.variables.count-1;
		if (n == 1) {
			lhs = wb_tuple_field(p, value, 0);
		} else if (n > 1) {
			// several values: gathered into a struct of the expression's type
			wbAddr lhs_addr = wb_add_temp(p, tv.type);
			Type *bt = base_type(tv.type);
			for (isize i = 0; i < n; i++) {
				Type *ft = nullptr;
				i64 offset = type_offset_of(bt, i, &ft);
				wb_addr_store(p, wb_addr_offset(lhs_addr, offset, ft), wb_tuple_field(p, value, i));
			}
			lhs = wb_value_memory(lhs_addr.index, lhs_addr.offset, tv.type);
		}
		rhs = wb_tuple_field(p, value, n);
	} else {
		rhs = value;
	}
	if (lhs_) *lhs_ = lhs;
	if (rhs_) *rhs_ = rhs;
}

// Pushes an i32 that is non-zero if `rhs` means "has a value" (true, or nil)
gb_internal void wb_push_try_has_value(wbProcedure *p, Ast *node, wbValue rhs) {
	if (is_type_boolean(rhs.type)) {
		wb_push(p, rhs);
		return;
	}
	wb_push(p, wb_emit_comp_against_nil(p, node, rhs, false));
}

gb_internal wbValue wb_build_or_else(wbProcedure *p, Ast *expr, Ast *arg, Ast *else_expr, TypeAndValue const &tv) {
	if (arg->state_flags & StateFlag_DirectiveWasFalse) {
		return wb_build_expr(p, else_expr);
	}
	wbValue lhs = {}, rhs = {};
	wb_emit_try_lhs_rhs(p, arg, tv, &lhs, &rhs);
	if (rhs.kind == wbValue_Invalid) {
		return wb_value_invalid();
	}
	Type *type = default_type(tv.type);
	bool diverging = is_diverging_expr(else_expr);
	bool has_value = type != nullptr && type_size_of(type) > 0 && lhs.kind != wbValue_Invalid;

	wbValType vt = has_value ? wb_valtype_of(type) : wbValType_Invalid;
	wbAddr res_mem = {};
	u32 res_local = 0;
	if (has_value) {
		if (vt == wbValType_Invalid) {
			res_mem = wb_add_temp(p, type);
		} else {
			res_local = wb_add_local(p, vt);
		}
	}
	auto store_result = [&](wbValue v) {
		if (!has_value) {
			return;
		}
		v = wb_emit_conv(p, v, type);
		if (vt == wbValType_Invalid) {
			wb_addr_store(p, res_mem, v);
		} else {
			wb_push(p, v);
			wb_local_set(p, res_local);
		}
	};

	wb_push_try_has_value(p, expr, rhs);
	wb_open_if(p);
	store_result(lhs);
	wb_else(p);
	if (diverging) {
		wb_build_expr(p, else_expr);
		wb_op(p, wbOp_unreachable);
	} else {
		store_result(wb_build_expr(p, else_expr));
	}
	wb_close(p);

	if (!has_value) {
		return wb_value_invalid();
	}
	if (vt == wbValType_Invalid) {
		return wb_value_memory(res_mem.index, res_mem.offset, type);
	}
	return wb_value_local(res_local, vt, type);
}

gb_internal wbValue wb_build_or_return(wbProcedure *p, Ast *expr, Ast *arg, TypeAndValue const &tv) {
	wbValue lhs = {}, rhs = {};
	wb_emit_try_lhs_rhs(p, arg, tv, &lhs, &rhs);
	if (rhs.kind == wbValue_Invalid) {
		return wb_value_invalid();
	}
	Type *pt = base_type(p->type);
	Type *results = pt->Proc.results;
	if (results == nullptr || results->Tuple.variables.count == 0) {
		wb_unsupported(p, expr, "or_return in a procedure without results");
		return wb_value_invalid();
	}
	TypeTuple *tuple = &results->Tuple;
	Entity *end_entity = tuple->variables[tuple->variables.count-1];

	wb_push_try_has_value(p, expr, rhs);
	wb_op(p, wbOp_i32_eqz);
	wb_open_if(p);
	{
		wbValue err = wb_value_fresh(p, wb_emit_conv(p, rhs, end_entity->type));
		auto values = array_make<wbValue>(temporary_allocator(), 0, tuple->variables.count);
		if (pt->Proc.has_named_results) {
			wb_addr_store(p, p->result_addrs[p->result_addrs.count-1], err);
			for (wbAddr addr : p->result_addrs) {
				array_add(&values, wb_value_fresh(p, wb_addr_load(p, addr)));
			}
		} else {
			if (tuple->variables.count != 1) {
				wb_unsupported(p, expr, "or_return with unnamed results");
				return wb_value_invalid();
			}
			array_add(&values, err);
		}
		wb_emit_return_values(p, values, false);
	}
	wb_close(p);

	if (tv.type != nullptr && lhs.kind != wbValue_Invalid) {
		return wb_emit_conv(p, lhs, tv.type);
	}
	return wb_value_invalid();
}

// Slicing

gb_internal wbValue wb_build_slice_expr(wbProcedure *p, Ast *expr) {
	ast_node(se, SliceExpr, expr);
	Type *result_type = type_of_expr(expr);
	Type *base_t = type_of_expr(se->expr);
	Type *bt = base_type(base_t);
	if (is_type_soa_struct(bt) || (bt->kind == Type_Pointer && is_type_soa_struct(type_deref(bt)))) {
		return wb_build_soa_slice_expr(p, expr);
	}

	// Source data pointer and length
	wbValue data = {};
	wbValue len = {};
	Type *elem_type = nullptr;
	if (bt->kind == Type_Pointer && is_type_array(type_deref(bt))) {
		wbValue ptr = wb_build_expr(p, se->expr);
		if (ptr.kind == wbValue_Invalid) return ptr;
		Type *at = base_type(type_deref(bt));
		data = wb_value_to_local(p, ptr);
		len = wb_value_const_int(t_int, at->Array.count);
		elem_type = at->Array.elem;
	} else if (bt->kind == Type_Array) {
		wbAddr addr = wb_build_addr(p, se->expr);
		if (addr.kind == wbAddr_Invalid) return wb_value_invalid();
		data = wb_addr_get_ptr(p, addr, t_rawptr);
		len = wb_value_const_int(t_int, bt->Array.count);
		elem_type = bt->Array.elem;
	} else if (bt->kind == Type_FixedCapacityDynamicArray ||
	           (bt->kind == Type_Pointer && is_type_fixed_capacity_dynamic_array(type_deref(bt)))) {
		wbAddr addr = {};
		if (bt->kind == Type_Pointer) {
			wbValue ptr = wb_build_expr(p, se->expr);
			if (ptr.kind == wbValue_Invalid) return ptr;
			addr = wb_addr_from_pointer(p, ptr, type_deref(bt));
			bt = base_type(type_deref(bt));
		} else {
			addr = wb_build_addr(p, se->expr);
		}
		if (addr.kind != wbAddr_Memory) return wb_value_invalid();
		data = wb_addr_get_ptr(p, addr, t_rawptr);
		len = wb_emit_load(p, addr.index, addr.offset + cast(i32)type_offset_of(bt, 1, nullptr), t_int);
		elem_type = bt->FixedCapacityDynamicArray.elem;
	} else if (bt->kind == Type_Slice || bt->kind == Type_DynamicArray || is_type_string(bt) ||
	           (bt->kind == Type_Pointer && (is_type_slice(type_deref(bt)) || is_type_dynamic_array(type_deref(bt)) || is_type_string(type_deref(bt))))) {
		wbValue s = wb_build_expr(p, se->expr);
		if (s.kind == wbValue_Invalid) return s;
		if (bt->kind == Type_Pointer) {
			// ^[]T etc. are sliced through the pointer
			Type *pointee = type_deref(bt);
			s = wb_addr_load(p, wb_addr_from_pointer(p, s, pointee));
			bt = base_type(pointee);
		}
		data = wb_emit_slice_data(p, s);
		len  = wb_emit_slice_len(p, s);
		if (bt->kind == Type_Slice) elem_type = bt->Slice.elem;
		else if (bt->kind == Type_DynamicArray) elem_type = bt->DynamicArray.elem;
		else if (is_type_string16(bt)) elem_type = t_u16;
		else elem_type = t_u8;
	} else if (bt->kind == Type_MultiPointer) {
		wbValue ptr = wb_build_expr(p, se->expr);
		if (ptr.kind == wbValue_Invalid) return ptr;
		data = wb_value_to_local(p, ptr);
		elem_type = bt->MultiPointer.elem;
		if (se->high == nullptr) {
			// [^]T[lo:] is a multi pointer (no bounds to check against)
			wbValue lo = se->low ? wb_emit_conv(p, wb_build_expr(p, se->low), t_int) : wb_value_const_int(t_int, 0);
			wbAddr d = wb_addr_from_pointer(p, data, elem_type);
			wbAddr a = wb_emit_elem_addr(p, d.index, d.offset, lo, elem_type);
			return wb_addr_get_ptr(p, a, result_type);
		}
	} else {
		wb_unsupported_type(p, expr, base_t);
		return wb_value_invalid();
	}
	if (data.kind == wbValue_Invalid) {
		return wb_value_invalid();
	}

	wbValue lo = se->low  ? wb_emit_conv(p, wb_build_expr(p, se->low),  t_int) : wb_value_const_int(t_int, 0);
	if (wb_expr_has_call(se->high)) lo = wb_value_fresh(p, lo);
	wbValue hi = se->high ? wb_emit_conv(p, wb_build_expr(p, se->high), t_int) : len;
	if (lo.kind == wbValue_Invalid || hi.kind == wbValue_Invalid) {
		return wb_value_invalid();
	}
	if (!wb_bounds_check_disabled(p)) {
		lo = wb_value_to_local(p, lo);
		hi = wb_value_to_local(p, hi);
		if (bt->kind == Type_MultiPointer) {
			wb_emit_multi_pointer_slice_bounds_check(p, se->open, lo, hi);
		} else {
			wb_emit_slice_bounds_check(p, se->open, lo, hi, len, se->low != nullptr);
		}
	}

	wbAddr result = wb_add_temp(p, result_type);
	// data + lo*size (data may be a constant address, e.g. when slicing a global array)
	wbAddr d = wb_addr_from_pointer(p, data, elem_type);
	wbAddr elem = wb_emit_elem_addr(p, d.index, d.offset, lo, elem_type);
	wb_emit_store(p, result.index, result.offset, wb_addr_get_ptr(p, elem, t_rawptr), t_rawptr);
	// hi - lo
	wb_push(p, hi);
	wb_push(p, lo);
	wb_op(p, wbOp_i32_sub);
	wbValue new_len = wb_pop_to_local(p, wbValType_i32, t_int);
	wb_emit_store(p, result.index, result.offset + cast(i32)build_context.int_size, new_len, t_int);
	return wb_value_memory(result.index, result.offset, result_type);
}

// Expressions

gb_internal wbValue wb_build_expr_internal(wbProcedure *p, Ast *expr);

// Selector call expressions `x->f(a)` are checked as `x.f(x, a)`, and `x` must
// only be evaluated once: the first evaluation is cached for the second
gb_internal wbValue wb_build_expr(wbProcedure *p, Ast *expr) {
	expr = unparen_expr(expr);
	if (expr->state_flags & StateFlag_SelectorCallExpr) {
		wbValue *pv = map_get(&p->selector_values, expr);
		if (pv != nullptr) {
			wbValue res = *pv;
			map_remove(&p->selector_values, expr);
			return res;
		}
		wbAddr *pa = map_get(&p->selector_addrs, expr);
		if (pa != nullptr) {
			wbAddr res = *pa;
			map_remove(&p->selector_addrs, expr);
			return wb_addr_load(p, res);
		}
	}
	wbValue res = wb_build_expr_internal(p, expr);
	if (expr->state_flags & StateFlag_SelectorCallExpr) {
		res = wb_value_fresh(p, res);
		map_set(&p->selector_values, expr, res);
	}
	return res;
}

gb_internal wbValue wb_build_expr_internal(wbProcedure *p, Ast *expr) {
	TypeAndValue tv = type_and_value_of_expr(expr);

	if (tv.value.kind != ExactValue_Invalid) {
		return wb_const(p, expr, tv.type, tv.value);
	}
	if (tv.mode == Addressing_Type) {
		// A type used as a value is its typeid
		return wb_typeid(tv.type);
	}

	switch (expr->kind) {
	case_ast_node(i, Ident, expr);
		Entity *e = entity_of_node(expr);
		if (e == nullptr) {
			wb_unsupported(p, expr, "identifier");
			return wb_value_invalid();
		}
		if (e->kind == Entity_Nil) {
			// untyped nil: a zero word, converted by wb_emit_conv at the use site
			if (is_type_untyped_nil(tv.type)) {
				wbValue v = {};
				v.kind = wbValue_Const;
				v.vt   = wbValType_i32;
				v.type = t_untyped_nil;
				return v;
			}
			ExactValue nil_value = {};
			return wb_const(p, expr, tv.type, nil_value);
		}
		if (e->kind == Entity_Variable) {
			wbAddr addr = wb_addr_of_entity(p, e, expr);
			if (addr.kind == wbAddr_Invalid) {
				return wb_value_invalid();
			}
			return wb_addr_load(p, addr);
		}
		if (e->kind == Entity_Procedure) {
			return wb_value_const_int(tv.type, wb_table_index(p->module, wb_procedure_for_entity(p->module, e)));
		}
		if (e->kind == Entity_Constant) {
			return wb_const(p, expr, tv.type, e->Constant.value);
		}
		wb_unsupported(p, expr, "identifier kind");
		return wb_value_invalid();
	case_end;

	case_ast_node(im, Implicit, expr);
		wbAddr addr = wb_build_addr(p, expr);
		if (addr.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		return wb_addr_load(p, addr);
	case_end;

	case_ast_node(se, SelectorExpr, expr);
		Ast *sel_node = unparen_expr(se->selector);
		TypeAndValue base_tv = type_and_value_of_expr(se->expr);
		if (base_tv.mode == Addressing_Invalid && sel_node->kind == Ast_Ident) {
			// pkg.name
			return wb_build_expr(p, sel_node);
		}
		if (base_tv.mode == Addressing_Type) {
			// Type.proc (pseudo field)
			Entity *e = entity_of_node(sel_node);
			if (e != nullptr && e->kind == Entity_Procedure) {
				return wb_value_const_int(tv.type, wb_table_index(p->module, wb_procedure_for_entity(p->module, e)));
			}
			wb_unsupported(p, expr, "selector on a type");
			return wb_value_invalid();
		}
		wbAddr addr = wb_build_addr(p, expr);
		if (addr.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		return wb_addr_load(p, addr);
	case_end;

	case_ast_node(ie, IndexExpr, expr);
		wbAddr addr = wb_build_addr(p, expr);
		if (addr.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		return wb_addr_load(p, addr);
	case_end;

	case_ast_node(ie, MatrixIndexExpr, expr);
		wbAddr addr = wb_build_addr(p, expr);
		if (addr.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		return wb_addr_load(p, addr);
	case_end;

	case_ast_node(de, DerefExpr, expr);
		wbAddr addr = wb_build_addr(p, expr);
		if (addr.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		return wb_addr_load(p, addr);
	case_end;

	case_ast_node(se, SliceExpr, expr);
		return wb_build_slice_expr(p, expr);
	case_end;

	case_ast_node(cl, CompoundLit, expr);
		wbAddr addr = wb_build_addr(p, expr);
		if (addr.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		return wb_addr_load(p, addr);
	case_end;

	case_ast_node(pl, ProcLit, expr);
		wbProcedure *callee = wb_procedure_for_proc_lit(p, expr);
		return wb_value_const_int(tv.type, wb_table_index(p->module, callee));
	case_end;

	case_ast_node(ue, UnaryExpr, expr);
		return wb_build_unary_expr(p, expr);
	case_end;

	case_ast_node(be, BinaryExpr, expr);
		return wb_build_binary_expr(p, expr);
	case_end;

	case_ast_node(se, SelectorCallExpr, expr);
		return wb_build_expr(p, se->call);
	case_end;

	case_ast_node(ce, CallExpr, expr);
		return wb_build_call_expr(p, expr);
	case_end;

	case_ast_node(tc, TypeCast, expr);
		wbValue x = wb_build_expr(p, tc->expr);
		if (tc->token.kind == Token_transmute) {
			return wb_emit_transmute(p, x, tv.type);
		}
		return wb_emit_conv(p, x, tv.type);
	case_end;

	case_ast_node(ac, AutoCast, expr);
		wbValue x = wb_build_expr(p, ac->expr);
		return wb_emit_conv(p, x, tv.type);
	case_end;

	case_ast_node(ta, TypeAssertion, expr);
		return wb_build_type_assertion(p, expr);
	case_end;

	case_ast_node(te, TernaryIfExpr, expr);
		Type *type = tv.type;
		if (is_type_untyped(type)) {
			type = default_type(type);
		}
		wbValType vt = wb_valtype_of(type);
		wbValue cond = wb_emit_conv(p, wb_build_expr(p, te->cond), t_bool);
		if (vt == wbValType_Invalid) {
			wbAddr res = wb_add_temp(p, type);
			wb_push(p, cond);
			wb_open_if(p);
			wb_addr_store(p, res, wb_emit_conv(p, wb_build_expr(p, te->x), type));
			wb_else(p);
			wb_addr_store(p, res, wb_emit_conv(p, wb_build_expr(p, te->y), type));
			wb_close(p);
			return wb_value_memory(res.index, res.offset, type);
		}
		u32 res = wb_add_local(p, vt);
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

	case_ast_node(oe, OrElseExpr, expr);
		return wb_build_or_else(p, expr, oe->x, oe->y, tv);
	case_end;

	case_ast_node(oe, OrReturnExpr, expr);
		return wb_build_or_return(p, expr, oe->expr, tv);
	case_end;

	case_ast_node(ob, OrBranchExpr, expr);
		// `x or_break` / `x or_continue`
		wbValue lhs = {}, rhs = {};
		wb_emit_try_lhs_rhs(p, ob->expr, tv, &lhs, &rhs);
		if (rhs.kind == wbValue_Invalid) {
			return wb_value_invalid();
		}
		Type *type = tv.type != nullptr ? default_type(tv.type) : nullptr;
		if (lhs.kind != wbValue_Invalid) {
			lhs = wb_value_fresh(p, wb_emit_conv(p, lhs, type));
		}
		wb_push_try_has_value(p, expr, rhs);
		wb_op(p, wbOp_i32_eqz);
		wb_open_if(p);
		{
			TokenKind kind = ob->token.kind == Token_or_break ? Token_break : Token_continue;
			wb_emit_branch(p, kind, ob->label, expr);
		}
		wb_close(p);
		return lhs;
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
