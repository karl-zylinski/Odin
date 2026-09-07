// #soa containers (lb_addr_soa_variable and friends)
//
// The checker lays out an #soa struct as a regular struct: one array (`#soa[N]T`) or
// multi-pointer (`#soa[]T`, `#soa[dynamic]T`) per field of the element type, followed by
// `__$len` (slices and dynamic arrays), `__$cap` and `allocator` (dynamic arrays).
// `soa[i]` is a wbAddr_SoaVariable: a load gathers the element's fields into a temporary,
// a store scatters them, and `soa[i].x` is the plain memory location of one component.

// Number of per-field arrays/multi-pointers (the trailing fields are `__$len`, `__$cap`, `allocator`)
gb_internal isize wb_soa_field_count(Type *soa_type) {
	Type *t = base_type(soa_type);
	GB_ASSERT(is_type_soa_struct(t));
	isize n = t->Struct.fields.count;
	switch (t->Struct.soa_kind) {
	case StructSoa_Slice:   n -= 1; break;
	case StructSoa_Dynamic: n -= 3; break;
	default: break;
	}
	return n;
}

// Offset of component `i` within an element (a struct field or an array element)
gb_internal i64 wb_soa_elem_field_offset(Type *elem_type, isize i, Type **field_type) {
	Type *et = base_type(elem_type);
	if (et->kind == Type_Array) {
		*field_type = et->Array.elem;
		return i * type_size_of(et->Array.elem);
	}
	return type_offset_of(et, i, field_type);
}

gb_internal wbAddr wb_addr_soa_variable(wbAddr soa, wbValue index, Ast *index_expr) {
	GB_ASSERT(soa.kind == wbAddr_Memory);
	wbAddr a = soa;
	a.kind           = wbAddr_SoaVariable;
	a.soa_type       = soa.type;
	a.type           = base_type(soa.type)->Struct.soa_elem;
	a.soa_index      = index;
	a.soa_index_expr = index_expr;
	return a;
}

// The container an element address refers to
gb_internal wbAddr wb_soa_container(wbAddr addr) {
	return wb_addr_memory(addr.index, addr.offset, addr.soa_type);
}

// `len(soa)`, `cap(soa)` of a container in memory
gb_internal wbValue wb_soa_len(wbProcedure *p, wbAddr soa) {
	Type *t = base_type(soa.type);
	if (t->Struct.soa_kind == StructSoa_Fixed) {
		return wb_value_const_int(t_int, t->Struct.soa_count);
	}
	i64 offset = type_offset_of(t, wb_soa_field_count(t), nullptr);
	return wb_emit_load(p, soa.index, soa.offset + cast(i32)offset, t_int);
}

gb_internal wbValue wb_soa_cap(wbProcedure *p, wbAddr soa) {
	Type *t = base_type(soa.type);
	if (t->Struct.soa_kind != StructSoa_Dynamic) {
		return wb_soa_len(p, soa);
	}
	i64 offset = type_offset_of(t, wb_soa_field_count(t)+1, nullptr);
	return wb_emit_load(p, soa.index, soa.offset + cast(i32)offset, t_int);
}

// Address of component `field` of element `index`. A non-constant `field` is only
// possible for array elements (`soa[i][j]`), whose per-component arrays all have the same type.
gb_internal wbAddr wb_soa_field_elem_addr(wbProcedure *p, wbAddr soa, wbValue field, wbValue index) {
	Type *t = base_type(soa.type);
	Type *ft = nullptr;
	wbAddr base = {};
	if (field.kind == wbValue_Const) {
		i64 offset = type_offset_of(t, field.i, &ft);
		base = wb_addr_offset(soa, offset, ft);
	} else {
		ft = t->Struct.fields[0]->type;
		base = wb_emit_elem_addr(p, soa.index, soa.offset + cast(i32)type_offset_of(t, 0, nullptr), field, ft);
	}
	ft = base_type(ft);
	if (t->Struct.soa_kind == StructSoa_Fixed) {
		GB_ASSERT(ft->kind == Type_Array);
		return wb_emit_elem_addr(p, base.index, base.offset, index, ft->Array.elem);
	}
	GB_ASSERT(ft->kind == Type_MultiPointer);
	wbValue ptr = wb_emit_load(p, base.index, base.offset, ft);
	wbAddr d = wb_addr_from_pointer(p, ptr, ft->MultiPointer.elem);
	return wb_emit_elem_addr(p, d.index, d.offset, index, ft->MultiPointer.elem);
}

