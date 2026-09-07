// Optimizer for the instruction stream of a procedure body (see `wbInstr`).
//
// The lowering pops every intermediate result into a fresh local and keeps
// every aggregate in the stack frame, which baseline wasm compilers turn into
// a load and a store of a stack slot for each. Working on the recorded
// instructions with a simulation of the operand stack, this pass:
//
//  - removes the code after unconditional branches,
//  - promotes frame slots that are only accessed by fixed offset loads and
//    stores (and whose address never escapes) to locals,
//  - keeps a value with a single use on the operand stack by moving the
//    instructions computing it to the use, instead of storing it in a local,
//  - deletes the stores to locals that are never read,
//  - drops unused locals and renumbers the rest,
//
// and then regenerates the byte code. Once every procedure is done, small
// callees are inlined (wb_inline_procedures) and the callers optimized again.

gb_internal u32  wb_add_local(wbProcedure *p, wbValType vt, String name);
gb_internal void wb_emit_epilogue(wbProcedure *p);
gb_internal void wb_finish_procedure(wbProcedure *p);
gb_internal wbProcedure *wb_alloc_procedure(wbModule *m, String name);
gb_internal u8 const *wb_const_data_bytes(wbModule *m, u32 addr, u32 size);

#define WB_OPT_MAX_PRODUCER 64
#define WB_OPT_MAX_PASSES   16

struct wbOptValue {
	i32  producer;  // index of the first instruction computing the value, -1 if opaque
	i32  pushed_at; // index of the instruction that pushed it
	i32  offset;    // fp relative offset when is_frame, constant when is_const
	bool is_frame;
	bool is_const;
};

struct wbOptBlock {
	i32  height;    // operand stack height at entry
	u8   arity;
	bool is_loop;
	i32  parent;    // index into blocks
	i32  end;       // index of the `end` (or `else`) instruction, -1 while open
	i32  start;     // index of the `block`/`loop`/`if`/`else` instruction
	i32  then_of;   // `else` blocks: the block of the `if` branch
};

struct wbOptAccess {
	u32 offset;     // within the slot
	u8  width;
	u8  op;         // load/store opcode
};

struct wbOptSlot {
	bool escaped;
	Array<wbOptAccess> accesses;
	Array<u32> leaf_offsets; // promoted leaves, increasing
	Array<u32> leaf_locals;
	Array<u8>  leaf_ext;     // 1: sign extending loads, 2: zero extending loads, 0: none
	Array<u8>  leaf_width;
};

struct wbOpt {
	wbProcedure *p;
	Array<wbInstr> in;    // the instructions being rewritten
	isize        n;
	wbBuffer     scratch; // bytes of synthesized instructions

	// Simulation results
	Array<i32>   producer;      // per instruction: producer start of the value it pushes (pushes == 1), else -1
	Array<i32>   set_producer;  // local.set: producer start of the stored value, -1 if opaque
	Array<bool>  addr_get;      // local.get: the address operand of a load or store
	Array<i32>   op0_start;     // per instruction: the range of instructions computing its first operand
	Array<i32>   op0_end;       // (-1 when not contiguous)
	Array<i32>   block_of;      // innermost block containing the instruction
	Array<i32>   height;        // operand stack height before the instruction
	Array<wbOptBlock> blocks;
	Array<bool>  deleted;
	Array<bool>  moved;         // part of a producer range emitted at its use instead
	Array<i32>   impure_prefix; // counts over [0, k)
	Array<i32>   memw_prefix;
	Array<i32>   globw_prefix;
	Array<i64>   acc_abs;       // load/store of a frame address: absolute frame offset, else -1
	Array<i32>   acc_astart;    // and the range of instructions computing the address
	Array<i32>   acc_aend;

	// Per local
	Array<i32>   gets, sets, tees;
	Array<i32>   single_set, single_get;
	Array<Array<i32>> writes;
	Array<Array<i32>> reads;       // per local: the local.get instructions // instruction indices of set/tee, increasing
	Array<i32>   moved_to;    // local: index of its get when its producer moves there, else -1
	Array<i32>   moved_start; // and the producer range [moved_start, single_set)

	// Edits
	Array<Array<wbInstr>> replacement; // per instruction: emitted instead (when non-empty)
	Array<bool>  replaced;
	Array<wbOptSlot> slots;
	bool         all_escaped;
	bool         changed;
	u32          fp_refs; // remaining local.get fp after the rewrite

	// Facts assumed by wb_opt_local_const_before on entry to the loop at
	// `assume_at` (the locals its exit test reads), for deciding a peel
	i32          assume_at;
	i32          assume_count;
	u32          assume_local[8];
	u32          assume_value[8];
	// and its memo, per instruction: 0 unknown, 1 constant, 2 not constant
	Array<u8>    memo_state;
	Array<u32>   memo_local;
	Array<u32>   memo_value;
	Array<i32>   memo_touched;
	// per instruction: the local a load's value was kept in for later reads
	// of the same address (load CSE), -1 if none
	Array<i32>   cse_local;
};

gb_internal i32 wb_opt_br_target(wbOpt *o, i32 k);
gb_internal bool wb_opt_range_untouched(wbOpt *o, i32 a, i32 b);

gb_internal isize wb_opt_find_slot(wbOpt *o, i64 offset) {
	// slots are allocated in increasing order
	isize lo = 0, hi = o->p->slots.count;
	while (lo < hi) {
		isize mid = (lo + hi) / 2;
		wbFrameSlot const &s = o->p->slots[mid];
		if (offset < s.offset) {
			hi = mid;
		} else if (offset >= cast(i64)s.offset + s.size) {
			lo = mid + 1;
		} else {
			return mid;
		}
	}
	return -1;
}

gb_internal void wb_opt_escape(wbOpt *o, i64 offset) {
	isize i = wb_opt_find_slot(o, offset);
	if (i < 0) {
		o->all_escaped = true;
	} else {
		o->slots[i].escaped = true;
	}
}

gb_internal bool wb_opt_is_pure(wbInstr const &in) {
	switch (in.kind) {
	case wbInstr_Other:
	case wbInstr_GlobalGet:
	case wbInstr_Load:
	case wbInstr_Const:
		return true;
	case wbInstr_Local:
		return in.op == wbOp_local_get;
	default:
		return false;
	}
}

gb_internal bool wb_opt_writes_memory(wbInstr const &in) {
	switch (in.kind) {
	case wbInstr_Store:
	case wbInstr_Call:
	case wbInstr_CallIndirect:
	case wbInstr_MemCopy:
	case wbInstr_MemFill:
	case wbInstr_MemGrow:
		return true;
	default:
		return false;
	}
}

gb_internal bool wb_opt_writes_globals(wbInstr const &in) {
	switch (in.kind) {
	case wbInstr_GlobalSet:
	case wbInstr_EpilogueSet:
	case wbInstr_Call:
	case wbInstr_CallIndirect:
		return true;
	default:
		return false;
	}
}

gb_internal bool wb_opt_reads_memory(wbInstr const &in) {
	return in.kind == wbInstr_Load || (in.kind == wbInstr_Other && in.op == wbOp_memory_size);
}

// Simulates the operand stack, recording the producers of values, the frame
// slot accesses and escapes, and the block structure. Unreachable code is
// marked deleted.
gb_internal void wb_opt_simulate(wbOpt *o) {
	wbProcedure *p = o->p;
	gbAllocator ta = temporary_allocator();
	isize n = o->n;

	o->producer      = array_make<i32>(ta, n);
	o->set_producer  = array_make<i32>(ta, n);
	o->block_of      = array_make<i32>(ta, n);
	o->height        = array_make<i32>(ta, n);
	o->deleted       = array_make<bool>(ta, n);
	o->moved         = array_make<bool>(ta, n);
	o->acc_abs       = array_make<i64>(ta, n);
	o->acc_astart    = array_make<i32>(ta, n);
	o->addr_get      = array_make<bool>(ta, n);
	o->op0_start     = array_make<i32>(ta, n);
	o->op0_end       = array_make<i32>(ta, n);
	o->acc_aend      = array_make<i32>(ta, n);
	o->impure_prefix = array_make<i32>(ta, n+1);
	o->memw_prefix   = array_make<i32>(ta, n+1);
	o->globw_prefix  = array_make<i32>(ta, n+1);
	for (isize k = 0; k < n; k++) {
		o->producer[k] = -1;
		o->addr_get[k] = false;
		o->op0_start[k] = -1;
		o->op0_end[k] = -1;
		o->set_producer[k] = -1;
		o->deleted[k] = false;
		o->moved[k] = false;
		o->acc_abs[k] = -1;
	}
	isize local_count = p->locals.count;
	o->gets = array_make<i32>(ta, local_count);
	o->sets = array_make<i32>(ta, local_count);
	o->tees = array_make<i32>(ta, local_count);
	o->single_set  = array_make<i32>(ta, local_count);
	o->single_get  = array_make<i32>(ta, local_count);
	o->writes      = array_make<Array<i32>>(ta, local_count);
	o->reads       = array_make<Array<i32>>(ta, local_count);
	o->moved_to    = array_make<i32>(ta, local_count);
	o->moved_start = array_make<i32>(ta, local_count);
	for (isize i = 0; i < local_count; i++) {
		o->gets[i] = o->sets[i] = o->tees[i] = 0;
		o->single_set[i] = o->single_get[i] = -1;
		o->moved_to[i] = -1;
		o->writes[i] = {};
		o->writes[i].allocator = ta;
		o->reads[i] = {};
		o->reads[i].allocator = ta;
	}

	o->blocks = array_make<wbOptBlock>(ta, 0, 64);
	wbOptBlock root = {0, 0, false, -1, -1};
	array_add(&o->blocks, root);
	i32 block = 0;

	auto stack = array_make<wbOptValue>(ta, 0, 64);
	bool unreachable = false;
	i32  dead_depth = 0;

	i32 impure = 0, memw = 0, globw = 0;

	for (isize k = 0; k < n; k++) {
		wbInstr const &in = o->in[k];
		o->impure_prefix[k] = impure;
		o->memw_prefix[k]   = memw;
		o->globw_prefix[k]  = globw;
		o->block_of[k] = block;
		o->height[k]   = cast(i32)stack.count;

		if (unreachable) {
			switch (in.kind) {
			case wbInstr_Block: case wbInstr_Loop: case wbInstr_If:
				dead_depth++;
				o->deleted[k] = true;
				continue;
			case wbInstr_End:
				if (dead_depth > 0) {
					dead_depth--;
					o->deleted[k] = true;
					continue;
				}
				break;
			case wbInstr_Else:
				if (dead_depth > 0) {
					o->deleted[k] = true;
					continue;
				}
				break;
			default:
				o->deleted[k] = true;
				continue;
			}
			unreachable = false;
			// the end/else of the block is reachable
		}

		if (!wb_opt_is_pure(in))       impure++;
		// a call that never returns (followed by `unreachable`) can't change
		// what the code after it sees
		bool diverging_call = (in.kind == wbInstr_Call || in.kind == wbInstr_CallIndirect) && k+1 < n && o->in[k+1].kind == wbInstr_Unreachable;
		if (wb_opt_writes_memory(in) && !diverging_call) memw++;
		if (wb_opt_writes_globals(in)) globw++;

		// Structured control flow
		switch (in.kind) {
		case wbInstr_Block: case wbInstr_Loop: case wbInstr_If: {
			if (in.kind == wbInstr_If && stack.count > o->blocks[block].height) {
				wbOptValue v = array_pop(&stack);
				if (v.is_frame) wb_opt_escape(o, v.offset);
			}
			wbOptBlock b = {cast(i32)stack.count, cast(u8)(in.imm == 0x40 ? 0 : 1), in.kind == wbInstr_Loop, block, -1, cast(i32)k, -1};
			array_add(&o->blocks, b);
			block = cast(i32)(o->blocks.count - 1);
			o->block_of[k] = block;
			continue;
		}
		case wbInstr_Else: {
			wbOptBlock b = o->blocks[block];
			for (isize i = b.height; i < stack.count; i++) {
				if (stack[i].is_frame) wb_opt_escape(o, stack[i].offset);
			}
			array_resize(&stack, b.height);
			o->blocks[block].end = cast(i32)k;
			wbOptBlock e = {b.height, b.arity, false, b.parent, -1, cast(i32)k, block};
			array_add(&o->blocks, e);
			block = cast(i32)(o->blocks.count - 1);
			o->block_of[k] = block;
			continue;
		}
		case wbInstr_End: {
			o->blocks[block].end = cast(i32)k;
			wbOptBlock b = o->blocks[block];
			for (isize i = b.height; i < stack.count; i++) {
				if (stack[i].is_frame) wb_opt_escape(o, stack[i].offset);
			}
			array_resize(&stack, b.height);
			for (u8 i = 0; i < b.arity; i++) {
				wbOptValue v = {-1, cast(i32)k, 0, false, false};
				array_add(&stack, v);
			}
			block = b.parent < 0 ? 0 : b.parent;
			continue;
		}
		case wbInstr_Br: case wbInstr_Return: case wbInstr_Unreachable:
			for_array(i, stack) {
				if (stack[i].is_frame) wb_opt_escape(o, stack[i].offset);
			}
			array_resize(&stack, o->blocks[block].height);
			unreachable = true;
			continue;
		default:
			break;
		}

		// Operands
		i32 pops = in.pops;
		i32 avail = cast(i32)(stack.count - o->blocks[block].height);
		wbOptValue ops[4] = {};
		bool opaque = false;
		if (pops > avail || pops > 4) {
			// not modelled: give up on tracking these values
			for (i32 i = 0; i < gb_min(pops, avail); i++) {
				wbOptValue v = array_pop(&stack);
				if (v.is_frame) wb_opt_escape(o, v.offset);
			}
			opaque = true;
			pops = 0;
		} else {
			for (i32 i = pops-1; i >= 0; i--) {
				ops[i] = array_pop(&stack);
			}
		}

		// The producer of a value is the contiguous range of instructions
		// computing it: the operands must have been pushed one after the
		// other, directly before this instruction
		wbOptValue result = {cast(i32)k, cast(i32)k, 0, false, false};
		if (pops > 0) {
			result.producer = ops[0].producer;
			for (i32 i = 0; i < pops; i++) {
				if (ops[i].producer < 0) result.producer = -1;
				i32 next = i+1 < pops ? ops[i+1].producer : cast(i32)k;
				if (ops[i].pushed_at + 1 != next) result.producer = -1;
			}
		}
		if (pops > 0 && ops[0].producer >= 0) {
			o->op0_start[k] = ops[0].producer;
			o->op0_end[k]   = ops[0].pushed_at + 1;
		}
		if (opaque || !wb_opt_is_pure(in)) result.producer = -1;
		if (result.producer >= 0 && o->impure_prefix[k] - o->impure_prefix[result.producer] != 0) {
			result.producer = -1;
		}

		switch (in.kind) {
		case wbInstr_Local:
			if (in.op == wbOp_local_get) {
				o->gets[in.imm]++;
				o->single_get[in.imm] = cast(i32)k;
				array_add(&o->reads[in.imm], cast(i32)k);
				if (in.imm == p->fp_local) {
					result.is_frame = true;
					result.offset = 0;
				}
			} else {
				if (ops[0].is_frame) wb_opt_escape(o, ops[0].offset);
				if (in.op == wbOp_local_set) {
					o->sets[in.imm]++;
					o->single_set[in.imm] = cast(i32)k;
					o->set_producer[k] = ops[0].pushed_at + 1 == cast(i32)k ? ops[0].producer : -1;
				} else {
					o->tees[in.imm]++;
				}
				array_add(&o->writes[in.imm], cast(i32)k);
				result.producer = -1;
			}
			break;
		case wbInstr_Const:
			if (in.op == wbOp_i32_const) {
				result.is_const = true;
				result.offset = cast(i32)in.imm;
			}
			break;
		case wbInstr_Other:
			if (in.op == wbOp_i32_add && ops[0].is_frame && ops[1].is_const) {
				result.is_frame = true;
				result.offset = ops[0].offset + ops[1].offset;
			} else if (in.op == wbOp_i32_add && ops[1].is_frame && ops[0].is_const) {
				result.is_frame = true;
				result.offset = ops[1].offset + ops[0].offset;
			} else if (in.op == wbOp_i32_sub && ops[0].is_frame && ops[1].is_const) {
				result.is_frame = true;
				result.offset = ops[0].offset - ops[1].offset;
			} else {
				for (i32 i = 0; i < pops; i++) {
					if (ops[i].is_frame) wb_opt_escape(o, ops[i].offset);
				}
			}
			break;
		case wbInstr_Load:
		case wbInstr_Store:
			if (ops[0].producer >= 0 && ops[0].producer == ops[0].pushed_at) {
				wbInstr const &a = o->in[ops[0].producer];
				if (a.kind == wbInstr_Local && a.op == wbOp_local_get) o->addr_get[ops[0].producer] = true;
			}
			if (ops[0].is_frame) {
				i64 abs = cast(i64)ops[0].offset + in.imm;
				isize si = wb_opt_find_slot(o, abs);
				if (si < 0) {
					o->all_escaped = true;
				} else if (abs + in.width > cast(i64)p->slots[si].offset + p->slots[si].size) {
					o->slots[si].escaped = true;
				} else {
					wbOptAccess a = {cast(u32)(abs - p->slots[si].offset), in.width, in.op};
					array_add(&o->slots[si].accesses, a);
					o->acc_abs[k]    = abs;
					o->acc_astart[k] = ops[0].producer;
					o->acc_aend[k]   = ops[0].pushed_at + 1;
				}
			}
			if (in.kind == wbInstr_Store && ops[1].is_frame) wb_opt_escape(o, ops[1].offset);
			break;
		default:
			for (i32 i = 0; i < pops; i++) {
				if (ops[i].is_frame) wb_opt_escape(o, ops[i].offset);
			}
			break;
		}

		if (in.pushes == 1) {
			o->producer[k] = result.producer;
			array_add(&stack, result);
		} else {
			for (i32 i = 0; i < in.pushes; i++) {
				wbOptValue v = {-1, cast(i32)k, 0, false, false};
				array_add(&stack, v);
			}
		}
	}
	o->impure_prefix[n] = impure;
	o->memw_prefix[n]   = memw;
	o->globw_prefix[n]  = globw;
	o->blocks[0].end = cast(i32)n;
	for_array(i, stack) {
		if (stack[i].is_frame) wb_opt_escape(o, stack[i].offset);
	}
}

// Classification of the memory access opcodes for frame slot promotion
gb_internal wbValType wb_opt_access_valtype(u8 op) {
	switch (op) {
	case wbOp_i64_load: case wbOp_i64_load8_s: case wbOp_i64_load8_u: case wbOp_i64_load16_s:
	case wbOp_i64_load16_u: case wbOp_i64_load32_s: case wbOp_i64_load32_u:
	case wbOp_i64_store: case wbOp_i64_store8: case wbOp_i64_store16: case wbOp_i64_store32:
		return wbValType_i64;
	case wbOp_f32_load: case wbOp_f32_store: return wbValType_f32;
	case wbOp_f64_load: case wbOp_f64_store: return wbValType_f64;
	default: return wbValType_i32;
	}
}
// 0: full width, 1: sign extending load, 2: zero extending load, 3: narrowing store
gb_internal int wb_opt_access_ext(u8 op) {
	switch (op) {
	case wbOp_i32_load8_s: case wbOp_i32_load16_s:
	case wbOp_i64_load8_s: case wbOp_i64_load16_s: case wbOp_i64_load32_s:
		return 1;
	case wbOp_i32_load8_u: case wbOp_i32_load16_u:
	case wbOp_i64_load8_u: case wbOp_i64_load16_u: case wbOp_i64_load32_u:
		return 2;
	case wbOp_i32_store8: case wbOp_i32_store16:
	case wbOp_i64_store8: case wbOp_i64_store16: case wbOp_i64_store32:
		return 3;
	default:
		return 0;
	}
}

