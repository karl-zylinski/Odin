// Maps: the Map_Info/Map_Cell_Info tables the runtime's `__dynamic_map_*`
// procedures work with, the generated key hasher/equality procedures they
// reference, and map element access (the wasm equivalents of
// lb_gen_map_info_ptr, lb_hasher_proc_for_type, lb_internal_dynamic_map_get_ptr, ...)

gb_internal void    wb_push_label(wbProcedure *p, Ast *label, u32 break_depth, u32 continue_depth, bool is_loop, bool is_switch, u32 fall_depth);
gb_internal void    wb_pop_label(wbProcedure *p);
gb_internal void    wb_store_range_val(wbProcedure *p, Ast *val, wbValue value);
gb_internal void    wb_bind_range_ref(wbProcedure *p, Ast *val, wbAddr elem);
gb_internal bool    wb_range_val_is_ref(Ast *val);
gb_internal wbValue wb_tuple_field(wbProcedure *p, wbValue tuple, isize index);
gb_internal void    wb_build_stmt(wbProcedure *p, Ast *node);

gb_internal String wb_gen_name_from_type(char const *prefix, Type *type) {
	gbString str = gb_string_make(permanent_allocator(), prefix);
	str = gb_string_appendc(str, "$$");
	gbString ct = temp_canonical_string(type);
	str = gb_string_append_length(str, ct, gb_string_length(ct));
	return make_string(cast(u8 const *)str, gb_string_length(str));
}

// Generated procedures

gb_internal wbProcedure *wb_gen_proc_for_type(wbModule *m, char const *prefix, Type *type, Type *proc_type, wbProcGen gen) {
	String name = wb_gen_name_from_type(prefix, type);
	wbProcedure **found = string_map_get(&m->gen_procs, name);
	if (found) {
		return *found;
	}
	wbProcedure *p = wb_alloc_procedure(m, name);
	p->type     = proc_type;
	p->gen      = gen;
	p->gen_type = type;
	wbFuncType ft = {};
	wb_functype_of_proc(m, proc_type, &ft);
	p->type_index = wb_add_functype(m, ft);
	for (wbValType vt : ft.results) {
		array_add(&p->results, vt);
	}
	string_map_set(&m->gen_procs, name, p);
	array_add(&m->procedures, p);
	array_add(&m->work_queue, p);
	return p;
}

// `proc "contextless" (data: rawptr, seed: uintptr) -> uintptr`
gb_internal wbProcedure *wb_hasher_proc_for_type(wbModule *m, Type *type) {
	type = core_type(type);
	GB_ASSERT(t_hasher_proc != nullptr);
	return wb_gen_proc_for_type(m, "__$hasher", type, t_hasher_proc, wbProcGen_Hasher);
}

// `proc "contextless" (a, b: rawptr) -> bool`
gb_internal wbProcedure *wb_equal_proc_for_type(wbModule *m, Type *type) {
	type = base_type(type);
	GB_ASSERT(t_equal_proc != nullptr);
	return wb_gen_proc_for_type(m, "__$equal", type, t_equal_proc, wbProcGen_Equal);
}

gb_internal wbValue wb_emit_gen_call(wbProcedure *p, wbProcedure *callee, wbValue a, wbValue b) {
	auto args = array_make<wbValue>(temporary_allocator(), 2);
	args[0] = a;
	args[1] = b;
	return wb_emit_call(p, callee->type, callee, wb_value_invalid(), args);
}

// local[base] + offset as a rawptr local
gb_internal wbValue wb_emit_ptr_add(wbProcedure *p, wbValue ptr, i64 offset) {
	if (offset == 0) {
		return ptr;
	}
	wb_push(p, ptr);
	wb_i32_const(p, cast(i32)offset);
	wb_op(p, wbOp_i32_add);
	return wb_pop_to_local(p, wbValType_i32, t_rawptr);
}