gb_internal void wb_soa_bounds_check(wbProcedure *p, wbAddr *addr) {
	if (addr->soa_index_expr == nullptr) {
		return;
	}
	Type *t = base_type(addr->soa_type);
	if (addr->soa_index.kind == wbValue_Const && t->Struct.soa_kind == StructSoa_Fixed) {
		// checked at compile time
		addr->soa_index_expr = nullptr;
		return;
	}
	addr->soa_index = wb_value_to_local(p, wb_emit_conv(p, addr->soa_index, t_int));
	wb_emit_bounds_check(p, ast_token(addr->soa_index_expr), addr->soa_index, wb_soa_len(p, wb_soa_container(*addr)));
	addr->soa_index_expr = nullptr;
}

// `soa[i]`: gathers the components into a temporary
gb_internal wbValue wb_soa_load(wbProcedure *p, wbAddr addr) {
	wb_soa_bounds_check(p, &addr);
	Type *elem = addr.type;
	wbAddr soa = wb_soa_container(addr);
	wbAddr tmp = wb_add_temp(p, elem);
	isize n = wb_soa_field_count(addr.soa_type);
	for (isize i = 0; i < n; i++) {
		Type *ft = nullptr;
		i64 offset = wb_soa_elem_field_offset(elem, i, &ft);
		wbAddr src = wb_soa_field_elem_addr(p, soa, wb_value_const_int(t_int, i), addr.soa_index);
		wb_addr_store(p, wb_addr_offset(tmp, offset, ft), wb_addr_load(p, src));
	}
	return wb_value_memory(tmp.index, tmp.offset, elem);
}

// `soa[i] = v`: scatters the components (`v` already has the element type)
gb_internal void wb_soa_store(wbProcedure *p, wbAddr addr, wbValue v) {
	wb_soa_bounds_check(p, &addr);
	Type *elem = addr.type;
	wbAddr soa = wb_soa_container(addr);
	wbAddr src = wb_value_to_addr(p, v);
	if (src.kind != wbAddr_Memory) {
		return;
	}
	isize n = wb_soa_field_count(addr.soa_type);
	for (isize i = 0; i < n; i++) {
		Type *ft = nullptr;
		i64 offset = wb_soa_elem_field_offset(elem, i, &ft);
		wbAddr dst = wb_soa_field_elem_addr(p, soa, wb_value_const_int(t_int, i), addr.soa_index);
		wb_addr_store(p, dst, wb_addr_load(p, wb_addr_offset(src, offset, ft)));
	}
}

// `&soa[i]`: an #soa pointer is the pair {^container, index}. Any other pointer type
// (which the checker does not produce for an element) points at a copy.
gb_internal wbValue wb_soa_get_ptr(wbProcedure *p, wbAddr addr, Type *ptr_type) {
	wb_soa_bounds_check(p, &addr);
	if (is_type_soa_pointer(ptr_type)) {
		wbAddr tmp = wb_add_temp(p, ptr_type);
		wbValue base = wb_addr_get_ptr(p, wb_soa_container(addr), t_rawptr);
		wb_emit_store(p, tmp.index, tmp.offset, base, t_rawptr);
		wb_emit_store(p, tmp.index, tmp.offset + cast(i32)build_context.int_size, wb_emit_conv(p, addr.soa_index, t_int), t_int);
		return wb_value_memory(tmp.index, tmp.offset, ptr_type);
	}
	wbValue v = wb_soa_load(p, addr);
	return wb_addr_get_ptr(p, wb_addr_memory(v.index, v.offset, v.type), ptr_type);
}

