// Run-time type information: the `type_table` hash map of `^Type_Info` and
// the Type_Info entries themselves, serialized into the data segment
// (the wasm equivalent of lb_setup_type_info_data)

// Helpers to write struct fields into a zeroed byte image of a struct
gb_internal void wb_rtti_int(u8 *dst, Type *st, isize field, i64 value) {
	Type *ft = nullptr;
	i64 offset = type_offset_of(base_type(st), field, &ft);
	wb_write_le(dst + offset, cast(u64)value, type_size_of(ft));
}

gb_internal void wb_rtti_string(wbModule *m, u8 *dst, Type *st, isize field, String const &s) {
	Type *ft = nullptr;
	i64 offset = type_offset_of(base_type(st), field, &ft);
	wb_write_const_data(m, ft, exact_value_string(s), dst + offset);
}

// Writes a slice {data, len} (data may be 0 for an empty slice)
gb_internal void wb_rtti_slice(u8 *dst, Type *st, isize field, u32 data, i64 len) {
	Type *ft = nullptr;
	i64 offset = type_offset_of(base_type(st), field, &ft);
	Type *bt = base_type(ft);
	GB_ASSERT(bt->kind == Type_Slice);
	wb_write_le(dst + offset, data, build_context.ptr_size);
	wb_write_le(dst + offset + type_offset_of(bt, 1, nullptr), cast(u64)len, build_context.int_size);
}

gb_internal u32 wb_rtti_type_info_ptr(wbModule *m, Type *type) {
	type = default_type(type);
	isize index = type_info_index(m->info, type, true);
	GB_ASSERT(index >= 0 && index < m->type_info_addrs.count);
	u32 addr = m->type_info_addrs[index];
	GB_ASSERT_MSG(addr != 0, "Type_Info for '%s' has no entry", type_to_string(type));
	return addr;
}

// An array of ^Type_Info in the data segment
gb_internal u32 wb_rtti_type_info_array(wbModule *m, Slice<Type *> const &types) {
	if (types.count == 0) {
		return 0;
	}
	i64 ptr_size = build_context.ptr_size;
	u32 addr = wb_data_alloc(m, ptr_size*types.count, ptr_size);
	u8 *bytes = gb_alloc_array(temporary_allocator(), u8, ptr_size*types.count);
	gb_zero_size(bytes, ptr_size*types.count);
	for_array(i, types) {
		wb_write_le(bytes + i*ptr_size, wb_rtti_type_info_ptr(m, types[i]), ptr_size);
	}
	wb_data_write(m, addr, bytes, ptr_size*types.count);
	return addr;
}

gb_internal u32 wb_rtti_string_array(wbModule *m, Slice<String> const &strings) {
	if (strings.count == 0) {
		return 0;
	}
	i64 size = type_size_of(t_string);
	u32 addr = wb_data_alloc(m, size*strings.count, type_align_of(t_string));
	u8 *bytes = gb_alloc_array(temporary_allocator(), u8, size*strings.count);
	gb_zero_size(bytes, size*strings.count);
	for_array(i, strings) {
		if (strings[i].len > 0) {
			wb_write_const_data(m, t_string, exact_value_string(strings[i]), bytes + i*size);
		}
	}
	wb_data_write(m, addr, bytes, size*strings.count);
	return addr;
}

// An array of `elem_size` byte integers
gb_internal u32 wb_rtti_int_array(wbModule *m, Slice<i64> const &values, i64 elem_size) {
	if (values.count == 0) {
		return 0;
	}
	u32 addr = wb_data_alloc(m, elem_size*values.count, elem_size);
	u8 *bytes = gb_alloc_array(temporary_allocator(), u8, elem_size*values.count);
	gb_zero_size(bytes, elem_size*values.count);
	for_array(i, values) {
		wb_write_le(bytes + i*elem_size, cast(u64)values[i], elem_size);
	}
	wb_data_write(m, addr, bytes, elem_size*values.count);
	return addr;
}