// The hash of the value of `type` at `data`, folded into `seed` (lb_hasher_proc_for_type)
gb_internal wbValue wb_emit_hash_value(wbProcedure *p, Type *type, wbValue data, wbValue seed) {
	wbModule *m = p->module;
	type = core_type(type);
	TEMPORARY_ALLOCATOR_GUARD();

	if (is_type_simple_compare(type)) {
		auto args = array_make<wbValue>(temporary_allocator(), 3);
		args[0] = data;
		args[1] = seed;
		args[2] = wb_value_const_int(t_int, type_size_of(type));
		return wb_emit_runtime_call(p, "default_hasher", args);
	}

	switch (type->kind) {
	case Type_Struct: {
		type_set_offsets(type);
		for_array(i, type->Struct.fields) {
			Entity *field = type->Struct.fields[i];
			wbValue ptr = wb_emit_ptr_add(p, data, type->Struct.offsets[i]);
			seed = wb_emit_gen_call(p, wb_hasher_proc_for_type(m, field->type), ptr, seed);
		}
		return seed;
	}
	case Type_Union: {
		if (is_type_union_maybe_pointer(type)) {
			return wb_emit_gen_call(p, wb_hasher_proc_for_type(m, type->Union.variants[0]), data, seed);
		}
		// res = seed; for each variant: if tag == i { res = hash_variant(data, res) }
		wbValue res = wb_value_to_local(p, seed);
		if (res.kind == wbValue_Const) {
			wb_push(p, res);
			res = wb_pop_to_local(p, res.vt, res.type);
		}
		Type *tag_type = union_tag_type(type);
		wbValue tag = wb_emit_load(p, data.index, wb_union_tag_offset(type), tag_type);
		for (Type *v : type->Union.variants) {
			wb_push(p, tag);
			wb_push(p, wb_value_const_int(tag_type, union_variant_index_checked(type, v)));
			wb_op(p, tag.vt == wbValType_i64 ? wbOp_i64_eq : wbOp_i32_eq);
			wb_open_if(p);
			wbValue h = wb_emit_gen_call(p, wb_hasher_proc_for_type(m, v), data, res);
			wb_push(p, h);
			wb_local_set(p, res.index);
			wb_close(p);
		}
		return res;
	}
	case Type_Array:
	case Type_EnumeratedArray: {
		Type *elem = type->kind == Type_Array ? type->Array.elem : type->EnumeratedArray.elem;
		i64 count  = type->kind == Type_Array ? type->Array.count : type->EnumeratedArray.count;
		i64 elem_size = type_size_of(elem);
		wbProcedure *elem_hasher = wb_hasher_proc_for_type(m, elem);

		wb_push(p, seed);
		wbValue res = wb_pop_to_local(p, seed.vt, seed.type);
		wb_i32_const(p, 0);
		wbValue i = wb_pop_to_local(p, wbValType_i32, t_int);

		u32 break_depth = wb_open_block(p);
		u32 loop_depth = wb_open_loop(p);
		wb_push(p, i);
		wb_i32_const(p, cast(i32)count);
		wb_op(p, wbOp_i32_ge_s);
		wb_br_if(p, break_depth);

		wb_push(p, data);
		wb_push(p, i);
		wb_i32_const(p, cast(i32)elem_size);
		wb_op(p, wbOp_i32_mul);
		wb_op(p, wbOp_i32_add);
		wbValue ptr = wb_pop_to_local(p, wbValType_i32, t_rawptr);
		wbValue h = wb_emit_gen_call(p, elem_hasher, ptr, res);
		wb_push(p, h);
		wb_local_set(p, res.index);

		wb_push(p, i);
		wb_i32_const(p, 1);
		wb_op(p, wbOp_i32_add);
		wb_local_set(p, i.index);
		wb_br(p, loop_depth);
		wb_close(p);
		wb_close(p);
		return res;
	}
	default:
		break;
	}

	if (is_type_cstring(type)) {
		auto args = array_make<wbValue>(temporary_allocator(), 2);
		args[0] = data;
		args[1] = seed;
		return wb_emit_runtime_call(p, "default_hasher_cstring", args);
	} else if (is_type_string(type)) {
		auto args = array_make<wbValue>(temporary_allocator(), 2);
		args[0] = data;
		args[1] = seed;
		return wb_emit_runtime_call(p, "default_hasher_string", args);
	} else if (is_type_float(type)) {
		auto args = array_make<wbValue>(temporary_allocator(), 2);
		args[0] = wb_emit_conv(p, wb_emit_load(p, data.index, 0, type), t_f64);
		args[1] = seed;
		return wb_emit_runtime_call(p, "default_hasher_f64", args);
	} else if (is_type_complex(type) || is_type_quaternion(type)) {
		isize n = is_type_complex(type) ? 2 : 4;
		Type *ft = base_complex_elem_type(type);
		i64 fsize = type_size_of(ft);
		auto args = array_make<wbValue>(temporary_allocator(), n+1);
		for (isize i = 0; i < n; i++) {
			args[i] = wb_emit_conv(p, wb_emit_load(p, data.index, cast(i32)(i*fsize), ft), t_f64);
		}
		args[n] = seed;
		return wb_emit_runtime_call(p, n == 2 ? "default_hasher_complex128" : "default_hasher_quaternion256", args);
	}

	wb_unsupported_type(p, nullptr, type);
	return seed;
}