// The element an #soa pointer refers to (`p^`, `p.x`). The index was bounds checked when
// the pointer was made.
gb_internal wbAddr wb_addr_soa_variable_from_soa_ptr(wbProcedure *p, wbValue soa_ptr) {
	wbAddr invalid = {};
	Type *pt = base_type(soa_ptr.type);
	GB_ASSERT(pt->kind == Type_SoaPointer);
	wbAddr sp = wb_value_to_addr(p, soa_ptr);
	if (sp.kind != wbAddr_Memory) {
		return invalid;
	}
	Type *container_type = pt->SoaPointer.elem;
	wbValue base  = wb_emit_load(p, sp.index, sp.offset, alloc_type_pointer(container_type));
	wbValue index = wb_emit_load(p, sp.index, sp.offset + cast(i32)build_context.int_size, t_int);
	wbAddr container = wb_addr_from_pointer(p, base, container_type);
	if (container.kind != wbAddr_Memory) {
		return invalid;
	}
	return wb_addr_soa_variable(container, index, nullptr);
}

// `soa[i].x` (and deeper selections into that component)
gb_internal wbAddr wb_soa_field_addr(wbProcedure *p, Ast *expr, wbAddr addr, Selection const &sel) {
	GB_ASSERT(addr.kind == wbAddr_SoaVariable);
	wb_soa_bounds_check(p, &addr);
	i32 first = sel.index[0];
	Type *ft = nullptr;
	wb_soa_elem_field_offset(addr.type, first, &ft);
	wbAddr item = wb_soa_field_elem_addr(p, wb_soa_container(addr), wb_value_const_int(t_int, first), addr.soa_index);
	if (sel.index.count > 1) {
		Selection sub_sel = sel;
		sub_sel.index.data  += 1;
		sub_sel.index.count -= 1;
		item = wb_addr_deep_field(p, expr, item, ft, sub_sel);
	}
	return item;
}

// `soa[i][j]`: component `index` of an array element
gb_internal wbAddr wb_soa_elem_index_addr(wbProcedure *p, wbAddr addr, wbValue index, Ast *index_expr) {
	GB_ASSERT(addr.kind == wbAddr_SoaVariable);
	wb_soa_bounds_check(p, &addr);
	Type *et = base_type(addr.type);
	GB_ASSERT(et->kind == Type_Array);
	index = wb_emit_conv(p, index, t_int);
	wb_emit_bounds_check(p, ast_token(index_expr), index, wb_value_const_int(t_int, et->Array.count));
	return wb_soa_field_elem_addr(p, wb_soa_container(addr), index, addr.soa_index);
}

// `v.xy` of an #soa element of array type: a swizzle whose base is the container
gb_internal wbAddr wb_soa_swizzle_addr(wbProcedure *p, wbAddr addr, Type *type, u8 count, u8 const *indices) {
	GB_ASSERT(addr.kind == wbAddr_SoaVariable);
	wb_soa_bounds_check(p, &addr);
	wbAddr a = addr;
	a.kind = wbAddr_Swizzle;
	a.type = type;
	a.swizzle_count = count;
	for (u8 i = 0; i < count; i++) {
		a.swizzle_indices[i] = indices[i];
	}
	return a;
}

gb_internal wbValue wb_soa_swizzle_load(wbProcedure *p, wbAddr addr) {
	Type *elem = base_type(addr.type)->Array.elem;
	i64 elem_size = type_size_of(elem);
	wbAddr soa = wb_soa_container(addr);
	wbAddr tmp = wb_add_temp(p, addr.type);
	for (u8 i = 0; i < addr.swizzle_count; i++) {
		wbAddr src = wb_soa_field_elem_addr(p, soa, wb_value_const_int(t_int, addr.swizzle_indices[i]), addr.soa_index);
		wb_emit_copy(p, tmp.index, tmp.offset + cast(i32)(i*elem_size), src.index, src.offset, elem);
	}
	return wb_value_memory(tmp.index, tmp.offset, addr.type);
}

gb_internal void wb_soa_swizzle_store(wbProcedure *p, wbAddr addr, wbValue v) {
	Type *elem = base_type(addr.type)->Array.elem;
	i64 elem_size = type_size_of(elem);
	wbAddr soa = wb_soa_container(addr);
	v = wb_value_copy(p, v);
	for (u8 i = 0; i < addr.swizzle_count; i++) {
		wbAddr dst = wb_soa_field_elem_addr(p, soa, wb_value_const_int(t_int, addr.swizzle_indices[i]), addr.soa_index);
		wb_emit_copy(p, dst.index, dst.offset, v.index, v.offset + cast(i32)(i*elem_size), elem);
	}
}