gb_internal u32 wb_rtti_source_code_location(wbModule *m, String const &procedure, TokenPos const &pos) {
	Type *type = t_source_code_location;
	i64 size = type_size_of(type);
	u8 *bytes = gb_alloc_array(temporary_allocator(), u8, size);
	gb_zero_size(bytes, size);
	wb_rtti_string(m, bytes, type, 0, get_file_path_string(pos.file_id));
	wb_rtti_int(bytes, type, 1, pos.line);
	wb_rtti_int(bytes, type, 2, pos.column);
	wb_rtti_string(m, bytes, type, 3, procedure);
	u32 addr = wb_data_alloc(m, size, type_align_of(type));
	wb_data_write(m, addr, bytes, size);
	return addr;
}

gb_internal u8 wb_rtti_endianness(Type *t) {
	if (t->Basic.flags & BasicFlag_EndianLittle) {
		return 1;
	} else if (t->Basic.flags & BasicFlag_EndianBig) {
		return 2;
	}
	return 0;
}

// Serializes the `variant` of the Type_Info for `t` into `dst` (a zeroed image
// of the variant's struct type) and returns that struct type (nullptr when the
// variant carries no data)
gb_internal Type *wb_rtti_write_variant(wbModule *m, Type *t, u8 *dst) {
	TEMPORARY_ALLOCATOR_GUARD();

	switch (t->kind) {
	case Type_Named: {
		Type *st = t_type_info_named;
		Entity *tn = t->Named.type_name;
		wb_rtti_string(m, dst, st, 0, tn->token.string);
		wb_rtti_int(dst, st, 1, wb_rtti_type_info_ptr(m, t->Named.base));
		if (tn->pkg) {
			wb_rtti_string(m, dst, st, 2, tn->pkg->name);
		}
		String proc_name = {};
		if (tn->parent_proc_decl) {
			DeclInfo *decl = tn->parent_proc_decl;
			Entity *e = decl->entity.load();
			if (e && e->kind == Entity_Procedure) {
				proc_name = e->token.string;
			}
		}
		wb_rtti_int(dst, st, 3, wb_rtti_source_code_location(m, proc_name, tn->token.pos));
		return st;
	}

	case Type_Basic:
		switch (t->Basic.kind) {
		case Basic_bool: case Basic_b8: case Basic_b16: case Basic_b32: case Basic_b64:
			return t_type_info_boolean;

		case Basic_i8:  case Basic_u8:
		case Basic_i16: case Basic_u16:
		case Basic_i32: case Basic_u32:
		case Basic_i64: case Basic_u64:
		case Basic_i128: case Basic_u128:
		case Basic_i16le: case Basic_u16le:
		case Basic_i32le: case Basic_u32le:
		case Basic_i64le: case Basic_u64le:
		case Basic_i128le: case Basic_u128le:
		case Basic_i16be: case Basic_u16be:
		case Basic_i32be: case Basic_u32be:
		case Basic_i64be: case Basic_u64be:
		case Basic_i128be: case Basic_u128be:
		case Basic_int: case Basic_uint: case Basic_uintptr: {
			Type *st = t_type_info_integer;
			wb_rtti_int(dst, st, 0, (t->Basic.flags & BasicFlag_Unsigned) == 0);
			wb_rtti_int(dst, st, 1, wb_rtti_endianness(t));
			return st;
		}

		case Basic_rune:
			return t_type_info_rune;

		case Basic_f16: case Basic_f32: case Basic_f64:
		case Basic_f16le: case Basic_f32le: case Basic_f64le:
		case Basic_f16be: case Basic_f32be: case Basic_f64be: {
			Type *st = t_type_info_float;
			wb_rtti_int(dst, st, 0, wb_rtti_endianness(t));
			return st;
		}

		case Basic_complex32: case Basic_complex64: case Basic_complex128:
			return t_type_info_complex;

		case Basic_quaternion64: case Basic_quaternion128: case Basic_quaternion256:
			return t_type_info_quaternion;

		case Basic_rawptr:
			return t_type_info_pointer;

		case Basic_string:
		case Basic_cstring:
		case Basic_string16:
		case Basic_cstring16: {
			Type *st = t_type_info_string;
			bool is_cstring = t->Basic.kind == Basic_cstring || t->Basic.kind == Basic_cstring16;
			bool is_16 = t->Basic.kind == Basic_string16 || t->Basic.kind == Basic_cstring16;
			wb_rtti_int(dst, st, 0, is_cstring);
			wb_rtti_int(dst, st, 1, is_16 ? 1 : 0);
			return st;
		}

		case Basic_any:
			return t_type_info_any;

		case Basic_typeid:
			return t_type_info_typeid;
		}
		return nullptr;

	case Type_Pointer: {
		Type *st = t_type_info_pointer;
		wb_rtti_int(dst, st, 0, wb_rtti_type_info_ptr(m, t->Pointer.elem));
		return st;
	}
	case Type_MultiPointer: {
		Type *st = t_type_info_multi_pointer;
		wb_rtti_int(dst, st, 0, wb_rtti_type_info_ptr(m, t->MultiPointer.elem));
		return st;
	}
	case Type_SoaPointer: {
		Type *st = t_type_info_soa_pointer;
		wb_rtti_int(dst, st, 0, wb_rtti_type_info_ptr(m, t->SoaPointer.elem));
		return st;
	}
	case Type_Array: {
		Type *st = t_type_info_array;
		wb_rtti_int(dst, st, 0, wb_rtti_type_info_ptr(m, t->Array.elem));
		wb_rtti_int(dst, st, 1, type_size_of(t->Array.elem));
		wb_rtti_int(dst, st, 2, t->Array.count);
		return st;
	}
	case Type_EnumeratedArray: {
		Type *st = t_type_info_enumerated_array;
		wb_rtti_int(dst, st, 0, wb_rtti_type_info_ptr(m, t->EnumeratedArray.elem));
		wb_rtti_int(dst, st, 1, wb_rtti_type_info_ptr(m, t->EnumeratedArray.index));
		wb_rtti_int(dst, st, 2, type_size_of(t->EnumeratedArray.elem));
		wb_rtti_int(dst, st, 3, t->EnumeratedArray.count);
		wb_rtti_int(dst, st, 4, exact_value_to_i64(*t->EnumeratedArray.min_value));
		wb_rtti_int(dst, st, 5, exact_value_to_i64(*t->EnumeratedArray.max_value));
		wb_rtti_int(dst, st, 6, t->EnumeratedArray.is_sparse);
		return st;
	}
	case Type_DynamicArray: {
		Type *st = t_type_info_dynamic_array;
		wb_rtti_int(dst, st, 0, wb_rtti_type_info_ptr(m, t->DynamicArray.elem));
		wb_rtti_int(dst, st, 1, type_size_of(t->DynamicArray.elem));
		return st;
	}
	case Type_FixedCapacityDynamicArray: {
		Type *st = t_type_info_fixed_capacity_dynamic_array;
		wb_rtti_int(dst, st, 0, wb_rtti_type_info_ptr(m, t->FixedCapacityDynamicArray.elem));
		wb_rtti_int(dst, st, 1, type_size_of(t->FixedCapacityDynamicArray.elem));
		wb_rtti_int(dst, st, 2, t->FixedCapacityDynamicArray.capacity);
		wb_rtti_int(dst, st, 3, type_offset_of(t, 1));
		return st;
	}
	case Type_Slice: {
		Type *st = t_type_info_slice;
		wb_rtti_int(dst, st, 0, wb_rtti_type_info_ptr(m, t->Slice.elem));
		wb_rtti_int(dst, st, 1, type_size_of(t->Slice.elem));
		return st;
	}
	case Type_Proc: {
		Type *st = t_type_info_procedure;
		if (t->Proc.params != nullptr) {
			wb_rtti_int(dst, st, 0, wb_rtti_type_info_ptr(m, t->Proc.params));
		}
		if (t->Proc.results != nullptr) {
			wb_rtti_int(dst, st, 1, wb_rtti_type_info_ptr(m, t->Proc.results));
		}
		wb_rtti_int(dst, st, 2, t->Proc.variadic);
		wb_rtti_int(dst, st, 3, t->Proc.calling_convention);
		return st;
	}
	case Type_Tuple: {
		Type *st = t_type_info_parameters;
		isize count = t->Tuple.variables.count;
		auto types = slice_make<Type *>(temporary_allocator(), count);
		auto names = slice_make<String>(temporary_allocator(), count);
		for_array(i, t->Tuple.variables) {
			Entity *f = t->Tuple.variables[i];
			types[i] = f->type;
			names[i] = f->token.string;
		}
		wb_rtti_slice(dst, st, 0, wb_rtti_type_info_array(m, types), count);
		wb_rtti_slice(dst, st, 1, wb_rtti_string_array(m, names), count);
		return st;
	}
	case Type_Enum: {
		Type *st = t_type_info_enum;
		GB_ASSERT(t->Enum.base_type != nullptr);
		wb_rtti_int(dst, st, 0, wb_rtti_type_info_ptr(m, t->Enum.base_type));
		isize count = t->Enum.fields.count;
		if (count > 0) {
			auto names  = slice_make<String>(temporary_allocator(), count);
			auto values = slice_make<i64>(temporary_allocator(), count);
			for_array(i, t->Enum.fields) {
				names[i]  = t->Enum.fields[i]->token.string;
				values[i] = exact_value_to_i64(t->Enum.fields[i]->Constant.value);
			}
			wb_rtti_slice(dst, st, 1, wb_rtti_string_array(m, names), count);
			wb_rtti_slice(dst, st, 2, wb_rtti_int_array(m, values, type_size_of(t_type_info_enum_value)), count);
		}
		return st;
	}
	case Type_Union: {
		Type *st = t_type_info_union;
		isize count = t->Union.variants.count;
		auto variants = slice_make<Type *>(temporary_allocator(), count);
		for_array(i, t->Union.variants) {
			variants[i] = t->Union.variants[i];
		}
		wb_rtti_slice(dst, st, 0, wb_rtti_type_info_array(m, variants), count);
		if (union_tag_size(t) > 0) {
			wb_rtti_int(dst, st, 1, align_formula(t->Union.variant_block_size, union_tag_size(t)));
			wb_rtti_int(dst, st, 2, wb_rtti_type_info_ptr(m, union_tag_type(t)));
		}
		if (is_type_comparable(t) && !is_type_simple_compare(t)) {
			wb_rtti_int(dst, st, 3, wb_table_index(m, wb_equal_proc_for_type(m, t)));
		}
		wb_rtti_int(dst, st, 4, t->Union.custom_align != 0);
		wb_rtti_int(dst, st, 5, t->Union.kind == UnionType_no_nil);
		wb_rtti_int(dst, st, 6, t->Union.kind == UnionType_shared_nil);
		return st;
	}
	case Type_Struct: {
		Type *st = t_type_info_struct;
		u8 flags = 0;
		if (t->Struct.is_packed)      flags |= 1<<0;
		if (t->Struct.is_raw_union)   flags |= 1<<1;
		if (t->Struct.is_all_or_none) flags |= 1<<2;
		if (t->Struct.custom_align)   flags |= 1<<3;
		wb_rtti_int(dst, st, 6, flags);
		if (is_type_comparable(t) && !is_type_simple_compare(t)) {
			wb_rtti_int(dst, st, 10, wb_table_index(m, wb_equal_proc_for_type(m, t)));
		}
		if (t->Struct.soa_kind != StructSoa_None) {
			wb_rtti_int(dst, st, 7, t->Struct.soa_kind);
			wb_rtti_int(dst, st, 8, t->Struct.soa_count);
			wb_rtti_int(dst, st, 9, wb_rtti_type_info_ptr(m, t->Struct.soa_elem));
		}
		isize count = t->Struct.fields.count;
		if (count > 0) {
			type_set_offsets(t);
			auto types   = slice_make<Type *>(temporary_allocator(), count);
			auto names   = slice_make<String>(temporary_allocator(), count);
			auto offsets = slice_make<i64>(temporary_allocator(), count);
			auto usings  = slice_make<i64>(temporary_allocator(), count);
			auto tags    = slice_make<String>(temporary_allocator(), count);
			for (isize i = 0; i < count; i++) {
				Entity *f = t->Struct.fields[i];
				types[i]   = f->type;
				names[i]   = f->token.string;
				offsets[i] = t->Struct.is_raw_union ? 0 : t->Struct.offsets[i];
				usings[i]  = (f->flags & EntityFlag_Using) != 0;
				tags[i]    = t->Struct.tags != nullptr ? t->Struct.tags[i] : String{};
			}
			wb_rtti_int(dst, st, 0, wb_rtti_type_info_array(m, types));
			wb_rtti_int(dst, st, 1, wb_rtti_string_array(m, names));
			wb_rtti_int(dst, st, 2, wb_rtti_int_array(m, offsets, build_context.ptr_size));
			wb_rtti_int(dst, st, 3, wb_rtti_int_array(m, usings, 1));
			wb_rtti_int(dst, st, 4, wb_rtti_string_array(m, tags));
			wb_rtti_int(dst, st, 5, count);
		}
		return st;
	}
	case Type_Map: {
		Type *st = t_type_info_map;
		init_map_internal_debug_types(t);
		wb_rtti_int(dst, st, 0, wb_rtti_type_info_ptr(m, t->Map.key));
		wb_rtti_int(dst, st, 1, wb_rtti_type_info_ptr(m, t->Map.value));
		wb_rtti_int(dst, st, 2, wb_map_info_addr(m, t));
		return st;
	}
	case Type_BitSet: {
		Type *st = t_type_info_bit_set;
		GB_ASSERT(is_type_typed(t->BitSet.elem));
		wb_rtti_int(dst, st, 0, wb_rtti_type_info_ptr(m, t->BitSet.elem));
		Type *underlying = t->BitSet.underlying != nullptr ? t->BitSet.underlying : bit_set_to_int(t);
		wb_rtti_int(dst, st, 1, wb_rtti_type_info_ptr(m, underlying));
		wb_rtti_int(dst, st, 2, t->BitSet.underlying != nullptr);
		wb_rtti_int(dst, st, 3, t->BitSet.lower);
		wb_rtti_int(dst, st, 4, t->BitSet.upper);
		return st;
	}
	case Type_SimdVector: {
		Type *st = t_type_info_simd_vector;
		wb_rtti_int(dst, st, 0, wb_rtti_type_info_ptr(m, t->SimdVector.elem));
		wb_rtti_int(dst, st, 1, type_size_of(t->SimdVector.elem));
		wb_rtti_int(dst, st, 2, t->SimdVector.count);
		return st;
	}
	case Type_Matrix: {
		Type *st = t_type_info_matrix;
		wb_rtti_int(dst, st, 0, wb_rtti_type_info_ptr(m, t->Matrix.elem));
		wb_rtti_int(dst, st, 1, type_size_of(t->Matrix.elem));
		wb_rtti_int(dst, st, 2, matrix_type_stride_in_elems(t));
		wb_rtti_int(dst, st, 3, t->Matrix.row_count);
		wb_rtti_int(dst, st, 4, t->Matrix.column_count);
		wb_rtti_int(dst, st, 5, t->Matrix.is_row_major);
		return st;
	}
	case Type_BitField: {
		Type *st = t_type_info_bit_field;
		wb_rtti_int(dst, st, 0, wb_rtti_type_info_ptr(m, t->BitField.backing_type));
		isize count = t->BitField.fields.count;
		if (count > 0) {
			auto names       = slice_make<String>(temporary_allocator(), count);
			auto types       = slice_make<Type *>(temporary_allocator(), count);
			auto bit_sizes   = slice_make<i64>(temporary_allocator(), count);
			auto bit_offsets = slice_make<i64>(temporary_allocator(), count);
			auto tags        = slice_make<String>(temporary_allocator(), count);
			i64 bit_offset = 0;
			for (isize i = 0; i < count; i++) {
				Entity *f = t->BitField.fields[i];
				names[i]       = f->token.string;
				types[i]       = f->type;
				bit_sizes[i]   = t->BitField.bit_sizes[i];
				bit_offsets[i] = bit_offset;
				tags[i]        = t->BitField.tags != nullptr ? t->BitField.tags[i] : String{};
				bit_offset += t->BitField.bit_sizes[i];
			}
			wb_rtti_int(dst, st, 1, wb_rtti_string_array(m, names));
			wb_rtti_int(dst, st, 2, wb_rtti_type_info_array(m, types));
			wb_rtti_int(dst, st, 3, wb_rtti_int_array(m, bit_sizes, build_context.ptr_size));
			wb_rtti_int(dst, st, 4, wb_rtti_int_array(m, bit_offsets, build_context.ptr_size));
			wb_rtti_int(dst, st, 5, wb_rtti_string_array(m, tags));
			wb_rtti_int(dst, st, 6, count);
		}
		return st;
	}
	}
	return nullptr;
}