gb_internal void wb_build_hasher_body(wbProcedure *p) {
	wbValue data = wb_value_local(0, wbValType_i32, t_rawptr);
	wbValue seed = wb_value_local(1, wb_valtype_of(t_uintptr), t_uintptr);
	wbValue res = wb_emit_hash_value(p, p->gen_type, data, seed);
	if (res.kind == wbValue_Invalid) {
		return;
	}
	auto values = array_make<wbValue>(temporary_allocator(), 1);
	values[0] = res;
	wb_emit_return_values(p, values, false);
}

// Whether the values of `type` at the two pointers are equal, as an i32 value
gb_internal wbValue wb_emit_equal_at(wbProcedure *p, Type *type, wbValue lhs, wbValue rhs) {
	Type *bt = base_type(type);
	if (wb_is_scalar(type) || wb_is_int128(type)) {
		wbValue l = wb_addr_load(p, wb_addr_memory(lhs.index, 0, type));
		wbValue r = wb_addr_load(p, wb_addr_memory(rhs.index, 0, type));
		return wb_emit_arith(p, nullptr, Token_CmpEq, l, r, type, t_bool);
	}
	if ((is_type_string(bt) && !is_type_cstring(bt)) || is_type_simple_compare(type)) {
		wbValue l = wb_value_memory(lhs.index, 0, type);
		wbValue r = wb_value_memory(rhs.index, 0, type);
		return wb_emit_aggregate_compare(p, nullptr, Token_CmpEq, l, r, type, t_bool);
	}
	return wb_emit_gen_call(p, wb_equal_proc_for_type(p->module, type), lhs, rhs);
}

gb_internal void wb_emit_return_bool(wbProcedure *p, bool b) {
	auto values = array_make<wbValue>(temporary_allocator(), 1);
	values[0] = wb_value_const_int(t_bool, b ? 1 : 0);
	wb_emit_return_values(p, values, false);
}

// `if (!cond) return false`
gb_internal void wb_emit_return_false_unless(wbProcedure *p, wbValue cond) {
	wb_push(p, cond);
	wb_op(p, wbOp_i32_eqz);
	wb_open_if(p);
	wb_emit_return_bool(p, false);
	wb_close(p);
}