gb_internal int wb_opt_access_compare(void const *a, void const *b) {
	wbOptAccess const *x = cast(wbOptAccess const *)a;
	wbOptAccess const *y = cast(wbOptAccess const *)b;
	if (x->offset != y->offset) return x->offset < y->offset ? -1 : 1;
	return 0;
}

// The address producer of a promoted access is deleted: it must be exactly
// the frame pointer plus constants
gb_internal bool wb_opt_is_frame_address_range(wbOpt *o, i32 start, i32 end) {
	if (start < 0 || end - start > 8) {
		return false;
	}
	for (i32 k = start; k < end; k++) {
		wbInstr const &in = o->in[k];
		if (in.kind == wbInstr_Local && in.op == wbOp_local_get && in.imm == o->p->fp_local) continue;
		if (in.kind == wbInstr_Const && in.op == wbOp_i32_const) continue;
		if (in.kind == wbInstr_Other && (in.op == wbOp_i32_add || in.op == wbOp_i32_sub)) continue;
		return false;
	}
	return true;
}

gb_internal wbInstr wb_opt_local_instr(wbOp op, u32 idx) {
	wbInstr in = {};
	in.op   = cast(u8)op;
	in.kind = wbInstr_Local;
	in.imm  = idx;
	in.pops   = op == wbOp_local_get ? 0 : 1;
	in.pushes = op == wbOp_local_set ? 0 : 1;
	return in;
}

gb_internal wbInstr wb_opt_scratch_begin(wbOpt *o, u8 kind, wbOp op, i8 pops, i8 pushes) {
	wbInstr in = {};
	in.offset = cast(u32)o->scratch.data.count;
	in.op     = cast(u8)op;
	in.kind   = kind;
	in.flags  = wbInstrFlag_Scratch;
	in.pops   = pops;
	in.pushes = pushes;
	wb_byte(&o->scratch, cast(u8)op);
	return in;
}
gb_internal void wb_opt_scratch_end(wbOpt *o, wbInstr *in) {
	in->length = cast(u8)(o->scratch.data.count - in->offset);
}

gb_internal void wb_opt_replace(wbOpt *o, isize k, wbInstr const &in) {
	if (!o->replaced[k]) {
		o->replaced[k] = true;
		array_init(&o->replacement[k], temporary_allocator(), 0, 4);
	}
	array_add(&o->replacement[k], in);
}

gb_internal void wb_opt_delete_range(wbOpt *o, i32 start, i32 end) {
	for (i32 k = start; k < end; k++) {
		o->deleted[k] = true;
	}
}

// True when the value a narrow store at `k` writes came straight from a load
// with the extension (1 = sign, 2 = zero) the promoted leaf's loads expect
gb_internal bool wb_opt_stored_value_is_extended(wbOpt *o, isize k, wbValType vt, u8 width, u8 ext) {
	isize prev = k-1;
	while (prev >= 0 && o->deleted[prev]) {
		prev--;
	}
	if (prev < 0 || o->replaced[prev] || o->in[prev].kind != wbInstr_Load) {
		return false;
	}
	wbOp op = cast(wbOp)o->in[prev].op;
	if (vt == wbValType_i32) {
		switch (width) {
		case 1: return op == (ext == 1 ? wbOp_i32_load8_s  : wbOp_i32_load8_u);
		case 2: return op == (ext == 1 ? wbOp_i32_load16_s : wbOp_i32_load16_u);
		}
	} else if (vt == wbValType_i64) {
		switch (width) {
		case 1: return op == (ext == 1 ? wbOp_i64_load8_s  : wbOp_i64_load8_u);
		case 2: return op == (ext == 1 ? wbOp_i64_load16_s : wbOp_i64_load16_u);
		case 4: return op == (ext == 1 ? wbOp_i64_load32_s : wbOp_i64_load32_u);
		}
	}
	return false;
}

// Decides which frame slots become locals and records the rewrites
gb_internal int wb_opt_u32_compare(void const *a, void const *b) {
	u32 x = *cast(u32 const *)a, y = *cast(u32 const *)b;
	return x < y ? -1 : x > y ? 1 : 0;
}

gb_internal u8 const *wb_opt_instr_bytes(wbOpt *o, wbInstr const &in) {
	return ((in.flags & wbInstrFlag_Scratch) ? o->scratch.data.data : o->p->code.data.data) + in.offset;
}

// The value of an integer constant instruction (i32.const or i64.const)
gb_internal bool wb_opt_const_int(wbOpt *o, wbInstr const &in, i64 *value) {
	if (in.kind != wbInstr_Const || (in.op != wbOp_i32_const && in.op != wbOp_i64_const)) {
		return false;
	}
	u8 const *b = wb_opt_instr_bytes(o, in);
	i64 result = 0;
	u32 shift = 0;
	u8 byte = 0;
	for (isize i = 1; i < in.length; i++) {
		byte = b[i];
		result |= cast(i64)(byte & 0x7f) << shift;
		shift += 7;
	}
	if (shift < 64 && (byte & 0x40)) {
		result |= -(cast(i64)1 << shift);
	}
	*value = result;
	return true;
}

// A wide store of an integer constant into a slot (the zeroing of a compound
// literal's unset fields) whose bytes are otherwise read and written in
// narrower pieces is split along those pieces, so that the slot can be
// promoted next pass
gb_internal bool wb_opt_split_const_stores(wbOpt *o, isize si) {
	wbProcedure *p = o->p;
	wbOptSlot &slot = o->slots[si];
	bool changed = false;
	for (isize k = 1; k < o->n; k++) {
		if (o->acc_abs[k] < 0 || o->deleted[k]) continue;
		wbInstr const &st = o->in[k];
		if (st.kind != wbInstr_Store) continue;
		if (wb_opt_find_slot(o, o->acc_abs[k]) != si) continue;
		if (o->acc_aend[k] != k-1 || o->deleted[k-1] || o->replaced[k-1] || o->replaced[k]) continue;
		i64 value = 0;
		if (!wb_opt_const_int(o, o->in[k-1], &value)) continue;
		if (!wb_opt_is_frame_address_range(o, o->acc_astart[k], o->acc_aend[k])) continue;
		for (i32 a = o->acc_astart[k]; a < o->acc_aend[k]; a++) {
			if (o->replaced[a] || o->moved[a]) goto next;
		}
		{
			u32 off = cast(u32)(o->acc_abs[k] - p->slots[si].offset);
			u32 end = off + st.width;
			// Boundaries of the other accesses inside the store: pieces
			// crossing the store's ends cannot be matched
			u32 bounds[16];
			isize nb = 0;
			bounds[nb++] = off;
			bounds[nb++] = end;
			bool ok = true;
			for (wbOptAccess const &a : slot.accesses) {
				u32 ae = a.offset + a.width;
				if (ae <= off || a.offset >= end) continue;
				if (a.offset == off && ae == end) continue;
				if (a.offset < off || ae > end) { ok = false; break; }
				if (nb + 2 > gb_count_of(bounds)) { ok = false; break; }
				bounds[nb++] = a.offset;
				bounds[nb++] = ae;
			}
			if (!ok) continue;
			gb_sort(bounds, nb, gb_size_of(u32), wb_opt_u32_compare);
			// The pieces, unread gaps chunked by powers of two
			u32 piece_off[32], piece_w[32];
			isize np = 0;
			for (isize i = 0; i+1 < nb; i++) {
				u32 lo = bounds[i], hi = bounds[i+1];
				while (lo < hi) {
					u32 left = hi - lo;
					u32 w = left >= 8 ? 8 : left >= 4 ? 4 : left >= 2 ? 2 : 1;
					if (np == gb_count_of(piece_off)) { ok = false; break; }
					piece_off[np] = lo;
					piece_w[np] = w;
					np++;
					lo += w;
				}
				if (!ok) break;
			}
			if (!ok) continue;
			// The type each piece's other accesses use: a single piece is
			// only rewritten when the store's type differs from it (an
			// integer zero stored into a float field)
			wbValType piece_vt[32];
			for (isize i = 0; i < np; i++) {
				u32 w = piece_w[i];
				piece_vt[i] = w == 8 ? wbValType_i64 : wbValType_i32;
				for (wbOptAccess const &a : slot.accesses) {
					if (a.offset == piece_off[i] && a.width == w && a.op != st.op) {
						piece_vt[i] = wb_opt_access_valtype(a.op);
						break;
					}
				}
			}
			if (np < 2 && piece_vt[0] == wb_opt_access_valtype(st.op)) continue;
			wb_opt_delete_range(o, o->acc_astart[k], cast(i32)k+1);
			o->deleted[k] = false;
			for (isize i = 0; i < np; i++) {
				u32 w = piece_w[i];
				wbValType vt = piece_vt[i];
				u64 bits = cast(u64)value >> (8*(piece_off[i] - off));
				if (w < 8) bits &= (cast(u64)1 << (8*w)) - 1;
				for (i32 a = o->acc_astart[k]; a < o->acc_aend[k]; a++) {
					wb_opt_replace(o, k, o->in[a]);
				}
				wbOp sop = wbOp_i32_store;
				switch (vt) {
				case wbValType_i32: {
					wbInstr c = wb_opt_scratch_begin(o, wbInstr_Const, wbOp_i32_const, 0, 1);
					c.imm = cast(u32)bits;
					wb_sleb(&o->scratch, cast(i32)bits);
					wb_opt_scratch_end(o, &c);
					wb_opt_replace(o, k, c);
					sop = w == 1 ? wbOp_i32_store8 : w == 2 ? wbOp_i32_store16 : wbOp_i32_store;
					break;
				}
				case wbValType_i64: {
					wbInstr c = wb_opt_scratch_begin(o, wbInstr_Const, wbOp_i64_const, 0, 1);
					wb_sleb(&o->scratch, cast(i64)bits);
					wb_opt_scratch_end(o, &c);
					wb_opt_replace(o, k, c);
					sop = w == 1 ? wbOp_i64_store8 : w == 2 ? wbOp_i64_store16 : w == 4 ? wbOp_i64_store32 : wbOp_i64_store;
					break;
				}
				case wbValType_f32: {
					wbInstr c = wb_opt_scratch_begin(o, wbInstr_Const, wbOp_f32_const, 0, 1);
					u32 b32 = cast(u32)bits;
					wb_bytes(&o->scratch, &b32, 4);
					wb_opt_scratch_end(o, &c);
					wb_opt_replace(o, k, c);
					sop = wbOp_f32_store;
					break;
				}
				case wbValType_f64: {
					wbInstr c = wb_opt_scratch_begin(o, wbInstr_Const, wbOp_f64_const, 0, 1);
					wb_bytes(&o->scratch, &bits, 8);
					wb_opt_scratch_end(o, &c);
					wb_opt_replace(o, k, c);
					sop = wbOp_f64_store;
					break;
				}
				default:
					GB_PANIC("unexpected value type");
				}
				wbInstr ns = wb_opt_scratch_begin(o, wbInstr_Store, sop, 2, 0);
				wb_uleb(&o->scratch, 0);
				wb_uleb(&o->scratch, st.imm + (piece_off[i] - off));
				ns.imm   = st.imm + (piece_off[i] - off);
				ns.width = cast(u8)w;
				wb_opt_scratch_end(o, &ns);
				wb_opt_replace(o, k, ns);
			}
			changed = true;
		}
	next:;
	}
	return changed;
}

gb_internal void wb_opt_promote_slots(wbOpt *o) {
	wbProcedure *p = o->p;
	if (o->all_escaped) {
		return;
	}
	gbAllocator ta = temporary_allocator();

	for_array(si, o->slots) {
		wbOptSlot &slot = o->slots[si];
		if (slot.escaped || slot.accesses.count == 0) {
			continue;
		}
		gb_sort(slot.accesses.data, slot.accesses.count, gb_size_of(wbOptAccess), wb_opt_access_compare);
		bool ok = true;
		u32 prev_end = 0;
		for (isize i = 0; i < slot.accesses.count && ok; ) {
			wbOptAccess const &a = slot.accesses[i];
			if (a.offset < prev_end) {
				ok = false; // overlaps the previous leaf
				break;
			}
			wbValType vt = wb_opt_access_valtype(a.op);
			int load_ext = 0;
			isize j = i;
			for (; j < slot.accesses.count && slot.accesses[j].offset == a.offset; j++) {
				wbOptAccess const &b = slot.accesses[j];
				if (b.width != a.width || wb_opt_access_valtype(b.op) != vt) {
					ok = false;
					break;
				}
				int ext = wb_opt_access_ext(b.op);
				if (ext == 1 || ext == 2) {
					if (load_ext != 0 && load_ext != ext) {
						ok = false;
						break;
					}
					load_ext = ext;
				}
			}
			if (!ok) break;
			if (slot.leaf_offsets.allocator.proc == nullptr) {
				array_init(&slot.leaf_offsets, ta, 0, 8);
				array_init(&slot.leaf_locals,  ta, 0, 8);
				array_init(&slot.leaf_ext,     ta, 0, 8);
				array_init(&slot.leaf_width,   ta, 0, 8);
			}
			array_add(&slot.leaf_offsets, a.offset);
			array_add(&slot.leaf_locals,  WB_NO_LOCAL);
			array_add(&slot.leaf_ext,     cast(u8)load_ext);
			array_add(&slot.leaf_width,   a.width);
			prev_end = a.offset + a.width;
			i = j;
		}
		if (!ok) {
			if (wb_opt_split_const_stores(o, si)) o->changed = true;
			slot.escaped = true;
			continue;
		}
		// The address computations must be deletable
		for (isize k = 0; k < o->n; k++) {
			if (o->acc_abs[k] < 0 || o->deleted[k]) continue;
			if (wb_opt_find_slot(o, o->acc_abs[k]) != si) continue;
			if (!wb_opt_is_frame_address_range(o, o->acc_astart[k], o->acc_aend[k])) {
				slot.escaped = true;
				break;
			}
		}
	}

	// Rewrite the accesses: the address computation is deleted and the
	// load/store becomes local.get/local.set
	for (isize k = 0; k < o->n; k++) {
		if (o->acc_abs[k] < 0 || o->deleted[k]) continue;
		isize si = wb_opt_find_slot(o, o->acc_abs[k]);
		wbOptSlot &slot = o->slots[si];
		if (slot.escaped) continue;
		u32 rel = cast(u32)(o->acc_abs[k] - p->slots[si].offset);
		isize li = -1;
		for_array(l, slot.leaf_offsets) {
			if (slot.leaf_offsets[l] == rel) { li = l; break; }
		}
		GB_ASSERT(li >= 0);
		wbInstr const &in = o->in[k];
		wbValType vt = wb_opt_access_valtype(in.op);
		if (slot.leaf_locals[li] == WB_NO_LOCAL) {
			slot.leaf_locals[li] = wb_add_local(p, vt, {});
		}
		u32 local = slot.leaf_locals[li];
		wb_opt_delete_range(o, o->acc_astart[k], o->acc_aend[k]);
		if (in.kind == wbInstr_Load) {
			wb_opt_replace(o, k, wb_opt_local_instr(wbOp_local_get, local));
		} else {
			// Narrow stores must leave the value the loads expect
			u8 ext = slot.leaf_ext[li];
			u8 width = slot.leaf_width[li];
			bool narrow = width < (vt == wbValType_i64 ? 8 : 4);
			if (narrow && wb_opt_stored_value_is_extended(o, k, vt, width, ext)) {
				narrow = false;
			}
			if (narrow && ext == 2) {
				wbInstr c = wb_opt_scratch_begin(o, wbInstr_Const, vt == wbValType_i64 ? wbOp_i64_const : wbOp_i32_const, 0, 1);
				c.imm = width == 1 ? 0xff : width == 2 ? 0xffff : 0xffffffff;
				wb_sleb(&o->scratch, cast(i64)c.imm);
				wb_opt_scratch_end(o, &c);
				wb_opt_replace(o, k, c);
				wbInstr a = wb_opt_scratch_begin(o, wbInstr_Other, vt == wbValType_i64 ? wbOp_i64_and : wbOp_i32_and, 2, 1);
				wb_opt_scratch_end(o, &a);
				wb_opt_replace(o, k, a);
			} else if (narrow && ext == 1) {
				wbOp op = wbOp_i32_extend8_s;
				if (vt == wbValType_i64) {
					op = width == 1 ? wbOp_i64_extend8_s : width == 2 ? wbOp_i64_extend16_s : wbOp_i64_extend32_s;
				} else {
					op = width == 1 ? wbOp_i32_extend8_s : wbOp_i32_extend16_s;
				}
				wbInstr e = wb_opt_scratch_begin(o, wbInstr_Other, op, 1, 1);
				wb_opt_scratch_end(o, &e);
				wb_opt_replace(o, k, e);
			}
			wb_opt_replace(o, k, wb_opt_local_instr(wbOp_local_set, local));
		}
		o->changed = true;
	}
}

// Copy merging
//
// Aggregates are copied one scalar leaf at a time so that frame slots can be
// promoted to locals. Once inlining is done, the copies whose ends stay in
// memory are merged into the widest loads and stores covering the bytes.

struct wbOptCopyPair {
	i32 first, last; // the local.get of the destination and the store
	u32 dst, src;    // base locals
	u32 dst_off, src_off, width;
};

// Matches `local.get dst; local.get src; load; store` at the live
// instructions from k on, returning the index after the store or -1
gb_internal i32 wb_opt_match_copy_pair(wbOpt *o, i32 k, wbOptCopyPair *pair) {
	i32 idx[4];
	int n = 0;
	for (i32 j = k; j < o->n && n < 4; j++) {
		if (o->deleted[j]) continue;
		if (o->replaced[j] || o->moved[j]) return -1;
		idx[n++] = j;
	}
	if (n < 4) {
		return -1;
	}
	wbInstr const &d  = o->in[idx[0]];
	wbInstr const &s  = o->in[idx[1]];
	wbInstr const &ld = o->in[idx[2]];
	wbInstr const &st = o->in[idx[3]];
	if (d.kind != wbInstr_Local || d.op != wbOp_local_get || o->moved_to[d.imm] == idx[0]) return -1;
	if (s.kind != wbInstr_Local || s.op != wbOp_local_get || o->moved_to[s.imm] == idx[1]) return -1;
	if (ld.kind != wbInstr_Load || st.kind != wbInstr_Store) return -1;
	if (ld.width != st.width || wb_opt_access_valtype(ld.op) != wb_opt_access_valtype(st.op)) return -1;
	pair->first   = idx[0];
	pair->last    = idx[3];
	pair->dst     = d.imm;
	pair->src     = s.imm;
	pair->dst_off = st.imm;
	pair->src_off = ld.imm;
	pair->width   = ld.width;
	return idx[3]+1;
}