// The container of a range/slice/len operand: a variable in place, a pointer's target,
// or any other value copied to a temporary
gb_internal wbAddr wb_soa_container_of_expr(wbProcedure *p, Ast *expr) {
	wbAddr invalid = {};
	Type *type = type_of_expr(expr);
	if (is_type_pointer(type)) {
		wbValue ptr = wb_build_expr(p, expr);
		if (ptr.kind == wbValue_Invalid) {
			return invalid;
		}
		return wb_addr_from_pointer(p, ptr, type_deref(type));
	}
	TypeAndValue tav = type_and_value_of_expr(expr);
	if (tav.mode == Addressing_Variable) {
		wbAddr addr = wb_addr_resolve_map(p, wb_build_addr(p, expr));
		if (addr.kind == wbAddr_Memory || addr.kind == wbAddr_Invalid) {
			return addr;
		}
	}
	wbValue v = wb_build_expr(p, expr);
	if (v.kind == wbValue_Invalid) {
		return invalid;
	}
	return wb_value_to_addr(p, v);
}

// `soa[lo:hi]` of any #soa container: an #soa slice
gb_internal wbValue wb_build_soa_slice_expr(wbProcedure *p, Ast *expr) {
	ast_node(se, SliceExpr, expr);
	Type *result_type = type_of_expr(expr);
	wbAddr soa = wb_soa_container_of_expr(p, se->expr);
	if (soa.kind != wbAddr_Memory) {
		return wb_value_invalid();
	}
	Type *t = base_type(soa.type);
	wbValue len = wb_value_to_local(p, wb_soa_len(p, soa));
	wbValue lo = se->low  ? wb_emit_conv(p, wb_build_expr(p, se->low),  t_int) : wb_value_const_int(t_int, 0);
	if (wb_expr_has_call(se->high)) lo = wb_value_fresh(p, lo);
	wbValue hi = se->high ? wb_emit_conv(p, wb_build_expr(p, se->high), t_int) : len;
	if (lo.kind == wbValue_Invalid || hi.kind == wbValue_Invalid) {
		return wb_value_invalid();
	}
	lo = wb_value_to_local(p, lo);
	hi = wb_value_to_local(p, hi);
	if (!wb_bounds_check_disabled(p)) {
		wb_emit_slice_bounds_check(p, se->open, lo, hi, len, se->low != nullptr);
	}

	wbAddr result = wb_add_temp(p, result_type);
	wb_addr_zero(p, result);
	isize n = wb_soa_field_count(t);
	for (isize i = 0; i < n; i++) {
		wbAddr field = wb_addr_field(result, i);
		wbAddr elem = wb_soa_field_elem_addr(p, soa, wb_value_const_int(t_int, i), lo);
		wb_addr_store(p, field, wb_addr_get_ptr(p, elem, field.type));
	}
	wb_push(p, hi);
	wb_push(p, lo);
	wb_op(p, wbOp_i32_sub);
	wbValue new_len = wb_pop_to_local(p, wbValType_i32, t_int);
	wb_addr_store(p, wb_addr_field(result, n), new_len);
	return wb_value_memory(result.index, result.offset, result_type);
}

// `soa_zip(a=x, b=y)`: an #soa slice over the given slices, as long as the shortest
gb_internal wbValue wb_build_soa_zip(wbProcedure *p, Ast *expr) {
	ast_node(ce, CallExpr, expr);
	Type *result_type = type_of_expr(expr);
	GB_ASSERT(is_type_soa_struct(result_type));
	auto slices = slice_make<wbValue>(temporary_allocator(), ce->args.count);
	for_array(i, slices) {
		Ast *arg = ce->args[i];
		if (arg->kind == Ast_FieldValue) {
			arg = arg->FieldValue.value;
		}
		slices[i] = wb_value_copy(p, wb_build_expr(p, arg));
		if (slices[i].kind != wbValue_Memory) {
			return wb_value_invalid();
		}
	}
	wbValue len = wb_value_to_local(p, wb_emit_slice_len(p, slices[0]));
	for (isize i = 1; i < slices.count; i++) {
		wbValue other = wb_emit_slice_len(p, slices[i]);
		wb_push(p, other);
		wb_push(p, len);
		wb_push(p, other);
		wb_push(p, len);
		wb_op(p, wbOp_i32_lt_s);
		wb_op(p, wbOp_select);
		len = wb_pop_to_local(p, wbValType_i32, t_int);
	}
	wbAddr result = wb_add_temp(p, result_type);
	wb_addr_zero(p, result);
	for_array(i, slices) {
		wbAddr field = wb_addr_field(result, i);
		wbValue data = wb_emit_slice_data(p, slices[i]);
		data.type = field.type;
		wb_addr_store(p, field, data);
	}
	wb_addr_store(p, wb_addr_field(result, slices.count), len);
	return wb_value_memory(result.index, result.offset, result_type);
}