gb_internal void wb_build_equal_body(wbProcedure *p) {
	Type *type = p->gen_type;
	wbValue lhs = wb_value_local(0, wbValType_i32, t_rawptr);
	wbValue rhs = wb_value_local(1, wbValType_i32, t_rawptr);

	// the same object is equal to itself
	wb_push(p, lhs);
	wb_push(p, rhs);
	wb_op(p, wbOp_i32_eq);
	wb_open_if(p);
	wb_emit_return_bool(p, true);
	wb_close(p);

	switch (type->kind) {
	case Type_Struct: {
		type_set_offsets(type);
		for_array(i, type->Struct.fields) {
			Entity *field = type->Struct.fields[i];
			if (type_size_of(field->type) == 0) {
				continue;
			}
			wbValue l = wb_emit_ptr_add(p, lhs, type->Struct.offsets[i]);
			wbValue r = wb_emit_ptr_add(p, rhs, type->Struct.offsets[i]);
			wb_emit_return_false_unless(p, wb_emit_equal_at(p, field->type, l, r));
		}
		wb_emit_return_bool(p, true);
		return;
	}
	case Type_Union: {
		if (type_size_of(type) == 0) {
			wb_emit_return_bool(p, true);
			return;
		}
		if (is_type_union_maybe_pointer(type)) {
			auto values = array_make<wbValue>(temporary_allocator(), 1);
			values[0] = wb_emit_equal_at(p, type->Union.variants[0], lhs, rhs);
			wb_emit_return_values(p, values, false);
			return;
		}
		Type *tag_type = union_tag_type(type);
		wbValue ltag = wb_emit_load(p, lhs.index, wb_union_tag_offset(type), tag_type);
		wbValue rtag = wb_emit_load(p, rhs.index, wb_union_tag_offset(type), tag_type);
		wb_emit_return_false_unless(p, wb_emit_arith(p, nullptr, Token_CmpEq, ltag, rtag, tag_type, t_bool));
		if (type->Union.kind != UnionType_no_nil) {
			// both nil
			wb_push(p, ltag);
			wb_op(p, ltag.vt == wbValType_i64 ? wbOp_i64_eqz : wbOp_i32_eqz);
			wb_open_if(p);
			wb_emit_return_bool(p, true);
			wb_close(p);
		}
		for (Type *v : type->Union.variants) {
			wb_push(p, ltag);
			wb_push(p, wb_value_const_int(tag_type, union_variant_index_checked(type, v)));
			wb_op(p, ltag.vt == wbValType_i64 ? wbOp_i64_eq : wbOp_i32_eq);
			wb_open_if(p);
			auto values = array_make<wbValue>(temporary_allocator(), 1);
			values[0] = wb_emit_equal_at(p, v, lhs, rhs);
			wb_emit_return_values(p, values, false);
			wb_close(p);
		}
		wb_emit_return_bool(p, false);
		return;
	}
	case Type_Array:
	case Type_EnumeratedArray: {
		Type *elem = type->kind == Type_Array ? type->Array.elem : type->EnumeratedArray.elem;
		i64 count  = type->kind == Type_Array ? type->Array.count : type->EnumeratedArray.count;
		i64 elem_size = type_size_of(elem);
		wb_i32_const(p, 0);
		wbValue i = wb_pop_to_local(p, wbValType_i32, t_int);

		u32 break_depth = wb_open_block(p);
		u32 loop_depth = wb_open_loop(p);
		wb_push(p, i);
		wb_i32_const(p, cast(i32)count);
		wb_op(p, wbOp_i32_ge_s);
		wb_br_if(p, break_depth);

		wb_push(p, i);
		wb_i32_const(p, cast(i32)elem_size);
		wb_op(p, wbOp_i32_mul);
		wbValue offset = wb_pop_to_local(p, wbValType_i32, t_int);
		wb_push(p, lhs); wb_push(p, offset); wb_op(p, wbOp_i32_add);
		wbValue l = wb_pop_to_local(p, wbValType_i32, t_rawptr);
		wb_push(p, rhs); wb_push(p, offset); wb_op(p, wbOp_i32_add);
		wbValue r = wb_pop_to_local(p, wbValType_i32, t_rawptr);
		wb_emit_return_false_unless(p, wb_emit_equal_at(p, elem, l, r));

		wb_push(p, i);
		wb_i32_const(p, 1);
		wb_op(p, wbOp_i32_add);
		wb_local_set(p, i.index);
		wb_br(p, loop_depth);
		wb_close(p);
		wb_close(p);
		wb_emit_return_bool(p, true);
		return;
	}
	default:
		break;
	}
	auto values = array_make<wbValue>(temporary_allocator(), 1);
	values[0] = wb_emit_equal_at(p, type, lhs, rhs);
	wb_emit_return_values(p, values, false);
}