gb_internal void wb_setup_type_info_data(wbModule *m) {
	if (build_context.no_rtti) {
		return;
	}
	CheckerInfo *info = m->info;
	isize count = info->type_info_types_hash_map.count;
	if (count == 0) {
		return;
	}

	Type *ti = base_type(t_type_info);
	GB_ASSERT(ti->kind == Type_Struct);
	Type *variant_type = ti->Struct.fields[4]->type;
	Type *ut = base_type(variant_type);
	GB_ASSERT(ut->kind == Type_Union);
	i64 ti_size = type_size_of(t_type_info);
	i64 ti_align = type_align_of(t_type_info);
	i64 variant_offset = type_offset_of(ti, 4);
	i64 tag_offset = variant_offset + wb_union_tag_offset(ut);
	Type *tag_type = union_tag_type(ut);

	// Entry 0 is the zero Type_Info; every used hash map slot gets its own entry
	array_init(&m->type_info_addrs, m->allocator, count);
	array_resize(&m->type_info_addrs, count);
	for (isize i = 0; i < count; i++) {
		m->type_info_addrs[i] = 0;
	}
	m->type_info_addrs[0] = wb_data_alloc(m, ti_size, ti_align);
	for_array(i, info->type_info_types_hash_map) {
		auto const &tt = info->type_info_types_hash_map[i];
		if (tt.type == nullptr || tt.type == t_invalid) {
			continue;
		}
		isize entry_index = type_info_index(info, tt, false);
		if (entry_index <= 0 || m->type_info_addrs[entry_index] != 0) {
			continue;
		}
		m->type_info_addrs[entry_index] = wb_data_alloc(m, ti_size, ti_align);
	}

	// Fill in the entries
	auto written = slice_make<bool>(temporary_allocator(), count);
	for_array(i, info->type_info_types_hash_map) {
		Type *t = info->type_info_types_hash_map[i].type;
		if (t == nullptr || t == t_invalid) {
			continue;
		}
		isize entry_index = type_info_index(info, t, false);
		if (entry_index <= 0 || written[entry_index]) {
			continue;
		}
		written[entry_index] = true;

		u8 *bytes = gb_alloc_array(temporary_allocator(), u8, ti_size);
		gb_zero_size(bytes, ti_size);
		wb_rtti_int(bytes, ti, 0, type_size_of(t));
		wb_rtti_int(bytes, ti, 1, type_align_of(t));
		wb_rtti_int(bytes, ti, 2, type_info_flags_of_type(t));
		wb_rtti_int(bytes, ti, 3, cast(i64)type_hash_canonical_type(t));

		Type *st = wb_rtti_write_variant(m, t, bytes + variant_offset);
		if (st != nullptr) {
			i64 tag_index = union_variant_index_checked(ut, st);
			wb_write_le(bytes + tag_offset, cast(u64)tag_index, type_size_of(tag_type));
		}
		wb_data_write(m, m->type_info_addrs[entry_index], bytes, ti_size);
	}

	// type_table: []^Type_Info over the hash map slots
	i64 ptr_size = build_context.ptr_size;
	u32 ptrs = wb_data_alloc(m, ptr_size*count, ptr_size);
	u8 *ptr_bytes = gb_alloc_array(temporary_allocator(), u8, ptr_size*count);
	gb_zero_size(ptr_bytes, ptr_size*count);
	for (isize i = 0; i < count; i++) {
		wb_write_le(ptr_bytes + i*ptr_size, m->type_info_addrs[i], ptr_size);
	}
	wb_data_write(m, ptrs, ptr_bytes, ptr_size*count);

	Entity *type_table = scope_lookup_current(info->runtime_package->scope, string_interner_insert(str_lit("type_table")));
	GB_ASSERT(type_table != nullptr);
	u32 table_addr = wb_global_addr(m, type_table);
	Type *slice_type = base_type(type_table->type);
	u8 *slice_bytes = gb_alloc_array(temporary_allocator(), u8, type_size_of(slice_type));
	gb_zero_size(slice_bytes, type_size_of(slice_type));
	wb_write_le(slice_bytes, ptrs, ptr_size);
	wb_write_le(slice_bytes + type_offset_of(slice_type, 1, nullptr), count, build_context.int_size);
	wb_data_write(m, table_addr, slice_bytes, type_size_of(slice_type));
}

// The `^Type_Info` constant for a type
gb_internal wbValue wb_type_info(wbProcedure *p, Type *type) {
	GB_ASSERT(!build_context.no_rtti);
	return wb_value_const_int(t_type_info_ptr, wb_rtti_type_info_ptr(p->module, type));
}