gb_internal void wb_opt_merge_copy_run(wbOpt *o, Array<wbOptCopyPair> const &run) {
	if (run.count < 2) {
		return;
	}
	wbOptCopyPair const &r0 = run[0];
	u32 total = 0;
	for (wbOptCopyPair const &r : run) total += r.width;
	if (r0.dst == r0.src && r0.dst_off < r0.src_off + total && r0.src_off < r0.dst_off + total) {
		return; // overlapping: the leaf order matters
	}
	isize chunks = 0;
	for (u32 done = 0; done < total; chunks++) {
		u32 left = total - done;
		done += left >= 8 ? 8 : left >= 4 ? 4 : left >= 2 ? 2 : 1;
	}
	if (chunks >= run.count) {
		return;
	}
	// The first instruction stays live and carries the replacement: deleted
	// instructions are never emitted, replaced or not
	wb_opt_delete_range(o, r0.first+1, r0.last+1);
	for (isize i = 1; i < run.count; i++) {
		wb_opt_delete_range(o, run[i].first, run[i].last+1);
	}
	isize k = r0.first;
	for (u32 done = 0; done < total; ) {
		u32 left = total - done;
		u32 w = left >= 8 ? 8 : left >= 4 ? 4 : left >= 2 ? 2 : 1;
		wbOp lop = wbOp_i32_load8_u, sop = wbOp_i32_store8;
		switch (w) {
		case 8: lop = wbOp_i64_load;     sop = wbOp_i64_store;   break;
		case 4: lop = wbOp_i32_load;     sop = wbOp_i32_store;   break;
		case 2: lop = wbOp_i32_load16_u; sop = wbOp_i32_store16; break;
		}
		wb_opt_replace(o, k, wb_opt_local_instr(wbOp_local_get, r0.dst));
		wb_opt_replace(o, k, wb_opt_local_instr(wbOp_local_get, r0.src));
		wbInstr ld = wb_opt_scratch_begin(o, wbInstr_Load, lop, 1, 1);
		wb_uleb(&o->scratch, 0); // alignment hint: the bases may be unaligned for the merged width
		wb_uleb(&o->scratch, r0.src_off + done);
		ld.imm   = r0.src_off + done;
		ld.width = cast(u8)w;
		wb_opt_scratch_end(o, &ld);
		wb_opt_replace(o, k, ld);
		wbInstr st = wb_opt_scratch_begin(o, wbInstr_Store, sop, 2, 0);
		wb_uleb(&o->scratch, 0);
		wb_uleb(&o->scratch, r0.dst_off + done);
		st.imm   = r0.dst_off + done;
		st.width = cast(u8)w;
		wb_opt_scratch_end(o, &st);
		wb_opt_replace(o, k, st);
		done += w;
	}
	o->changed = true;
}

gb_internal void wb_opt_merge_copies(wbOpt *o) {
	auto run = array_make<wbOptCopyPair>(temporary_allocator(), 0, 16);
	i32 k = 0;
	while (k < o->n) {
		wbOptCopyPair pair;
		i32 next = wb_opt_match_copy_pair(o, k, &pair);
		if (next < 0) {
			wb_opt_merge_copy_run(o, run);
			run.count = 0;
			k++;
			continue;
		}
		if (run.count > 0) {
			wbOptCopyPair const &prev = run[run.count-1];
			if (pair.dst != prev.dst || pair.src != prev.src ||
			    pair.dst_off != prev.dst_off + prev.width || pair.src_off != prev.src_off + prev.width) {
				wb_opt_merge_copy_run(o, run);
				run.count = 0;
			}
		}
		array_add(&run, pair);
		k = next;
	}
	wb_opt_merge_copy_run(o, run);
}

// Whether the procedure has two adjacent leaf copies wb_opt_merge_copies
// could merge, for skipping the optimization after inlining
gb_internal bool wb_has_copy_runs(wbProcedure *p) {
	Array<wbInstr> const &in = p->instrs;
	for (isize k = 0; k + 8 <= in.count; k++) {
		if (in[k].kind != wbInstr_Local || in[k].op != wbOp_local_get) continue;
		if (in[k+1].kind != wbInstr_Local || in[k+1].op != wbOp_local_get) continue;
		if (in[k+2].kind != wbInstr_Load || in[k+3].kind != wbInstr_Store) continue;
		if (in[k+4].kind != wbInstr_Local || in[k+4].imm != in[k].imm) continue;
		if (in[k+5].kind != wbInstr_Local || in[k+5].imm != in[k+1].imm) continue;
		if (in[k+6].kind != wbInstr_Load || in[k+7].kind != wbInstr_Store) continue;
		if (in[k+6].imm == in[k+2].imm + in[k+2].width && in[k+7].imm == in[k+3].imm + in[k+3].width) {
			return true;
		}
	}
	return false;
}

// Locals read by the producer range [start, end), following the producers
// that move into it
gb_internal bool wb_opt_range_reads_local_written(wbOpt *o, i32 start, i32 end, i32 region_lo, i32 region_hi, int depth);

gb_internal bool wb_opt_local_written_between(wbOpt *o, u32 local, i32 lo, i32 hi) {
	// any write to `local` with lo < index < hi
	Array<i32> const &w = o->writes[local];
	isize a = 0, b = w.count;
	while (a < b) {
		isize mid = (a + b) / 2;
		if (w[mid] <= lo) a = mid + 1; else b = mid;
	}
	return a < w.count && w[a] < hi;
}

gb_internal bool wb_opt_range_reads_local_written(wbOpt *o, i32 start, i32 end, i32 region_lo, i32 region_hi, int depth) {
	if (depth > 8) {
		return true;
	}
	for (i32 k = start; k < end; k++) {
		if (o->deleted[k]) continue;
		wbInstr const &in = o->in[k];
		if (in.kind == wbInstr_Local && in.op == wbOp_local_get) {
			u32 l = in.imm;
			if (o->moved_to[l] == k) {
				if (wb_opt_range_reads_local_written(o, o->moved_start[l], o->single_set[l], region_lo, region_hi, depth+1)) {
					return true;
				}
			} else if (wb_opt_local_written_between(o, l, region_lo, region_hi)) {
				return true;
			}
		} else if (in.kind == wbInstr_Load || (in.kind == wbInstr_Other && in.op == wbOp_memory_size)) {
			if (o->memw_prefix[region_hi] - o->memw_prefix[region_lo+1] != 0) {
				return true;
			}
		} else if (in.kind == wbInstr_GlobalGet) {
			if (o->globw_prefix[region_hi] - o->globw_prefix[region_lo+1] != 0) {
				return true;
			}
		}
	}
	return false;
}

// A value stored to a local with a single use may stay on the operand stack
// when its computation can move to the use
gb_internal void wb_opt_move_producers(wbOpt *o) {
	wbProcedure *p = o->p;
	// Later sets first: a moved range may contain the use of an earlier
	// set, which then moves further to where the range now executes.
	auto effective = array_make<i32>(temporary_allocator(), o->n);
	for (isize k = 0; k < o->n; k++) {
		effective[k] = cast(i32)k;
	}
	for (isize k = o->n-1; k >= 0; k--) {
		if (o->deleted[k]) continue;
		wbInstr const &in = o->in[k];
		if (in.kind != wbInstr_Local || in.op != wbOp_local_set) continue;
		u32 l = in.imm;
		if (l < p->param_count || l == p->fp_local || l == p->old_sp_local) continue;
		if (o->sets[l] != 1 || o->tees[l] != 0 || o->gets[l] != 1) continue;
		i32 i = cast(i32)k;
		i32 j = o->single_get[l];
		if (j <= i || o->deleted[j]) continue;
		i32 s = o->set_producer[k];
		if (s < 0 || i - s > WB_OPT_MAX_PRODUCER) continue;
		i32 use = j;
		j = effective[j];
		// the get must be reached from the set without leaving its block or
		// entering a loop
		i32 b = o->block_of[j];
		i32 target = o->block_of[i];
		bool ok = true;
		while (b != target) {
			if (b < 0 || o->blocks[b].is_loop) { ok = false; break; }
			b = o->blocks[b].parent;
		}
		if (!ok) continue;
		if (wb_opt_range_reads_local_written(o, s, i, i, j, 0)) continue;
		o->moved_to[l] = use;
		o->moved_start[l] = s;
		for (i32 m = s; m <= i; m++) {
			o->moved[m] = true;
			effective[m] = j;
		}
		o->changed = true;
	}
}

// Replaces the reads of a local that is a copy of another local (`local.get
// a; local.set b`, the only write to b) by reads of the original, when that
// still holds the same value there. Inlining makes many such copies of its
// arguments. Returns true when anything was replaced.
// A copy of a simple instruction (no immediates beyond `imm`) in the scratch buffer
gb_internal wbInstr wb_opt_copy_instr(wbOpt *o, i32 k) {
	wbInstr const &in = o->in[k];
	wbInstr c = wb_opt_scratch_begin(o, in.kind, cast(wbOp)in.op, in.pops, in.pushes);
	c.imm = in.imm;
	c.flags |= in.flags & ~wbInstrFlag_Scratch;
	if (in.kind == wbInstr_Local) {
		wb_uleb(&o->scratch, in.imm); // may have been created without bytes
	} else if (in.length > 1) {
		wb_bytes(&o->scratch, wb_opt_instr_bytes(o, in) + 1, in.length - 1);
	}
	wb_opt_scratch_end(o, &c);
	return c;
}

// Whether the local set at `k` only holds a frame address (`fp + c`, which
// never changes) and is only read to address loads and stores: those can
// compute the address themselves, which lets the slot be promoted
gb_internal bool wb_opt_is_frame_address_local(wbOpt *o, i32 k) {
	i32 s = o->set_producer[k];
	if (s < 0 || !wb_opt_is_frame_address_range(o, s, k)) return false;
	bool has_fp = false;
	for (i32 i = s; i < k; i++) {
		if (o->deleted[i]) return false;
		if (o->in[i].kind == wbInstr_Local) has_fp = true;
	}
	if (!has_fp) return false;
	for (i32 j : o->reads[o->in[k].imm]) {
		if (!o->addr_get[j]) return false;
	}
	return true;
}

gb_internal bool wb_opt_propagate_copies(wbOpt *o) {
	wbProcedure *p = o->p;
	bool changed = false;
	auto removed = array_make<bool>(temporary_allocator(), p->locals.count);
	for_array(i, removed) {
		removed[i] = false;
	}
	for (isize k = 1; k < o->n; k++) {
		if (o->deleted[k] || o->deleted[k-1]) continue;
		wbInstr const &set = o->in[k];
		wbInstr const &get = o->in[k-1];
		if (set.kind != wbInstr_Local || set.op != wbOp_local_set) continue;
		u32 b = set.imm;
		if (b < p->param_count || b == p->fp_local || b == p->old_sp_local) continue;
		if (o->sets[b] != 1 || o->tees[b] != 0 || o->gets[b] == 0) continue;
		bool frame_addr = wb_opt_is_frame_address_local(o, cast(i32)k);
		// a constant: reads become the constant (which the peephole rules
		// and the wasm compiler fold)
		bool constant = !frame_addr && o->set_producer[k] == cast(i32)(k-1) && get.kind == wbInstr_Const;
		u32 a = p->fp_local;
		if (constant) {
			// nothing: the value is the same wherever it is read
		} else if (!frame_addr) {
			if (get.kind != wbInstr_Local || get.op != wbOp_local_get) continue;
			a = get.imm;
			if (a == b) continue;
			if (removed[a]) continue; // a copy of a copy: the next pass sees through it
			if (o->set_producer[k] != cast(i32)(k-1)) continue;
		}
		bool all = true;
		for (i32 j : o->reads[b]) {
			if (j <= cast(i32)k || o->deleted[j]) { all = false; continue; }
			// the set must reach the get: same block or an enclosing one, and
			// `a` must keep its value on every path in between (also around
			// the loops the get is in that the set is not)
			i32 blk = o->block_of[j], target = o->block_of[k];
			i32 limit = j;
			bool ok = true;
			while (blk != target) {
				if (blk < 0) { ok = false; break; }
				if (o->blocks[blk].is_loop && o->blocks[blk].end > limit) limit = o->blocks[blk].end;
				blk = o->blocks[blk].parent;
			}
			if (!ok || (!constant && wb_opt_local_written_between(o, a, cast(i32)k, limit+1))) { all = false; continue; }
			if (constant) {
				wb_opt_replace(o, j, wb_opt_copy_instr(o, cast(i32)k-1));
			} else if (frame_addr) {
				for (i32 i = o->set_producer[k]; i < cast(i32)k; i++) {
					wb_opt_replace(o, j, wb_opt_copy_instr(o, i));
				}
			} else {
				wb_opt_replace(o, j, wb_opt_local_instr(wbOp_local_get, a));
			}
			changed = true;
		}
		if (all) {
			wb_opt_delete_range(o, frame_addr ? o->set_producer[k] : cast(i32)k-1, cast(i32)k+1);
			removed[b] = true;
		}
	}
	return changed;
}

// `A B C local.set c local.set b local.set a` (how the inliner binds
// arguments) becomes `A local.set a B local.set b C local.set c`, so that
// each value is stored right after being computed and can be propagated
gb_internal bool wb_opt_pair_sets(wbOpt *o) {
	bool changed = false;
	for (i32 k = 0; k < o->n; k++) {
		if (o->deleted[k]) continue;
		wbInstr const &in = o->in[k];
		if (in.kind != wbInstr_Local || in.op != wbOp_local_set) continue;
		// The run of sets and the values they take, top of the stack first
		i32 count = 0;
		i32 end = k;
		i32 last = k;
		while (last < o->n && !o->deleted[last] && o->in[last].kind == wbInstr_Local && o->in[last].op == wbOp_local_set) {
			i32 vs = o->op0_start[last], ve = o->op0_end[last];
			if (vs < 0 || ve != end) break;
			bool pure = true;
			for (i32 i = vs; i < ve; i++) {
				if (o->deleted[i] || !wb_opt_is_pure(o->in[i])) { pure = false; break; }
			}
			if (!pure) break;
			end = vs;
			count += 1;
			last += 1;
		}
		if (count < 2) continue;
		// The set at k+i takes the value computed before those of the sets
		// k..k+i-1, and now happens before them: they must not mention its
		// local (which also has to be distinct from theirs)
		bool ok = true;
		for (i32 i = count-1; i >= 1 && ok; i--) {
			u32 l = o->in[k+i].imm;
			for (i32 j = i-1; j >= 0 && ok; j--) {
				if (o->in[k+j].imm == l) { ok = false; break; }
				for (i32 x = o->op0_start[k+j]; x < o->op0_end[k+j]; x++) {
					if (o->in[x].kind == wbInstr_Local && o->in[x].imm == l) { ok = false; break; }
				}
			}
		}
		if (!ok) continue;
		wb_opt_delete_range(o, end, k+count);
		o->deleted[k] = false;
		for (i32 i = count-1; i >= 0; i--) {
			for (i32 x = o->op0_start[k+i]; x < o->op0_end[k+i]; x++) {
				wb_opt_replace(o, k, o->in[x]);
			}
			wb_opt_replace(o, k, o->in[k+i]);
		}
		changed = true;
		k += count-1;
	}
	return changed;
}

// Whether every read of local `l` only feeds a store to `l` itself (a loop
// counter nobody looks at: `l = l + 1`), so that its stores are dead
gb_internal bool wb_opt_local_feeds_itself(wbOpt *o, u32 l) {
	if (o->tees[l] != 0 || o->gets[l] == 0) return false;
	for (i32 r : o->reads[l]) {
		bool inside = false;
		for (i32 k : o->writes[l]) {
			i32 s = o->set_producer[k];
			if (s >= 0 && s <= r && r < k) { inside = true; break; }
		}
		if (!inside) return false;
	}
	return true;
}

// A store to a local that is stored again before being read (the zeroing
// of a compound literal's fields before they are assigned)
gb_internal void wb_opt_remove_overwritten_sets(wbOpt *o) {
	wbProcedure *p = o->p;
	for (isize l = p->param_count; l < o->writes.count; l++) {
		if (l == p->fp_local || l == p->old_sp_local || o->writes[l].count < 2) continue;
		Array<i32> const &writes = o->writes[l];
		Array<i32> const &reads  = o->reads[l];
		isize r = 0;
		for (isize w = 0; w+1 < writes.count; w++) {
			i32 k = writes[w], k2 = writes[w+1];
			if (o->deleted[k] || o->deleted[k2] || o->replaced[k]) continue;
			if (o->in[k].op != wbOp_local_set || o->block_of[k] != o->block_of[k2]) continue;
			while (r < reads.count && reads[r] < k) r++;
			if (r < reads.count && reads[r] < k2) continue;
			// nothing between may leave for a place the first value is
			// visible: branches only go to blocks that begin in between
			bool ok = true;
			for (i32 i = k+1; i < k2 && ok; i++) {
				if (o->deleted[i]) continue;
				wbInstr const &in = o->in[i];
				if (in.kind == wbInstr_Br || in.kind == wbInstr_BrIf || (in.kind == wbInstr_Other && in.op == wbOp_br_table)) {
					i32 t = wb_opt_br_target(o, i);
					if (t < 0 || o->blocks[t].start <= k) ok = false;
				}
			}
			if (!ok) continue;
			i32 s = o->set_producer[k];
			if (s >= 0 && cast(i32)k - s <= WB_OPT_MAX_PRODUCER && wb_opt_range_untouched(o, s, k+1)) {
				bool pure = true;
				for (i32 i = s; i < k; i++) {
					if (!o->deleted[i] && !wb_opt_is_pure(o->in[i])) { pure = false; break; }
				}
				if (!pure) continue;
				wb_opt_delete_range(o, s, k+1);
			} else {
				wbInstr d = wb_opt_scratch_begin(o, wbInstr_Other, wbOp_drop, 1, 0);
				wb_opt_scratch_end(o, &d);
				wb_opt_replace(o, k, d);
			}
			o->changed = true;
		}
	}
}

// Stores to locals nobody reads
gb_internal void wb_opt_remove_dead_sets(wbOpt *o) {
	wbProcedure *p = o->p;
	auto dead = array_make<bool>(temporary_allocator(), o->gets.count); // locals added by this pass have no reads
	for_array(l, dead) {
		dead[l] = o->gets[l] == 0 || wb_opt_local_feeds_itself(o, cast(u32)l);
	}
	for (isize k = 0; k < o->n; k++) {
		if (o->deleted[k] || o->replaced[k]) continue;
		wbInstr const &in = o->in[k];
		if (in.kind != wbInstr_Local || in.op == wbOp_local_get) continue;
		u32 l = in.imm;
		if (l < p->param_count || l == p->fp_local || l == p->old_sp_local) continue;
		if (l >= cast(u32)dead.count || !dead[l]) continue;
		if (in.op == wbOp_local_tee) {
			o->deleted[k] = true;
		} else {
			i32 s = o->set_producer[k];
			if (s >= 0 && cast(i32)k - s <= WB_OPT_MAX_PRODUCER) {
				wb_opt_delete_range(o, s, cast(i32)k + 1);
			} else {
				wbInstr d = wb_opt_scratch_begin(o, wbInstr_Other, wbOp_drop, 1, 0);
				wb_opt_scratch_end(o, &d);
				wb_opt_replace(o, k, d);
			}
		}
		o->changed = true;
	}
}

