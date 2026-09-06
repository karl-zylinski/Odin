// Direct WebAssembly backend: statements

// Scopes, defers and labels

gb_internal void wb_open_scope(wbProcedure *p) {
	array_add(&p->scopes, p->defers.count);
}

// Emits the deferred statements registered at index >= first, in reverse order,
// without unregistering them (used for break/continue/return)
gb_internal void wb_emit_defers_down_to(wbProcedure *p, isize first) {
	for (isize i = p->defers.count-1; i >= first; i--) {
		wbDefer d = p->defers[i];
		// Statements inside a deferred statement must not see the defers registered
		// after it (nor itself), so temporarily hide them. Likewise they see the
		// `context` that was current when the defer was registered.
		isize saved_count = p->defers.count;
		p->defers.count = i;
		auto saved_contexts = array_make<wbContextData>(temporary_allocator(), 0, p->context_stack.count);
		for (isize j = d.context_stack_count; j < p->context_stack.count; j++) {
			array_add(&saved_contexts, p->context_stack[j]);
		}
		p->context_stack.count = d.context_stack_count;

		wb_open_scope(p);
		wb_build_stmt(p, d.stmt);
		wb_close_scope(p);

		p->context_stack.count = d.context_stack_count;
		for (wbContextData const &cd : saved_contexts) {
			array_add(&p->context_stack, cd);
		}
		p->defers.count = saved_count;
	}
}

gb_internal void wb_close_scope(wbProcedure *p) {
	isize marker = array_pop(&p->scopes);
	wb_emit_defers_down_to(p, marker);
	p->defers.count = marker;
	// `context` values created in this scope go out of scope
	isize scope_index = p->scopes.count;
	while (p->context_stack.count > 0 && p->context_stack[p->context_stack.count-1].scope_index >= scope_index) {
		p->context_stack.count -= 1;
	}
}

// True if the assignment target is `context` or a (nested) field of it
gb_internal bool wb_lhs_is_context(Ast *lhs) {
	lhs = unparen_expr(lhs);
	while (lhs->kind == Ast_SelectorExpr) {
		Ast *base = unparen_expr(lhs->SelectorExpr.expr);
		if (is_type_pointer(type_of_expr(base))) {
			return false;
		}
		lhs = base;
	}
	return lhs->kind == Ast_Implicit && lhs->Implicit.kind == Token_context;
}

gb_internal void wb_push_label(wbProcedure *p, Ast *label, u32 break_depth, u32 continue_depth, bool is_loop, bool is_switch = false, u32 fall_depth = 0) {
	wbLabel l = {};
	l.label = label;
	l.break_depth = break_depth;
	l.continue_depth = continue_depth;
	l.fall_depth = fall_depth;
	l.is_loop = is_loop;
	l.is_switch = is_switch;
	l.scope_index = p->defers.count;
	array_add(&p->labels, l);
}

gb_internal void wb_pop_label(wbProcedure *p) {
	array_pop(&p->labels);
}

gb_internal void wb_build_stmt_list(wbProcedure *p, Slice<Ast *> const &stmts) {
	for (Ast *stmt : stmts) {
		wb_build_stmt(p, stmt);
	}
}

// Value lists (expanding multiple-result calls)

gb_internal wbValue wb_tuple_field(wbProcedure *p, wbValue tuple, isize index) {
	Type *ft = nullptr;
	i64 offset = type_offset_of(tuple.type, index, &ft);
	if (tuple.kind != wbValue_Memory) {
		return wb_value_invalid();
	}
	if (wb_is_scalar(ft)) {
		return wb_emit_load(p, tuple.index, tuple.offset + cast(i32)offset, ft);
	}
	return wb_value_memory(tuple.index, tuple.offset + cast(i32)offset, ft);
}

gb_internal void wb_build_expr_list(wbProcedure *p, Slice<Ast *> const &exprs, Array<wbValue> *values) {
	for (Ast *expr : exprs) {
		wbValue v = wb_build_expr(p, expr);
		Type *t = v.type != nullptr ? base_type(v.type) : nullptr;
		if (t != nullptr && t->kind == Type_Tuple) {
			for_array(i, t->Tuple.variables) {
				array_add(values, wb_tuple_field(p, v, i));
			}
		} else {
			array_add(values, v);
		}
	}
}

// Simple statements

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