// `soa_unzip(s)`: the slices of an #soa slice
gb_internal wbValue wb_build_soa_unzip(wbProcedure *p, Ast *expr) {
	ast_node(ce, CallExpr, expr);
	Type *result_type = type_of_expr(expr);
	wbValue s = wb_value_copy(p, wb_build_expr(p, ce->args[0]));
	if (s.kind != wbValue_Memory) {
		return wb_value_invalid();
	}
	wbAddr soa = wb_addr_memory(s.index, s.offset, s.type);
	Type *t = base_type(s.type);
	GB_ASSERT(is_type_soa_struct(t) && t->Struct.soa_kind == StructSoa_Slice);
	wbValue len = wb_value_to_local(p, wb_soa_len(p, soa));

	wbAddr result = wb_add_temp(p, result_type);
	isize n = wb_soa_field_count(t);
	auto fill = [&](wbAddr dst, isize i) {
		Type *ft = nullptr;
		i64 offset = type_offset_of(t, i, &ft);
		wbValue data = wb_emit_load(p, soa.index, soa.offset + cast(i32)offset, ft);
		wb_emit_store(p, dst.index, dst.offset, data, t_rawptr);
		wb_emit_store(p, dst.index, dst.offset + cast(i32)build_context.int_size, len, t_int);
	};
	if (is_type_tuple(result_type)) {
		for (isize i = 0; i < n; i++) {
			fill(wb_addr_field(result, i), i);
		}
	} else {
		GB_ASSERT(is_type_slice(result_type));
		fill(result, 0);
	}
	return wb_value_memory(result.index, result.offset, result_type);
}

// `intrinsics.soa_copy_from_slice(array, offset, args)`: stores the elements of the
// slice `args` into the #soa dynamic array starting at element `offset` (one loop per
// component, as the LLVM backend does)
gb_internal void wb_build_soa_copy_from_slice(wbProcedure *p, Ast *expr) {
	ast_node(ce, CallExpr, expr);
	wbValue ptr    = wb_build_expr(p, ce->args[0]);
	wbValue offset = wb_emit_conv(p, wb_build_expr(p, ce->args[1]), t_int);
	wbValue args   = wb_value_copy(p, wb_build_expr(p, ce->args[2]));
	if (ptr.kind == wbValue_Invalid || offset.kind == wbValue_Invalid || args.kind != wbValue_Memory) {
		return;
	}
	Type *array_type = base_type(type_deref(ptr.type));
	GB_ASSERT(is_type_soa_dynamic_array(array_type));
	Type *elem = array_type->Struct.soa_elem;
	i64 elem_size = type_size_of(elem);
	isize field_count = wb_soa_field_count(array_type);
	if (field_count == 0) {
		return;
	}

	ptr = wb_value_to_local(p, ptr);
	offset = wb_value_to_local(p, offset);
	wbAddr soa = wb_addr_memory(ptr.index, 0, array_type);
	wbValue arg_ptr = wb_value_to_local(p, wb_emit_slice_data(p, args));
	wbValue arg_len = wb_emit_slice_len(p, args);

	// max_len = min(arg_len, len(array) - offset)
	wbValue soa_len = wb_soa_len(p, soa);
	wb_push(p, soa_len);
	wb_push(p, offset);
	wb_op(p, wbOp_i32_sub);
	wbValue max_soa_len = wb_pop_to_local(p, wbValType_i32, t_int);
	wb_push(p, arg_len);
	wb_push(p, max_soa_len);
	wb_push(p, arg_len);
	wb_push(p, max_soa_len);
	wb_op(p, wbOp_i32_lt_s);
	wb_op(p, wbOp_select);
	wbValue max_len = wb_pop_to_local(p, wbValType_i32, t_int);

	for (isize i = 0; i < field_count; i++) {
		Type *ft = nullptr;
		i64 src_offset = wb_soa_elem_field_offset(elem, i, &ft);
		i64 field_size = type_size_of(ft);
		if (field_size == 0) {
			continue;
		}
		Type *mpt = nullptr;
		i64 dst_field_offset = type_offset_of(array_type, i, &mpt);
		// dst = array.field + offset*size
		wb_push(p, wb_emit_load(p, soa.index, soa.offset + cast(i32)dst_field_offset, mpt));
		wb_push(p, offset);
		wb_i32_const(p, cast(i32)field_size);
		wb_op(p, wbOp_i32_mul);
		wb_op(p, wbOp_i32_add);
		wbValue dst = wb_pop_to_local(p, wbValType_i32, t_rawptr);
		// src = &args[0].field
		wbValue src = wb_emit_ptr_add(p, arg_ptr, src_offset);
		if (src.index == arg_ptr.index) {
			wb_push(p, src);
			src = wb_pop_to_local(p, wbValType_i32, t_rawptr);
		}

		u32 j = wb_add_local(p, wbValType_i32);
		wb_i32_const(p, 0);
		wb_local_set(p, j);
		u32 break_depth = wb_open_block(p);
		u32 loop_depth  = wb_open_loop(p);
		wb_local_get(p, j);
		wb_push(p, max_len);
		wb_op(p, wbOp_i32_ge_s);
		wb_br_if(p, break_depth);

		wb_emit_copy(p, dst.index, 0, src.index, 0, ft);

		wb_push(p, dst);
		wb_i32_const(p, cast(i32)field_size);
		wb_op(p, wbOp_i32_add);
		wb_local_set(p, dst.index);
		wb_push(p, src);
		wb_i32_const(p, cast(i32)elem_size);
		wb_op(p, wbOp_i32_add);
		wb_local_set(p, src.index);
		wb_local_get(p, j);
		wb_i32_const(p, 1);
		wb_op(p, wbOp_i32_add);
		wb_local_set(p, j);
		wb_br(p, loop_depth);
		wb_close(p);
		wb_close(p);
	}
}