// Map_Info constants

gb_internal u32 wb_map_cell_info_addr(wbModule *m, Type *type) {
	String name = wb_gen_name_from_type("map_cell_info", type);
	u32 *found = string_map_get(&m->map_cell_infos, name);
	if (found) {
		return *found;
	}
	GB_ASSERT(t_map_cell_info != nullptr);
	i64 size = 0, len = 0;
	map_cell_size_and_len(type, &size, &len);

	Type *st = t_map_cell_info;
	i64 st_size = type_size_of(st);
	u8 *bytes = gb_alloc_array(temporary_allocator(), u8, st_size);
	gb_zero_size(bytes, st_size);
	wb_rtti_int(bytes, st, 0, type_size_of(type));
	wb_rtti_int(bytes, st, 1, type_align_of(type));
	wb_rtti_int(bytes, st, 2, size);
	wb_rtti_int(bytes, st, 3, len);

	u32 addr = wb_data_alloc(m, st_size, type_align_of(st));
	wb_data_write(m, addr, bytes, st_size);
	string_map_set(&m->map_cell_infos, name, addr);
	return addr;
}

gb_internal u32 wb_map_info_addr(wbModule *m, Type *map_type) {
	map_type = base_type(map_type);
	GB_ASSERT(map_type->kind == Type_Map);
	String name = wb_gen_name_from_type("map_info", map_type);
	u32 *found = string_map_get(&m->map_infos, name);
	if (found) {
		return *found;
	}
	GB_ASSERT(t_map_info != nullptr);

	u32 ks     = wb_map_cell_info_addr(m, map_type->Map.key);
	u32 vs     = wb_map_cell_info_addr(m, map_type->Map.value);
	u32 hasher = wb_table_index(m, wb_hasher_proc_for_type(m, map_type->Map.key));
	u32 equal  = wb_table_index(m, wb_equal_proc_for_type(m, map_type->Map.key));

	Type *st = t_map_info;
	i64 st_size = type_size_of(st);
	u8 *bytes = gb_alloc_array(temporary_allocator(), u8, st_size);
	gb_zero_size(bytes, st_size);
	wb_rtti_int(bytes, st, 0, ks);
	wb_rtti_int(bytes, st, 1, vs);
	wb_rtti_int(bytes, st, 2, hasher);
	wb_rtti_int(bytes, st, 3, equal);

	u32 addr = wb_data_alloc(m, st_size, type_align_of(st));
	wb_data_write(m, addr, bytes, st_size);
	string_map_set(&m->map_infos, name, addr);
	return addr;
}

gb_internal wbValue wb_map_info_value(wbModule *m, Type *map_type) {
	return wb_value_const_int(t_map_info_ptr, wb_map_info_addr(m, map_type));
}

// Element access

// The address of `key` in `map` (a Raw_Map in memory); the key is copied to
// the frame so that the runtime can take its address
gb_internal wbAddr wb_map_elem_addr(wbProcedure *p, wbAddr map, Type *map_type, wbValue key, Type *result_type) {
	wbAddr invalid = {};
	Type *bt = base_type(map_type);
	GB_ASSERT(bt->kind == Type_Map);
	if (map.kind != wbAddr_Memory) {
		return invalid;
	}
	key = wb_emit_conv(p, key, bt->Map.key);
	if (key.kind == wbValue_Invalid) {
		return invalid;
	}
	wbAddr key_tmp = wb_add_temp(p, bt->Map.key);
	wb_addr_store(p, key_tmp, key);

	wb_push_address(p, map.index, map.offset);
	wbValue map_ptr = wb_pop_to_local(p, wbValType_i32, t_raw_map_ptr);

	wbAddr a = {};
	a.kind     = wbAddr_Map;
	a.type     = result_type;
	a.index    = map_ptr.index;
	a.offset   = key_tmp.offset;
	a.map_type = map_type;
	return a;
}