// Local rewrites of instruction patterns the lowering leaves behind
gb_internal bool wb_opt_live(wbOpt *o, i32 k) {
	return k >= 0 && k < o->n && !o->deleted[k] && !o->replaced[k] && !o->moved[k];
}
gb_internal bool wb_opt_range_live(wbOpt *o, i32 a, i32 b) {
	for (i32 k = a; k <= b; k++) {
		if (!wb_opt_live(o, k)) return false;
	}
	return true;
}
// Whether nothing in [a, b) has been replaced or moved elsewhere, so that the
// range can be deleted as a whole
gb_internal bool wb_opt_range_untouched(wbOpt *o, i32 a, i32 b) {
	for (i32 k = a; k < b; k++) {
		if (o->replaced[k] || o->moved[k]) return false;
		if (o->in[k].kind == wbInstr_Local && o->in[k].op == wbOp_local_get && o->moved_to[o->in[k].imm] == k) return false;
	}
	return true;
}
gb_internal wbInstr wb_opt_i32_const(wbOpt *o, u32 value) {
	wbInstr in = wb_opt_scratch_begin(o, wbInstr_Const, wbOp_i32_const, 0, 1);
	in.imm = value;
	wb_sleb(&o->scratch, cast(i32)value);
	wb_opt_scratch_end(o, &in);
	return in;
}
// A constant of any value type from its bit pattern
gb_internal wbInstr wb_opt_const_bits(wbOpt *o, wbValType vt, u64 bits) {
	wbOp op = wbOp_i32_const;
	switch (vt) {
	case wbValType_i64: op = wbOp_i64_const; break;
	case wbValType_f32: op = wbOp_f32_const; break;
	case wbValType_f64: op = wbOp_f64_const; break;
	default: break;
	}
	wbInstr in = wb_opt_scratch_begin(o, wbInstr_Const, op, 0, 1);
	switch (vt) {
	case wbValType_i64: wb_sleb(&o->scratch, cast(i64)bits); break;
	case wbValType_f32: for (int i = 0; i < 4; i++) wb_byte(&o->scratch, cast(u8)(bits >> (8*i))); break;
	case wbValType_f64: for (int i = 0; i < 8; i++) wb_byte(&o->scratch, cast(u8)(bits >> (8*i))); break;
	default: in.imm = cast(u32)bits; wb_sleb(&o->scratch, cast(i32)cast(u32)bits); break;
	}
	wb_opt_scratch_end(o, &in);
	return in;
}
gb_internal wbInstr wb_opt_memarg(wbOpt *o, wbOp op, u32 offset, u32 width) {
	bool is_store = op >= wbOp_i32_store;
	wbInstr in = wb_opt_scratch_begin(o, is_store ? wbInstr_Store : wbInstr_Load, op, is_store ? 2 : 1, is_store ? 0 : 1);
	in.imm   = offset;
	in.width = cast(u8)width;
	u32 align = 0;
	while ((1u << align) < width) align++;
	wb_uleb(&o->scratch, align);
	wb_uleb(&o->scratch, offset);
	wb_opt_scratch_end(o, &in);
	return in;
}
// The block a branch at `k` targets
gb_internal i32 wb_opt_br_target(wbOpt *o, i32 k) {
	i32 b = o->block_of[k];
	for (u32 d = 0; d < o->in[k].imm && b >= 0; d++) {
		b = o->blocks[b].parent;
	}
	return b;
}

#define WB_OPT_CONST_SCAN_BUDGET 4096
#define WB_OPT_PEEL_MAX_INSTRS   512

// Whether local `l` certainly holds the `i32` constant `*value` when
// execution reaches instruction `k`: the straight-line code before `k` is
// walked backwards for the write of `l`, out of the blocks `k` is in and
// into the blocks that end before it through every exit those have
gb_internal bool wb_opt_loop_writes_local(wbOpt *o, i32 blk, u32 l, i32 *budget) {
	i32 e = o->blocks[blk].end;
	if (e < 0) return true;
	for (i32 i = o->blocks[blk].start+1; i < e; i++) {
		if (--*budget < 0) return true;
		if (!o->deleted[i] && o->in[i].kind == wbInstr_Local && o->in[i].op != wbOp_local_get && o->in[i].imm == l) return true;
	}
	return false;
}

gb_internal bool wb_opt_local_const_before(wbOpt *o, i32 k, u32 l, u32 *value, bool *have, int depth, i32 *budget);
gb_internal bool wb_opt_eval_i32(wbOpt *o, i32 start, i32 end, i32 at, u32 *result, int depth, i32 *budget);

// The value of `l` at the end of a block (`end` or `else` at `end_index`):
// the same constant on every path reaching it. `*any` is set when there is
// a path at all.
gb_internal bool wb_opt_block_exits_const(wbOpt *o, i32 blk, i32 end_index, u32 l, u32 *value, bool *have, int depth, i32 *budget, bool *any) {
	wbOptBlock const &b = o->blocks[blk];
	// falling out of the block
	i32 last = end_index-1;
	while (last > b.start && o->deleted[last]) last--;
	u8 lk = o->in[last].kind;
	if (last == b.start || (lk != wbInstr_Br && lk != wbInstr_Return && lk != wbInstr_Unreachable)) {
		if (!wb_opt_local_const_before(o, end_index, l, value, have, depth+1, budget)) return false;
		*any = true;
	}
	// branching to its end
	for (i32 i = b.start+1; i < end_index; i++) {
		if (o->deleted[i]) continue;
		u8 ik = o->in[i].kind;
		if (ik == wbInstr_Other && o->in[i].op == wbOp_br_table) return false;
		if (ik != wbInstr_Br && ik != wbInstr_BrIf) continue;
		if (wb_opt_br_target(o, i) != blk) continue;
		if (!wb_opt_local_const_before(o, i, l, value, have, depth+1, budget)) return false;
		*any = true;
	}
	return true;
}
gb_internal bool wb_opt_local_const_before_scan(wbOpt *o, i32 k, u32 l, u32 *value, bool *have, int depth, i32 *budget);
gb_internal bool wb_opt_local_const_before(wbOpt *o, i32 k, u32 l, u32 *value, bool *have, int depth, i32 *budget) {
	if (depth > 64) return false;
	// The paths through nested blocks multiply, so what is found on
	// arrival at an instruction is remembered for the query
	if (o->memo_state[k] != 0 && o->memo_local[k] == l) {
		if (o->memo_state[k] == 2) return false;
		if (*have && *value != o->memo_value[k]) return false;
		*have = true;
		*value = o->memo_value[k];
		return true;
	}
	bool had = *have;
	u32 old = *value;
	bool ok = wb_opt_local_const_before_scan(o, k, l, value, have, depth, budget);
	if (o->memo_state[k] == 0) array_add(&o->memo_touched, k);
	o->memo_local[k] = l;
	if (ok) {
		o->memo_state[k] = 1;
		o->memo_value[k] = *value;
	} else if (had && *have && *value != old) {
		o->memo_state[k] = 0; // failed on the consistency check alone
	} else {
		o->memo_state[k] = 2;
	}
	return ok;
}
gb_internal void wb_opt_memo_reset(wbOpt *o) {
	for (i32 k : o->memo_touched) o->memo_state[k] = 0;
	o->memo_touched.count = 0;
}
gb_internal bool wb_opt_local_const_before_scan(wbOpt *o, i32 k, u32 l, u32 *value, bool *have, int depth, i32 *budget) {
	for (i32 j = k-1; j >= 0; j--) {
		if (--*budget < 0) return false;
		if (o->deleted[j]) continue;
		wbInstr const &in = o->in[j];
		bool writes_l = in.kind == wbInstr_Local && in.op != wbOp_local_get && in.imm == l;
		if (o->moved[j]) {
			if (writes_l) return false;
			continue;
		}
		if (o->replaced[j]) {
			// the rewrites of this pass: the values replacing a read or a
			// computation are pure, anything else is not looked through
			bool pure = in.kind == wbInstr_Other || in.kind == wbInstr_Const || in.kind == wbInstr_Load ||
			            in.kind == wbInstr_MemCopy || in.kind == wbInstr_CallIndirect ||
			            (in.kind == wbInstr_Local && in.op == wbOp_local_get);
			if (!pure) return false;
			continue;
		}
		switch (in.kind) {
		case wbInstr_Local: {
			if (!writes_l) continue;
			u32 c = 0;
			if (j >= 1 && !o->deleted[j-1] && !o->replaced[j-1] && !o->moved[j-1] &&
			    o->in[j-1].kind == wbInstr_Const && o->in[j-1].op == wbOp_i32_const) {
				c = o->in[j-1].imm;
			} else if (o->op0_start[j] < 0 || o->op0_end[j] != j || !wb_opt_eval_i32(o, o->op0_start[j], j, j, &c, depth+1, budget)) {
				return false;
			}
			if (*have && *value != c) return false;
			*have = true;
			*value = c;
			return true;
		}
		case wbInstr_Block:
			continue; // out of the block `k` is in
		case wbInstr_End: {
			i32 blk = o->block_of[j];
			wbOptBlock const &b = o->blocks[blk];
			if (b.is_loop) {
				// past a loop that does not write the local
				if (wb_opt_loop_writes_local(o, blk, l, budget)) return false;
				j = b.start;
				continue;
			}
			bool any = false;
			switch (o->in[b.start].kind) {
			case wbInstr_Block:
				return wb_opt_block_exits_const(o, blk, j, l, value, have, depth, budget, &any) && any;
			case wbInstr_If:
				// taken or skipped
				return wb_opt_block_exits_const(o, blk, j, l, value, have, depth, budget, &any) &&
				       wb_opt_local_const_before(o, b.start, l, value, have, depth+1, budget);
			case wbInstr_Else:
				return wb_opt_block_exits_const(o, b.then_of, b.start, l, value, have, depth, budget, &any) &&
				       wb_opt_block_exits_const(o, blk, j, l, value, have, depth, budget, &any) && any;
			default:
				return false;
			}
		}
		case wbInstr_If:
			continue; // out of the branch `k` is in, to what came before the `if`
		case wbInstr_Else:
			j = o->blocks[o->blocks[o->block_of[j]].then_of].start; // skips the other branch
			continue;
		case wbInstr_Loop: {
			if (j == o->assume_at) {
				for (i32 i = 0; i < o->assume_count; i++) {
					if (o->assume_local[i] != l) continue;
					if (*have && *value != o->assume_value[i]) return false;
					*have = true;
					*value = o->assume_value[i];
					return true;
				}
			}
			// out of the loop `k` is in, when nothing in it writes the local
			if (wb_opt_loop_writes_local(o, o->block_of[j], l, budget)) return false;
			continue;
		}
		default:
			continue;
		}
	}
	return false;
}
gb_internal bool wb_opt_local_is_const(wbOpt *o, i32 k, u32 l, u32 *value) {
	bool have = false;
	i32 budget = WB_OPT_CONST_SCAN_BUDGET;
	bool ok = wb_opt_local_const_before(o, k, l, value, &have, 0, &budget);
	wb_opt_memo_reset(o);
	return ok;
}
gb_internal bool wb_opt_eval_i32(wbOpt *o, i32 start, i32 end, i32 at, u32 *result) {
	i32 budget = WB_OPT_CONST_SCAN_BUDGET;
	bool ok = wb_opt_eval_i32(o, start, end, at, result, 0, &budget);
	wb_opt_memo_reset(o);
	return ok;
}
// Evaluates a binary `i32` operation on constants (not the trapping ones)
gb_internal bool wb_opt_fold_i32(wbOp op, u32 a, u32 c, u32 *r) {
	i32 sa = cast(i32)a, sc = cast(i32)c;
	switch (op) {
	case wbOp_i32_add:   *r = a + c;  break;
	case wbOp_i32_sub:   *r = a - c;  break;
	case wbOp_i32_mul:   *r = a * c;  break;
	case wbOp_i32_and:   *r = a & c;  break;
	case wbOp_i32_or:    *r = a | c;  break;
	case wbOp_i32_xor:   *r = a ^ c;  break;
	case wbOp_i32_shl:   *r = a << (c & 31); break;
	case wbOp_i32_shr_u: *r = a >> (c & 31); break;
	case wbOp_i32_shr_s: *r = cast(u32)(sa >> (c & 31)); break;
	case wbOp_i32_div_u: if (c == 0) return false; *r = a / c; break;
	case wbOp_i32_rem_u: if (c == 0) return false; *r = a % c; break;
	case wbOp_i32_eq:    *r = a == c;   break;
	case wbOp_i32_ne:    *r = a != c;   break;
	case wbOp_i32_lt_s:  *r = sa < sc;  break;
	case wbOp_i32_lt_u:  *r = a < c;    break;
	case wbOp_i32_gt_s:  *r = sa > sc;  break;
	case wbOp_i32_gt_u:  *r = a > c;    break;
	case wbOp_i32_le_s:  *r = sa <= sc; break;
	case wbOp_i32_le_u:  *r = a <= c;   break;
	case wbOp_i32_ge_s:  *r = sa >= sc; break;
	case wbOp_i32_ge_u:  *r = a >= c;   break;
	default: return false;
	}
	return true;
}

// Evaluates the `i32` expression in [start, end) when it is made of
// constants, locals known to hold constants at `at` and foldable operations
gb_internal bool wb_opt_local_const_before(wbOpt *o, i32 k, u32 l, u32 *value, bool *have, int depth, i32 *budget);

gb_internal bool wb_opt_eval_i32(wbOpt *o, i32 start, i32 end, i32 at, u32 *result, int depth, i32 *budget) {
	u32 stack[16];
	i32 sp = 0;
	if (depth > 64) return false;
	for (i32 j = start; j < end; j++) {
		if (--*budget < 0) return false;
		if (o->deleted[j]) continue;
		if (o->replaced[j] || o->moved[j]) return false;
		wbInstr const &in = o->in[j];
		u32 c = 0;
		bool have = false;
		switch (in.kind) {
		case wbInstr_Const:
			if (in.op != wbOp_i32_const || sp == gb_count_of(stack)) return false;
			stack[sp++] = in.imm;
			break;
		case wbInstr_Local:
			if (in.op != wbOp_local_get || sp == gb_count_of(stack) || !wb_opt_local_const_before(o, at, in.imm, &c, &have, depth+1, budget)) return false;
			stack[sp++] = c;
			break;
		case wbInstr_Other:
			if (in.op == wbOp_i32_eqz) {
				if (sp < 1) return false;
				stack[sp-1] = stack[sp-1] == 0;
			} else if (in.op == wbOp_select) {
				if (sp < 3) return false;
				stack[sp-3] = stack[sp-1] != 0 ? stack[sp-3] : stack[sp-2];
				sp -= 2;
			} else {
				if (sp < 2 || !wb_opt_fold_i32(cast(wbOp)in.op, stack[sp-2], stack[sp-1], &c)) return false;
				stack[sp-2] = c;
				sp--;
			}
			break;
		default:
			return false;
		}
	}
	if (sp != 1) return false;
	*result = stack[0];
	return true;
}
#define WB_OPT_CSE_SCAN_BUDGET 512

// Whether [a, b] is a pure expression (of local reads, constants, loads and
// operators) that can be compared to another textually
gb_internal bool wb_opt_range_is_expr(wbOpt *o, i32 a, i32 b) {
	for (i32 j = a; j <= b; j++) {
		if (o->deleted[j] || o->replaced[j] || o->moved[j]) return false;
		wbInstr const &x = o->in[j];
		switch (x.kind) {
		case wbInstr_Local:
			if (x.op != wbOp_local_get || o->moved_to[x.imm] == j) return false;
			break;
		case wbInstr_Const: case wbInstr_Load:
			break;
		case wbInstr_Other:
			if (x.op == wbOp_memory_size || x.op == wbOp_br_table) return false;
			break;
		default:
			return false;
		}
	}
	return true;
}
gb_internal bool wb_opt_ranges_equal(wbOpt *o, i32 a, i32 b, i32 len) {
	for (i32 i = 0; i < len; i++) {
		wbInstr const &x = o->in[a+i];
		wbInstr const &y = o->in[b+i];
		if (x.kind != y.kind || x.op != y.op || x.imm != y.imm || x.length != y.length || x.width != y.width) return false;
		if (x.length > 1 && gb_memcompare(wb_opt_instr_bytes(o, x), wb_opt_instr_bytes(o, y), x.length) != 0) return false;
	}
	return true;
}

// Searches back from the load at `k2` for an earlier load of the same
// address expression whose value it can reuse: nothing in between may write
// memory or the locals the expression reads, and the earlier load must be on
// every path to `k2`, so completed blocks are skipped (checking that they
// don't write memory) and the search continues through the openers of the
// enclosing blocks; a loop entered in between is fine when nothing in it
// writes memory or those locals. Returns the load, and in `hi` the end of
// the range the locals must be unwritten in.
gb_internal i32 wb_opt_cse_find_load(wbOpt *o, i32 k2, i32 *hi_out) {
	i32 s2 = o->producer[k2];
	if (s2 < 0 || s2 >= k2 || !wb_opt_range_is_expr(o, s2, k2)) return -1;
	wbInstr const &in2 = o->in[k2];
	i32 len = k2 - s2 + 1;
	i32 hi = k2;
	i32 budget = WB_OPT_CSE_SCAN_BUDGET;
	i32 j = s2 - 1;
	while (j >= 0 && budget-- > 0) {
		if (o->deleted[j]) { j--; continue; }
		wbInstr const &x = o->in[j];
		switch (x.kind) {
		case wbInstr_End: {
			// a block completed before k2: skip it
			i32 nb = o->block_of[j];
			i32 st = o->blocks[nb].start;
			if (o->blocks[nb].then_of >= 0) st = o->blocks[o->blocks[nb].then_of].start;
			if (st < 0 || o->memw_prefix[j+1] - o->memw_prefix[st] != 0) return -1;
			if (o->blocks[nb].then_of < 0 && o->in[st].kind != wbInstr_If) {
				// a block or loop: its straight-line prefix, up to the first
				// branch or nested block, runs on every path through it
				i32 pre = st + 1;
				while (pre < j && budget-- > 0) {
					u8 kind = o->in[pre].kind;
					if (!o->deleted[pre] && (kind == wbInstr_Br || kind == wbInstr_BrIf || kind == wbInstr_Return || (kind == wbInstr_Other && o->in[pre].op == wbOp_br_table) ||
					    kind == wbInstr_Unreachable || kind == wbInstr_Block || kind == wbInstr_Loop || kind == wbInstr_If)) break;
					pre++;
				}
				j = pre - 1;
				continue;
			}
			j = st - 1;
			continue;
		}
		case wbInstr_Else: {
			// k2 is in the else branch: the then branch never ran
			i32 tb = o->blocks[o->block_of[j]].then_of;
			if (tb < 0) return -1;
			j = o->blocks[tb].start - 1;
			continue;
		}
		case wbInstr_Loop: {
			// an enclosing loop: later iterations must find the value unchanged
			i32 e = o->blocks[o->block_of[j]].end;
			if (e < 0 || o->memw_prefix[e+1] - o->memw_prefix[j] != 0) return -1;
			hi = gb_max(hi, e);
			j--;
			continue;
		}
		case wbInstr_Block: case wbInstr_If:
			j--;
			continue;
		case wbInstr_Load:
			if (x.op == in2.op && x.imm == in2.imm && x.width == in2.width && o->producer[j] >= 0 && j - o->producer[j] + 1 == len &&
			    !o->moved[j] && (!o->replaced[j] || o->cse_local[j] >= 0) &&
			    wb_opt_range_is_expr(o, o->producer[j], j-1) && wb_opt_ranges_equal(o, o->producer[j], s2, len)) {
				bool ok = true;
				for (i32 i = o->producer[j]; i < j && ok; i++) {
					wbInstr const &g = o->in[i];
					if (g.kind == wbInstr_Local && wb_opt_local_written_between(o, g.imm, j, hi)) ok = false;
				}
				if (ok) {
					*hi_out = hi;
					return j;
				}
			}
			break;
		default:
			if (wb_opt_writes_memory(x)) return -1;
			break;
		}
		j--;
	}
	return -1;
}