// `for v, i in soa`: `v` denotes the element in place (`&v` is an #soa pointer)
gb_internal void wb_build_range_soa(wbProcedure *p, AstRangeStmt *rs, Ast *node, Ast *val0, Ast *val1) {
	Ast *range_expr = unparen_expr(rs->expr);
	wbAddr soa = wb_soa_container_of_expr(p, range_expr);
	if (soa.kind != wbAddr_Memory) {
		return;
	}
	wbValue count = wb_value_to_local(p, wb_soa_len(p, soa));

	u32 index = wb_add_local(p, wbValType_i32);
	if (rs->reverse) {
		wb_push(p, count);
		wb_i32_const(p, 1);
		wb_op(p, wbOp_i32_sub);
	} else {
		wb_i32_const(p, 0);
	}
	wb_local_set(p, index);

	u32 break_depth = wb_open_block(p);
	u32 loop_depth  = wb_open_loop(p);

	wb_local_get(p, index);
	if (rs->reverse) {
		wb_i32_const(p, 0);
		wb_op(p, wbOp_i32_lt_s);
	} else {
		wb_push(p, count);
		wb_op(p, wbOp_i32_ge_s);
	}
	wb_br_if(p, break_depth);

	wb_open_scope(p);
	wbValue index_value = wb_value_local(index, wbValType_i32, t_int);
	if (val0 != nullptr && !is_blank_ident(val0)) {
		Entity *e = entity_of_node(val0);
		if (e != nullptr) {
			map_set(&p->variables, e, wb_addr_soa_variable(soa, index_value, nullptr));
		}
	}
	wb_store_range_val(p, val1, index_value);

	u32 continue_depth = wb_open_block(p);
	wb_push_label(p, rs->label, break_depth, continue_depth, true);
	wb_build_stmt(p, rs->body);
	wb_pop_label(p);
	wb_close(p); // continue
	wb_close_scope(p);

	wb_local_get(p, index);
	wb_i32_const(p, 1);
	wb_op(p, rs->reverse ? wbOp_i32_sub : wbOp_i32_add);
	wb_local_set(p, index);
	wb_br(p, loop_depth);
	wb_close(p); // loop
	wb_close(p); // break
}
