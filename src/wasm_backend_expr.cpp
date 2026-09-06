// Direct WebAssembly backend: expressions

gb_internal wbValue wb_build_call_expr(wbProcedure *p, Ast *expr);
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
	       is_type_cstring(t) || is_type_bit_set(t) || is_type_typeid(t);
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
		// A local declared in an enclosing procedure (closure capture) or a
		// variable we have not seen a declaration for
		wb_unsupported(p, node, "reference to a variable of an enclosing procedure");
		wbAddr a = {};
		return a;
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
		wb_unsupported(p, expr, "swizzle");
		return invalid;
	}
	Selection sel = lookup_field(tav.type, sel_node->Ident.interned, false);
	if (sel.entity == nullptr || sel.pseudo_field || sel.is_bit_field) {
		wb_unsupported(p, expr, "selector kind");
		return invalid;
	}

	wbAddr addr = wb_build_addr(p, se->expr);
	if (addr.kind == wbAddr_Invalid) {
		return invalid;
	}
	Type *type = tav.type;
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
		default:
			wb_unsupported_type(p, expr, type);
			return invalid;
		}
		Type *ft = nullptr;
		i64 offset = type_offset_of(bt, index, &ft);
		if (ft == nullptr) {
			wb_unsupported(p, expr, "field selection");
			return invalid;
		}
		addr = wb_addr_offset(addr, offset, ft);
		type = ft;
	}
	addr.type = type_of_expr(expr);
	return addr;
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
	} else {
		base = wb_build_addr(p, ie->expr);
	}
	if (base.kind == wbAddr_Invalid) {
		return invalid;
	}

	wbValue index = wb_build_expr(p, ie->index);
	if (index.kind == wbValue_Invalid) {
		return invalid;
	}

	switch (bt->kind) {
	case Type_Array:
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
		return wb_emit_elem_addr(p, base.index, base.offset, index, elem_type);
	}
	case Type_MultiPointer:
		return wb_emit_elem_addr(p, base.index, base.offset, index, elem_type);
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
		return wb_emit_elem_addr(p, data.index, 0, index, elem_type);
	}
	default:
		break;
	}
	wb_unsupported_type(p, expr, base_t);
	return invalid;
}