gb_internal bool wb_opt_is_op(wbOpt *o, i32 k, wbInstrKind kind, wbOp op) {
	return wb_opt_live(o, k) && o->in[k].kind == kind && o->in[k].op == op;
}
// The bit pattern of any constant instruction
gb_internal bool wb_opt_const_bits_of(wbOpt *o, i32 k, u64 *bits) {
	if (!wb_opt_live(o, k) || o->in[k].kind != wbInstr_Const) return false;
	wbInstr const &in = o->in[k];
	if (in.op == wbOp_f32_const || in.op == wbOp_f64_const) {
		u8 const *b = wb_opt_instr_bytes(o, in);
		u64 v = 0;
		for (isize i = 1; i < in.length; i++) v |= cast(u64)b[i] << (8*(i-1));
		*bits = v;
		return true;
	}
	i64 v = 0;
	if (!wb_opt_const_int(o, in, &v)) return false;
	*bits = in.op == wbOp_i32_const ? cast(u64)cast(u32)v : cast(u64)v;
	return true;
}
gb_internal bool wb_opt_is_i32_const(wbOpt *o, i32 k, u32 *value) {
	if (!wb_opt_is_op(o, k, wbInstr_Const, wbOp_i32_const)) return false;
	*value = o->in[k].imm;
	return true;
}

// The previous live instruction before k, -1 if none
gb_internal i32 wb_opt_prev_live(wbOpt *o, i32 k) {
	i32 j = k-1;
	while (j >= 0 && o->deleted[j]) j--;
	return wb_opt_live(o, j) ? j : -1;
}
gb_internal i32 wb_opt_next_live(wbOpt *o, i32 k) {
	i32 j = k+1;
	while (j < o->n && o->deleted[j]) j++;
	return wb_opt_live(o, j) ? j : -1;
}

// The exits of the (non-loop) block b: the branches to it (and, for the
// else branch of an `if`, to the then branch and its fallthrough at the
// `else`), and the fallthrough at its `end`. False when a br_if targets it
// or something in it is not as the simulation saw it.
gb_internal bool wb_opt_block_exits(wbOpt *o, i32 b, Array<i32> *exits) {
	wbOptBlock const &blk = o->blocks[b];
	i32 start = blk.start, end = blk.end, then_b = -1, then_start = -1;
	if (blk.is_loop || start < 0 || end < 0 || !wb_opt_live(o, start) || !wb_opt_live(o, end)) return false;
	if (o->in[start].kind == wbInstr_Else) {
		then_b = blk.then_of;
		then_start = o->blocks[then_b].start;
		if (!wb_opt_live(o, then_start)) return false;
		array_add(exits, start);
	}
	i32 first = then_start >= 0 ? then_start : start;
	for (i32 i = first+1; i < end; i++) {
		if (o->deleted[i]) continue;
		if (o->in[i].kind == wbInstr_BrIf) {
			i32 t = wb_opt_br_target(o, i);
			if (t == b || t == then_b) return false;
		} else if (o->in[i].kind == wbInstr_Br) {
			i32 t = wb_opt_br_target(o, i);
			if (t != b && t != then_b) continue;
			if (!wb_opt_live(o, i)) return false;
			array_add(exits, i);
		} else if (o->in[i].kind == wbInstr_Other && o->in[i].op == wbOp_br_table) {
			return false;
		}
	}
	array_add(exits, end);
	return true;
}

// The type byte of the block b (of the `if` for an else branch)
gb_internal u32 wb_opt_block_type(wbOpt *o, i32 b) {
	i32 st = o->blocks[b].start;
	if (st < 0) return 0x40;
	if (o->in[st].kind == wbInstr_Else) st = o->blocks[o->blocks[b].then_of].start;
	return o->in[st].imm;
}
// Whether the instruction at k yields 0 or 1
gb_internal bool wb_opt_is_boolean(wbOpt *o, i32 k) {
	wbInstr const &x = o->in[k];
	if (x.kind != wbInstr_Other) return false;
	return x.op == wbOp_i32_eqz || x.op == wbOp_i64_eqz || (x.op >= wbOp_i32_eq && x.op <= wbOp_f64_ge);
}

// The comparison that computes the opposite of `op`, or 0
gb_internal wbOp wb_opt_inverse_compare(u8 op) {
	switch (op) {
	case wbOp_i32_eq:   return wbOp_i32_ne;
	case wbOp_i32_ne:   return wbOp_i32_eq;
	case wbOp_i32_lt_s: return wbOp_i32_ge_s;
	case wbOp_i32_lt_u: return wbOp_i32_ge_u;
	case wbOp_i32_gt_s: return wbOp_i32_le_s;
	case wbOp_i32_gt_u: return wbOp_i32_le_u;
	case wbOp_i32_le_s: return wbOp_i32_gt_s;
	case wbOp_i32_le_u: return wbOp_i32_gt_u;
	case wbOp_i32_ge_s: return wbOp_i32_lt_s;
	case wbOp_i32_ge_u: return wbOp_i32_lt_u;
	case wbOp_i64_eq:   return wbOp_i64_ne;
	case wbOp_i64_ne:   return wbOp_i64_eq;
	case wbOp_i64_lt_s: return wbOp_i64_ge_s;
	case wbOp_i64_lt_u: return wbOp_i64_ge_u;
	case wbOp_i64_gt_s: return wbOp_i64_le_s;
	case wbOp_i64_gt_u: return wbOp_i64_le_u;
	case wbOp_i64_le_s: return wbOp_i64_gt_s;
	case wbOp_i64_le_u: return wbOp_i64_gt_u;
	case wbOp_i64_ge_s: return wbOp_i64_lt_s;
	case wbOp_i64_ge_u: return wbOp_i64_lt_u;
	default: return cast(wbOp)0;
	}
}