// Hash of the key of a map element address (lb_gen_map_key_hash)
gb_internal wbValue wb_map_key_hash(wbProcedure *p, wbAddr addr, wbValue key_ptr) {
	Type *bt = base_type(addr.map_type);
	GB_ASSERT(wb_valtype_of(t_uintptr) == wbValType_i32);

	// seed = map_seed_from_map_data(data & ~(MAP_CACHE_LINE_SIZE-1))
	wbValue data = wb_emit_load(p, addr.index, 0, t_uintptr);
	wb_push(p, data);
	wb_i32_const(p, ~cast(i32)(MAP_CACHE_LINE_SIZE-1));
	wb_op(p, wbOp_i32_and);
	auto args = array_make<wbValue>(temporary_allocator(), 1);
	args[0] = wb_pop_to_local(p, wbValType_i32, t_uintptr);
	wbValue seed = wb_emit_runtime_call(p, "map_seed_from_map_data", args);

	return wb_emit_gen_call(p, wb_hasher_proc_for_type(p->module, bt->Map.key), key_ptr, seed);
}

// Pointer to the value stored for the key, nil if absent
gb_internal wbValue wb_map_get_ptr(wbProcedure *p, wbAddr addr, Type *ptr_type) {
	GB_ASSERT(addr.kind == wbAddr_Map);
	Type *bt = base_type(addr.map_type);
	wbValue map_ptr = wb_value_local(addr.index, wbValType_i32, t_raw_map_ptr);
	wbValue key_ptr = wb_addr_get_ptr(p, wb_addr_memory(p->fp_local, addr.offset, bt->Map.key), t_rawptr);
	wbValue hash = wb_map_key_hash(p, addr, key_ptr);

	auto args = array_make<wbValue>(temporary_allocator(), 4);
	args[0] = map_ptr;
	args[1] = wb_map_info_value(p->module, bt);
	args[2] = hash;
	args[3] = key_ptr;
	wbValue ptr = wb_emit_runtime_call(p, "__dynamic_map_get", args);
	if (ptr.kind != wbValue_Invalid) {
		ptr.type = ptr_type != nullptr ? ptr_type : alloc_type_pointer(bt->Map.value);
	}
	return ptr;
}

// `m[key]` (zero value when absent) or `v, ok := m[key]`
gb_internal wbValue wb_map_load(wbProcedure *p, wbAddr addr) {
	Type *bt = base_type(addr.map_type);
	Type *value_type = bt->Map.value;
	bool is_tuple = is_type_tuple(addr.type);

	wbAddr result = wb_add_temp(p, addr.type);
	wb_addr_zero(p, result);
	wbValue ptr = wb_map_get_ptr(p, addr, nullptr);
	if (ptr.kind == wbValue_Invalid) {
		return wb_value_invalid();
	}
	wb_push(p, ptr);
	wb_open_if(p);
	wbAddr value_addr = is_tuple ? wb_addr_field(result, 0) : result;
	wb_addr_store(p, value_addr, wb_addr_load(p, wb_addr_memory(ptr.index, 0, value_type)));
	if (is_tuple) {
		wb_addr_store(p, wb_addr_field(result, 1), wb_value_const_int(t_bool, 1));
	}
	wb_close(p);
	return wb_addr_load(p, result);
}

