// SIMD vectors for the wasm backend
//
// `#simd[N]T` is lowered without the wasm SIMD proposal: a vector is a memory
// aggregate laid out like `[N]T` and every intrinsic is expanded lane by lane
// with scalar operations. This is slow but keeps the value model simple
// (v128 locals never appear); the runtime and core packages only rely on the
// intrinsics for portable fast paths, so correctness is what matters here.

gb_internal wbAddr wb_simd_lane(wbAddr v, i64 i) {
	Type *bt = base_type(v.type);
	GB_ASSERT(bt->kind == Type_SimdVector);
	Type *elem = bt->SimdVector.elem;
	return wb_addr_memory(v.index, v.offset + cast(i32)(i*type_size_of(elem)), elem);
}

gb_internal wbValue wb_simd_lane_load(wbProcedure *p, wbAddr v, i64 i) {
	return wb_addr_load(p, wb_simd_lane(v, i));
}

gb_internal void wb_simd_lane_store(wbProcedure *p, wbAddr v, i64 i, wbValue x) {
	wb_addr_store(p, wb_simd_lane(v, i), x);
}

// Builds a vector argument into memory it exclusively owns (later arguments
// may be calls that clobber a shared temporary)
gb_internal wbAddr wb_simd_arg(wbProcedure *p, Ast *arg, Type *type) {
	wbValue v = wb_emit_conv(p, wb_build_expr(p, arg), type);
	if (v.kind == wbValue_Invalid) {
		wbAddr invalid = {};
		return invalid;
	}
	v = wb_value_copy(p, v);
	return wb_value_to_addr(p, v);
}

gb_internal wbValue wb_emit_conv_to_simd(wbProcedure *p, wbValue v, Type *dst) {
	Type *bd = base_type(dst);
	GB_ASSERT(bd->kind == Type_SimdVector);
	Type *delem = bd->SimdVector.elem;
	i64 count = bd->SimdVector.count;
	Type *bs = base_type(v.type);

	wbAddr res = wb_add_temp(p, dst);
	if (bs->kind == Type_SimdVector) {
		if (bs->SimdVector.count != count) {
			return wb_conv_unsupported(p, v.type, dst);
		}
		wbAddr src = wb_value_to_addr(p, v);
		for (i64 i = 0; i < count; i++) {
			wbValue x = wb_emit_conv(p, wb_simd_lane_load(p, src, i), delem);
			if (x.kind == wbValue_Invalid) {
				return x;
			}
			wb_simd_lane_store(p, res, i, x);
		}
		return wb_value_memory(res.index, res.offset, dst);
	}

	// splat
	wbValue x = wb_value_to_local(p, wb_emit_conv(p, v, delem));
	if (x.kind == wbValue_Invalid) {
		return x;
	}
	for (i64 i = 0; i < count; i++) {
		wb_simd_lane_store(p, res, i, x);
	}
	return wb_value_memory(res.index, res.offset, dst);
}

// A comparison result lane: all bits set for true, zero for false (0 - cond)
gb_internal wbValue wb_simd_lane_mask(wbProcedure *p, wbValue cond, Type *lane_type) {
	wbValType vt = wb_valtype_of(lane_type);
	if (vt == wbValType_i64) {
		wb_i64_const(p, 0);
		wb_push(p, cond);
		wb_op(p, wbOp_i64_extend_i32_u);
		wb_op(p, wbOp_i64_sub);
	} else {
		wb_i32_const(p, 0);
		wb_push(p, cond);
		wb_op(p, wbOp_i32_sub);
		wb_emit_normalize(p, lane_type);
	}
	return wb_pop_to_local(p, vt, lane_type);
}

// min/max of two scalars already held in values
gb_internal wbValue wb_emit_min_max(wbProcedure *p, wbValue a, wbValue b, Type *type, bool is_max) {
	wbValType vt = wb_valtype_of(type);
	if (vt == wbValType_f32 || vt == wbValType_f64) {
		wb_push(p, a);
		wb_push(p, b);
		if (vt == wbValType_f32) wb_op(p, is_max ? wbOp_f32_max : wbOp_f32_min);
		else                     wb_op(p, is_max ? wbOp_f64_max : wbOp_f64_min);
		return wb_pop_to_local(p, vt, type);
	}
	a = wb_value_to_local(p, a);
	b = wb_value_to_local(p, b);
	wb_push(p, a);
	wb_push(p, b);
	wb_push(p, a);
	wb_push(p, b);
	wb_emit_binary_op(p, is_max ? Token_Gt : Token_Lt, vt, wb_type_is_signed(type));
	wb_op(p, wbOp_select);
	return wb_pop_to_local(p, vt, type);
}

// A mask lane is set when its lowest bit is set: LLVM truncates the whole mask
// vector to <N x i1>, which keeps bit 0 of every lane
gb_internal wbValue wb_simd_mask_lane(wbProcedure *p, wbAddr mask, i64 i, Type *melem) {
	wb_push(p, wb_simd_lane_load(p, mask, i));
	if (wb_valtype_of(melem) == wbValType_i64) {
		wb_i64_const(p, 1);
		wb_op(p, wbOp_i64_and);
		wb_op(p, wbOp_i32_wrap_i64);
	} else {
		wb_i32_const(p, 1);
		wb_op(p, wbOp_i32_and);
	}
	return wb_pop_to_local(p, wbValType_i32, t_bool);
}