gb_internal void wb_opt_peephole(wbOpt *o) {
	for (i32 k = 0; k < o->n; k++) {
		if (!wb_opt_live(o, k)) continue;
		wbInstr const &in = o->in[k];
		u32 c = 0;

		// A read of a local known to hold a constant
		if (in.kind == wbInstr_Local && in.op == wbOp_local_get && in.imm != o->p->fp_local && o->moved_to[in.imm] != k &&
		    wb_opt_local_is_const(o, k, in.imm, &c)) {
			wb_opt_replace(o, k, wb_opt_i32_const(o, c));
			o->changed = true;
			continue;
		}

		// A load from constant data: the value itself
		if (in.kind == wbInstr_Load && o->producer[k] == k-1 && wb_opt_is_i32_const(o, k-1, &c)) {
			u8 const *bytes = wb_const_data_bytes(o->p->module, c + in.imm, in.width);
			if (bytes) {
				u64 bits = 0;
				for (u32 i = 0; i < in.width; i++) bits |= cast(u64)bytes[i] << (8*i);
				if (wb_opt_access_ext(in.op) == 1 && in.width < 8 && (bits >> (8*in.width - 1)) & 1) {
					bits |= ~cast(u64)0 << (8*in.width);
				}
				wbValType vt = wb_opt_access_valtype(in.op);
				if (vt == wbValType_i32) bits &= 0xffffffff;
				o->deleted[k-1] = true;
				wb_opt_replace(o, k, wb_opt_const_bits(o, vt, bits));
				o->changed = true;
				continue;
			}
		}

		// A load of the same address as an earlier one, with nothing in between
		// that could change the value: the earlier load keeps its value in a
		// local, which is read instead
		if (in.kind == wbInstr_Load && o->producer[k] >= 0 && o->producer[k] < k) {
			i32 hi = k;
			i32 k1 = wb_opt_cse_find_load(o, k, &hi);
			if (k1 >= 0) {
				u32 t;
				if (o->cse_local[k1] >= 0) {
					t = cast(u32)o->cse_local[k1];
				} else {
					t = wb_add_local(o->p, wb_opt_access_valtype(in.op), {});
					wb_opt_replace(o, k1, wb_opt_memarg(o, cast(wbOp)in.op, in.imm, in.width));
					// a set and a get rather than a tee: loads of what
					// the value addresses stay comparable
					wb_opt_replace(o, k1, wb_opt_local_instr(wbOp_local_set, t));
					wb_opt_replace(o, k1, wb_opt_local_instr(wbOp_local_get, t));
					o->cse_local[k1] = cast(i32)t;
				}
				wb_opt_delete_range(o, o->producer[k], k);
				wb_opt_replace(o, k, wb_opt_local_instr(wbOp_local_get, t));
				o->changed = true;
				continue;
			}
		}

		// A loop whose exit test is decided by locals that hold constants on
		// entry: `for width != 0 { ... }` with the width known, or the loops
		// of copies specialized on their arguments. When the test exits at
		// once the loop goes; otherwise its first iteration is peeled off:
		// the peeled iteration becomes a block in the loop's place, which the
		// branches to the loop then leave, so the body is copied as it is,
		// and the loop follows. That needs the body to end in a branch back
		// to the loop (falling out of the block would enter the loop instead
		// of leaving it). The peeled iteration, computing on constants,
		// generally decides the test of the next one.
		if (in.kind == wbInstr_Loop && in.imm == 0x40) {
			i32 b = o->block_of[k];
			i32 end = o->blocks[b].end;
			if (end < 0 || !wb_opt_range_untouched(o, k, end+1)) continue;
			// the exit test
			i32 j = k+1;
			while (j < end && (o->deleted[j] || o->in[j].kind != wbInstr_BrIf)) {
				if (!o->deleted[j] && o->in[j].kind != wbInstr_Const && o->in[j].kind != wbInstr_Other &&
				    !(o->in[j].kind == wbInstr_Local && o->in[j].op == wbOp_local_get)) break;
				j++;
			}
			if (j == end || o->in[j].kind != wbInstr_BrIf || o->block_of[j] != b || o->in[j].imm == 0) continue;
			if (!wb_opt_eval_i32(o, k+1, j, k, &c)) continue;
			if (c != 0) {
				// never entered: the loop becomes the branch out
				wb_opt_delete_range(o, k, end+1);
				o->deleted[k] = false;
				wbInstr br = wb_opt_scratch_begin(o, wbInstr_Br, wbOp_br, 0, 0);
				br.imm = o->in[j].imm - 1;
				wb_uleb(&o->scratch, br.imm);
				wb_opt_scratch_end(o, &br);
				wb_opt_replace(o, k, br);
				o->changed = true;
				continue;
			}
			if ((in.flags & wbInstrFlag_Peeled) || end - k > WB_OPT_PEEL_MAX_INSTRS || o->p->instrs.count >= 8192) continue;
			i32 last = end-1;
			while (last > k && o->deleted[last]) last--;
			if (last == k || o->in[last].kind != wbInstr_Br || o->block_of[last] != b || o->in[last].imm != 0) continue;
			// Only worth it when the first iteration is also the last: the
			// test is evaluated again at the branch back, with what the
			// locals held on entry assumed at the loop
			o->assume_at = k;
			o->assume_count = 0;
			for (i32 i = k+1; i < j && o->assume_count < gb_count_of(o->assume_local); i++) {
				if (o->deleted[i] || o->in[i].kind != wbInstr_Local) continue;
				u32 l = o->in[i].imm;
				bool seen = false;
				for (i32 a = 0; a < o->assume_count; a++) {
					if (o->assume_local[a] == l) seen = true;
				}
				if (seen || !wb_opt_local_is_const(o, k, l, &c)) continue;
				o->assume_local[o->assume_count] = l;
				o->assume_value[o->assume_count] = c;
				o->assume_count++;
			}
			bool exits = wb_opt_eval_i32(o, k+1, j, last, &c) && c != 0;
			o->assume_at = -1;
			o->assume_count = 0;
			if (!exits) continue;
			wbInstr block = wb_opt_scratch_begin(o, wbInstr_Block, wbOp_block, 0, 0);
			block.imm = 0x40;
			wb_byte(&o->scratch, 0x40);
			wb_opt_scratch_end(o, &block);
			wb_opt_replace(o, k, block);
			for (i32 i = k+1; i < end; i++) {
				if (o->deleted[i]) continue;
				wbInstr copy = o->in[i];
				u8 const *bytes = wb_opt_instr_bytes(o, copy);
				copy.offset = cast(u32)o->scratch.data.count;
				copy.flags |= wbInstrFlag_Scratch;
				wb_bytes(&o->scratch, bytes, copy.length);
				if (copy.kind == wbInstr_Call) {
					copy.imm = cast(u32)o->p->call_relocs.count;
					wbCallReloc r = {-1, o->p->call_relocs[o->in[i].imm].target};
					array_add(&o->p->call_relocs, r);
				}
				wb_opt_replace(o, k, copy);
			}
			wbInstr close = wb_opt_scratch_begin(o, wbInstr_End, wbOp_end, 0, 0);
			wb_opt_scratch_end(o, &close);
			wb_opt_replace(o, k, close);
			wbInstr loop = in;
			loop.flags |= wbInstrFlag_Peeled;
			wb_opt_replace(o, k, loop);
			o->changed = true;
			continue;
		}

		// `local.set x; local.get x` with no other access of x
		if (in.kind == wbInstr_Local && in.op == wbOp_local_get && wb_opt_is_op(o, k-1, wbInstr_Local, wbOp_local_set) && o->in[k-1].imm == in.imm &&
		    in.imm >= o->p->param_count && in.imm != o->p->fp_local && in.imm != o->p->old_sp_local && in.imm < cast(u32)o->gets.count &&
		    o->sets[in.imm] == 1 && o->gets[in.imm] == 1 && o->tees[in.imm] == 0 && o->moved_to[in.imm] != k) {
			o->deleted[k-1] = true;
			o->deleted[k] = true;
			o->changed = true;
			continue;
		}

		// A comparison (or other operator) with a constant applied to the
		// result of a block that only produces constants: the ordering an
		// inlined comparator returns, tested with `>= .Equal`. The operator
		// moves into the block, to each exit, where it folds.
		u32 dummy = 0;
		if (in.kind == wbInstr_Other && (in.op == wbOp_i32_eqz || (wb_opt_is_i32_const(o, k-1, &c) && wb_opt_fold_i32(cast(wbOp)in.op, 0, c, &dummy)))) {
			bool unary = in.op == wbOp_i32_eqz;
			u32 cc = unary ? 0 : o->in[k-1].imm;
			i32 v = wb_opt_prev_live(o, unary ? k : k-1);
			// possibly through a local set and read just here
			if (v >= 1 && wb_opt_is_op(o, v, wbInstr_Local, wbOp_local_get) && wb_opt_is_op(o, v-1, wbInstr_Local, wbOp_local_set) && o->in[v-1].imm == o->in[v].imm) {
				u32 l = o->in[v].imm;
				if (l < cast(u32)o->gets.count && o->sets[l] == 1 && o->gets[l] == 1 && o->tees[l] == 0 && o->moved_to[l] != v) {
					v = wb_opt_prev_live(o, v-1);
				} else {
					v = -1;
				}
			}
			i32 b = v >= 0 ? o->block_of[v] : -1;
			auto exits = array_make<i32>(temporary_allocator(), 0, 8);
			bool ok = v >= 0 && o->in[v].kind == wbInstr_End && o->blocks[b].arity == 1 && wb_opt_block_type(o, b) == 0x7f &&
			          wb_opt_block_exits(o, b, &exits);
			// the value at each exit (none when the fallthrough is dead)
			auto values = array_make<i32>(temporary_allocator(), 0, 8);
			for_array(i, exits) {
				if (!ok) break;
				i32 e = exits[i];
				i32 q = wb_opt_prev_live(o, e);
				if (q >= 0 && o->in[e].kind != wbInstr_Br && (o->in[q].kind == wbInstr_Br || o->in[q].kind == wbInstr_Return || o->in[q].kind == wbInstr_Unreachable)) continue;
				ok = q >= 0 && wb_opt_is_i32_const(o, q, &c) && o->height[q] == o->blocks[b].height;
				array_add(&values, q);
			}
			if (ok) {
				for_array(i, values) {
					i32 q = values[i];
					u32 r = 0;
					if (unary) r = o->in[q].imm == 0; else wb_opt_fold_i32(cast(wbOp)in.op, o->in[q].imm, cc, &r);
					wb_opt_replace(o, q, wb_opt_i32_const(o, r));
				}
				if (!unary) o->deleted[k-1] = true;
				o->deleted[k] = true;
				o->changed = true;
				continue;
			}
		}

		// A branch out of a block with the constant the block's end would
		// produce anyway, nothing else happening in between
		if (in.kind == wbInstr_Br && wb_opt_is_i32_const(o, k-1, &c)) {
			i32 b = wb_opt_br_target(o, k);
			if (b > 0 && !o->blocks[b].is_loop && o->blocks[b].arity == 1 && o->height[k-1] == o->blocks[b].height) {
				i32 j = wb_opt_next_live(o, k);
				while (j >= 0 && o->in[j].kind == wbInstr_End && o->block_of[j] != b) j = wb_opt_next_live(o, j);
				u32 c2 = 0;
				if (j >= 0 && wb_opt_is_i32_const(o, j, &c2) && c2 == c) {
					i32 e = wb_opt_next_live(o, j);
					if (e >= 0 && o->in[e].kind == wbInstr_End && o->block_of[e] == b) {
						o->deleted[k-1] = true;
						o->deleted[k] = true;
						o->changed = true;
						continue;
					}
				}
			}
		}

		// An `if` with nothing in it: the condition is dropped
		if (in.kind == wbInstr_If && in.imm == 0x40) {
			i32 b = o->block_of[k];
			i32 e1 = o->blocks[b].end;
			if (e1 >= 0 && wb_opt_live(o, e1)) {
				i32 e2 = e1;
				bool empty = wb_opt_next_live(o, k) == e1;
				if (empty && o->in[e1].kind == wbInstr_Else) {
					e2 = o->blocks[o->block_of[e1]].end;
					empty = e2 >= 0 && wb_opt_live(o, e2) && wb_opt_next_live(o, e1) == e2;
				}
				if (empty) {
					o->deleted[k] = true;
					o->deleted[e1] = true;
					o->deleted[e2] = true;
					i32 q = wb_opt_prev_live(o, k);
					if (q >= 0 && o->producer[q] >= 0 && wb_opt_range_untouched(o, o->producer[q], q+1)) {
						wb_opt_delete_range(o, o->producer[q], q+1);
					} else {
						wbInstr drop = wb_opt_scratch_begin(o, wbInstr_Other, wbOp_drop, 1, 0);
						wb_opt_scratch_end(o, &drop);
						wb_opt_replace(o, k, drop);
					}
					o->changed = true;
					continue;
				}
			}
			// an `else` with nothing in it
			if (e1 >= 0 && o->in[e1].kind == wbInstr_Else && wb_opt_live(o, e1)) {
				i32 e2 = o->blocks[o->block_of[e1]].end;
				if (e2 >= 0 && wb_opt_live(o, e2) && wb_opt_next_live(o, e1) == e2) {
					o->deleted[e1] = true;
					o->changed = true;
					continue;
				}
			}
		}

		// `block i32 { c; if { K1; br 1 } K2 }` -> `K1; K2; c; select` (or
		// just `c`, or its inverse, for 1/0 and 0/1)
		if (in.kind == wbInstr_Block && in.imm == 0x7f) {
			i32 b = o->block_of[k];
			i32 end = o->blocks[b].end;
			i32 f = -1;
			// the `if` is the first instruction after the condition
			for (i32 i = k+1; i < end; i++) {
				if (o->deleted[i]) continue;
				if (o->in[i].kind == wbInstr_If && o->blocks[o->block_of[i]].parent == b) { f = i; break; }
			}
			i32 q = f >= 0 ? wb_opt_prev_live(o, f) : -1;
			i32 pq = q >= 0 ? o->producer[q] : -1;
			if (pq < 0 || o->in[f].imm != 0x40 || !wb_opt_range_untouched(o, k, end+1) || o->height[pq] != o->blocks[b].height) continue;
			// what precedes the condition in the block (stores of what it
			// reads, say) simply runs before it
			bool prefix_ok = true;
			for (i32 i = k+1; i < pq && prefix_ok; i++) {
				if (o->deleted[i]) continue;
				switch (o->in[i].kind) {
				case wbInstr_Block: case wbInstr_Loop: case wbInstr_If: case wbInstr_Else: case wbInstr_End:
				case wbInstr_Br: case wbInstr_BrIf: case wbInstr_Return: case wbInstr_Unreachable:
					prefix_ok = false;
					break;
				case wbInstr_Other:
					if (o->in[i].op == wbOp_br_table) prefix_ok = false;
					break;
				default:
					break;
				}
			}
			if (!prefix_ok) continue;
			i32 fb = o->block_of[f];
			i32 fe = o->blocks[fb].end;
			if (fe < 0 || o->in[fe].kind != wbInstr_End) continue;
			i32 k1 = wb_opt_next_live(o, f);
			i32 br = k1 >= 0 ? wb_opt_next_live(o, k1) : -1;
			u32 c1 = 0, c2 = 0;
			if (br < 0 || !wb_opt_is_i32_const(o, k1, &c1) || o->in[br].kind != wbInstr_Br || wb_opt_br_target(o, br) != b || wb_opt_next_live(o, br) != fe) continue;
			i32 k2 = wb_opt_next_live(o, fe);
			if (k2 < 0 || !wb_opt_is_i32_const(o, k2, &c2) || wb_opt_next_live(o, k2) != end) continue;
			o->deleted[k] = true;
			wb_opt_delete_range(o, f, end+1);
			if (c1 == 1 && c2 == 0 && wb_opt_is_boolean(o, q)) {
				// the condition itself
			} else if (c1 == 0 && c2 == 1) {
				wbOp inv = wb_opt_inverse_compare(o->in[q].op);
				if (inv != 0 && o->in[q].kind == wbInstr_Other) {
					wbInstr cmp = wb_opt_scratch_begin(o, wbInstr_Other, inv, 2, 1);
					wb_opt_scratch_end(o, &cmp);
					wb_opt_replace(o, q, cmp);
				} else {
					wbInstr eqz = wb_opt_scratch_begin(o, wbInstr_Other, wbOp_i32_eqz, 1, 1);
					wb_opt_scratch_end(o, &eqz);
					o->deleted[f] = false;
					wb_opt_replace(o, f, eqz);
				}
			} else {
				wb_opt_replace(o, pq, wb_opt_i32_const(o, c1));
				wb_opt_replace(o, pq, wb_opt_i32_const(o, c2));
				wb_opt_replace(o, pq, wb_opt_copy_instr(o, pq));
				wbInstr sel = wb_opt_scratch_begin(o, wbInstr_Other, wbOp_select, 3, 1);
				wb_opt_scratch_end(o, &sel);
				o->deleted[f] = false;
				wb_opt_replace(o, f, sel);
			}
			o->changed = true;
			continue;
		}

		// A comparison negated: the opposite comparison
		if (in.kind == wbInstr_Other && in.op == wbOp_i32_eqz) {
			i32 j = wb_opt_prev_live(o, k);
			wbOp inv = j >= 0 && o->in[j].kind == wbInstr_Other ? wb_opt_inverse_compare(o->in[j].op) : cast(wbOp)0;
			if (inv != 0) {
				wbInstr cmp = wb_opt_scratch_begin(o, wbInstr_Other, inv, 2, 1);
				wb_opt_scratch_end(o, &cmp);
				wb_opt_replace(o, j, cmp);
				o->deleted[k] = true;
				o->changed = true;
				continue;
			}
		}

		// An `if` on a condition an enclosing `if` decided already (the
		// inlined bounds check tests `index < count` again, under
		// `index >= count`), nothing in between changing what it reads
		if (in.kind == wbInstr_If) {
			i32 q = wb_opt_prev_live(o, k);
			i32 pq = q >= 0 ? o->producer[q] : -1;
			if (pq >= 0 && wb_opt_range_is_expr(o, pq, q) && o->in[q].kind == wbInstr_Other) {
				i32 len = q - pq;
				i32 hi = k;
				i32 b = o->blocks[o->block_of[k]].parent;
				int known = -1;
				while (b > 0 && known < 0) {
					wbOptBlock const &blk = o->blocks[b];
					if (blk.is_loop && blk.end > hi) hi = blk.end;
					i32 st = blk.start;
					bool in_else = st >= 0 && o->in[st].kind == wbInstr_Else;
					if (in_else) st = o->blocks[blk.then_of].start;
					if (st >= 0 && o->in[st].kind == wbInstr_If) {
						i32 eq = wb_opt_prev_live(o, st);
						i32 epq = eq >= 0 ? o->producer[eq] : -1;
						if (epq >= 0 && eq - epq == len && wb_opt_range_live(o, epq, eq) && o->in[eq].kind == wbInstr_Other &&
						    wb_opt_ranges_equal(o, epq, pq, len)) {
							int value = -1;
							if (o->in[eq].op == o->in[q].op) value = in_else ? 0 : 1;
							else if (wb_opt_inverse_compare(o->in[eq].op) == o->in[q].op) value = in_else ? 1 : 0;
							if (value >= 0) {
								bool ok = o->memw_prefix[hi] - o->memw_prefix[st] == 0;
								for (i32 i = pq; i <= q && ok; i++) {
									if (o->in[i].kind == wbInstr_Local && wb_opt_local_written_between(o, o->in[i].imm, st, hi)) ok = false;
								}
								if (ok) known = value;
							}
						}
					}
					b = blk.parent;
				}
				if (known >= 0) {
					wb_opt_delete_range(o, pq, q);
					wb_opt_replace(o, q, wb_opt_i32_const(o, cast(u32)known));
					o->changed = true;
					continue;
				}
			}
		}

		// Float arithmetic with an identity constant (the ones and zeros of
		// a constant matrix): `x * 1`, `x / 1`, `x - 0`, `x + -0` are x, and
		// `x * -1` is `-x` (all exact); the negation of a constant is folded
		if (in.kind == wbInstr_Other && in.op >= wbOp_f32_neg && in.op <= wbOp_f64_div && o->producer[k] >= 0) {
			bool f64 = in.op >= wbOp_f64_neg;
			u64 bits = 0;
			u64 one = f64 ? 0x3ff0000000000000ull : 0x3f800000ull;
			u64 neg_one = one | (f64 ? 0x8000000000000000ull : 0x80000000ull);
			u64 neg_zero = f64 ? 0x8000000000000000ull : 0x80000000ull;
			u8 op = in.op - (f64 ? wbOp_f64_neg : wbOp_f32_neg) + wbOp_f32_neg;
			if (op == wbOp_f32_neg && wb_opt_const_bits_of(o, k-1, &bits)) {
				o->deleted[k-1] = true;
				wb_opt_replace(o, k, wb_opt_const_bits(o, f64 ? wbValType_f64 : wbValType_f32, bits ^ neg_zero));
				o->changed = true;
				continue;
			}
			if (in.pops == 2 && wb_opt_const_bits_of(o, k-1, &bits) && o->op0_end[k] == k-1) {
				bool identity = (op == wbOp_f32_mul && bits == one) || (op == wbOp_f32_div && bits == one) ||
				                (op == wbOp_f32_sub && bits == 0) || (op == wbOp_f32_add && bits == neg_zero);
				if (identity) {
					o->deleted[k-1] = true;
					o->deleted[k] = true;
					o->changed = true;
					continue;
				}
				if ((op == wbOp_f32_mul || op == wbOp_f32_div) && bits == neg_one) {
					o->deleted[k-1] = true;
					wbInstr n = wb_opt_scratch_begin(o, wbInstr_Other, f64 ? wbOp_f64_neg : wbOp_f32_neg, 1, 1);
					wb_opt_scratch_end(o, &n);
					wb_opt_replace(o, k, n);
					o->changed = true;
					continue;
				}
			}
			if (in.pops == 2 && (op == wbOp_f32_mul || op == wbOp_f32_add) && o->op0_start[k] >= 0 && o->op0_end[k] == o->op0_start[k]+1 &&
			    wb_opt_const_bits_of(o, o->op0_start[k], &bits) && wb_opt_range_untouched(o, o->op0_end[k], k)) {
				if ((op == wbOp_f32_mul && bits == one) || (op == wbOp_f32_add && bits == neg_zero)) {
					o->deleted[o->op0_start[k]] = true;
					o->deleted[k] = true;
					o->changed = true;
					continue;
				}
				if (op == wbOp_f32_mul && bits == neg_one) {
					o->deleted[o->op0_start[k]] = true;
					wbInstr n = wb_opt_scratch_begin(o, wbInstr_Other, f64 ? wbOp_f64_neg : wbOp_f32_neg, 1, 1);
					wb_opt_scratch_end(o, &n);
					wb_opt_replace(o, k, n);
					o->changed = true;
					continue;
				}
			}
		}

		// A dropped pure value
		if (in.kind == wbInstr_Other && in.op == wbOp_drop) {
			i32 j = k-1;
			while (j >= 0 && o->deleted[j]) j--;
			if (j < 0 || !wb_opt_live(o, j)) continue;
			wbInstr const &v = o->in[j];
			if (v.pushes == 1 && v.pops == 0 && wb_opt_is_pure(v)) {
				o->deleted[j] = true;
				o->deleted[k] = true;
				o->changed = true;
				continue;
			}
			// The dropped result of a block (the wrapper of an inlined call,
			// typically): the block loses its result, which is dropped
			// inside instead, at each branch to it and at its end
			if (v.kind == wbInstr_End && o->blocks[o->block_of[j]].arity == 1) {
				i32 b = o->block_of[j];
				wbOptBlock const &blk = o->blocks[b];
				i32 start = blk.start, then_b = -1, then_start = -1;
				if (blk.is_loop || !wb_opt_live(o, start)) continue;
				if (o->in[start].kind == wbInstr_Else) {
					then_b = blk.then_of;
					then_start = o->blocks[then_b].start;
					if (!wb_opt_live(o, then_start)) continue;
				}
				i32 first = then_start >= 0 ? then_start : start;
				bool ok = true;
				for (i32 i = first+1; i < j && ok; i++) {
					if (o->deleted[i]) continue;
					if (o->in[i].kind == wbInstr_BrIf) {
						i32 t = wb_opt_br_target(o, i);
						if (t == b || t == then_b) ok = false;
					} else if (o->in[i].kind == wbInstr_Br) {
						i32 t = wb_opt_br_target(o, i);
						if ((t == b || t == then_b) && !wb_opt_live(o, i)) ok = false;
					}
				}
				if (!ok) continue;
				wbInstr drop = wb_opt_scratch_begin(o, wbInstr_Other, wbOp_drop, 1, 0);
				wb_opt_scratch_end(o, &drop);
				for (i32 i = first+1; i < j; i++) {
					if (o->deleted[i] || o->in[i].kind != wbInstr_Br) continue;
					i32 t = wb_opt_br_target(o, i);
					if (t != b && t != then_b) continue;
					wb_opt_replace(o, i, drop);
					wb_opt_replace(o, i, wb_opt_copy_instr(o, i));
				}
				wbInstr open = wb_opt_scratch_begin(o, o->in[first].kind, cast(wbOp)o->in[first].op, o->in[first].pops, 0);
				open.imm = 0x40;
				wb_byte(&o->scratch, 0x40);
				wb_opt_scratch_end(o, &open);
				wb_opt_replace(o, first, open);
				if (then_start >= 0) {
					wb_opt_replace(o, start, drop);
					wb_opt_replace(o, start, wb_opt_copy_instr(o, start));
				}
				wb_opt_replace(o, j, drop);
				wb_opt_replace(o, j, wb_opt_copy_instr(o, j));
				o->deleted[k] = true;
				o->changed = true;
				continue;
			}
			continue;
		}

		// `x & M` narrowed by a store, or of a value the load already narrowed
		if (in.kind == wbInstr_Store && (in.op == wbOp_i32_store8 || in.op == wbOp_i32_store16) &&
		    wb_opt_is_op(o, k-1, wbInstr_Other, wbOp_i32_and) && wb_opt_is_i32_const(o, k-2, &c)) {
			u32 mask = in.op == wbOp_i32_store8 ? 0xff : 0xffff;
			if ((c & mask) == mask) {
				wb_opt_delete_range(o, k-2, k);
				o->changed = true;
			}
			continue;
		}
		if (in.kind == wbInstr_Other && in.op == wbOp_i32_and && wb_opt_is_i32_const(o, k-1, &c) && wb_opt_live(o, k-2)) {
			wbInstr const &v = o->in[k-2];
			u32 have = 0; // bits the value may have set
			if (v.kind == wbInstr_Load && v.op == wbOp_i32_load8_u)  have = 0xff;
			if (v.kind == wbInstr_Load && v.op == wbOp_i32_load16_u) have = 0xffff;
			if (v.kind == wbInstr_Other && v.op == wbOp_i32_and && wb_opt_is_i32_const(o, k-3, &have)) {}
			if (have != 0 && (have & ~c) == 0) {
				wb_opt_delete_range(o, k-1, k+1);
				o->changed = true;
			}
			continue;
		}

		// `x + 0`, `x - 0`, `x | 0`, `x ^ 0`, `x << 0`, `x >> 0`, `x * 1`
		if (in.kind == wbInstr_Other && wb_opt_is_i32_const(o, k-1, &c)) {
			bool identity = false;
			switch (in.op) {
			case wbOp_i32_add: case wbOp_i32_sub: case wbOp_i32_or: case wbOp_i32_xor:
			case wbOp_i32_shl: case wbOp_i32_shr_s: case wbOp_i32_shr_u:
				identity = c == 0;
				break;
			case wbOp_i32_mul:
				identity = c == 1;
				break;
			}
			if (identity) {
				wb_opt_delete_range(o, k-1, k+1);
				o->changed = true;
				continue;
			}
		}
		// `0 + x`, `0 | x`, `0 ^ x`, `1 * x`
		if (in.kind == wbInstr_Other && (in.op == wbOp_i32_add || in.op == wbOp_i32_or || in.op == wbOp_i32_xor || in.op == wbOp_i32_mul) &&
		    wb_opt_live(o, k-1) && o->producer[k-1] >= 1 && wb_opt_range_live(o, o->producer[k-1], k)) {
			i32 j = o->producer[k-1] - 1;
			if (wb_opt_is_i32_const(o, j, &c) && c == (in.op == wbOp_i32_mul ? 1u : 0u)) {
				o->deleted[j] = true;
				o->deleted[k] = true;
				o->changed = true;
				continue;
			}
		}

		// `a + (0 - x)` -> `a - x`
		if (in.kind == wbInstr_Other && in.op == wbOp_i32_add && wb_opt_is_op(o, k-1, wbInstr_Other, wbOp_i32_sub)) {
			i32 j = o->producer[k-1];
			if (j >= 0 && j < k-2 && wb_opt_is_i32_const(o, j, &c) && c == 0 && o->producer[k-2] == j+1 && wb_opt_range_live(o, j, k)) {
				o->deleted[j] = true;
				o->deleted[k-1] = true;
				wbInstr sub = wb_opt_scratch_begin(o, wbInstr_Other, wbOp_i32_sub, 2, 1);
				wb_opt_scratch_end(o, &sub);
				wb_opt_replace(o, k, sub);
				o->changed = true;
			}
			continue;
		}

		// A call through a constant procedure value
		if (in.kind == wbInstr_CallIndirect && wb_opt_is_i32_const(o, k-1, &c)) {
			wbModule *m = o->p->module;
			wbProcedure *t = c < cast(u32)m->table.count ? m->table[c] : nullptr;
			if (t != nullptr && t->type_index == in.imm && t->alias == nullptr && !t->failed) {
				o->deleted[k-1] = true;
				wbInstr call = wb_opt_scratch_begin(o, wbInstr_Call, wbOp_call, cast(i8)(in.pops-1), in.pushes);
				call.imm = cast(u32)o->p->call_relocs.count;
				wbCallReloc r = {-1, t};
				array_add(&o->p->call_relocs, r);
				wb_uleb_fixed5(&o->scratch, 0);
				wb_opt_scratch_end(o, &call);
				wb_opt_replace(o, k, call);
				o->changed = true;
			}
			continue;
		}

		// Constant folding of `i32` arithmetic and comparisons: mostly
		// what a specialized copy of a procedure has been handed
		if (in.kind == wbInstr_Other && in.op == wbOp_i32_eqz && wb_opt_is_i32_const(o, k-1, &c)) {
			o->deleted[k-1] = true;
			wb_opt_replace(o, k, wb_opt_i32_const(o, c == 0 ? 1 : 0));
			o->changed = true;
			continue;
		}
		if (in.kind == wbInstr_Other && in.pops == 2 && in.pushes == 1 && wb_opt_is_i32_const(o, k-1, &c)) {
			u32 a = 0;
			if (wb_opt_is_i32_const(o, k-2, &a)) {
				u32 r = 0;
				bool folded = wb_opt_fold_i32(cast(wbOp)in.op, a, c, &r);
				if (folded) {
					o->deleted[k-2] = true;
					o->deleted[k-1] = true;
					wb_opt_replace(o, k, wb_opt_i32_const(o, r));
					o->changed = true;
					continue;
				}
			}
		}

		// `select` on a constant: the operand not chosen is dropped when it
		// has no side effects
		if (in.kind == wbInstr_Other && in.op == wbOp_select && wb_opt_is_i32_const(o, k-1, &c)) {
			i32 bs = o->producer[k-2];
			if (bs < 1 || !wb_opt_range_live(o, bs, k-2)) continue;
			i32 as = o->producer[bs-1];
			if (as < 0 || !wb_opt_range_live(o, as, bs-1)) continue;
			i32 ds = c != 0 ? bs : as, de = c != 0 ? k-1 : bs;
			if (o->impure_prefix[de] != o->impure_prefix[ds]) continue;
			wb_opt_delete_range(o, ds, de);
			o->deleted[k-1] = true;
			o->deleted[k]   = true;
			o->changed = true;
			continue;
		}

		// `if` on a constant: the branch taken becomes a block (so that the
		// labels branches inside it count stay right), the other one goes
		if (in.kind == wbInstr_If && wb_opt_is_i32_const(o, k-1, &c)) {
			i32 b    = o->block_of[k];
			i32 end1 = o->blocks[b].end;
			if (end1 < 0) continue;
			bool has_else = o->in[end1].kind == wbInstr_Else;
			i32 end2 = has_else ? o->blocks[o->block_of[end1]].end : end1;
			if (end2 < 0 || !wb_opt_range_untouched(o, k-1, end2+1)) continue;
			wbInstr block = wb_opt_scratch_begin(o, wbInstr_Block, wbOp_block, 0, 0);
			block.imm = in.imm;
			wb_byte(&o->scratch, cast(u8)in.imm);
			wb_opt_scratch_end(o, &block);
			if (c != 0) {
				o->deleted[k-1] = true;
				wb_opt_replace(o, k, block);
				if (has_else) wb_opt_delete_range(o, end1, end2);
			} else if (has_else) {
				wb_opt_delete_range(o, k-1, end1);
				wb_opt_replace(o, end1, block);
			} else {
				wb_opt_delete_range(o, k-1, end1+1);
			}
			o->changed = true;
			continue;
		}

		// `memory.copy` of a small constant size (the width of an element in a
		// specialized sort, typically): engines call out to a runtime routine
		// for it, so loads and stores are much cheaper. The loads all happen
		// before the stores, as the regions may overlap.
		if (in.kind == wbInstr_MemCopy && wb_opt_is_i32_const(o, k-1, &c) && c > 0 && c <= 16) {
			wbProcedure *p = o->p;
			u32 d = wb_add_local(p, wbValType_i32, {});
			u32 s = wb_add_local(p, wbValType_i32, {});
			u32 temps[16], offsets[16], widths[16];
			i32 count = 0;
			for (u32 done = 0; done < c; ) {
				u32 rem = c - done;
				u32 w = rem >= 8 ? 8 : rem >= 4 ? 4 : rem >= 2 ? 2 : 1;
				temps[count]   = wb_add_local(p, w == 8 ? wbValType_i64 : wbValType_i32, {});
				offsets[count] = done;
				widths[count]  = w;
				count++;
				done += w;
			}
			o->deleted[k-1] = true;
			wb_opt_replace(o, k, wb_opt_local_instr(wbOp_local_set, s));
			wb_opt_replace(o, k, wb_opt_local_instr(wbOp_local_set, d));
			for (i32 i = 0; i < count; i++) {
				wbOp op = widths[i] == 8 ? wbOp_i64_load : widths[i] == 4 ? wbOp_i32_load : widths[i] == 2 ? wbOp_i32_load16_u : wbOp_i32_load8_u;
				wb_opt_replace(o, k, wb_opt_local_instr(wbOp_local_get, s));
				wb_opt_replace(o, k, wb_opt_memarg(o, op, offsets[i], widths[i]));
				wb_opt_replace(o, k, wb_opt_local_instr(wbOp_local_set, temps[i]));
			}
			for (i32 i = 0; i < count; i++) {
				wbOp op = widths[i] == 8 ? wbOp_i64_store : widths[i] == 4 ? wbOp_i32_store : widths[i] == 2 ? wbOp_i32_store16 : wbOp_i32_store8;
				wb_opt_replace(o, k, wb_opt_local_instr(wbOp_local_get, d));
				wb_opt_replace(o, k, wb_opt_local_instr(wbOp_local_get, temps[i]));
				wb_opt_replace(o, k, wb_opt_memarg(o, op, offsets[i], widths[i]));
			}
			o->changed = true;
			continue;
		}

		// Branches on constants: `for {}` conditions and the like
		if (in.kind == wbInstr_BrIf) {
			i32 j = k-1;
			bool negate = false;
			if (wb_opt_is_op(o, j, wbInstr_Other, wbOp_i32_eqz)) { negate = true; j--; }
			if (!wb_opt_is_i32_const(o, j, &c)) continue;
			bool taken = (c != 0) != negate;
			wb_opt_delete_range(o, j, k);
			if (taken) {
				wbInstr br = wb_opt_scratch_begin(o, wbInstr_Br, wbOp_br, 0, 0);
				wb_uleb(&o->scratch, in.imm);
				br.imm = in.imm;
				wb_opt_scratch_end(o, &br);
				wb_opt_replace(o, k, br);
			} else {
				o->deleted[k] = true;
			}
			o->changed = true;
			continue;
		}

		// A branch to the end of the block it is in
		if (in.kind == wbInstr_Br && in.imm == 0) {
			i32 b = o->block_of[k];
			wbOptBlock const &blk = o->blocks[b];
			if (blk.is_loop || b == 0 || blk.end < 0) continue;
			if (o->height[k] != blk.height + blk.arity) continue;
			i32 next = k+1;
			while (next < blk.end && o->deleted[next]) next++;
			if (next != blk.end) continue;
			o->deleted[k] = true;
			o->changed = true;
			continue;
		}
	}
}