// `m[key] = v`
gb_internal void wb_map_set(wbProcedure *p, wbAddr addr, wbValue v) {
	Type *bt = base_type(addr.map_type);
	v = wb_emit_conv(p, v, bt->Map.value);
	if (v.kind == wbValue_Invalid) {
		return;
	}
	wbAddr value_addr = wb_value_to_addr(p, v);
	wbValue value_ptr = wb_addr_get_ptr(p, value_addr, t_rawptr);

	wbValue map_ptr = wb_value_local(addr.index, wbValType_i32, t_raw_map_ptr);
	wbValue key_ptr = wb_addr_get_ptr(p, wb_addr_memory(p->fp_local, addr.offset, bt->Map.key), t_rawptr);
	wbValue hash = wb_map_key_hash(p, addr, key_ptr);

	String proc_name = p->entity != nullptr ? p->entity->token.string : p->name;
	TokenPos pos = p->curr_stmt != nullptr ? ast_token(p->curr_stmt).pos : TokenPos{};

	auto args = array_make<wbValue>(temporary_allocator(), 6);
	args[0] = map_ptr;
	args[1] = wb_map_info_value(p->module, bt);
	args[2] = hash;
	args[3] = key_ptr;
	args[4] = value_ptr;
	args[5] = wb_source_code_location(p, proc_name, pos);
	wb_emit_runtime_call(p, "__dynamic_map_set", args);
}

// Converts a map element address into the memory it refers to (nil pointer
// when the key is absent, as in lb_addr_get_ptr)
gb_internal wbAddr wb_addr_resolve_map(wbProcedure *p, wbAddr addr) {
	if (addr.kind != wbAddr_Map) {
		return addr;
	}
	Type *value_type = base_type(addr.map_type)->Map.value;
	return wb_addr_from_pointer(p, wb_map_get_ptr(p, addr, nullptr), value_type);
}

// `len(m)`
gb_internal wbValue wb_emit_map_len(wbProcedure *p, wbValue m) {
	GB_ASSERT(m.kind == wbValue_Memory);
	Type *ft = nullptr;
	i64 offset = type_offset_of(t_raw_map, 1, &ft);
	return wb_emit_conv(p, wb_emit_load(p, m.index, m.offset + cast(i32)offset, ft), t_int);
}

// `cap(m)`: 1 << log2_cap stored in the low bits of `data`, 0 for an empty map
gb_internal wbValue wb_emit_map_cap(wbProcedure *p, wbValue m) {
	GB_ASSERT(m.kind == wbValue_Memory);
	GB_ASSERT(wb_valtype_of(t_uintptr) == wbValType_i32);
	wbValue data = wb_emit_load(p, m.index, m.offset, t_uintptr);
	wb_i32_const(p, 1);
	wb_push(p, data);
	wb_i32_const(p, MAP_CACHE_LINE_SIZE-1);
	wb_op(p, wbOp_i32_and);
	wb_op(p, wbOp_i32_shl);
	wb_i32_const(p, 0);
	wb_push(p, data);
	wb_op(p, wbOp_select); // data != 0 ? 1<<log2_cap : 0
	return wb_pop_to_local(p, wbValType_i32, t_int);
}

