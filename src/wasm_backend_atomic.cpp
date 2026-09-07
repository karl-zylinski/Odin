// Atomic intrinsics for the wasm backend.
//
// The generated modules are single threaded (no shared memory, no threads
// proposal), so every atomic operation is lowered to its plain, non-atomic
// equivalent and the memory orderings are ignored. Fences are no-ops.

gb_internal wbValue wb_build_atomic_call(wbProcedure *p, Ast *expr, BuiltinProcId id) {
	ast_node(ce, CallExpr, expr);
	Type *type = expr->tav.type;
	Slice<Ast *> const &args = ce->args;

	switch (id) {
	case BuiltinProc_atomic_type_is_lock_free:
		return wb_const(p, expr, t_bool, exact_value_bool(true));
	case BuiltinProc_atomic_thread_fence:
	case BuiltinProc_atomic_signal_fence:
		return wb_value_invalid();
	default:
		break;
	}

	wbValue ptr = wb_build_expr(p, args[0]);
	if (ptr.kind == wbValue_Invalid) return wb_value_invalid();
	Type *elem = type_deref(ptr.type);
	if (args.count > 1 && wb_expr_has_call(args[1])) ptr = wb_value_fresh(p, ptr);
	wbAddr dst = wb_addr_from_pointer(p, ptr, elem);

	switch (id) {
	case BuiltinProc_atomic_load:
	case BuiltinProc_atomic_load_explicit:
		return wb_addr_load(p, dst);

	case BuiltinProc_atomic_store:
	case BuiltinProc_atomic_store_explicit: {
		wbValue v = wb_emit_conv(p, wb_build_expr(p, args[1]), elem);
		if (v.kind == wbValue_Invalid) return v;
		wb_addr_store(p, dst, v);
		return wb_value_invalid();
	}

	case BuiltinProc_atomic_exchange:
	case BuiltinProc_atomic_exchange_explicit:
	case BuiltinProc_atomic_add:
	case BuiltinProc_atomic_add_explicit:
	case BuiltinProc_atomic_sub:
	case BuiltinProc_atomic_sub_explicit:
	case BuiltinProc_atomic_and:
	case BuiltinProc_atomic_and_explicit:
	case BuiltinProc_atomic_nand:
	case BuiltinProc_atomic_nand_explicit:
	case BuiltinProc_atomic_or:
	case BuiltinProc_atomic_or_explicit:
	case BuiltinProc_atomic_xor:
	case BuiltinProc_atomic_xor_explicit: {
		// returns the previous value
		wbValue v = wb_emit_conv(p, wb_build_expr(p, args[1]), elem);
		if (v.kind == wbValue_Invalid) return v;
		wbValue old = wb_value_fresh(p, wb_addr_load(p, dst));
		if (old.kind == wbValue_Invalid) return old;

		wbValue res = v;
		TokenKind op = Token_Invalid;
		switch (id) {
		case BuiltinProc_atomic_add:  case BuiltinProc_atomic_add_explicit:  op = Token_Add; break;
		case BuiltinProc_atomic_sub:  case BuiltinProc_atomic_sub_explicit:  op = Token_Sub; break;
		case BuiltinProc_atomic_and:  case BuiltinProc_atomic_and_explicit:
		case BuiltinProc_atomic_nand: case BuiltinProc_atomic_nand_explicit: op = Token_And; break;
		case BuiltinProc_atomic_or:   case BuiltinProc_atomic_or_explicit:   op = Token_Or;  break;
		case BuiltinProc_atomic_xor:  case BuiltinProc_atomic_xor_explicit:  op = Token_Xor; break;
		default: break;
		}
		if (op != Token_Invalid) {
			res = wb_emit_arith(p, expr, op, old, v, elem, elem);
			if (id == BuiltinProc_atomic_nand || id == BuiltinProc_atomic_nand_explicit) {
				res = wb_emit_arith(p, expr, Token_Xor, res, wb_const(p, expr, elem, exact_value_i64(-1)), elem, elem);
			}
		}
		if (res.kind == wbValue_Invalid) return res;
		wb_addr_store(p, dst, res);
		return old;
	}

	case BuiltinProc_atomic_compare_exchange_strong:
	case BuiltinProc_atomic_compare_exchange_weak:
	case BuiltinProc_atomic_compare_exchange_strong_explicit:
	case BuiltinProc_atomic_compare_exchange_weak_explicit: {
		// (old, old == expected); stores `new` when they compare equal
		if (wb_expr_has_call(args[2])) ptr = wb_value_fresh(p, ptr);
		dst = wb_addr_from_pointer(p, ptr, elem);
		wbValue expected = wb_emit_conv(p, wb_build_expr(p, args[1]), elem);
		if (expected.kind == wbValue_Invalid) return expected;
		if (wb_expr_has_call(args[2])) expected = wb_value_fresh(p, expected);
		wbValue new_value = wb_emit_conv(p, wb_build_expr(p, args[2]), elem);
		if (new_value.kind == wbValue_Invalid) return new_value;

		wbValue old = wb_value_fresh(p, wb_addr_load(p, dst));
		if (old.kind == wbValue_Invalid) return old;
		wbValue ok = wb_emit_compare(p, expr, Token_CmpEq, old, expected, elem, t_bool);
		if (ok.kind == wbValue_Invalid) return ok;
		ok = wb_value_to_local(p, ok);

		wb_push(p, ok);
		wb_open_if(p);
		wb_addr_store(p, dst, new_value);
		wb_close(p);

		if (is_type_tuple(type)) {
			wbAddr res = wb_add_temp(p, type);
			Type *bt = nullptr;
			i64 bool_offset = type_offset_of(type, 1, &bt);
			wb_addr_store(p, wb_addr_offset(res, 0, elem), old);
			wb_addr_store(p, wb_addr_offset(res, cast(i32)bool_offset, bt), ok);
			return wb_value_memory(res.index, res.offset, type);
		}
		return old;
	}

	default:
		wb_unsupported(p, expr, "atomic builtin procedure");
		return wb_value_invalid();
	}
}