// Emission of the rewritten instruction list
gb_internal void wb_opt_emit_range(wbOpt *o, Array<wbInstr> *out, i32 start, i32 end, int depth) {
	GB_ASSERT(depth < 64);
	for (i32 k = start; k < end; k++) {
		wbInstr const &in = o->in[k];
		if (o->deleted[k] || (depth == 0 && o->moved[k])) continue;
		if (in.kind == wbInstr_Local && in.op == wbOp_local_get && o->moved_to[in.imm] == k) {
			u32 l = in.imm;
			wb_opt_emit_range(o, out, o->moved_start[l], o->single_set[l], depth+1);
			continue;
		}
		if (o->replaced[k]) {
			for (wbInstr const &r : o->replacement[k]) {
				array_add(out, r);
			}
			continue;
		}
		array_add(out, in);
	}
}

gb_internal void wb_opt_dump(wbProcedure *p, char const *title) {
	gb_printf_err("== %s %.*s (%td instrs, %td locals, frame %u)\n", title, LIT(p->name), p->instrs.count, p->locals.count, p->frame_size);
	for_array(i, p->instrs) {
		wbInstr const &in = p->instrs[i];
		gb_printf_err("  %4td: op=%02x kind=%d pops=%d pushes=%d imm=%u len=%u\n", i, in.op, in.kind, in.pops, in.pushes, in.imm, in.length);
	}
}

// One rewriting pass; returns true when something changed
gb_internal bool wb_opt_pass(wbProcedure *p, bool final_pass) {
	gbAllocator ta = temporary_allocator();
	wbOpt opt = {};
	wbOpt *o = &opt;
	o->p  = p;
	o->assume_at = -1;
	o->in = p->instrs;
	o->n  = p->instrs.count;
	wb_buffer_init(&o->scratch, ta, 256);
	o->memo_state   = array_make<u8>(ta, o->n);
	o->memo_local   = array_make<u32>(ta, o->n);
	o->memo_value   = array_make<u32>(ta, o->n);
	o->memo_touched = array_make<i32>(ta, 0, 64);
	for (isize k = 0; k < o->n; k++) o->memo_state[k] = 0;
	o->cse_local = array_make<i32>(ta, o->n);
	for (isize k = 0; k < o->n; k++) o->cse_local[k] = -1;

	o->slots = array_make<wbOptSlot>(ta, p->slots.count);
	for_array(i, o->slots) {
		o->slots[i] = {};
		o->slots[i].accesses = array_make<wbOptAccess>(ta, 0, 8);
	}
	o->replacement = array_make<Array<wbInstr>>(ta, o->n);
	o->replaced    = array_make<bool>(ta, o->n);
	for (isize k = 0; k < o->n; k++) {
		o->replaced[k] = false;
	}

	wb_opt_simulate(o);
	for (isize k = 0; k < o->n; k++) {
		if (o->deleted[k]) o->changed = true;
	}
	// Propagating copies changes the use counts the other phases rely on,
	// so those wait for the next pass
	if (wb_opt_propagate_copies(o)) {
		o->changed = true;
	} else if (wb_opt_pair_sets(o)) {
		o->changed = true;
	} else {
		wb_opt_promote_slots(o);
		wb_opt_move_producers(o);
		wb_opt_remove_overwritten_sets(o);
		wb_opt_remove_dead_sets(o);
		if (p->merge_copies) {
			wb_opt_merge_copies(o);
		}
		wb_opt_peephole(o);
	}

	if (!o->changed && !final_pass) {
		return false;
	}

	auto out = array_make<wbInstr>(p->module->allocator, 0, o->n);
	wb_opt_emit_range(o, &out, 0, cast(i32)o->n, 0);

	// Without any frame access the frame (and the epilogue) can go
	u32 fp_refs = 0;
	for (wbInstr const &in : out) {
		if (in.kind == wbInstr_Local && in.op == wbOp_local_get && in.imm == p->fp_local) fp_refs++;
	}
	if (fp_refs == 0) {
		p->frame_size = 0;
	}
	if (p->frame_size == 0 && !p->uses_alloca) {
		isize w = 0;
		for_array(i, out) {
			if (out[i].kind == wbInstr_EpilogueGet || out[i].kind == wbInstr_EpilogueSet) continue;
			out[w++] = out[i];
		}
		out.count = w;
	}

	// Regenerate the byte code
	wbBuffer code = {};
	wb_buffer_init(&code, p->module->allocator, p->code.data.count + 16);
	for_array(i, p->call_relocs) {
		p->call_relocs[i].offset = -1; // deleted unless a call remains
	}
	for_array(i, out) {
		wbInstr &in = out[i];
		u32 offset = cast(u32)code.data.count;
		switch (in.kind) {
		case wbInstr_Local:
			wb_byte(&code, in.op);
			wb_uleb(&code, in.imm);
			break;
		case wbInstr_EpilogueGet:
			wb_byte(&code, in.op);
			wb_uleb_fixed5(&code, in.imm);
			break;
		default: {
			u8 const *src = (in.flags & wbInstrFlag_Scratch) ? o->scratch.data.data : p->code.data.data;
			wb_bytes(&code, src + in.offset, in.length);
			if (in.kind == wbInstr_Call) {
				p->call_relocs[in.imm].offset = offset + 1;
			}
			break;
		}
		}
		in.offset = offset;
		in.length = cast(u8)(code.data.count - offset);
		in.flags &= ~wbInstrFlag_Scratch;
	}
	array_free(&p->code.data);
	array_free(&p->instrs);
	p->code = code;
	p->instrs = out;
	return true;
}

// Removes the locals without any use and renumbers the rest (params keep
// their indices)
gb_internal void wb_opt_renumber_locals(wbProcedure *p) {
	gbAllocator ta = temporary_allocator();
	isize local_count = p->locals.count;
	auto used = array_make<bool>(ta, local_count);
	for (isize i = 0; i < local_count; i++) {
		used[i] = i < p->param_count;
	}
	for (wbInstr const &in : p->instrs) {
		if (in.kind == wbInstr_Local || in.kind == wbInstr_EpilogueGet) {
			used[in.imm] = true;
		}
	}
	if ((p->frame_size > 0 || p->uses_alloca) && p->old_sp_local != WB_NO_LOCAL) {
		used[p->old_sp_local] = true;
	}
	if (p->frame_size > 0 && p->fp_local != WB_NO_LOCAL) {
		used[p->fp_local] = true;
	}
	auto new_index = array_make<u32>(ta, local_count);
	u32 next = 0;
	bool any_dropped = false;
	for (isize i = 0; i < local_count; i++) {
		if (used[i]) {
			new_index[i] = next++;
		} else {
			new_index[i] = ~0u;
			any_dropped = true;
		}
	}
	if (!any_dropped) {
		return;
	}
	for (wbInstr &in : p->instrs) {
		if (in.kind == wbInstr_Local || in.kind == wbInstr_EpilogueGet) {
			in.imm = new_index[in.imm];
		}
	}
	auto locals = array_make<wbLocal>(p->module->allocator, 0, next);
	for (isize i = 0; i < local_count; i++) {
		if (used[i]) {
			array_add(&locals, p->locals[i]);
		}
	}
	array_free(&p->locals);
	p->locals = locals;
	p->fp_local     = p->fp_local     != WB_NO_LOCAL ? new_index[p->fp_local]     : WB_NO_LOCAL;
	p->old_sp_local = p->old_sp_local != WB_NO_LOCAL ? new_index[p->old_sp_local] : WB_NO_LOCAL;
	// context and sret are parameters, which keep their indices

	// Re-encode the local instructions
	wbBuffer code = {};
	wb_buffer_init(&code, p->module->allocator, p->code.data.count);
	for (wbInstr &in : p->instrs) {
		u32 offset = cast(u32)code.data.count;
		switch (in.kind) {
		case wbInstr_Local:
			wb_byte(&code, in.op);
			wb_uleb(&code, in.imm);
			break;
		case wbInstr_EpilogueGet:
			wb_byte(&code, in.op);
			wb_uleb_fixed5(&code, in.imm);
			break;
		default:
			wb_bytes(&code, p->code.data.data + in.offset, in.length);
			if (in.kind == wbInstr_Call) {
				p->call_relocs[in.imm].offset = offset + 1;
			}
			break;
		}
		in.offset = offset;
		in.length = cast(u8)(code.data.count - offset);
	}
	array_free(&p->code.data);
	p->code = code;
}

gb_internal void wb_optimize_procedure(wbProcedure *p) {
	if (p->is_raw_body || p->failed || p->instrs.count == 0) {
		return;
	}
	if (build_context.optimization_level < 0) {
		return;
	}
	TEMPORARY_ALLOCATOR_GUARD();

	bool debug = gb_get_env("ODIN_WB_OPT_DEBUG", temporary_allocator()) != nullptr;
	if (debug) wb_opt_dump(p, "before");
	// The first pass always regenerates the code: it drops the epilogues of
	// procedures without a frame
	for (int pass = 0; pass < WB_OPT_MAX_PASSES; pass++) {
		if (!wb_opt_pass(p, pass == 0)) {
			break;
		}
		if (debug) wb_opt_dump(p, "pass");
	}
	wb_opt_renumber_locals(p);
	if (debug) wb_opt_dump(p, "after");
}

// Inlining
//
// Runs once all procedures are lowered and optimized. Small callees are
// copied into their callers at the wasm level: the arguments are stored to
// fresh locals, the callee's locals get new indices, its frame becomes a
// region of the caller's frame and its returns become branches to a block
// wrapping the body. The caller is optimized again afterwards, which
// promotes the callee's frame slots and parameter copies to locals.

gb_internal void wb_link_mark_object_refs(wbModule *m);

#define WB_INLINE_MAX_INSTRS       48   // callee size, in instructions
#define WB_INLINE_MAX_IN_LOOP      96   // for calls inside a loop of the caller
#define WB_INLINE_MAX_FORCED       512  // for #force_inline
#define WB_INLINE_MAX_SINGLE       512  // callee with a single call site (nothing else can reach it)
#define WB_INLINE_MAX_GROWTH       4096 // instructions added per caller
#define WB_INLINE_MAX_FRAME        1024 // bytes of callee frame

gb_internal bool wb_inline_is_cold(wbProcedure *c) {
	return c->entity != nullptr && (c->entity->flags & EntityFlag_Cold) != 0;
}

gb_internal bool wb_inline_candidate(wbProcedure *caller, wbProcedure *c, bool in_loop) {
	if (c == caller || c->is_raw_body || c->failed || c->is_foreign || c->alias != nullptr) {
		return false;
	}
	if (c->instrs.count == 0 || c->uses_alloca) {
		return false;
	}
	switch (c->gen) {
	case wbProcGen_Body:
		if (c->body == nullptr) return false;
		break;
	case wbProcGen_Hasher:
	case wbProcGen_Equal:
	case wbProcGen_MapGet:
	case wbProcGen_MapSet:
		break;
	default:
		return false;
	}
	if (c->frame_size > WB_INLINE_MAX_FRAME) {
		return false;
	}
	// Error paths stay out of line: cold procedures and the runtime checks
	// the backend calls once its own inline test has failed
	if (wb_inline_is_cold(c) || c->never_inline) {
		return false;
	}
	isize limit = in_loop ? WB_INLINE_MAX_IN_LOOP : WB_INLINE_MAX_INSTRS;
	if (c->call_sites == 1 && c->gen == wbProcGen_Body && !c->is_export && !c->object_ref && c->table_index == 0) {
		// The body moves rather than gets duplicated
		limit = WB_INLINE_MAX_SINGLE;
	}
	if (c->entity != nullptr && c->entity->kind == Entity_Procedure && c->entity->decl_info != nullptr &&
	    c->entity->decl_info->proc_lit != nullptr && c->entity->decl_info->proc_lit->kind == Ast_ProcLit) {
		switch (c->entity->decl_info->proc_lit->ProcLit.inlining) {
		case ProcInlining_inline:    limit = WB_INLINE_MAX_FORCED; break;
		case ProcInlining_no_inline: return false;
		default: break;
		}
	}
	return c->instrs.count <= limit;
}

// Appends an instruction of another stream unchanged
gb_internal void wb_inline_copy_instr(wbProcedure *p, wbInstr in, wbBuffer const &code) {
	u32 offset = in.offset;
	in.offset = cast(u32)p->code.data.count;
	in.flags &= ~wbInstrFlag_Scratch;
	array_add(&p->instrs, in);
	wb_bytes(&p->code, code.data.data + offset, in.length);
}

gb_internal void wb_inline_body(wbProcedure *p, wbProcedure *c) {
	gbAllocator ta = temporary_allocator();

	// The callee frame becomes a region of the caller frame
	u32 region = 0;
	if (c->frame_size > 0) {
		region = (p->frame_size + 15) & ~15u;
		p->frame_size = region + c->frame_size;
		for (wbFrameSlot slot : c->slots) {
			slot.offset += region;
			array_add(&p->slots, slot);
		}
	}

	auto local_map = array_make<u32>(ta, c->locals.count);
	for_array(i, c->locals) {
		local_map[i] = WB_NO_LOCAL;
		if (i == c->fp_local || i == c->old_sp_local) {
			continue;
		}
		local_map[i] = wb_add_local(p, c->locals[i].vt, c->locals[i].name);
	}

	// The arguments are on the operand stack
	for (isize i = cast(isize)c->param_count-1; i >= 0; i--) {
		wb_local_set(p, local_map[i]);
	}

	wbFuncType const &ft = p->module->types[c->type_index];
	u32 wrapper = wb_open_block(p, ft.results.count == 0 ? 0x40 : cast(u8)ft.results[0]);
	for (wbInstr const &in : c->instrs) {
		switch (in.kind) {
		case wbInstr_EpilogueGet:
		case wbInstr_EpilogueSet:
			break;
		case wbInstr_Return:
			wb_br(p, wrapper);
			break;
		case wbInstr_Block: wb_open_block(p, cast(u8)in.imm); break;
		case wbInstr_Loop:  wb_open_loop(p, cast(u8)in.imm);  break;
		case wbInstr_If:    wb_open_if(p, cast(u8)in.imm);    break;
		case wbInstr_Else:  wb_else(p);  break;
		case wbInstr_End:   wb_close(p); break;
		case wbInstr_Local:
			if (in.imm == c->fp_local) {
				GB_ASSERT(in.op == wbOp_local_get);
				wb_local_get(p, p->fp_local);
				if (region != 0) {
					wb_i32_const(p, cast(i32)region);
					wb_op(p, wbOp_i32_add);
				}
			} else {
				GB_ASSERT(local_map[in.imm] != WB_NO_LOCAL);
				wb_local_op(p, cast(wbOp)in.op, local_map[in.imm]);
			}
			break;
		case wbInstr_Call:
			wb_call(p, c->call_relocs[in.imm].target);
			break;
		default:
			wb_inline_copy_instr(p, in, c->code);
			break;
		}
	}
	wb_close(p);
}