// `for key, value in m`: walks the cells, skipping empty and deleted hashes
gb_internal void wb_build_range_map(wbProcedure *p, AstRangeStmt *rs, Ast *node, Ast *val0, Ast *val1) {
	Ast *range_expr = unparen_expr(rs->expr);
	Type *expr_type = type_of_expr(range_expr);
	wbAddr map = {};
	if (is_type_pointer(expr_type)) {
		expr_type = type_deref(expr_type);
		map = wb_addr_from_pointer(p, wb_build_expr(p, range_expr), expr_type);
	} else {
		map = wb_build_addr(p, range_expr);
	}
	if (map.kind != wbAddr_Memory) {
		return;
	}
	Type *bt = base_type(expr_type);
	GB_ASSERT(bt->kind == Type_Map);
	Type *key_type = bt->Map.key;
	Type *value_type = bt->Map.value;

	wbValue raw_map = wb_value_memory(map.index, map.offset, t_raw_map);
	wbValue cap = wb_emit_map_cap(p, raw_map);

	// ks, vs, hs, _, _ := map_kvh_data_dynamic(m, info)
	auto args = array_make<wbValue>(temporary_allocator(), 2);
	args[0] = raw_map;
	args[1] = wb_map_info_value(p->module, bt);
	wbValue kvh = wb_emit_runtime_call(p, "map_kvh_data_dynamic", args);
	if (kvh.kind == wbValue_Invalid) {
		return;
	}
	wbValue ks = wb_value_to_local(p, wb_tuple_field(p, kvh, 0));
	wbValue vs = wb_value_to_local(p, wb_tuple_field(p, kvh, 1));
	wbValue hs = wb_value_to_local(p, wb_tuple_field(p, kvh, 2));
	wbValue ks_info = wb_value_const_int(t_map_cell_info_ptr, wb_map_cell_info_addr(p->module, key_type));
	wbValue vs_info = wb_value_const_int(t_map_cell_info_ptr, wb_map_cell_info_addr(p->module, value_type));

	wb_i32_const(p, 0);
	wbValue idx = wb_pop_to_local(p, wbValType_i32, t_uintptr);

	u32 break_depth = wb_open_block(p);
	u32 loop_depth = wb_open_loop(p);
	wb_push(p, idx);
	wb_push(p, cap);
	wb_op(p, wbOp_i32_ge_u);
	wb_br_if(p, break_depth);

	u32 continue_depth = wb_open_block(p);
	{
		// a valid hash is non-zero with the top (deleted) bit clear: hash > 0 as a signed value
		wb_push(p, hs);
		wb_push(p, idx);
		wb_i32_const(p, cast(i32)type_size_of(t_uintptr));
		wb_op(p, wbOp_i32_mul);
		wb_op(p, wbOp_i32_add);
		wbValue hash_ptr = wb_pop_to_local(p, wbValType_i32, t_rawptr);
		wbValue hash = wb_emit_load(p, hash_ptr.index, 0, t_uintptr);
		wb_push(p, hash);
		wb_i32_const(p, 0);
		wb_op(p, wbOp_i32_gt_s);
		wb_op(p, wbOp_i32_eqz);
		wb_br_if(p, continue_depth);

		wb_open_scope(p);
		auto cargs = array_make<wbValue>(temporary_allocator(), 3);
		if (val0 != nullptr && !is_blank_ident(val0)) {
			cargs[0] = ks; cargs[1] = ks_info; cargs[2] = idx;
			wbValue key_ptr = wb_emit_runtime_call(p, "map_cell_index_dynamic", cargs);
			wbAddr key_addr = wb_addr_from_pointer(p, key_ptr, key_type);
			if (wb_range_val_is_ref(val0)) {
				wb_bind_range_ref(p, val0, key_addr);
			} else {
				wb_store_range_val(p, val0, wb_addr_load(p, key_addr));
			}
		}
		if (val1 != nullptr && !is_blank_ident(val1)) {
			cargs[0] = vs; cargs[1] = vs_info; cargs[2] = idx;
			wbValue val_ptr = wb_emit_runtime_call(p, "map_cell_index_dynamic", cargs);
			wbAddr val_addr = wb_addr_from_pointer(p, val_ptr, value_type);
			if (wb_range_val_is_ref(val1)) {
				wb_bind_range_ref(p, val1, val_addr);
			} else {
				wb_store_range_val(p, val1, wb_addr_load(p, val_addr));
			}
		}

		wb_push_label(p, rs->label, break_depth, continue_depth, true, false, 0);
		wb_build_stmt(p, rs->body);
		wb_pop_label(p);
		wb_close_scope(p);
	}
	wb_close(p); // continue

	wb_push(p, idx);
	wb_i32_const(p, 1);
	wb_op(p, wbOp_i32_add);
	wb_local_set(p, idx.index);
	wb_br(p, loop_depth);
	wb_close(p); // loop
	wb_close(p); // break
}