// `simd_gather`/`simd_scatter` and the masked load/store family
//
// All six take (ptr, values, mask) and touch memory only for the set lanes;
// the ones that produce a vector keep `values` in the lanes that are not set.
// `simd_masked_expand_load`/`simd_masked_compress_store` address a packed
// buffer, so their memory index only advances on a set lane.
gb_internal wbValue wb_build_simd_memory_builtin(wbProcedure *p, Ast *expr, BuiltinProcId id) {
	ast_node(ce, CallExpr, expr);
	Slice<Ast *> const &args = ce->args;

	Type *val_type  = type_of_expr(args[1]);
	Type *mask_type = type_of_expr(args[2]);
	Type *bvt = base_type(val_type);
	GB_ASSERT(bvt->kind == Type_SimdVector);
	Type *elem  = bvt->SimdVector.elem;
	i64   count = bvt->SimdVector.count;
	Type *melem = base_type(mask_type)->SimdVector.elem;
	if (wb_valtype_of(elem) == wbValType_Invalid) {
		wb_unsupported_type(p, expr, val_type);
		return wb_value_invalid();
	}

	bool ptr_per_lane = id == BuiltinProc_simd_gather || id == BuiltinProc_simd_scatter;

	wbAddr ptrs = {}; // #simd[N]rawptr, one address per lane
	wbAddr base = {}; // a single contiguous buffer
	if (ptr_per_lane) {
		ptrs = wb_simd_arg(p, args[0], type_of_expr(args[0]));
		if (ptrs.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
	} else {
		wbValue ptr = wb_build_expr(p, args[0]);
		if (ptr.kind == wbValue_Invalid) {
			return wb_value_invalid();
		}
		if (wb_expr_has_call(args[1]) || wb_expr_has_call(args[2])) {
			ptr = wb_value_fresh(p, ptr);
		}
		base = wb_addr_from_pointer(p, ptr, elem);
	}
	wbAddr val  = wb_simd_arg(p, args[1], val_type);
	wbAddr mask = wb_simd_arg(p, args[2], mask_type);
	if (val.kind == wbAddr_Invalid || mask.kind == wbAddr_Invalid) {
		return wb_value_invalid();
	}

	bool produces_value = id == BuiltinProc_simd_gather ||
	                      id == BuiltinProc_simd_masked_load ||
	                      id == BuiltinProc_simd_masked_expand_load;
	wbAddr res = {};
	if (produces_value) {
		res = wb_add_temp(p, val_type);
	}

	// the index into the packed buffer of expand/compress
	u32 packed = WB_NO_LOCAL;
	if (id == BuiltinProc_simd_masked_expand_load || id == BuiltinProc_simd_masked_compress_store) {
		packed = wb_add_local(p, wbValType_i32);
		wb_i32_const(p, 0);
		wb_local_set(p, packed);
	}

	i64 elem_size = type_size_of(elem);
	for (i64 i = 0; i < count; i++) {
		if (produces_value) {
			wb_simd_lane_store(p, res, i, wb_simd_lane_load(p, val, i));
		}
		wb_push(p, wb_simd_mask_lane(p, mask, i, melem));
		wb_open_if(p);
		switch (id) {
		case BuiltinProc_simd_gather:
			wb_simd_lane_store(p, res, i, wb_addr_load(p, wb_addr_from_pointer(p, wb_simd_lane_load(p, ptrs, i), elem)));
			break;
		case BuiltinProc_simd_scatter:
			wb_addr_store(p, wb_addr_from_pointer(p, wb_simd_lane_load(p, ptrs, i), elem), wb_simd_lane_load(p, val, i));
			break;
		case BuiltinProc_simd_masked_load:
			wb_simd_lane_store(p, res, i, wb_addr_load(p, wb_addr_offset(base, i*elem_size, elem)));
			break;
		case BuiltinProc_simd_masked_store:
			wb_addr_store(p, wb_addr_offset(base, i*elem_size, elem), wb_simd_lane_load(p, val, i));
			break;
		case BuiltinProc_simd_masked_expand_load:
		case BuiltinProc_simd_masked_compress_store: {
			wbAddr slot = wb_emit_elem_addr(p, base.index, base.offset, wb_value_local(packed, wbValType_i32, t_i32), elem);
			if (id == BuiltinProc_simd_masked_expand_load) {
				wb_simd_lane_store(p, res, i, wb_addr_load(p, slot));
			} else {
				wb_addr_store(p, slot, wb_simd_lane_load(p, val, i));
			}
			wb_local_get(p, packed);
			wb_i32_const(p, 1);
			wb_op(p, wbOp_i32_add);
			wb_local_set(p, packed);
			break;
		}
		default:
			break;
		}
		wb_close(p);
	}
	if (produces_value) {
		return wb_value_memory(res.index, res.offset, val_type);
	}
	return wb_value_invalid();
}

gb_internal wbValue wb_build_simd_builtin(wbProcedure *p, Ast *expr, BuiltinProcId id) {
	ast_node(ce, CallExpr, expr);
	switch (id) {
	case BuiltinProc_simd_gather:
	case BuiltinProc_simd_scatter:
	case BuiltinProc_simd_masked_load:
	case BuiltinProc_simd_masked_store:
	case BuiltinProc_simd_masked_expand_load:
	case BuiltinProc_simd_masked_compress_store:
		// these take pointers, so the vector type is not the type of the
		// first argument nor of the result (which may be nothing at all)
		return wb_build_simd_memory_builtin(p, expr, id);
	default:
		break;
	}
	Type *type = expr->tav.type;
	if (type != nullptr && is_type_untyped(type)) {
		type = default_type(type);
	}
	Slice<Ast *> const &args = ce->args;
	Type *arg0_type = args.count > 0 ? type_of_expr(args[0]) : nullptr;
	Type *vt = arg0_type != nullptr && is_type_simd_vector(arg0_type) ? arg0_type : type;
	Type *bvt = vt != nullptr ? base_type(vt) : nullptr;
	if (bvt == nullptr || bvt->kind != Type_SimdVector) {
		wb_unsupported(p, expr, "simd builtin procedure");
		return wb_value_invalid();
	}
	Type *elem = bvt->SimdVector.elem;
	i64 count = bvt->SimdVector.count;
	if (wb_valtype_of(elem) == wbValType_Invalid) {
		wb_unsupported_type(p, expr, vt);
		return wb_value_invalid();
	}

	switch (id) {
	// Lane-wise binary operators
	case BuiltinProc_simd_add:
	case BuiltinProc_simd_sub:
	case BuiltinProc_simd_mul:
	case BuiltinProc_simd_div:
	case BuiltinProc_simd_shl:
	case BuiltinProc_simd_shr:
	case BuiltinProc_simd_shl_masked:
	case BuiltinProc_simd_shr_masked:
	case BuiltinProc_simd_bit_and:
	case BuiltinProc_simd_bit_or:
	case BuiltinProc_simd_bit_xor:
	case BuiltinProc_simd_bit_and_not:
	case BuiltinProc_simd_min:
	case BuiltinProc_simd_max:
	case BuiltinProc_simd_lanes_eq:
	case BuiltinProc_simd_lanes_ne:
	case BuiltinProc_simd_lanes_lt:
	case BuiltinProc_simd_lanes_le:
	case BuiltinProc_simd_lanes_gt:
	case BuiltinProc_simd_lanes_ge: {
		Type *rhs_type = type_of_expr(args[1]);
		wbAddr a = wb_simd_arg(p, args[0], vt);
		wbAddr b = wb_simd_arg(p, args[1], rhs_type);
		if (a.kind == wbAddr_Invalid || b.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		Type *belem = base_type(rhs_type)->SimdVector.elem;
		Type *relem = base_type(type)->SimdVector.elem;
		wbAddr res = wb_add_temp(p, type);
		for (i64 i = 0; i < count; i++) {
			wbValue x = wb_simd_lane_load(p, a, i);
			wbValue y = wb_simd_lane_load(p, b, i);
			wbValue r = {};
			switch (id) {
			case BuiltinProc_simd_add:         r = wb_emit_arith(p, expr, Token_Add,    x, y, elem, elem); break;
			case BuiltinProc_simd_sub:         r = wb_emit_arith(p, expr, Token_Sub,    x, y, elem, elem); break;
			case BuiltinProc_simd_mul:         r = wb_emit_arith(p, expr, Token_Mul,    x, y, elem, elem); break;
			case BuiltinProc_simd_div:         r = wb_emit_arith(p, expr, Token_Quo,    x, y, elem, elem); break;
			case BuiltinProc_simd_bit_and:     r = wb_emit_arith(p, expr, Token_And,    x, y, elem, elem); break;
			case BuiltinProc_simd_bit_or:      r = wb_emit_arith(p, expr, Token_Or,     x, y, elem, elem); break;
			case BuiltinProc_simd_bit_xor:     r = wb_emit_arith(p, expr, Token_Xor,    x, y, elem, elem); break;
			case BuiltinProc_simd_bit_and_not: r = wb_emit_arith(p, expr, Token_AndNot, x, y, elem, elem); break;
			case BuiltinProc_simd_shl:         r = wb_emit_arith(p, expr, Token_Shl,    x, wb_emit_conv(p, y, elem), elem, elem); break;
			case BuiltinProc_simd_shr:         r = wb_emit_arith(p, expr, Token_Shr,    x, wb_emit_conv(p, y, elem), elem, elem); break;
			case BuiltinProc_simd_shl_masked:
			case BuiltinProc_simd_shr_masked: {
				// the shift amount wraps around the lane width
				wbValue amount = wb_emit_arith(p, expr, Token_And, y, wb_value_const_int(belem, 8*type_size_of(elem)-1), belem, belem);
				r = wb_emit_arith(p, expr, id == BuiltinProc_simd_shl_masked ? Token_Shl : Token_Shr, x, wb_emit_conv(p, amount, elem), elem, elem);
				break;
			}
			case BuiltinProc_simd_min:         r = wb_emit_min_max(p, x, y, elem, false); break;
			case BuiltinProc_simd_max:         r = wb_emit_min_max(p, x, y, elem, true);  break;
			case BuiltinProc_simd_lanes_eq: r = wb_simd_lane_mask(p, wb_emit_arith(p, expr, Token_CmpEq, x, y, elem, t_bool), relem); break;
			case BuiltinProc_simd_lanes_ne: r = wb_simd_lane_mask(p, wb_emit_arith(p, expr, Token_NotEq, x, y, elem, t_bool), relem); break;
			case BuiltinProc_simd_lanes_lt: r = wb_simd_lane_mask(p, wb_emit_arith(p, expr, Token_Lt,    x, y, elem, t_bool), relem); break;
			case BuiltinProc_simd_lanes_le: r = wb_simd_lane_mask(p, wb_emit_arith(p, expr, Token_LtEq,  x, y, elem, t_bool), relem); break;
			case BuiltinProc_simd_lanes_gt: r = wb_simd_lane_mask(p, wb_emit_arith(p, expr, Token_Gt,    x, y, elem, t_bool), relem); break;
			case BuiltinProc_simd_lanes_ge: r = wb_simd_lane_mask(p, wb_emit_arith(p, expr, Token_GtEq,  x, y, elem, t_bool), relem); break;
			default: break;
			}
			if (r.kind == wbValue_Invalid) {
				return r;
			}
			wb_simd_lane_store(p, res, i, r);
		}
		return wb_value_memory(res.index, res.offset, type);
	}

	// Lane-wise unary operators
	case BuiltinProc_simd_neg:
	case BuiltinProc_simd_abs:
	case BuiltinProc_simd_ceil:
	case BuiltinProc_simd_floor:
	case BuiltinProc_simd_trunc:
	case BuiltinProc_simd_nearest: {
		wbAddr a = wb_simd_arg(p, args[0], vt);
		if (a.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		wbValType evt = wb_valtype_of(elem);
		bool is_float = evt == wbValType_f32 || evt == wbValType_f64;
		wbAddr res = wb_add_temp(p, type);
		for (i64 i = 0; i < count; i++) {
			wbValue x = wb_simd_lane_load(p, a, i);
			wbValue r = {};
			if (id == BuiltinProc_simd_neg) {
				r = wb_emit_arith(p, expr, Token_Sub, wb_emit_conv(p, wb_value_const_int(t_int, 0), elem), x, elem, elem);
			} else if (id == BuiltinProc_simd_abs && !is_float) {
				// select(x, -x, x >= 0)
				x = wb_value_to_local(p, x);
				wbValue neg = wb_emit_arith(p, expr, Token_Sub, wb_emit_conv(p, wb_value_const_int(t_int, 0), elem), x, elem, elem);
				wb_push(p, x);
				wb_push(p, neg);
				wb_push(p, wb_emit_arith(p, expr, Token_GtEq, x, wb_emit_conv(p, wb_value_const_int(t_int, 0), elem), elem, t_bool));
				wb_op(p, wbOp_select);
				r = wb_pop_to_local(p, evt, elem);
			} else if (is_float) {
				wb_push(p, x);
				bool is_64 = evt == wbValType_f64;
				switch (id) {
				case BuiltinProc_simd_abs:     wb_op(p, is_64 ? wbOp_f64_abs     : wbOp_f32_abs);     break;
				case BuiltinProc_simd_ceil:    wb_op(p, is_64 ? wbOp_f64_ceil    : wbOp_f32_ceil);    break;
				case BuiltinProc_simd_floor:   wb_op(p, is_64 ? wbOp_f64_floor   : wbOp_f32_floor);   break;
				case BuiltinProc_simd_trunc:   wb_op(p, is_64 ? wbOp_f64_trunc   : wbOp_f32_trunc);   break;
				case BuiltinProc_simd_nearest: wb_op(p, is_64 ? wbOp_f64_nearest : wbOp_f32_nearest); break;
				default: break;
				}
				r = wb_pop_to_local(p, evt, elem);
			} else {
				wb_unsupported(p, expr, "simd builtin procedure on this lane type");
				return wb_value_invalid();
			}
			if (r.kind == wbValue_Invalid) {
				return r;
			}
			wb_simd_lane_store(p, res, i, r);
		}
		return wb_value_memory(res.index, res.offset, type);
	}

	case BuiltinProc_simd_clamp: {
		wbAddr a  = wb_simd_arg(p, args[0], vt);
		wbAddr lo = wb_simd_arg(p, args[1], vt);
		wbAddr hi = wb_simd_arg(p, args[2], vt);
		if (a.kind == wbAddr_Invalid || lo.kind == wbAddr_Invalid || hi.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		wbAddr res = wb_add_temp(p, type);
		for (i64 i = 0; i < count; i++) {
			wbValue r = wb_emit_min_max(p, wb_simd_lane_load(p, a, i), wb_simd_lane_load(p, lo, i), elem, true);
			r = wb_emit_min_max(p, r, wb_simd_lane_load(p, hi, i), elem, false);
			wb_simd_lane_store(p, res, i, r);
		}
		return wb_value_memory(res.index, res.offset, type);
	}

	// Reductions
	case BuiltinProc_simd_reduce_add_bisect:
	case BuiltinProc_simd_reduce_mul_bisect:
	case BuiltinProc_simd_reduce_add_ordered:
	case BuiltinProc_simd_reduce_mul_ordered:
	case BuiltinProc_simd_reduce_min:
	case BuiltinProc_simd_reduce_max:
	case BuiltinProc_simd_reduce_and:
	case BuiltinProc_simd_reduce_or:
	case BuiltinProc_simd_reduce_xor: {
		wbAddr a = wb_simd_arg(p, args[0], vt);
		if (a.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		wbValue acc = wb_value_to_local(p, wb_simd_lane_load(p, a, 0));
		for (i64 i = 1; i < count; i++) {
			wbValue x = wb_simd_lane_load(p, a, i);
			switch (id) {
			case BuiltinProc_simd_reduce_add_bisect:
			case BuiltinProc_simd_reduce_add_ordered: acc = wb_emit_arith(p, expr, Token_Add, acc, x, elem, elem); break;
			case BuiltinProc_simd_reduce_mul_bisect:
			case BuiltinProc_simd_reduce_mul_ordered: acc = wb_emit_arith(p, expr, Token_Mul, acc, x, elem, elem); break;
			case BuiltinProc_simd_reduce_and:         acc = wb_emit_arith(p, expr, Token_And, acc, x, elem, elem); break;
			case BuiltinProc_simd_reduce_or:          acc = wb_emit_arith(p, expr, Token_Or,  acc, x, elem, elem); break;
			case BuiltinProc_simd_reduce_xor:         acc = wb_emit_arith(p, expr, Token_Xor, acc, x, elem, elem); break;
			case BuiltinProc_simd_reduce_min:         acc = wb_emit_min_max(p, acc, x, elem, false); break;
			case BuiltinProc_simd_reduce_max:         acc = wb_emit_min_max(p, acc, x, elem, true);  break;
			default: break;
			}
			if (acc.kind == wbValue_Invalid) {
				return acc;
			}
			acc = wb_value_to_local(p, acc);
		}
		return wb_emit_conv(p, acc, type);
	}
	case BuiltinProc_simd_reduce_any:
	case BuiltinProc_simd_reduce_all: {
		// any: some lane is non-zero; all: every lane is non-zero
		wbAddr a = wb_simd_arg(p, args[0], vt);
		if (a.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		wbValue zero = wb_emit_conv(p, wb_value_const_int(t_int, 0), elem);
		wbValue acc = wb_value_to_local(p, wb_emit_arith(p, expr, Token_NotEq, wb_simd_lane_load(p, a, 0), zero, elem, t_bool));
		for (i64 i = 1; i < count; i++) {
			wbValue x = wb_emit_arith(p, expr, Token_NotEq, wb_simd_lane_load(p, a, i), zero, elem, t_bool);
			wb_push(p, acc);
			wb_push(p, x);
			wb_op(p, id == BuiltinProc_simd_reduce_any ? wbOp_i32_or : wbOp_i32_and);
			acc = wb_pop_to_local(p, wbValType_i32, t_bool);
		}
		return wb_emit_conv(p, acc, type);
	}

	case BuiltinProc_simd_extract: {
		wbAddr a = wb_simd_arg(p, args[0], vt);
		if (a.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		wbValue index = wb_emit_conv(p, wb_build_expr(p, args[1]), t_int);
		if (index.kind == wbValue_Invalid) {
			return index;
		}
		wbAddr lane = wb_emit_elem_addr(p, a.index, a.offset, index, elem);
		return wb_emit_conv(p, wb_addr_load(p, lane), type);
	}
	case BuiltinProc_simd_replace: {
		wbAddr a = wb_simd_arg(p, args[0], vt);
		if (a.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		wbValue index = wb_emit_conv(p, wb_build_expr(p, args[1]), t_int);
		if (wb_expr_has_call(args[2])) index = wb_value_fresh(p, index);
		wbValue x = wb_emit_conv(p, wb_build_expr(p, args[2]), elem);
		if (index.kind == wbValue_Invalid || x.kind == wbValue_Invalid) {
			return wb_value_invalid();
		}
		wbAddr lane = wb_emit_elem_addr(p, a.index, a.offset, index, elem);
		wb_addr_store(p, lane, x);
		return wb_value_memory(a.index, a.offset, type);
	}

	case BuiltinProc_simd_select: {
		// cond lane != 0 ? true_lane : false_lane
		Type *cond_type = type_of_expr(args[0]);
		wbAddr c = wb_simd_arg(p, args[0], cond_type);
		wbAddr a = wb_simd_arg(p, args[1], type);
		wbAddr b = wb_simd_arg(p, args[2], type);
		if (c.kind == wbAddr_Invalid || a.kind == wbAddr_Invalid || b.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		Type *celem = base_type(cond_type)->SimdVector.elem;
		Type *relem = base_type(type)->SimdVector.elem;
		wbValType rvt = wb_valtype_of(relem);
		wbAddr res = wb_add_temp(p, type);
		for (i64 i = 0; i < count; i++) {
			wb_push(p, wb_simd_lane_load(p, a, i));
			wb_push(p, wb_simd_lane_load(p, b, i));
			wb_push(p, wb_simd_lane_load(p, c, i));
			wb_op(p, wb_valtype_of(celem) == wbValType_i64 ? wbOp_i64_eqz : wbOp_i32_eqz);
			wb_op(p, wbOp_i32_eqz);
			wb_op(p, wbOp_select);
			wb_simd_lane_store(p, res, i, wb_pop_to_local(p, rvt, relem));
		}
		return wb_value_memory(res.index, res.offset, type);
	}

	case BuiltinProc_simd_shuffle: {
		// constant indices into the concatenation of both vectors
		wbAddr a = wb_simd_arg(p, args[0], vt);
		wbAddr b = wb_simd_arg(p, args[1], vt);
		if (a.kind == wbAddr_Invalid || b.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		wbAddr res = wb_add_temp(p, type);
		for (isize i = 2; i < args.count; i++) {
			TypeAndValue tav = type_and_value_of_expr(args[i]);
			GB_ASSERT(tav.value.kind == ExactValue_Integer);
			i64 index = exact_value_to_i64(tav.value);
			wbValue x = index < count ? wb_simd_lane_load(p, a, index) : wb_simd_lane_load(p, b, index-count);
			wb_simd_lane_store(p, res, i-2, x);
		}
		return wb_value_memory(res.index, res.offset, type);
	}
	case BuiltinProc_simd_lanes_reverse: {
		wbAddr a = wb_simd_arg(p, args[0], vt);
		if (a.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		wbAddr res = wb_add_temp(p, type);
		for (i64 i = 0; i < count; i++) {
			wb_simd_lane_store(p, res, i, wb_simd_lane_load(p, a, count-1-i));
		}
		return wb_value_memory(res.index, res.offset, type);
	}
	case BuiltinProc_simd_lanes_rotate_left:
	case BuiltinProc_simd_lanes_rotate_right: {
		wbAddr a = wb_simd_arg(p, args[0], vt);
		if (a.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		TypeAndValue tav = type_and_value_of_expr(args[1]);
		GB_ASSERT(tav.value.kind == ExactValue_Integer);
		i64 n = exact_value_to_i64(tav.value) % count;
		if (id == BuiltinProc_simd_lanes_rotate_right) {
			n = (count - n) % count;
		}
		wbAddr res = wb_add_temp(p, type);
		for (i64 i = 0; i < count; i++) {
			wb_simd_lane_store(p, res, i, wb_simd_lane_load(p, a, (i+n) % count));
		}
		return wb_value_memory(res.index, res.offset, type);
	}

	case BuiltinProc_simd_indices: {
		// {0, 1, 2, ...}
		wbAddr res = wb_add_temp(p, type);
		for (i64 i = 0; i < count; i++) {
			wb_simd_lane_store(p, res, i, wb_emit_conv(p, wb_value_const_int(t_int, i), elem));
		}
		return wb_value_memory(res.index, res.offset, type);
	}

	case BuiltinProc_simd_to_bits:
	case BuiltinProc_simd_to_bits_signed: {
		// the lanes keep their representation
		wbValue v = wb_build_expr(p, args[0]);
		if (v.kind == wbValue_Invalid) {
			return v;
		}
		v = wb_value_copy(p, v);
		v.type = type;
		return v;
	}

	case BuiltinProc_simd_approx_recip:
	case BuiltinProc_simd_approx_recip_sqrt: {
		// exact 1/x and 1/sqrt(x). Only an approximation is promised, and LLVM
		// takes that liberty: a reciprocal estimate instruction on x86, and a
		// fast inverse square root for f64 lanes anywhere. Dividing is within
		// the contract and is what LLVM does for f32 lanes off x86 as well.
		wbAddr a = wb_simd_arg(p, args[0], vt);
		if (a.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		wbValType evt = wb_valtype_of(elem);
		if (evt != wbValType_f32 && evt != wbValType_f64) {
			wb_unsupported(p, expr, "simd builtin procedure on this lane type");
			return wb_value_invalid();
		}
		wbValue one = wb_emit_conv(p, wb_value_const_int(t_int, 1), elem);
		wbAddr res = wb_add_temp(p, type);
		for (i64 i = 0; i < count; i++) {
			wbValue x = wb_simd_lane_load(p, a, i);
			if (id == BuiltinProc_simd_approx_recip_sqrt) {
				wb_push(p, x);
				wb_op(p, evt == wbValType_f64 ? wbOp_f64_sqrt : wbOp_f32_sqrt);
				x = wb_pop_to_local(p, evt, elem);
			}
			wb_simd_lane_store(p, res, i, wb_emit_arith(p, expr, Token_Quo, one, x, elem, elem));
		}
		return wb_value_memory(res.index, res.offset, type);
	}

	case BuiltinProc_simd_saturating_add:
	case BuiltinProc_simd_saturating_sub: {
		// wrapping arithmetic followed by an overflow test; an overflowing lane
		// takes the nearest end of the lane type's range
		wbAddr a = wb_simd_arg(p, args[0], vt);
		wbAddr b = wb_simd_arg(p, args[1], vt);
		if (a.kind == wbAddr_Invalid || b.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		bool is_add = id == BuiltinProc_simd_saturating_add;
		bool is_signed = wb_type_is_signed(elem);
		wbValType evt = wb_valtype_of(elem);
		i64 bits = 8*type_size_of(elem);
		i64 umax = bits >= 64 ? cast(i64)0xffffffffffffffffull : cast(i64)((cast(u64)1<<bits) - 1);
		i64 smax = bits >= 64 ? cast(i64)0x7fffffffffffffffull : (cast(i64)1<<(bits-1)) - 1;
		i64 smin = bits >= 64 ? cast(i64)0x8000000000000000ull : -(cast(i64)1<<(bits-1));
		wbValue zero = wb_value_const_int(elem, 0);
		wbAddr res = wb_add_temp(p, type);
		for (i64 i = 0; i < count; i++) {
			wbValue x = wb_value_to_local(p, wb_simd_lane_load(p, a, i));
			wbValue y = wb_value_to_local(p, wb_simd_lane_load(p, b, i));
			wbValue r = wb_value_to_local(p, wb_emit_arith(p, expr, is_add ? Token_Add : Token_Sub, x, y, elem, elem));
			wbValue over = {};
			wbValue sat  = {};
			if (is_signed) {
				// the sign of the result went the wrong way
				wbValue l = wb_emit_arith(p, expr, Token_Xor, x, r, elem, elem);
				wbValue m = is_add ? wb_emit_arith(p, expr, Token_Xor, y, r, elem, elem)
				                   : wb_emit_arith(p, expr, Token_Xor, x, y, elem, elem);
				wbValue bad = wb_emit_arith(p, expr, Token_And, l, m, elem, elem);
				over = wb_emit_arith(p, expr, Token_Lt, bad, zero, elem, t_bool);
				// select(min, max, x < 0)
				wb_push(p, wb_value_const_int(elem, smin));
				wb_push(p, wb_value_const_int(elem, smax));
				wb_push(p, wb_emit_arith(p, expr, Token_Lt, x, zero, elem, t_bool));
				wb_op(p, wbOp_select);
				sat = wb_pop_to_local(p, evt, elem);
			} else {
				over = is_add ? wb_emit_arith(p, expr, Token_Lt, r, x, elem, t_bool)
				              : wb_emit_arith(p, expr, Token_Lt, x, y, elem, t_bool);
				sat = wb_value_const_int(elem, is_add ? umax : 0);
			}
			if (over.kind == wbValue_Invalid) {
				return over;
			}
			wb_push(p, sat);
			wb_push(p, r);
			wb_push(p, over);
			wb_op(p, wbOp_select);
			wb_simd_lane_store(p, res, i, wb_pop_to_local(p, evt, elem));
		}
		return wb_value_memory(res.index, res.offset, type);
	}

	case BuiltinProc_simd_extract_lsbs:
	case BuiltinProc_simd_extract_msbs: {
		// a bit_set holding the lowest / highest bit of every lane
		wbAddr a = wb_simd_arg(p, args[0], vt);
		if (a.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		wbValType rvt = wb_valtype_of(type);
		if (rvt != wbValType_i32 && rvt != wbValType_i64) {
			wb_unsupported_type(p, expr, type);
			return wb_value_invalid();
		}
		i64 bits = 8*type_size_of(elem);
		// boolean lanes are tested as the unsigned integer of the same width
		Type *ielem = elem;
		if (!is_type_integer(ielem)) {
			switch (type_size_of(elem)) {
			case 1:  ielem = t_u8;  break;
			case 2:  ielem = t_u16; break;
			case 4:  ielem = t_u32; break;
			default: ielem = t_u64; break;
			}
		}
		i64 bit = 1;
		if (id == BuiltinProc_simd_extract_msbs) {
			bit = bits >= 64 ? cast(i64)0x8000000000000000ull : (cast(i64)1<<(bits-1));
		}
		wbValue mask = wb_value_const_int(ielem, bit);
		wbValue zero = wb_value_const_int(ielem, 0);
		u32 acc = wb_add_local(p, rvt);
		if (rvt == wbValType_i64) wb_i64_const(p, 0); else wb_i32_const(p, 0);
		wb_local_set(p, acc);
		for (i64 i = 0; i < count; i++) {
			wbValue x = wb_emit_arith(p, expr, Token_And, wb_simd_lane_load(p, a, i), mask, ielem, ielem);
			wbValue set = wb_emit_arith(p, expr, Token_NotEq, x, zero, ielem, t_bool);
			if (set.kind == wbValue_Invalid) {
				return set;
			}
			wb_local_get(p, acc);
			wb_push(p, set);
			if (rvt == wbValType_i64) {
				wb_op(p, wbOp_i64_extend_i32_u);
				wb_i64_const(p, i);
				wb_op(p, wbOp_i64_shl);
				wb_op(p, wbOp_i64_or);
			} else {
				wb_i32_const(p, cast(i32)i);
				wb_op(p, wbOp_i32_shl);
				wb_op(p, wbOp_i32_or);
			}
			wb_local_set(p, acc);
		}
		return wb_value_local(acc, rvt, type);
	}

	case BuiltinProc_simd_interleave: {
		// a riffle of every argument: result[i*n + j] == args[j][i]
		i64 n = cast(i64)args.count;
		wbAddr *srcs = gb_alloc_array(temporary_allocator(), wbAddr, n);
		for (i64 j = 0; j < n; j++) {
			srcs[j] = wb_simd_arg(p, args[j], vt);
			if (srcs[j].kind == wbAddr_Invalid) {
				return wb_value_invalid();
			}
		}
		wbAddr res = wb_add_temp(p, type);
		for (i64 i = 0; i < count; i++) {
			for (i64 j = 0; j < n; j++) {
				wb_simd_lane_store(p, res, i*n + j, wb_simd_lane_load(p, srcs[j], i));
			}
		}
		return wb_value_memory(res.index, res.offset, type);
	}
	case BuiltinProc_simd_deinterleave: {
		// the inverse: output `j` is the input strided by `n` starting at lane `j`
		wbAddr a = wb_simd_arg(p, args[0], vt);
		if (a.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		TypeAndValue tav = type_and_value_of_expr(args[1]);
		GB_ASSERT(tav.value.kind == ExactValue_Integer);
		i64 n = exact_value_to_i64(tav.value);
		i64 part = count / n;
		bool is_tuple = base_type(type)->kind == Type_Tuple;
		wbAddr res = wb_add_temp(p, type);
		for (i64 j = 0; j < n; j++) {
			wbAddr out = is_tuple ? wb_addr_field(res, j) : res;
			for (i64 i = 0; i < part; i++) {
				wb_simd_lane_store(p, out, i, wb_simd_lane_load(p, a, i*n + j));
			}
		}
		return wb_value_memory(res.index, res.offset, type);
	}

	case BuiltinProc_simd_odd_even: {
		// the odd lanes of `a` followed by the even lanes of `b`
		wbAddr a = wb_simd_arg(p, args[0], vt);
		wbAddr b = wb_simd_arg(p, args[1], vt);
		if (a.kind == wbAddr_Invalid || b.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		i64 half = count/2;
		wbAddr res = wb_add_temp(p, type);
		for (i64 i = 0; i < half; i++) {
			wb_simd_lane_store(p, res, i,      wb_simd_lane_load(p, a, 2*i + 1));
			wb_simd_lane_store(p, res, i+half, wb_simd_lane_load(p, b, 2*i));
		}
		return wb_value_memory(res.index, res.offset, type);
	}
	case BuiltinProc_simd_pairwise_add:
	case BuiltinProc_simd_pairwise_sub: {
		// {a0+a1, a2+a3, ..., b0+b1, b2+b3, ...}
		wbAddr a = wb_simd_arg(p, args[0], vt);
		wbAddr b = wb_simd_arg(p, args[1], vt);
		if (a.kind == wbAddr_Invalid || b.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		TokenKind op = id == BuiltinProc_simd_pairwise_add ? Token_Add : Token_Sub;
		i64 half = count/2;
		wbAddr res = wb_add_temp(p, type);
		for (i64 i = 0; i < half; i++) {
			wbValue x = wb_emit_arith(p, expr, op, wb_simd_lane_load(p, a, 2*i), wb_simd_lane_load(p, a, 2*i + 1), elem, elem);
			wbValue y = wb_emit_arith(p, expr, op, wb_simd_lane_load(p, b, 2*i), wb_simd_lane_load(p, b, 2*i + 1), elem, elem);
			if (x.kind == wbValue_Invalid || y.kind == wbValue_Invalid) {
				return wb_value_invalid();
			}
			wb_simd_lane_store(p, res, i,      x);
			wb_simd_lane_store(p, res, i+half, y);
		}
		return wb_value_memory(res.index, res.offset, type);
	}

	case BuiltinProc_simd_reduce_add_pairs:
	case BuiltinProc_simd_reduce_mul_pairs: {
		// a tree reduction that always combines adjacent lanes; the private copy
		// of the argument is folded in place
		wbAddr a = wb_simd_arg(p, args[0], vt);
		if (a.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		TokenKind op = id == BuiltinProc_simd_reduce_add_pairs ? Token_Add : Token_Mul;
		for (i64 rem = count/2; rem >= 1; rem /= 2) {
			for (i64 i = 0; i < rem; i++) {
				wbValue x = wb_simd_lane_load(p, a, 2*i);
				wbValue y = wb_simd_lane_load(p, a, 2*i + 1);
				wbValue r = wb_emit_arith(p, expr, op, x, y, elem, elem);
				if (r.kind == wbValue_Invalid) {
					return r;
				}
				wb_simd_lane_store(p, a, i, r);
			}
		}
		return wb_emit_conv(p, wb_simd_lane_load(p, a, 0), type);
	}

	case BuiltinProc_simd_sums_of_n: {
		// the sums of every `n` consecutive lanes
		wbAddr a = wb_simd_arg(p, args[0], vt);
		if (a.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		TypeAndValue tav = type_and_value_of_expr(args[1]);
		GB_ASSERT(tav.value.kind == ExactValue_Integer);
		i64 n = exact_value_to_i64(tav.value);
		i64 result_count = count / n;
		wbValType evt = wb_valtype_of(elem);
		bool is_float = evt == wbValType_f32 || evt == wbValType_f64;
		// LLVM only pairs the lanes up with a plain addition when a single one
		// suffices, otherwise it reduces starting from a zero, which for floats
		// is an observable difference (-0.0 + 0.0 == 0.0)
		bool start_at_zero = is_float && !(n == 2 && result_count > 1);
		wbAddr res = {};
		if (result_count > 1) {
			res = wb_add_temp(p, type);
		}
		wbValue acc = {};
		for (i64 j = 0; j < result_count; j++) {
			i64 i = 0;
			if (start_at_zero) {
				acc = wb_emit_conv(p, wb_value_const_int(t_int, 0), elem);
			} else {
				acc = wb_simd_lane_load(p, a, j*n);
				i = 1;
			}
			for (; i < n; i++) {
				acc = wb_emit_arith(p, expr, Token_Add, acc, wb_simd_lane_load(p, a, j*n + i), elem, elem);
				if (acc.kind == wbValue_Invalid) {
					return acc;
				}
			}
			if (result_count > 1) {
				wb_simd_lane_store(p, res, j, acc);
			}
		}
		if (result_count > 1) {
			return wb_value_memory(res.index, res.offset, type);
		}
		return wb_emit_conv(p, acc, type);
	}

	case BuiltinProc_simd_runtime_swizzle: {
		wbAddr a = wb_simd_arg(p, args[0], vt);
		wbAddr b = wb_simd_arg(p, args[1], vt);
		if (a.kind == wbAddr_Invalid || b.kind == wbAddr_Invalid) {
			return wb_value_invalid();
		}
		// LLVM lowers 16 lanes of bytes on a wasm target to `llvm.wasm.swizzle`,
		// which reads the index as an unsigned byte and gives a zero for anything
		// outside the table. Every other shape goes through its emulation, which
		// masks the index down to the lane count instead.
		bool is_table_lookup = type_size_of(elem) == 1 && count == 16;
		wbValType evt = wb_valtype_of(elem);
		wbAddr res = wb_add_temp(p, type);
		for (i64 i = 0; i < count; i++) {
			wbValue index = wb_simd_lane_load(p, b, i);
			if (is_table_lookup) {
				wb_push(p, index);
				wb_i32_const(p, 0xff);
				wb_op(p, wbOp_i32_and);
				wbValue u = wb_pop_to_local(p, wbValType_i32, t_i32);
				wb_push(p, u);
				wb_i32_const(p, cast(i32)count);
				wb_op(p, wbOp_i32_lt_u);
				wbValue in_range = wb_pop_to_local(p, wbValType_i32, t_bool);
				// the load is always in the table, the out of range lanes are
				// selected away afterwards
				wb_push(p, u);
				wb_i32_const(p, cast(i32)(count-1));
				wb_op(p, wbOp_i32_and);
				wbValue safe = wb_pop_to_local(p, wbValType_i32, t_i32);
				wbValue x = wb_addr_load(p, wb_emit_elem_addr(p, a.index, a.offset, safe, elem));
				wb_push(p, x);
				wb_i32_const(p, 0);
				wb_push(p, in_range);
				wb_op(p, wbOp_select);
				wb_simd_lane_store(p, res, i, wb_pop_to_local(p, evt, elem));
			} else {
				wbValue m = wb_emit_arith(p, expr, Token_And, index, wb_value_const_int(elem, count-1), elem, elem);
				if (m.kind == wbValue_Invalid) {
					return m;
				}
				wb_simd_lane_store(p, res, i, wb_addr_load(p, wb_emit_elem_addr(p, a.index, a.offset, m, elem)));
			}
		}
		return wb_value_memory(res.index, res.offset, type);
	}

	default:
		break;
	}
	wb_unsupported(p, expr, "simd builtin procedure");
	return wb_value_invalid();
}