// Inlines the eligible calls of `p`. Returns true when the body changed.
gb_internal bool wb_inline_calls(wbProcedure *p) {
	if (p->is_raw_body || p->failed || p->instrs.count == 0 || p->body == nullptr) {
		return false;
	}
	TEMPORARY_ALLOCATOR_GUARD();

	// Decide up front, to know whether the caller needs a frame
	isize growth = 0;
	bool any = false, need_frame = p->frame_size > 0;
	auto inline_at = array_make<bool>(temporary_allocator(), p->instrs.count);
	auto is_loop = array_make<bool>(temporary_allocator(), 0, 32); // the open blocks
	for_array(k, p->instrs) {
		wbInstr const &in = p->instrs[k];
		inline_at[k] = false;
		switch (in.kind) {
		case wbInstr_Block: case wbInstr_If: array_add(&is_loop, false); continue;
		case wbInstr_Loop:                   array_add(&is_loop, true);  continue;
		case wbInstr_End:                    if (is_loop.count > 0) array_pop(&is_loop); continue;
		default: break;
		}
		if (in.kind != wbInstr_Call) {
			continue;
		}
		bool in_loop = false;
		for (bool l : is_loop) in_loop |= l;
		wbProcedure *c = p->call_relocs[in.imm].target;
		if (!wb_inline_candidate(p, c, in_loop) || growth + c->instrs.count > WB_INLINE_MAX_GROWTH) {
			continue;
		}
		inline_at[k] = true;
		growth += c->instrs.count;
		any = true;
		if (c->frame_size > 0) {
			need_frame = true;
		}
	}
	if (!any) {
		return false;
	}

	bool had_frame = p->frame_size > 0;
	if (!had_frame) {
		// The optimizer dropped the frame (and with it the epilogues and the
		// fp/old_sp locals) of this procedure
		p->slots.count = 0;
		if (need_frame) {
			if (p->old_sp_local == WB_NO_LOCAL) {
				p->old_sp_local = wb_add_local(p, wbValType_i32, str_lit("old_sp"));
			}
			p->fp_local = wb_add_local(p, wbValType_i32, str_lit("fp"));
		}
	}
	bool add_epilogues = need_frame && !had_frame && !p->uses_alloca;

	Array<wbInstr>     instrs = p->instrs;
	wbBuffer           code   = p->code;
	Array<wbCallReloc> relocs = p->call_relocs;
	array_init(&p->instrs, p->module->allocator, 0, instrs.count + growth);
	array_init(&p->call_relocs, p->module->allocator, 0, relocs.count + 8);
	wb_buffer_init(&p->code, p->module->allocator, code.data.count + growth*4);
	GB_ASSERT(p->depth == 0);

	for_array(k, instrs) {
		wbInstr const &in = instrs[k];
		switch (in.kind) {
		case wbInstr_Call:
			if (inline_at[k]) {
				wb_inline_body(p, relocs[in.imm].target);
			} else {
				wb_call(p, relocs[in.imm].target);
			}
			break;
		case wbInstr_Return:
			if (add_epilogues) {
				wb_emit_epilogue(p);
			}
			wb_inline_copy_instr(p, in, code);
			break;
		case wbInstr_Block: wb_open_block(p, cast(u8)in.imm); break;
		case wbInstr_Loop:  wb_open_loop(p, cast(u8)in.imm);  break;
		case wbInstr_If:    wb_open_if(p, cast(u8)in.imm);    break;
		case wbInstr_Else:  wb_else(p);  break;
		case wbInstr_End:   wb_close(p); break;
		default:
			wb_inline_copy_instr(p, in, code);
			break;
		}
	}
	if (add_epilogues) {
		wb_emit_epilogue(p);
	}
	GB_ASSERT(p->depth == 0);
	array_free(&instrs);
	array_free(&relocs);
	array_free(&code.data);
	return true;
}

// Specialization
//
// A procedure called with a constant procedure value (the comparator of a
// sort, a callback) for a parameter it only calls through, or passes on to
// another such parameter, gets a copy with that parameter replaced by the
// constant: the indirect calls become direct ones which can be inlined.
// Other constant integer arguments of the call are bound in the copy too.

#define WB_SPEC_MAX_INSTRS 2048 // callee size
#define WB_SPEC_MAX_COPIES 512  // per module

// The instruction ranges computing the `count` values on top of the stack
// before instruction `k`, in order; false when they cannot be told apart
// (control flow in between)
gb_internal bool wb_spec_arg_starts(wbProcedure *p, isize k, i32 count, i32 *starts) {
	i32 need = 1;
	i32 arg = count-1;
	for (isize j = k-1; j >= 0 && arg >= 0; j--) {
		wbInstr const &in = p->instrs[j];
		switch (in.kind) {
		case wbInstr_Block: case wbInstr_Loop: case wbInstr_If: case wbInstr_Else: case wbInstr_End:
		case wbInstr_Br: case wbInstr_BrIf: case wbInstr_Return: case wbInstr_Unreachable:
			return false;
		default:
			break;
		}
		if (in.pushes > 1) {
			return false;
		}
		need += in.pops - in.pushes;
		if (need == 0) {
			starts[arg--] = cast(i32)j;
			need = 1;
		} else if (need < 0) {
			return false;
		}
	}
	return arg < 0;
}

gb_internal bool wb_spec_candidate(wbProcedure *c) {
	return c->gen == wbProcGen_Body && c->body != nullptr && !c->is_raw_body && !c->failed && !c->is_foreign &&
	       c->alias == nullptr && !c->uses_alloca && c->instrs.count > 0 && c->instrs.count <= WB_SPEC_MAX_INSTRS &&
	       c->param_count <= 64;
}

gb_internal u64 wb_spec_callback_mask(wbProcedure *c) {
	if (c->spec_state == 2) return c->spec_mask;
	if (c->spec_state == 1) return 0; // recursion: the parameters found so far
	c->spec_state = 1;
	u64 mask = 0;
	if (wb_spec_candidate(c)) {
		i32 starts[64];
		for_array(k, c->instrs) {
			wbInstr const &in = c->instrs[k];
			if (in.kind == wbInstr_CallIndirect && k > 0) {
				wbInstr const &f = c->instrs[k-1];
				if (f.kind == wbInstr_Local && f.op == wbOp_local_get && f.imm < c->param_count) {
					mask |= cast(u64)1 << f.imm;
				}
			} else if (in.kind == wbInstr_Call) {
				wbProcedure *t = c->call_relocs[in.imm].target;
				if (t == nullptr || t == c || t->param_count > 64) continue;
				u64 tmask = wb_spec_callback_mask(t);
				if (tmask == 0 || !wb_spec_arg_starts(c, k, cast(i32)t->param_count, starts)) continue;
				for (u32 j = 0; j < t->param_count; j++) {
					if ((tmask & (cast(u64)1 << j)) == 0) continue;
					i32 s = starts[j];
					i32 e = j+1 < t->param_count ? starts[j+1] : cast(i32)k;
					if (e != s+1) continue;
					wbInstr const &a = c->instrs[s];
					if (a.kind == wbInstr_Local && a.op == wbOp_local_get && a.imm < c->param_count) {
						mask |= cast(u64)1 << a.imm;
					}
				}
			}
		}
		// Parameters that are written cannot be replaced by a constant
		for (wbInstr const &in : c->instrs) {
			if (in.kind == wbInstr_Local && in.op != wbOp_local_get && in.imm < c->param_count) {
				mask &= ~(cast(u64)1 << in.imm);
			}
		}
	}
	c->spec_mask = mask;
	c->spec_state = 2;
	return mask;
}

// Whether procedure `c` loads through its parameter `j`, directly or in a
// procedure it passes the parameter on to
gb_internal bool wb_spec_loads_through(wbProcedure *c, u32 j, int depth) {
	if (depth > 3 || !wb_spec_candidate(c)) return false;
	i32 starts[64];
	for (isize k = 0; k < c->instrs.count; k++) {
		wbInstr const &in = c->instrs[k];
		if (in.kind == wbInstr_Local && in.op == wbOp_local_get && in.imm == j) {
			if (k+1 < c->instrs.count && c->instrs[k+1].kind == wbInstr_Load) return true;
		} else if (in.kind == wbInstr_Call) {
			wbProcedure *t = c->call_relocs[in.imm].target;
			if (t == nullptr || t == c || t->param_count > 64 || !wb_spec_arg_starts(c, k, cast(i32)t->param_count, starts)) continue;
			for (u32 a = 0; a < t->param_count; a++) {
				i32 s = starts[a];
				i32 e = a+1 < t->param_count ? starts[a+1] : cast(i32)k;
				if (e != s+1) continue;
				wbInstr const &g = c->instrs[s];
				if (g.kind == wbInstr_Local && g.op == wbOp_local_get && g.imm == j && wb_spec_loads_through(t, a, depth+1)) return true;
			}
		}
	}
	return false;
}

struct wbSpecCopy {
	wbProcedure *proc;   // the original
	wbProcedure *copy;
	u64          bound;  // parameters bound to constants
	u32          values[64];
};

gb_internal wbProcedure *wb_spec_copy(wbModule *m, Array<wbSpecCopy> *copies, wbProcedure *c, u64 bound, u32 const *values) {
	for (wbSpecCopy const &sc : *copies) {
		if (sc.proc != c || sc.bound != bound) continue;
		bool same = true;
		for (u32 j = 0; j < c->param_count && same; j++) {
			if ((bound & (cast(u64)1 << j)) && sc.values[j] != values[j]) same = false;
		}
		if (same) return sc.copy;
	}
	if (copies->count >= WB_SPEC_MAX_COPIES) {
		return nullptr;
	}
	char name_text[512];
	isize name_len = gb_snprintf(name_text, gb_size_of(name_text), "%.*s$spec-%d", LIT(c->name), cast(int)copies->count+1);
	String name = copy_string(m->allocator, make_string(cast(u8 const *)name_text, name_len-1));
	wbProcedure *p = wb_alloc_procedure(m, name);
	p->entity        = c->entity;
	p->type          = c->type;
	p->body          = c->body;
	p->type_index    = c->type_index;
	p->gen           = wbProcGen_Body;
	p->is_spec       = true;
	p->param_count   = c->param_count;
	p->context_local = c->context_local;
	p->sret_local    = c->sret_local;
	p->fp_local      = c->fp_local;
	p->old_sp_local  = c->old_sp_local;
	p->frame_size    = c->frame_size;
	p->state_flags   = c->state_flags;
	for (wbFrameSlot slot : c->slots) array_add(&p->slots, slot);
	for (wbLocal l : c->locals)       array_add(&p->locals, l);
	for (wbValType vt : c->results)   array_add(&p->results, vt);

	GB_ASSERT(p->depth == 0);
	for (wbInstr const &in : c->instrs) {
		switch (in.kind) {
		case wbInstr_Local:
			if (in.op == wbOp_local_get && in.imm < 64 && (bound & (cast(u64)1 << in.imm))) {
				wb_i32_const(p, cast(i32)values[in.imm]);
				p->instrs[p->instrs.count-1].flags |= wbInstrFlag_SpecConst;
			} else {
				wb_inline_copy_instr(p, in, c->code);
			}
			break;
		case wbInstr_Call:
			wb_call(p, c->call_relocs[in.imm].target);
			break;
		case wbInstr_Block: wb_open_block(p, cast(u8)in.imm); break;
		case wbInstr_Loop:  wb_open_loop(p, cast(u8)in.imm);  break;
		case wbInstr_If:    wb_open_if(p, cast(u8)in.imm);    break;
		case wbInstr_Else:  wb_else(p);  break;
		case wbInstr_End:   wb_close(p); break;
		default:
			wb_inline_copy_instr(p, in, c->code);
			break;
		}
	}
	GB_ASSERT(p->depth == 0);
	wb_finish_procedure(p);

	wbSpecCopy sc = {c, p, bound};
	for (u32 j = 0; j < c->param_count; j++) sc.values[j] = values[j];
	array_add(copies, sc);
	array_add(&m->procedures, p);
	return p;
}

gb_internal void wb_specialize_procedures(wbModule *m) {
	gbAllocator ta = temporary_allocator();
	auto copies = array_make<wbSpecCopy>(ta, 0, 64);
	i32 starts[64];
	u32 values[64];
	// The copies are appended and get scanned in turn: their constants may
	// flow on into what they call
	for (isize i = 0; i < m->procedures.count; i++) {
		wbProcedure *p = m->procedures[i];
		if (p->is_raw_body || p->failed || p->is_foreign || p->instrs.count == 0) continue;
		for_array(k, p->instrs) {
			wbInstr const &in = p->instrs[k];
			if (in.kind != wbInstr_Call) continue;
			wbProcedure *c = p->call_relocs[in.imm].target;
			if (c == nullptr || c == p || !wb_spec_candidate(c)) continue;
			u64 cmask = wb_spec_callback_mask(c);
			if (!wb_spec_arg_starts(p, k, cast(i32)c->param_count, starts)) continue;
			u64 bound = 0;
			// A copy is made for a callback, or when a constant that was
			// substituted into this copy is passed on: it may matter just as
			// much further down (the width of the elements being sorted, say)
			bool worthwhile = false;
			for (u32 j = 0; j < c->param_count; j++) {
				i32 s = starts[j];
				i32 e = j+1 < c->param_count ? starts[j+1] : cast(i32)k;
				if (e != s+1) continue;
				wbInstr const &a = p->instrs[s];
				if (a.kind != wbInstr_Const || a.op != wbOp_i32_const) continue;
				if (cmask & (cast(u64)1 << j)) {
					if (a.imm == 0 || a.imm >= cast(u32)m->table.count) continue;
					worthwhile = true;
				} else {
					// Any other constant comes along, as long as the parameter is never written
					bool written = false;
					for (wbInstr const &ci : c->instrs) {
						if (ci.kind == wbInstr_Local && ci.op != wbOp_local_get && ci.imm == j) { written = true; break; }
					}
					if (written) continue;
					if (a.flags & wbInstrFlag_SpecConst) worthwhile = true;
					// A pointer to constant data (the info of a map) the
					// callee loads through: the loads fold in the copy
					if (!worthwhile && wb_const_data_bytes(m, a.imm, 1) != nullptr && wb_spec_loads_through(c, j, 0)) {
						worthwhile = true;
					}
				}
				bound |= cast(u64)1 << j;
				values[j] = a.imm;
			}
			if (!worthwhile) continue;
			wbProcedure *copy = wb_spec_copy(m, &copies, c, bound, values);
			if (copy != nullptr) {
				p->call_relocs[in.imm].target = copy;
			}
		}
	}
}

// Inlines calls in every procedure, callees before callers so that the
// copies made of a callee already contain its own inlined calls
gb_internal void wb_inline_procedures(wbModule *m) {
	if (build_context.optimization_level < 0 || gb_get_env("WB_NO_INLINE", temporary_allocator())) {
		return;
	}
	wb_specialize_procedures(m);
	gbAllocator ta = temporary_allocator();
	isize n = m->procedures.count;
	auto state = array_make<u8>(ta, n); // 0 unvisited, 1 on the stack, 2 done
	for (isize i = 0; i < n; i++) {
		state[i] = 0;
		m->procedures[i]->inline_order = cast(u32)i;
	}
	for (wbProcedure *p : m->procedures) {
		for (wbCallReloc const &r : p->call_relocs) {
			if (r.offset >= 0 && r.target != nullptr) {
				r.target->call_sites += 1;
			}
		}
	}
	wb_link_mark_object_refs(m);
	struct Frame { wbProcedure *p; isize next; };
	auto stack = array_make<Frame>(ta, 0, 64);
	for (isize root = 0; root < n; root++) {
		if (state[root] != 0) continue;
		state[root] = 1;
		Frame f = {m->procedures[root], 0};
		array_add(&stack, f);
		while (stack.count > 0) {
			Frame &top = stack[stack.count-1];
			wbProcedure *p = top.p;
			if (top.next < p->call_relocs.count) {
				wbCallReloc const &r = p->call_relocs[top.next++];
				wbProcedure *c = r.target;
				if (r.offset < 0 || c == nullptr || c->is_foreign || c->is_raw_body) {
					continue;
				}
				isize ci = c->inline_order;
				if (ci < n && m->procedures[ci] == c && state[ci] == 0) {
					state[ci] = 1;
					Frame g = {c, 0};
					array_add(&stack, g);
				}
				continue;
			}
			array_pop(&stack);
			state[p->inline_order] = 2;
			bool inlined = wb_inline_calls(p);
			p->merge_copies = true;
			if (inlined || wb_has_copy_runs(p)) {
				wb_finish_procedure(p);
			}
		}
	}
}

// Drops the procedures nothing reaches any more: inlining and
// specialization leave their originals without callers
gb_internal void wb_gc_procedures(wbModule *m) {
	gbAllocator ta = temporary_allocator();
	auto live = array_make<bool>(ta, m->procedures.count);
	auto worklist = array_make<wbProcedure *>(ta, 0, 256);
	for_array(i, m->procedures) {
		live[i] = false;
		m->procedures[i]->inline_order = cast(u32)i;
	}
	auto reach = [&](wbProcedure *p) {
		if (p == nullptr) return;
		if (p->alias != nullptr) p = p->alias;
		isize i = p->inline_order;
		if (i < m->procedures.count && m->procedures[i] == p && !live[i]) {
			live[i] = true;
			array_add(&worklist, p);
		}
	};
	for (wbProcedure *p : m->procedures) {
		if (p->is_export || p->object_ref || p->is_raw_body || p->gen != wbProcGen_Body || p->body == nullptr) {
			reach(p);
		}
	}
	for (wbProcedure *p : m->table)   reach(p);
	for (wbProcedure *p : m->aliased) reach(p->alias);
	reach(m->startup);
	reach(m->startup_runtime);
	while (worklist.count > 0) {
		wbProcedure *p = array_pop(&worklist);
		for (wbCallReloc const &r : p->call_relocs) {
			if (r.offset >= 0) reach(r.target);
		}
		for (wbProcedure *t : p->link_refs) reach(t);
	}
	isize kept = 0;
	for_array(i, m->procedures) {
		if (live[i]) {
			m->procedures[kept++] = m->procedures[i];
		}
	}
	m->procedures.count = kept;
}