gb_internal wbAddr wb_build_addr(wbProcedure *p, Ast *expr) {
	expr = unparen_expr(expr);
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
		if (im->kind == Token_context && p->context_local >= 0) {
			return wb_addr_memory(cast(u32)p->context_local, 0, t_context);
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

	case_ast_node(de, DerefExpr, expr);
		wbValue ptr = wb_build_expr(p, de->expr);
		Type *type = type_of_expr(expr);
		if (is_type_soa_pointer(type_of_expr(de->expr))) {
			wb_unsupported(p, expr, "soa pointer");
			return invalid;
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

gb_internal void wb_build_compound_lit_array_elems(wbProcedure *p, Ast *expr, Slice<Ast *> const &elems, wbAddr dst, Type *elem_type, i64 min_value = 0) {
	i64 elem_size = type_size_of(elem_type);
	i64 index = 0;
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
					wb_addr_store(p, wb_addr_offset(dst, k*elem_size, elem_type), v);
				}
				index = hi;
			} else {
				index = exact_value_to_i64(fv->field->tav.value) - min_value;
				wbValue v = wb_emit_conv(p, wb_build_expr(p, fv->value), elem_type);
				wb_addr_store(p, wb_addr_offset(dst, index*elem_size, elem_type), v);
				index++;
			}
		} else {
			wbValue v = wb_emit_conv(p, wb_build_expr(p, elem), elem_type);
			wb_addr_store(p, wb_addr_offset(dst, index*elem_size, elem_type), v);
			index++;
		}
	}
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

	switch (bt->kind) {
	case Type_Struct: {
		if (bt->Struct.is_raw_union) {
			wb_unsupported(p, expr, "raw union literal");
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
	case Type_Array:
		wb_build_compound_lit_array_elems(p, expr, cl->elems, dst, bt->Array.elem);
		return;
	case Type_EnumeratedArray:
		wb_build_compound_lit_array_elems(p, expr, cl->elems, dst, bt->EnumeratedArray.elem, exact_value_to_i64(*bt->EnumeratedArray.min_value));
		return;
	case Type_Slice: {
		// Backing array on the stack
		Type *et = bt->Slice.elem;
		i64 count = gb_max(cl->max_count, cast(i64)cl->elems.count);
		Type *array_type = alloc_type_array(et, count);
		wbAddr backing = wb_add_temp(p, array_type);
		wb_addr_zero(p, backing);
		wb_build_compound_lit_array_elems(p, expr, cl->elems, backing, et);
		wbValue data = wb_addr_get_ptr(p, backing, alloc_type_multi_pointer(et));
		wb_emit_store(p, dst.index, dst.offset, data, t_rawptr);
		wb_emit_store(p, dst.index, dst.offset + cast(i32)build_context.int_size, wb_value_const_int(t_int, count), t_int);
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
		wbAddr addr = wb_build_addr(p, inner);
		if (addr.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		return wb_addr_get_ptr(p, addr, type);
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

// Comparison of aggregate operands (only `x == nil` style comparisons for now)
gb_internal wbValue wb_build_aggregate_compare(wbProcedure *p, Ast *expr, AstBinaryExpr *be, Type *operand_type, Type *result_type) {
	bool left_nil  = is_type_untyped_nil(be->left->tav.type);
	bool right_nil = is_type_untyped_nil(be->right->tav.type);
	if ((be->op.kind == Token_CmpEq || be->op.kind == Token_NotEq) && (left_nil || right_nil)) {
		Type *bt = base_type(operand_type);
		bool first_word = bt->kind == Type_Slice || bt->kind == Type_DynamicArray || is_type_string(bt) || bt->kind == Type_Union;
		if (first_word) {
			wbValue v = wb_build_expr(p, left_nil ? be->right : be->left);
			if (v.kind == wbValue_Invalid) {
				return v;
			}
			if (v.kind == wbValue_Memory) {
				i32 offset = 0;
				Type *word_type = t_rawptr;
				if (bt->kind == Type_Union) {
					// tag is after the variants
					offset = cast(i32)bt->Union.variant_block_size;
					word_type = union_tag_type(bt);
				}
				v = wb_emit_load(p, v.index, v.offset + offset, word_type);
			}
			wb_push(p, v);
			if (v.vt == wbValType_i64) {
				wb_op(p, wbOp_i64_eqz);
			} else {
				wb_op(p, wbOp_i32_eqz);
			}
			if (be->op.kind == Token_NotEq) {
				wb_op(p, wbOp_i32_eqz);
			}
			return wb_pop_to_local(p, wbValType_i32, result_type);
		}
	}
	wb_unsupported(p, expr, "comparison of non-scalar values");
	return wb_value_invalid();
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
	case Token_in:
	case Token_not_in:
		wb_unsupported(p, expr, "'in' operator");
		return wb_value_invalid();
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
		if (!wb_is_scalar(operand_type)) {
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
	return wb_emit_arith(p, expr, be->op.kind, left, right, operand_type, type);
}

// Calls

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
	default:
		wb_unsupported(p, call, "default parameter value kind");
		return wb_value_invalid();
	}
}

gb_internal wbValue wb_build_builtin_call(wbProcedure *p, Ast *expr, BuiltinProcId id) {
	ast_node(ce, CallExpr, expr);
	Type *type = expr->tav.type;
	if (type != nullptr && is_type_untyped(type)) {
		type = default_type(type);
	}
	wbValType vt = type != nullptr ? wb_valtype_of(type) : wbValType_Invalid;
	Slice<Ast *> const &args = ce->args;

	switch (id) {
	case BuiltinProc_len:
	case BuiltinProc_cap: {
		Type *at = base_type(type_of_expr(args[0]));
		if (is_type_pointer(at)) {
			at = base_type(type_deref(at));
		}
		if (at->kind == Type_Array) {
			return wb_value_const_int(t_int, at->Array.count);
		}
		wbValue s = wb_build_expr(p, args[0]);
		if (s.kind == wbValue_Invalid) {
			return s;
		}
		if (s.kind == wbValue_Local) {
			// pointer to slice/array (auto dereference)
			s = wb_value_memory(s.index, 0, type_deref(s.type));
		}
		if (at->kind == Type_Slice || is_type_string(at) || (at->kind == Type_DynamicArray && id == BuiltinProc_len)) {
			return wb_emit_slice_len(p, s);
		}
		if (at->kind == Type_DynamicArray) {
			return wb_emit_dynamic_array_cap(p, s);
		}
		break;
	}
	case BuiltinProc_raw_data: {
		Type *at = base_type(type_of_expr(args[0]));
		if (at->kind == Type_Slice || is_type_string(at) || at->kind == Type_DynamicArray) {
			wbValue s = wb_build_expr(p, args[0]);
			if (s.kind == wbValue_Invalid) return s;
			wbValue data = wb_emit_slice_data(p, s);
			data.type = type;
			return data;
		}
		if (at->kind == Type_Pointer && is_type_array(type_deref(at))) {
			wbValue ptr = wb_build_expr(p, args[0]);
			ptr.type = type;
			return ptr;
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
		wbValue x = wb_emit_conv(p, wb_build_expr(p, args[0]), type);
		if (x.kind == wbValue_Invalid) return x;
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
	case BuiltinProc_count_ones:
	case BuiltinProc_count_trailing_zeros:
	case BuiltinProc_count_leading_zeros: {
		wbValue x = wb_emit_conv(p, wb_build_expr(p, args[0]), type);
		if (x.kind == wbValue_Invalid) return x;
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
	case BuiltinProc_sqrt: {
		wbValue x = wb_emit_conv(p, wb_build_expr(p, args[0]), type);
		if (x.kind == wbValue_Invalid) return x;
		wb_push(p, x);
		wb_op(p, vt == wbValType_f32 ? wbOp_f32_sqrt : wbOp_f64_sqrt);
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
	default:
		break;
	}
	wb_unsupported(p, expr, "builtin procedure");
	return wb_value_invalid();
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
	if (pt->Proc.variadic || pt->Proc.c_vararg) {
		wb_unsupported(p, expr, "variadic procedure call");
		return wb_value_invalid();
	}

	// Direct call if the callee is a known procedure, otherwise through the table
	Entity *e = entity_of_node(ce->proc);
	wbProcedure *callee = nullptr;
	wbValue proc_value = {};
	if (e != nullptr && e->kind == Entity_Procedure) {
		callee = wb_procedure_for_entity(p->module, e);
	} else {
		proc_value = wb_build_expr(p, ce->proc);
		if (proc_value.kind == wbValue_Invalid) {
			return proc_value;
		}
		proc_value = wb_value_fresh(p, proc_value);
	}

	// Map arguments onto parameters: positional, then named, then defaults
	GB_ASSERT(ce->split_args != nullptr);
	isize param_count = pt->Proc.params ? pt->Proc.params->Tuple.variables.count : 0;
	auto arg_exprs = array_make<Ast *>(temporary_allocator(), param_count);
	for_array(i, arg_exprs) {
		arg_exprs[i] = nullptr;
	}
	Slice<Ast *> const &positional = ce->split_args->positional;
	if (positional.count > param_count) {
		wb_unsupported(p, expr, "procedure call argument count");
		return wb_value_invalid();
	}
	for_array(i, positional) {
		arg_exprs[i] = positional[i];
	}
	for (Ast *arg : ce->split_args->named) {
		ast_node(fv, FieldValue, arg);
		String name = fv->field->Ident.token.string;
		isize index = lookup_procedure_parameter(pt, name);
		if (index < 0) {
			wb_unsupported(p, arg, "named argument");
			return wb_value_invalid();
		}
		arg_exprs[index] = fv->value;
	}

	// Evaluate arguments in order (into fresh storage), then push them
	auto args = array_make<wbValue>(temporary_allocator(), 0, param_count);
	for (isize i = 0; i < param_count; i++) {
		Entity *param = pt->Proc.params->Tuple.variables[i];
		Type *param_type = param->type;
		wbValue v = {};
		if (arg_exprs[i] != nullptr) {
			v = wb_emit_conv(p, wb_build_expr(p, arg_exprs[i]), param_type);
		} else if (param->kind == Entity_Variable && has_parameter_value(param->Variable.param_value)) {
			v = wb_build_param_value(p, expr, param_type, param->Variable.param_value);
		} else if (param->kind == Entity_TypeName || param->kind == Entity_Constant) {
			// polymorphic parameters take no space
			continue;
		} else {
			wb_unsupported(p, expr, "missing argument");
			return wb_value_invalid();
		}
		if (v.kind == wbValue_Invalid) {
			return v;
		}
		if (v.kind == wbValue_Memory) {
			// aggregates are passed by pointer to a copy owned by the caller
			v = wb_value_copy(p, v);
		} else {
			bool later_call = false;
			for (isize j = i+1; j < param_count; j++) {
				if (arg_exprs[j] != nullptr && wb_expr_has_call(arg_exprs[j])) {
					later_call = true;
				}
			}
			if (later_call) {
				v = wb_value_fresh(p, v);
			}
		}
		array_add(&args, v);
	}

	bool sret = wb_uses_sret(pt);
	wbAddr result_addr = {};
	if (sret) {
		result_addr = wb_add_temp(p, pt->Proc.results);
		wb_push_address(p, result_addr.index, result_addr.offset);
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
		wbValType vt = wb_valtype_of(rt);
		return wb_pop_to_local(p, vt, rt);
	}
	return wb_value_invalid();
}

// Slicing

gb_internal wbValue wb_build_slice_expr(wbProcedure *p, Ast *expr) {
	ast_node(se, SliceExpr, expr);
	Type *result_type = type_of_expr(expr);
	Type *base_t = type_of_expr(se->expr);
	Type *bt = base_type(base_t);

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
	} else if (bt->kind == Type_Slice || bt->kind == Type_DynamicArray || is_type_string(bt)) {
		wbValue s = wb_build_expr(p, se->expr);
		if (s.kind == wbValue_Invalid) return s;
		data = wb_emit_slice_data(p, s);
		len  = wb_emit_slice_len(p, s);
		if (bt->kind == Type_Slice) elem_type = bt->Slice.elem;
		else if (bt->kind == Type_DynamicArray) elem_type = bt->DynamicArray.elem;
		else elem_type = t_u8;
	} else if (bt->kind == Type_MultiPointer) {
		wbValue ptr = wb_build_expr(p, se->expr);
		if (ptr.kind == wbValue_Invalid) return ptr;
		data = wb_value_to_local(p, ptr);
		elem_type = bt->MultiPointer.elem;
		if (se->high == nullptr) {
			// [^]T[lo:] is a multi pointer
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

	case_ast_node(te, TernaryWhenExpr, expr);
		TypeAndValue cond_tv = type_and_value_of_expr(te->cond);
		GB_ASSERT(cond_tv.value.kind == ExactValue_Bool);
		return wb_emit_conv(p, wb_build_expr(p, cond_tv.value.value_bool ? te->x : te->y), tv.type);
	case_end;
	}

	wb_unsupported(p, expr, "expression");
	return wb_value_invalid();
}