gb_internal void wb_build_assign_stmt(wbProcedure *p, AstAssignStmt *as, Ast *node) {
	if (as->op.kind == Token_Eq) {
		if (as->lhs.count == 1 && as->rhs.count == 1) {
			Ast *lhs = unparen_expr(as->lhs[0]);
			if (lhs->kind == Ast_Ident && is_blank_ident(lhs)) {
				wb_build_expr(p, as->rhs[0]);
				return;
			}
			if (wb_lhs_is_context(lhs)) {
				// Writing to the context copies it first (see wb_context_for_write)
				wbValue v = wb_value_fresh(p, wb_emit_conv(p, wb_build_expr(p, as->rhs[0]), type_of_expr(lhs)));
				wb_context_for_write(p);
				wb_addr_store(p, wb_build_addr(p, lhs), v);
				return;
			}
			wbAddr addr = wb_build_addr(p, lhs);
			wbValue v = wb_build_expr(p, as->rhs[0]);
			wb_addr_store(p, addr, v);
			return;
		}

		// Evaluate all addresses and values before storing (for `a, b = b, a`)
		auto addrs = array_make<wbAddr>(temporary_allocator(), 0, as->lhs.count);
		for (Ast *lhs : as->lhs) {
			lhs = unparen_expr(lhs);
			wbAddr addr = {};
			if (wb_lhs_is_context(lhs)) {
				wb_unsupported(p, lhs, "assignment to context in a multiple assignment");
				return;
			}
			if (!(lhs->kind == Ast_Ident && is_blank_ident(lhs))) {
				addr = wb_build_addr(p, lhs);
			}
			array_add(&addrs, addr);
		}
		auto values = array_make<wbValue>(temporary_allocator(), 0, as->lhs.count);
		wb_build_expr_list(p, as->rhs, &values);
		if (values.count != addrs.count) {
			wb_unsupported(p, node, "assignment with mismatched counts");
			return;
		}
		for_array(i, values) {
			if (addrs[i].kind != wbAddr_Invalid) {
				values[i] = wb_value_fresh(p, wb_emit_conv(p, values[i], addrs[i].type));
			}
		}
		for_array(i, addrs) {
			if (addrs[i].kind != wbAddr_Invalid) {
				wb_addr_store(p, addrs[i], values[i]);
			}
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
	if (wb_lhs_is_context(lhs)) {
		wb_context_for_write(p);
	}
	wbAddr addr = wb_build_addr(p, lhs);
	if (addr.kind == wbAddr_Invalid) {
		return;
	}
	wbValue left = wb_addr_load(p, addr);
	if (wb_expr_has_call(as->rhs[0])) {
		left = wb_value_fresh(p, left);
	}
	wbValue right = wb_build_expr(p, as->rhs[0]);
	if (op != Token_Shl && op != Token_Shr) {
		right = wb_emit_conv(p, right, type);
	}
	wbValue res = wb_emit_arith(p, node, op, left, right, type, type);
	wb_addr_store(p, addr, res);
}

gb_internal void wb_build_return_stmt(wbProcedure *p, AstReturnStmt *rs, Ast *node) {
	Type *pt = base_type(p->type);
	isize result_count = pt->Proc.result_count;
	if (result_count == 0) {
		wb_emit_defers_down_to(p, 0);
		wb_emit_epilogue(p);
		wb_op(p, wbOp_return);
		return;
	}
	TypeTuple *tuple = &pt->Proc.results->Tuple;

	// The returned values are fixed before the deferred statements run
	auto values = array_make<wbValue>(temporary_allocator(), 0, result_count);
	if (rs->results.count == 0) {
		GB_ASSERT(p->result_addrs.count == result_count);
		for (wbAddr addr : p->result_addrs) {
			array_add(&values, wb_addr_load(p, addr));
		}
	} else {
		wb_build_expr_list(p, rs->results, &values);
	}
	if (values.count != result_count) {
		wb_unsupported(p, node, "return value count");
		return;
	}
	bool has_defers = p->defers.count > 0;
	for_array(i, values) {
		values[i] = wb_emit_conv(p, values[i], tuple->variables[i]->type);
		if (values[i].kind == wbValue_Invalid) {
			return;
		}
		if (has_defers || rs->results.count == 0 || values[i].kind == wbValue_Memory) {
			values[i] = wb_value_fresh(p, values[i]);
		}
	}
	wb_emit_return_values(p, values, rs->results.count != 0);
}

// Returns the given (converted, fresh) values: runs the defers and leaves the procedure
gb_internal void wb_emit_return_values(wbProcedure *p, Array<wbValue> const &values, bool store_named) {
	Type *pt = base_type(p->type);
	GB_ASSERT(values.count == pt->Proc.result_count);
	if (store_named && p->result_addrs.count > 0) {
		for_array(i, values) {
			wb_addr_store(p, p->result_addrs[i], values[i]);
		}
	}

	wb_emit_defers_down_to(p, 0);

	if (p->sret_local >= 0) {
		for_array(i, values) {
			Type *ft = nullptr;
			i64 offset = type_offset_of(pt->Proc.results, i, &ft);
			wb_addr_store(p, wb_addr_memory(cast(u32)p->sret_local, cast(i32)offset, ft), values[i]);
		}
	} else if (values.count > 0) {
		wb_push(p, values[0]);
	}
	wb_emit_epilogue(p);
	wb_op(p, wbOp_return);
}

// Emits a break/continue/fallthrough (also used by `or_break`/`or_continue`)
gb_internal void wb_emit_branch(wbProcedure *p, TokenKind kind, Ast *label, Ast *node) {
	Ast *target_label = nullptr;
	if (label != nullptr) {
		Entity *e = entity_of_node(label);
		GB_ASSERT(e != nullptr && e->kind == Entity_Label);
		target_label = e->Label.node;
	}

	for (isize i = p->labels.count-1; i >= 0; i--) {
		wbLabel const &l = p->labels[i];
		if (target_label != nullptr) {
			if (l.label != target_label) {
				continue;
			}
		} else {
			// unlabelled: break targets loops and switches, continue loops, fallthrough switches
			switch (kind) {
			case Token_break:       if (!l.is_loop && !l.is_switch) continue; break;
			case Token_continue:    if (!l.is_loop) continue; break;
			case Token_fallthrough: if (!l.is_switch) continue; break;
			default: break;
			}
		}
		wb_emit_defers_down_to(p, l.scope_index);
		switch (kind) {
		case Token_break:
			wb_br(p, l.break_depth);
			break;
		case Token_continue:
			if (!l.is_loop) {
				wb_unsupported(p, node, "'continue' to a non-loop label");
				return;
			}
			wb_br(p, l.continue_depth);
			break;
		case Token_fallthrough:
			wb_br(p, l.fall_depth);
			break;
		default:
			wb_unsupported(p, node, "branch statement");
			break;
		}
		return;
	}
	wb_unsupported(p, node, "branch target");
}

gb_internal void wb_build_branch_stmt(wbProcedure *p, AstBranchStmt *bs, Ast *node) {
	wb_emit_branch(p, bs->token.kind, bs->label, node);
}

gb_internal void wb_build_value_decl(wbProcedure *p, AstValueDecl *vd, Ast *node) {
	if (!vd->is_mutable) {
		return;
	}

	// Static variables are initialized once with constants, not on every execution
	bool is_static = false;
	for (Ast *name : vd->names) {
		Entity *e = entity_of_node(name);
		if (e != nullptr && (e->flags & EntityFlag_Static)) {
			is_static = true;
		}
	}

	// Evaluate initializers before declaring the new variables
	// (`x: T = ---` is simply zero initialized here)
	bool is_uninit = vd->values.count == 1 && unparen_expr(vd->values[0])->kind == Ast_Uninit;
	auto values = array_make<wbValue>(temporary_allocator(), 0, vd->names.count);
	if (!is_static && !is_uninit) {
		wb_build_expr_list(p, vd->values, &values);
	}
	if (values.count != 0 && values.count != vd->names.count) {
		wb_unsupported(p, node, "declaration with mismatched counts");
		return;
	}

	for_array(i, vd->names) {
		Ast *name = vd->names[i];
		if (is_blank_ident(name)) {
			continue;
		}
		Entity *e = entity_of_node(name);
		GB_ASSERT(e != nullptr && e->kind == Entity_Variable);
		if (e->flags & EntityFlag_Static) {
			// Initialized once, in the data segment or the start function
			wb_add_variable(p, e, vd->values.count > i ? vd->values[i] : nullptr);
			continue;
		}
		wbAddr addr = wb_add_variable(p, e);
		if (values.count > 0) {
			wb_addr_store(p, addr, values[i]);
		} else {
			// Zero initialize (the declaration may be re-executed inside a loop)
			wb_addr_zero(p, addr);
		}
	}
}

// Control flow

gb_internal void wb_build_block_stmt(wbProcedure *p, AstBlockStmt *bs, Ast *node) {
	if (bs->label != nullptr) {
		u32 depth = wb_open_block(p);
		wb_push_label(p, bs->label, depth, 0, false);
		wb_open_scope(p);
		wb_build_stmt_list(p, bs->stmts);
		wb_close_scope(p);
		wb_pop_label(p);
		wb_close(p);
	} else {
		wb_open_scope(p);
		wb_build_stmt_list(p, bs->stmts);
		wb_close_scope(p);
	}
}

gb_internal void wb_build_if_stmt(wbProcedure *p, AstIfStmt *is, Ast *node) {
	wb_open_scope(p);
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
	wb_close_scope(p);
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
	wb_open_scope(p);
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
	wb_close_scope(p);
}

gb_internal Ast *wb_strip_and_prefix(Ast *ident) {
	if (ident != nullptr) {
		if (ident->kind == Ast_UnaryExpr && ident->UnaryExpr.op.kind == Token_And) {
			ident = ident->UnaryExpr.expr;
		}
		GB_ASSERT(ident->kind == Ast_Ident);
	}
	return ident;
}

// Declares a range statement variable and stores the current element into it
gb_internal void wb_store_range_val(wbProcedure *p, Ast *val, wbValue value) {
	if (val == nullptr || is_blank_ident(val)) {
		return;
	}
	Entity *e = entity_of_node(val);
	if (e == nullptr) {
		return;
	}
	wbAddr addr = wb_add_variable(p, e);
	wb_addr_store(p, addr, value);
}

// Binds a by-reference range variable (`for &v in x`) to the element's address
gb_internal void wb_bind_range_ref(wbProcedure *p, Ast *val, wbAddr elem) {
	Entity *e = entity_of_node(val);
	if (e == nullptr) {
		return;
	}
	if (elem.kind != wbAddr_Memory) {
		wb_unsupported(p, val, "reference to a register element (internal error)");
		return;
	}
	// The element address changes each iteration, so put it into its own local
	u32 local = wb_add_local(p, wbValType_i32, e->token.string);
	wb_push_address(p, elem.index, elem.offset);
	wb_local_set(p, local);
	map_set(&p->variables, e, wb_addr_memory(local, 0, e->type));
}

gb_internal bool wb_range_val_is_ref(Ast *val) {
	if (val == nullptr || is_blank_ident(val)) {
		return false;
	}
	Entity *e = entity_of_node(val);
	return e != nullptr && (e->flags & EntityFlag_Value) == 0;
}

gb_internal void wb_build_range_interval(wbProcedure *p, AstRangeStmt *rs, Ast *node, Ast *val0, Ast *val1) {
	ast_node(be, BinaryExpr, unparen_expr(rs->expr));
	if (rs->reverse) {
		wb_unsupported(p, node, "'#reverse' interval loop");
		return;
	}
	bool inclusive = be->op.kind != Token_RangeHalf;

	Type *val_type = val0 != nullptr && !is_blank_ident(val0) ? type_of_expr(val0) : nullptr;
	if (val_type == nullptr) {
		val_type = be->left->tav.type;
		if (is_type_untyped(val_type)) {
			val_type = default_type(val_type);
		}
	}
	wbValType vt = wb_valtype_of(val_type);
	if (vt != wbValType_i32 && vt != wbValType_i64) {
		wb_unsupported_type(p, node, val_type);
		return;
	}
	bool is_signed = wb_type_is_signed(val_type);

	// value = lower; index = 0
	wbValue lower = wb_emit_conv(p, wb_build_expr(p, be->left), val_type);
	if (lower.kind == wbValue_Invalid) {
		return;
	}
	u32 value = wb_add_local(p, vt);
	u32 index = wb_add_local(p, wbValType_i32);
	wb_push(p, lower);
	wb_local_set(p, value);
	wb_i32_const(p, 0);
	wb_local_set(p, index);

	u32 break_depth = wb_open_block(p);
	u32 loop_depth  = wb_open_loop(p);

	// The upper bound is re-evaluated every iteration
	wbValue upper = wb_emit_conv(p, wb_build_expr(p, be->right), val_type);
	if (upper.kind == wbValue_Invalid) {
		wb_close(p); wb_close(p);
		return;
	}
	upper = wb_value_to_local(p, upper);
	wb_local_get(p, value);
	wb_push(p, upper);
	wb_emit_binary_op(p, inclusive ? Token_LtEq : Token_Lt, vt, is_signed);
	wb_op(p, wbOp_i32_eqz);
	wb_br_if(p, break_depth);

	wb_open_scope(p);
	wb_store_range_val(p, val0, wb_value_local(value, vt, val_type));
	wb_store_range_val(p, val1, wb_value_local(index, wbValType_i32, t_int));

	u32 continue_depth = wb_open_block(p);
	wb_push_label(p, rs->label, break_depth, continue_depth, true);
	wb_build_stmt(p, rs->body);
	wb_pop_label(p);
	wb_close(p); // continue
	wb_close_scope(p);

	if (inclusive) {
		// Stop before incrementing past the upper bound (which may be the maximum value)
		wb_local_get(p, value);
		wb_push(p, upper);
		wb_op(p, vt == wbValType_i64 ? wbOp_i64_eq : wbOp_i32_eq);
		wb_br_if(p, break_depth);
	}
	wb_local_get(p, value);
	if (vt == wbValType_i64) { wb_i64_const(p, 1); wb_op(p, wbOp_i64_add); }
	else                     { wb_i32_const(p, 1); wb_op(p, wbOp_i32_add); wb_emit_normalize(p, val_type); }
	wb_local_set(p, value);
	wb_local_get(p, index);
	wb_i32_const(p, 1);
	wb_op(p, wbOp_i32_add);
	wb_local_set(p, index);
	wb_br(p, loop_depth);
	wb_close(p); // loop
	wb_close(p); // break
}

// Ranges over arrays, slices, strings (as bytes are not iterated: runes are) and dynamic arrays
gb_internal void wb_build_range_indexed(wbProcedure *p, AstRangeStmt *rs, Ast *node, Ast *val0, Ast *val1) {
	Ast *range_expr = unparen_expr(rs->expr);
	Type *expr_type = type_of_expr(range_expr);
	Type *bt = base_type(expr_type);
	bool val0_ref = wb_range_val_is_ref(val0);

	// Storage of the iterated value and its element count
	wbAddr base = {};
	wbValue count = {};
	Type *elem_type = nullptr;
	bool is_string = false;

	if (bt->kind == Type_Pointer) {
		// ^[N]T etc. iterate the pointed-to value in place
		Type *pointee = type_deref(expr_type);
		wbValue ptr = wb_build_expr(p, range_expr);
		if (ptr.kind == wbValue_Invalid) return;
		base = wb_addr_from_pointer(p, ptr, pointee);
		bt = base_type(pointee);
	} else if (val0_ref) {
		base = wb_build_addr(p, range_expr);
	} else {
		wbValue v = wb_build_expr(p, range_expr);
		if (v.kind == wbValue_Invalid) return;
		v = wb_value_copy(p, v);
		base = wb_value_to_addr(p, v);
	}
	if (base.kind != wbAddr_Memory) {
		return;
	}

	switch (bt->kind) {
	case Type_Array:
		count = wb_value_const_int(t_int, bt->Array.count);
		elem_type = bt->Array.elem;
		break;
	case Type_Slice:
		elem_type = bt->Slice.elem;
		break;
	case Type_DynamicArray:
		elem_type = bt->DynamicArray.elem;
		break;
	case Type_Basic:
		if (is_type_string(bt) && !is_type_cstring(bt)) {
			is_string = true;
			elem_type = t_rune;
			break;
		}
		/*fallthrough*/
	default:
		wb_unsupported_type(p, node, expr_type);
		return;
	}

	// Element base pointer for slice-like types
	u32 data_local = base.index;
	i32 data_offset = base.offset;
	if (bt->kind != Type_Array) {
		wbValue s = wb_value_memory(base.index, base.offset, bt);
		wbValue data = wb_emit_slice_data(p, s);
		count = wb_emit_slice_len(p, s);
		data_local = data.index;
		data_offset = 0;
	}
	count = wb_value_to_local(p, count);

	u32 index = wb_add_local(p, wbValType_i32);
	if (rs->reverse) {
		wb_push(p, count);
		if (!is_string) {
			wb_i32_const(p, 1);
			wb_op(p, wbOp_i32_sub);
		}
	} else {
		wb_i32_const(p, 0);
	}
	wb_local_set(p, index);

	u32 break_depth = wb_open_block(p);
	u32 loop_depth  = wb_open_loop(p);

	// Loop condition
	wb_local_get(p, index);
	if (rs->reverse) {
		wb_i32_const(p, 0);
		wb_op(p, is_string ? wbOp_i32_le_s : wbOp_i32_lt_s);
	} else {
		wb_push(p, count);
		wb_op(p, wbOp_i32_ge_s);
	}
	wb_br_if(p, break_depth);

	wb_open_scope(p);
	if (is_string) {
		// c, w := string_decode_rune(s[index:]) / string_decode_last_rune(s[:index])
		wbProcedure *decode = wb_lookup_runtime_procedure(p->module, rs->reverse ? "string_decode_last_rune" : "string_decode_rune");
		wbAddr arg = wb_add_temp(p, t_string);
		wbAddr result = wb_add_temp(p, base_type(decode->type)->Proc.results);
		if (rs->reverse) {
			wb_emit_store(p, arg.index, arg.offset, wb_value_local(data_local, wbValType_i32, t_rawptr), t_rawptr);
			wb_emit_store(p, arg.index, arg.offset + cast(i32)build_context.int_size, wb_value_local(index, wbValType_i32, t_int), t_int);
		} else {
			wb_local_get(p, data_local);
			wb_local_get(p, index);
			wb_op(p, wbOp_i32_add);
			wbValue data = wb_pop_to_local(p, wbValType_i32, t_rawptr);
			wb_emit_store(p, arg.index, arg.offset, data, t_rawptr);
			wb_push(p, count);
			wb_local_get(p, index);
			wb_op(p, wbOp_i32_sub);
			wbValue len = wb_pop_to_local(p, wbValType_i32, t_int);
			wb_emit_store(p, arg.index, arg.offset + cast(i32)build_context.int_size, len, t_int);
		}
		wb_push_address(p, result.index, result.offset);
		wb_push_address(p, arg.index, arg.offset);
		wb_call(p, decode);

		wbValue tuple = wb_value_memory(result.index, result.offset, result.type);
		wbValue r = wb_tuple_field(p, tuple, 0);
		wbValue w = wb_tuple_field(p, tuple, 1);
		if (rs->reverse) {
			// the rune ends at index, so the index of the rune is index - w
			wb_local_get(p, index);
			wb_push(p, w);
			wb_op(p, wbOp_i32_sub);
			wb_local_set(p, index);
			wb_store_range_val(p, val0, r);
			wb_store_range_val(p, val1, wb_value_local(index, wbValType_i32, t_int));
		} else {
			wb_store_range_val(p, val0, r);
			wb_store_range_val(p, val1, wb_value_local(index, wbValType_i32, t_int));
			// advance past the rune
			wb_local_get(p, index);
			wb_push(p, w);
			wb_op(p, wbOp_i32_add);
			wb_local_set(p, index);
		}
	} else {
		wbAddr elem = wb_emit_elem_addr(p, data_local, data_offset, wb_value_local(index, wbValType_i32, t_int), elem_type);
		if (val0 != nullptr && !is_blank_ident(val0)) {
			if (val0_ref) {
				wb_bind_range_ref(p, val0, elem);
			} else {
				wb_store_range_val(p, val0, wb_addr_load(p, elem));
			}
		}
		wb_store_range_val(p, val1, wb_value_local(index, wbValType_i32, t_int));
	}

	u32 continue_depth = wb_open_block(p);
	wb_push_label(p, rs->label, break_depth, continue_depth, true);
	wb_build_stmt(p, rs->body);
	wb_pop_label(p);
	wb_close(p); // continue
	wb_close_scope(p);

	if (!is_string) {
		wb_local_get(p, index);
		wb_i32_const(p, 1);
		wb_op(p, rs->reverse ? wbOp_i32_sub : wbOp_i32_add);
		wb_local_set(p, index);
	} else if (rs->reverse) {
		// index already moved to the start of the decoded rune; the loop
		// continues while it is > 0
		wb_local_get(p, index);
		wb_op(p, wbOp_i32_eqz);
		wb_br_if(p, break_depth);
	}
	wb_br(p, loop_depth);
	wb_close(p); // loop
	wb_close(p); // break
}

// Range over a call returning (values..., ok: bool): the call is repeated until
// the last result is false
gb_internal void wb_build_range_tuple(wbProcedure *p, AstRangeStmt *rs, Ast *node, Type *tuple_type) {
	GB_ASSERT(tuple_type->kind == Type_Tuple);
	isize tuple_count = tuple_type->Tuple.variables.count;
	GB_ASSERT(tuple_count >= 1);
	GB_ASSERT(rs->vals.count <= tuple_count);

	u32 break_depth = wb_open_block(p);
	u32 loop_depth = wb_open_loop(p);

	wbValue tuple = wb_build_expr(p, unparen_expr(rs->expr));
	if (tuple.kind == wbValue_Invalid) {
		wb_close(p);
		wb_close(p);
		return;
	}
	wbValue cond = wb_tuple_field(p, tuple, tuple_count-1);
	wb_push(p, cond);
	wb_op(p, wbOp_i32_eqz);
	wb_br_if(p, break_depth);

	wb_open_scope(p);
	for_array(i, rs->vals) {
		Ast *val = wb_strip_and_prefix(rs->vals[i]);
		if (val != nullptr) {
			wb_store_range_val(p, val, wb_tuple_field(p, tuple, i));
		}
	}

	u32 continue_depth = wb_open_block(p);
	wb_push_label(p, rs->label, break_depth, continue_depth, true);
	wb_build_stmt(p, rs->body);
	wb_pop_label(p);
	wb_close(p); // continue
	wb_close_scope(p);

	wb_br(p, loop_depth);
	wb_close(p); // loop
	wb_close(p); // break
}

// Range over the elements of a bit_set, in increasing (or with #reverse
// decreasing) order: repeatedly extract and clear the lowest (highest) set bit
gb_internal void wb_build_range_bit_set(wbProcedure *p, AstRangeStmt *rs, Ast *node, Ast *val0) {
	Ast *range_expr = unparen_expr(rs->expr);
	Type *expr_type = type_of_expr(range_expr);
	wbValue set = wb_build_expr(p, range_expr);
	if (set.kind == wbValue_Invalid) {
		return;
	}
	if (is_type_pointer(set.type)) {
		set = wb_addr_load(p, wb_addr_from_pointer(p, set, type_deref(set.type)));
		expr_type = type_deref(expr_type);
	}
	Type *bt = base_type(expr_type);
	GB_ASSERT(bt->kind == Type_BitSet);
	Type *elem = bt->BitSet.elem;
	Type *mask_type = bit_set_to_int(bt);
	i64 lower = bt->BitSet.lower;
	bool wide = wb_is_int128(mask_type);
	wbValType vt = wide ? wbValType_i64 : wb_valtype_of(mask_type);
	i64 bits = 8*type_size_of(mask_type);

	// remaining = set & all_set_mask
	set.type = mask_type;
	wbValue masked = wb_emit_arith(p, node, Token_And, set, wb_const(p, node, mask_type, exact_bit_set_all_set_mask(bt)), mask_type, mask_type);
	if (masked.kind == wbValue_Invalid) {
		return;
	}
	u32 lo = wb_add_local(p, vt);
	u32 hi = 0;
	if (wide) {
		wbAddr a = wb_value_to_addr(p, masked);
		hi = wb_add_local(p, vt);
		wb_push(p, wb_emit_load(p, a.index, a.offset,     t_u64)); wb_local_set(p, lo);
		wb_push(p, wb_emit_load(p, a.index, a.offset + 8, t_u64)); wb_local_set(p, hi);
	} else {
		wb_push(p, masked);
		wb_local_set(p, lo);
	}

	u32 break_depth = wb_open_block(p);
	u32 loop_depth = wb_open_loop(p);

	// exit when nothing remains
	wb_local_get(p, lo);
	if (wide) {
		wb_local_get(p, hi);
		wb_op(p, wbOp_i64_or);
	}
	wb_op(p, vt == wbValType_i64 ? wbOp_i64_eqz : wbOp_i32_eqz);
	wb_br_if(p, break_depth);

	// index of the bit to visit, and its removal from the remaining set
	u32 index = wb_add_local(p, vt);
	if (wide) {
		if (rs->reverse) {
			// hi != 0 ? 127 - clz(hi) : 63 - clz(lo)
			wb_i64_const(p, 127); wb_local_get(p, hi); wb_op(p, wbOp_i64_clz); wb_op(p, wbOp_i64_sub);
			wb_i64_const(p, 63);  wb_local_get(p, lo); wb_op(p, wbOp_i64_clz); wb_op(p, wbOp_i64_sub);
			wb_local_get(p, hi); wb_i64_const(p, 0); wb_op(p, wbOp_i64_ne);
			wb_op(p, wbOp_select);
			wb_local_set(p, index);
			// clear bit `index` in whichever word holds it
			wb_local_get(p, hi);
			wb_i64_const(p, 1); wb_local_get(p, index); wb_i64_const(p, 64); wb_op(p, wbOp_i64_sub); wb_op(p, wbOp_i64_shl);
			wb_i64_const(p, 0);
			wb_local_get(p, hi); wb_i64_const(p, 0); wb_op(p, wbOp_i64_ne);
			wb_op(p, wbOp_select);
			wb_op(p, wbOp_i64_xor);
			wb_local_get(p, lo);
			wb_i64_const(p, 0);
			wb_i64_const(p, 1); wb_local_get(p, index); wb_op(p, wbOp_i64_shl);
			wb_local_get(p, hi); wb_i64_const(p, 0); wb_op(p, wbOp_i64_ne);
			wb_op(p, wbOp_select);
			wb_op(p, wbOp_i64_xor);
			wb_local_set(p, lo);
			wb_local_set(p, hi);
		} else {
			// lo != 0 ? ctz(lo) : 64 + ctz(hi)
			wb_local_get(p, lo); wb_op(p, wbOp_i64_ctz);
			wb_local_get(p, hi); wb_op(p, wbOp_i64_ctz); wb_i64_const(p, 64); wb_op(p, wbOp_i64_add);
			wb_local_get(p, lo); wb_i64_const(p, 0); wb_op(p, wbOp_i64_ne);
			wb_op(p, wbOp_select);
			wb_local_set(p, index);
			// hi' = lo != 0 ? hi : hi & (hi-1); lo' = lo & (lo-1)
			wb_local_get(p, hi);
			wb_local_get(p, hi); wb_local_get(p, hi); wb_i64_const(p, 1); wb_op(p, wbOp_i64_sub); wb_op(p, wbOp_i64_and);
			wb_local_get(p, lo); wb_i64_const(p, 0); wb_op(p, wbOp_i64_ne);
			wb_op(p, wbOp_select);
			wb_local_set(p, hi);
			wb_local_get(p, lo); wb_local_get(p, lo); wb_i64_const(p, 1); wb_op(p, wbOp_i64_sub); wb_op(p, wbOp_i64_and);
			wb_local_set(p, lo);
		}
	} else if (vt == wbValType_i64) {
		if (rs->reverse) {
			wb_i64_const(p, bits-1); wb_local_get(p, lo); wb_op(p, wbOp_i64_clz); wb_op(p, wbOp_i64_sub);
			wb_local_set(p, index);
			wb_local_get(p, lo); wb_i64_const(p, 1); wb_local_get(p, index); wb_op(p, wbOp_i64_shl); wb_op(p, wbOp_i64_xor);
		} else {
			wb_local_get(p, lo); wb_op(p, wbOp_i64_ctz);
			wb_local_set(p, index);
			wb_local_get(p, lo); wb_local_get(p, lo); wb_i64_const(p, 1); wb_op(p, wbOp_i64_sub); wb_op(p, wbOp_i64_and);
		}
		wb_local_set(p, lo);
	} else {
		if (rs->reverse) {
			wb_i32_const(p, 31); wb_local_get(p, lo); wb_op(p, wbOp_i32_clz); wb_op(p, wbOp_i32_sub);
			wb_local_set(p, index);
			wb_local_get(p, lo); wb_i32_const(p, 1); wb_local_get(p, index); wb_op(p, wbOp_i32_shl); wb_op(p, wbOp_i32_xor);
		} else {
			wb_local_get(p, lo); wb_op(p, wbOp_i32_ctz);
			wb_local_set(p, index);
			wb_local_get(p, lo); wb_local_get(p, lo); wb_i32_const(p, 1); wb_op(p, wbOp_i32_sub); wb_op(p, wbOp_i32_and);
		}
		wb_local_set(p, lo);
	}

	wb_open_scope(p);
	if (val0 != nullptr) {
		Type *it = vt == wbValType_i64 ? t_i64 : t_i32;
		wb_local_get(p, index);
		if (vt == wbValType_i64) { wb_i64_const(p, lower); wb_op(p, wbOp_i64_add); }
		else                     { wb_i32_const(p, cast(i32)lower); wb_op(p, wbOp_i32_add); }
		wbValue v = wb_pop_to_local(p, vt, it);
		wb_store_range_val(p, val0, wb_emit_conv(p, v, elem));
	}

	u32 continue_depth = wb_open_block(p);
	wb_push_label(p, rs->label, break_depth, continue_depth, true);
	wb_build_stmt(p, rs->body);
	wb_pop_label(p);
	wb_close(p); // continue
	wb_close_scope(p);

	wb_br(p, loop_depth);
	wb_close(p); // loop
	wb_close(p); // break
}

gb_internal void wb_build_range_stmt(wbProcedure *p, AstRangeStmt *rs, Ast *node) {
	wb_open_scope(p);
	if (rs->init != nullptr) {
		wb_build_stmt(p, rs->init);
	}
	Ast *val0 = rs->vals.count > 0 ? wb_strip_and_prefix(rs->vals[0]) : nullptr;
	Ast *val1 = rs->vals.count > 1 ? wb_strip_and_prefix(rs->vals[1]) : nullptr;

	Ast *expr = unparen_expr(rs->expr);
	TypeAndValue tv = type_and_value_of_expr(expr);
	if (is_ast_range(expr)) {
		wb_build_range_interval(p, rs, node, val0, val1);
	} else if (tv.mode == Addressing_Type) {
		wb_unsupported(p, node, "range over a type");
	} else if (tv.type != nullptr && base_type(tv.type)->kind == Type_Tuple) {
		wb_build_range_tuple(p, rs, node, base_type(tv.type));
	} else {
		Type *bt = base_type(tv.type);
		if (bt->kind == Type_Pointer) {
			bt = base_type(type_deref(bt));
		}
		switch (bt->kind) {
		case Type_Array:
		case Type_Slice:
		case Type_DynamicArray:
			wb_build_range_indexed(p, rs, node, val0, val1);
			break;
		case Type_BitSet:
			wb_build_range_bit_set(p, rs, node, val0);
			break;
		case Type_Map:
			wb_build_range_map(p, rs, node, val0, val1);
			break;
		case Type_Basic:
			if (is_type_string(bt) && !is_type_cstring(bt)) {
				wb_build_range_indexed(p, rs, node, val0, val1);
			} else if (is_type_integer(bt)) {
				wb_unsupported(p, node, "range over an integer");
			} else {
				wb_unsupported_type(p, node, tv.type);
			}
			break;
		default:
			wb_unsupported_type(p, node, tv.type);
			break;
		}
	}
	wb_close_scope(p);
}

gb_internal void wb_build_switch_stmt(wbProcedure *p, AstSwitchStmt *ss, Ast *node) {
	// block $exit
	//   block $body_{n-1} ... block $body_0
	//     dispatch: br_if $body_i ...; br $default_or_exit
	//   end stmts_0; br $exit
	//   ...
	//   end stmts_{n-1}
	// end
	wb_open_scope(p);
	if (ss->init != nullptr) {
		wb_build_stmt(p, ss->init);
	}

	wbValue tag = {};
	Type *tag_type = nullptr;
	if (ss->tag != nullptr) {
		tag_type = type_of_expr(ss->tag);
		if (is_type_untyped(tag_type)) {
			tag_type = default_type(tag_type);
		}
		tag = wb_emit_conv(p, wb_build_expr(p, ss->tag), tag_type);
		if (wb_is_scalar(tag_type)) {
			tag = wb_value_fresh(p, tag);
		} else if (tag.kind != wbValue_Invalid) {
			// aggregate (string, struct, ...): keep a private copy in a temp
			tag = wb_value_copy(p, tag);
		}
		if (tag.kind == wbValue_Invalid) {
			wb_close_scope(p);
			return;
		}
	}

	ast_node(body, BlockStmt, ss->body);
	Slice<Ast *> const &clauses = body->stmts;
	isize n = clauses.count;
	isize default_index = -1;
	for_array(i, clauses) {
		ast_node(cc, CaseClause, clauses[i]);
		if (cc->list.count == 0) {
			default_index = i;
		}
	}

	u32 exit_depth = wb_open_block(p);
	auto body_depths = array_make<u32>(temporary_allocator(), n);
	for (isize i = n-1; i >= 0; i--) {
		body_depths[i] = wb_open_block(p);
	}

	// Dispatch
	for_array(i, clauses) {
		ast_node(cc, CaseClause, clauses[i]);
		for (Ast *expr : cc->list) {
			expr = unparen_expr(expr);
			wbValue cond = {};
			if (tag_type == nullptr) {
				cond = wb_emit_conv(p, wb_build_expr(p, expr), t_bool);
			} else if (is_ast_range(expr)) {
				ast_node(be, BinaryExpr, expr);
				TokenKind hi_op = be->op.kind == Token_RangeHalf ? Token_Lt : Token_LtEq;
				wbValue lo = wb_emit_conv(p, wb_build_expr(p, be->left),  tag_type);
				wbValue hi = wb_emit_conv(p, wb_build_expr(p, be->right), tag_type);
				wbValue c0 = wb_emit_compare(p, expr, Token_GtEq, tag, lo, tag_type, t_bool);
				wbValue c1 = wb_emit_compare(p, expr, hi_op,      tag, hi, tag_type, t_bool);
				if (c0.kind == wbValue_Invalid || c1.kind == wbValue_Invalid) {
					continue;
				}
				wb_push(p, c0);
				wb_push(p, c1);
				wb_op(p, wbOp_i32_and);
				cond = wb_pop_to_local(p, wbValType_i32, t_bool);
			} else {
				wbValue v = wb_emit_conv(p, wb_build_expr(p, expr), tag_type);
				cond = wb_emit_compare(p, expr, Token_CmpEq, tag, v, tag_type, t_bool);
			}
			if (cond.kind == wbValue_Invalid) {
				continue;
			}
			wb_push(p, cond);
			wb_br_if(p, body_depths[i]);
		}
	}
	wb_br(p, default_index >= 0 ? body_depths[default_index] : exit_depth);

	// Clause bodies
	for_array(i, clauses) {
		ast_node(cc, CaseClause, clauses[i]);
		wb_close(p); // body_i
		u32 fall_depth = i+1 < n ? body_depths[i+1] : exit_depth;
		wb_push_label(p, ss->label, exit_depth, 0, false, true, fall_depth);
		wb_open_scope(p);
		wb_build_stmt_list(p, cc->stmts);
		wb_close_scope(p);
		wb_pop_label(p);
		wb_br(p, exit_depth);
	}
	wb_close(p); // exit
	wb_close_scope(p);
}

// Binds the implicit variable of a type switch clause to the value at `data`
// (by value: a copy is made so that later assignments to the parent do not
// change it; by reference: the parent's storage is aliased)
gb_internal void wb_bind_type_case(wbProcedure *p, Ast *clause, wbAddr data) {
	Entity *e = implicit_entity_of_node(clause);
	GB_ASSERT(e != nullptr);
	if (data.kind != wbAddr_Memory) {
		wb_unsupported(p, clause, "type switch on a value held in a register (internal error)");
		return;
	}
	data.type = e->type;
	if (e->flags & EntityFlag_Value) {
		wbAddr var = wb_add_variable(p, e);
		wb_addr_store(p, var, wb_addr_load(p, data));
	} else {
		map_set(&p->variables, e, data);
	}
}

// switch v in u { case T: ... } (lb_build_type_switch_stmt)
gb_internal void wb_build_type_switch_stmt(wbProcedure *p, AstTypeSwitchStmt *ss, Ast *node) {
	wb_open_scope(p);

	ast_node(as, AssignStmt, ss->tag);
	GB_ASSERT(as->lhs.count == 1);
	GB_ASSERT(as->rhs.count == 1);

	wbValue parent = wb_build_expr(p, as->rhs[0]);
	if (parent.kind == wbValue_Invalid) {
		wb_close_scope(p);
		return;
	}
	Type *parent_base_type = type_deref(parent.type);
	TypeSwitchKind switch_kind = check_valid_type_switch_type(parent.type);
	GB_ASSERT(switch_kind != TypeSwitch_Invalid);

	// The parent's storage: the union/any itself, or what the pointer points to
	wbAddr parent_addr = {};
	if (is_type_pointer(parent.type)) {
		parent_addr = wb_addr_from_pointer(p, wb_value_fresh(p, parent), parent_base_type);
	} else {
		parent_addr = wb_value_to_addr(p, parent);
	}
	if (parent_addr.kind != wbAddr_Memory) {
		wb_unsupported(p, node, "type switch operand");
		wb_close_scope(p);
		return;
	}
	wbValue parent_value = wb_value_memory(parent_addr.index, parent_addr.offset, parent_base_type);

	wbValue tag = {};
	Type *tag_type = nullptr;
	if (switch_kind == TypeSwitch_Union) {
		tag = wb_value_fresh(p, wb_emit_union_tag(p, node, parent_value));
		tag_type = tag.type;
	} else {
		tag = wb_addr_load(p, wb_addr_field(parent_addr, 1));
		tag_type = t_typeid;
	}

	ast_node(body, BlockStmt, ss->body);
	Slice<Ast *> const &clauses = body->stmts;
	isize n = clauses.count;
	isize default_index = -1;
	for_array(i, clauses) {
		ast_node(cc, CaseClause, clauses[i]);
		if (cc->list.count == 0) {
			default_index = i;
		}
	}

	u32 exit_depth = wb_open_block(p);
	auto body_depths = array_make<u32>(temporary_allocator(), n);
	for (isize i = n-1; i >= 0; i--) {
		body_depths[i] = wb_open_block(p);
	}

	// Dispatch
	for_array(i, clauses) {
		ast_node(cc, CaseClause, clauses[i]);
		for (Ast *type_expr : cc->list) {
			Type *case_type = type_of_expr(type_expr);
			wbValue on_val = {};
			if (switch_kind == TypeSwitch_Union) {
				on_val = wb_const_union_tag(parent_base_type, case_type);
			} else if (is_type_untyped_nil(case_type)) {
				on_val = wb_value_const_int(t_typeid, 0);
			} else {
				on_val = wb_typeid(case_type);
			}
			wbValue cond = wb_emit_arith(p, type_expr, Token_CmpEq, tag, wb_emit_conv(p, on_val, tag_type), tag_type, t_bool);
			if (cond.kind == wbValue_Invalid) {
				continue;
			}
			wb_push(p, cond);
			wb_br_if(p, body_depths[i]);
		}
	}
	wb_br(p, default_index >= 0 ? body_depths[default_index] : exit_depth);

	// Clause bodies
	for_array(i, clauses) {
		ast_node(cc, CaseClause, clauses[i]);
		wb_close(p); // body_i
		wb_push_label(p, ss->label, exit_depth, 0, false, true, exit_depth);
		wb_open_scope(p);

		bool saw_nil = false;
		for (Ast *type_expr : cc->list) {
			if (is_type_untyped_nil(type_of_expr(type_expr))) {
				saw_nil = true;
			}
		}
		if (cc->list.count == 1 && !saw_nil) {
			// the variant's data
			wbAddr data = {};
			if (switch_kind == TypeSwitch_Union) {
				data = parent_addr;
			} else {
				wbValue any_data = wb_addr_load(p, wb_addr_field(parent_addr, 0));
				data = wb_addr_from_pointer(p, any_data, type_of_expr(cc->list[0]));
			}
			wb_bind_type_case(p, clauses[i], data);
		} else {
			// the whole parent
			wb_bind_type_case(p, clauses[i], parent_addr);
		}

		wb_build_stmt_list(p, cc->stmts);
		wb_close_scope(p);
		wb_pop_label(p);
		wb_br(p, exit_depth);
	}
	wb_close(p); // exit
	wb_close_scope(p);
}

gb_internal void wb_build_stmt(wbProcedure *p, Ast *node) {
	p->curr_stmt = node;
	switch (node->kind) {
	case Ast_EmptyStmt:
		break;

	case_ast_node(bs, BlockStmt, node);
		wb_build_block_stmt(p, bs, node);
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

	case_ast_node(rs, RangeStmt, node);
		wb_build_range_stmt(p, rs, node);
	case_end;

	case_ast_node(ss, SwitchStmt, node);
		wb_build_switch_stmt(p, ss, node);
	case_end;

	case_ast_node(ss, TypeSwitchStmt, node);
		wb_build_type_switch_stmt(p, ss, node);
	case_end;

	case_ast_node(bs, BranchStmt, node);
		wb_build_branch_stmt(p, bs, node);
	case_end;

	case_ast_node(fb, ForeignBlockDecl, node);
		// foreign procedures are generated when referenced
	case_end;

	case_ast_node(ds, DeferStmt, node);
		wbDefer d = {};
		d.stmt = ds->stmt;
		d.scope_index = p->scopes.count;
		d.context_stack_count = p->context_stack.count;
		array_add(&p->defers, d);
	case_end;

	default:
		wb_unsupported(p, node, "statement");
		break;
	}
}

// Finds the variables whose address is taken so that they can be given
// memory instead of a register.

gb_internal void wb_mark_addressed_root(wbProcedure *p, Ast *expr) {
	for (;;) {
		if (expr == nullptr) {
			return;
		}
		expr = unparen_expr(expr);
		switch (expr->kind) {
		case Ast_Ident: {
			Entity *e = entity_of_node(expr);
			if (e != nullptr && e->kind == Entity_Variable) {
				ptr_set_add(&p->addressed, e);
			}
			return;
		}
		case Ast_SelectorExpr: {
			Ast *base = expr->SelectorExpr.expr;
			Type *t = type_of_expr(base);
			if (t == nullptr || is_type_pointer(t)) {
				return;
			}
			expr = base;
			break;
		}
		case Ast_IndexExpr: {
			Ast *base = expr->IndexExpr.expr;
			Type *t = type_of_expr(base);
			if (t == nullptr || !is_type_array(base_type(t))) {
				return;
			}
			expr = base;
			break;
		}
		default:
			return;
		}
	}
}

gb_internal void wb_prescan_addressed(wbProcedure *p, Ast *node) {
	if (node == nullptr) {
		return;
	}
	switch (node->kind) {
	case Ast_ParenExpr:    wb_prescan_addressed(p, node->ParenExpr.expr); break;
	case Ast_UnaryExpr:
		if (node->UnaryExpr.op.kind == Token_And) {
			wb_mark_addressed_root(p, node->UnaryExpr.expr);
		}
		wb_prescan_addressed(p, node->UnaryExpr.expr);
		break;
	case Ast_BinaryExpr:
		wb_prescan_addressed(p, node->BinaryExpr.left);
		wb_prescan_addressed(p, node->BinaryExpr.right);
		break;
	case Ast_SelectorExpr: wb_prescan_addressed(p, node->SelectorExpr.expr); break;
	case Ast_IndexExpr:
		wb_prescan_addressed(p, node->IndexExpr.expr);
		wb_prescan_addressed(p, node->IndexExpr.index);
		break;
	case Ast_SliceExpr:
		// slicing an array takes its address, but arrays always live in memory
		wb_prescan_addressed(p, node->SliceExpr.expr);
		wb_prescan_addressed(p, node->SliceExpr.low);
		wb_prescan_addressed(p, node->SliceExpr.high);
		break;
	case Ast_DerefExpr:    wb_prescan_addressed(p, node->DerefExpr.expr); break;
	case Ast_CallExpr:
		wb_prescan_addressed(p, node->CallExpr.proc);
		for (Ast *arg : node->CallExpr.args) wb_prescan_addressed(p, arg);
		break;
	case Ast_FieldValue:   wb_prescan_addressed(p, node->FieldValue.value); break;
	case Ast_CompoundLit:
		for (Ast *elem : node->CompoundLit.elems) wb_prescan_addressed(p, elem);
		break;
	case Ast_TypeCast:     wb_prescan_addressed(p, node->TypeCast.expr); break;
	case Ast_AutoCast:     wb_prescan_addressed(p, node->AutoCast.expr); break;
	case Ast_TernaryIfExpr:
		wb_prescan_addressed(p, node->TernaryIfExpr.cond);
		wb_prescan_addressed(p, node->TernaryIfExpr.x);
		wb_prescan_addressed(p, node->TernaryIfExpr.y);
		break;
	case Ast_TernaryWhenExpr:
		wb_prescan_addressed(p, node->TernaryWhenExpr.x);
		wb_prescan_addressed(p, node->TernaryWhenExpr.y);
		break;
	case Ast_OrElseExpr:
		wb_prescan_addressed(p, node->OrElseExpr.x);
		wb_prescan_addressed(p, node->OrElseExpr.y);
		break;
	case Ast_OrReturnExpr: wb_prescan_addressed(p, node->OrReturnExpr.expr); break;
	case Ast_TypeAssertion: wb_prescan_addressed(p, node->TypeAssertion.expr); break;

	case Ast_ExprStmt:     wb_prescan_addressed(p, node->ExprStmt.expr); break;
	case Ast_AssignStmt:
		for (Ast *e : node->AssignStmt.lhs) wb_prescan_addressed(p, e);
		for (Ast *e : node->AssignStmt.rhs) wb_prescan_addressed(p, e);
		break;
	case Ast_BlockStmt:
		for (Ast *s : node->BlockStmt.stmts) wb_prescan_addressed(p, s);
		break;
	case Ast_IfStmt:
		wb_prescan_addressed(p, node->IfStmt.init);
		wb_prescan_addressed(p, node->IfStmt.cond);
		wb_prescan_addressed(p, node->IfStmt.body);
		wb_prescan_addressed(p, node->IfStmt.else_stmt);
		break;
	case Ast_WhenStmt:
		wb_prescan_addressed(p, node->WhenStmt.body);
		wb_prescan_addressed(p, node->WhenStmt.else_stmt);
		break;
	case Ast_ReturnStmt:
		for (Ast *e : node->ReturnStmt.results) wb_prescan_addressed(p, e);
		break;
	case Ast_ForStmt:
		wb_prescan_addressed(p, node->ForStmt.init);
		wb_prescan_addressed(p, node->ForStmt.cond);
		wb_prescan_addressed(p, node->ForStmt.post);
		wb_prescan_addressed(p, node->ForStmt.body);
		break;
	case Ast_RangeStmt:
		wb_prescan_addressed(p, node->RangeStmt.init);
		wb_prescan_addressed(p, node->RangeStmt.expr);
		wb_prescan_addressed(p, node->RangeStmt.body);
		break;
	case Ast_SwitchStmt:
		wb_prescan_addressed(p, node->SwitchStmt.init);
		wb_prescan_addressed(p, node->SwitchStmt.tag);
		wb_prescan_addressed(p, node->SwitchStmt.body);
		break;
	case Ast_TypeSwitchStmt:
		wb_prescan_addressed(p, node->TypeSwitchStmt.tag);
		wb_prescan_addressed(p, node->TypeSwitchStmt.body);
		break;
	case Ast_CaseClause:
		for (Ast *e : node->CaseClause.list) wb_prescan_addressed(p, e);
		for (Ast *s : node->CaseClause.stmts) wb_prescan_addressed(p, s);
		break;
	case Ast_DeferStmt:    wb_prescan_addressed(p, node->DeferStmt.stmt); break;
	case Ast_ValueDecl:
		for (Ast *e : node->ValueDecl.values) wb_prescan_addressed(p, e);
		break;
	default:
		// Procedure literals are separate procedures; other node kinds cannot
		// take addresses (or are unsupported and reported during lowering)
		break;
	}
}
