// A direct WebAssembly backend for the Odin compiler.
//
// This backend does not depend on LLVM or on an external linker: it lowers
// the checked AST straight to WebAssembly and writes a complete, self-linked
// `.wasm` module. It is enabled with `-backend:wasm` and only supports the
// wasm targets (`freestanding_wasm32`, `js_wasm32`, `wasi_wasm32`, ...).
//
// Overall structure:
//   wasm_backend.hpp      - data structures shared by the backend
//   wasm_backend_emit.cpp - low level instruction encoding and binary writer
//   wasm_backend.cpp      - lowering of procedures/statements/expressions
//
// Value model:
//   Every scalar expression result lives in a wasm local (a "virtual
//   register"). Instruction sequences leave the wasm operand stack empty
//   between statements, which makes structured control flow trivial as no
//   values are ever live on the operand stack across a block boundary.
//   Aggregates will live in linear memory (not yet implemented).
//
// Control flow:
//   Odin has no `goto`, so all control flow is structured and maps directly
//   onto wasm `block`/`loop`/`if` with `br` to an enclosing label. No CFG or
//   relooper is needed.

#ifndef WASM_BACKEND_HPP
#define WASM_BACKEND_HPP

enum wbValType : u8 {
	wbValType_Invalid = 0,
	wbValType_i32     = 0x7f,
	wbValType_i64     = 0x7e,
	wbValType_f32     = 0x7d,
	wbValType_f64     = 0x7c,
	wbValType_v128    = 0x7b,
	wbValType_funcref = 0x70,
};

enum wbSectionId : u8 {
	wbSection_Custom   = 0,
	wbSection_Type     = 1,
	wbSection_Import   = 2,
	wbSection_Function = 3,
	wbSection_Table    = 4,
	wbSection_Memory   = 5,
	wbSection_Global   = 6,
	wbSection_Export   = 7,
	wbSection_Start    = 8,
	wbSection_Element  = 9,
	wbSection_Code     = 10,
	wbSection_Data     = 11,
	wbSection_DataCount = 12,
};

enum wbExternalKind : u8 {
	wbExternal_Function = 0,
	wbExternal_Table    = 1,
	wbExternal_Memory   = 2,
	wbExternal_Global   = 3,
};

// Wasm opcodes used by the backend
enum wbOp : u8 {
	wbOp_unreachable   = 0x00,
	wbOp_nop           = 0x01,
	wbOp_block         = 0x02,
	wbOp_loop          = 0x03,
	wbOp_if            = 0x04,
	wbOp_else          = 0x05,
	wbOp_end           = 0x0b,
	wbOp_br            = 0x0c,
	wbOp_br_if         = 0x0d,
	wbOp_br_table      = 0x0e,
	wbOp_return        = 0x0f,
	wbOp_call          = 0x10,
	wbOp_call_indirect = 0x11,
	wbOp_drop          = 0x1a,
	wbOp_select        = 0x1b,
	wbOp_local_get     = 0x20,
	wbOp_local_set     = 0x21,
	wbOp_local_tee     = 0x22,
	wbOp_global_get    = 0x23,
	wbOp_global_set    = 0x24,

	wbOp_i32_load      = 0x28,
	wbOp_i64_load      = 0x29,
	wbOp_f32_load      = 0x2a,
	wbOp_f64_load      = 0x2b,
	wbOp_i32_load8_s   = 0x2c,
	wbOp_i32_load8_u   = 0x2d,
	wbOp_i32_load16_s  = 0x2e,
	wbOp_i32_load16_u  = 0x2f,
	wbOp_i64_load8_s   = 0x30,
	wbOp_i64_load8_u   = 0x31,
	wbOp_i64_load16_s  = 0x32,
	wbOp_i64_load16_u  = 0x33,
	wbOp_i64_load32_s  = 0x34,
	wbOp_i64_load32_u  = 0x35,
	wbOp_i32_store     = 0x36,
	wbOp_i64_store     = 0x37,
	wbOp_f32_store     = 0x38,
	wbOp_f64_store     = 0x39,
	wbOp_i32_store8    = 0x3a,
	wbOp_i32_store16   = 0x3b,
	wbOp_i64_store8    = 0x3c,
	wbOp_i64_store16   = 0x3d,
	wbOp_i64_store32   = 0x3e,
	wbOp_memory_size   = 0x3f,
	wbOp_memory_grow   = 0x40,

	wbOp_i32_const     = 0x41,
	wbOp_i64_const     = 0x42,
	wbOp_f32_const     = 0x43,
	wbOp_f64_const     = 0x44,

	wbOp_i32_eqz       = 0x45,
	wbOp_i32_eq        = 0x46,
	wbOp_i32_ne        = 0x47,
	wbOp_i32_lt_s      = 0x48,
	wbOp_i32_lt_u      = 0x49,
	wbOp_i32_gt_s      = 0x4a,
	wbOp_i32_gt_u      = 0x4b,
	wbOp_i32_le_s      = 0x4c,
	wbOp_i32_le_u      = 0x4d,
	wbOp_i32_ge_s      = 0x4e,
	wbOp_i32_ge_u      = 0x4f,

	wbOp_i64_eqz       = 0x50,
	wbOp_i64_eq        = 0x51,
	wbOp_i64_ne        = 0x52,
	wbOp_i64_lt_s      = 0x53,
	wbOp_i64_lt_u      = 0x54,
	wbOp_i64_gt_s      = 0x55,
	wbOp_i64_gt_u      = 0x56,
	wbOp_i64_le_s      = 0x57,
	wbOp_i64_le_u      = 0x58,
	wbOp_i64_ge_s      = 0x59,
	wbOp_i64_ge_u      = 0x5a,

	wbOp_f32_eq        = 0x5b,
	wbOp_f32_ne        = 0x5c,
	wbOp_f32_lt        = 0x5d,
	wbOp_f32_gt        = 0x5e,
	wbOp_f32_le        = 0x5f,
	wbOp_f32_ge        = 0x60,

	wbOp_f64_eq        = 0x61,
	wbOp_f64_ne        = 0x62,
	wbOp_f64_lt        = 0x63,
	wbOp_f64_gt        = 0x64,
	wbOp_f64_le        = 0x65,
	wbOp_f64_ge        = 0x66,

	wbOp_i32_clz       = 0x67,
	wbOp_i32_ctz       = 0x68,
	wbOp_i32_popcnt    = 0x69,
	wbOp_i32_add       = 0x6a,
	wbOp_i32_sub       = 0x6b,
	wbOp_i32_mul       = 0x6c,
	wbOp_i32_div_s     = 0x6d,
	wbOp_i32_div_u     = 0x6e,
	wbOp_i32_rem_s     = 0x6f,
	wbOp_i32_rem_u     = 0x70,
	wbOp_i32_and       = 0x71,
	wbOp_i32_or        = 0x72,
	wbOp_i32_xor       = 0x73,
	wbOp_i32_shl       = 0x74,
	wbOp_i32_shr_s     = 0x75,
	wbOp_i32_shr_u     = 0x76,
	wbOp_i32_rotl      = 0x77,
	wbOp_i32_rotr      = 0x78,

	wbOp_i64_clz       = 0x79,
	wbOp_i64_ctz       = 0x7a,
	wbOp_i64_popcnt    = 0x7b,
	wbOp_i64_add       = 0x7c,
	wbOp_i64_sub       = 0x7d,
	wbOp_i64_mul       = 0x7e,
	wbOp_i64_div_s     = 0x7f,
	wbOp_i64_div_u     = 0x80,
	wbOp_i64_rem_s     = 0x81,
	wbOp_i64_rem_u     = 0x82,
	wbOp_i64_and       = 0x83,
	wbOp_i64_or        = 0x84,
	wbOp_i64_xor       = 0x85,
	wbOp_i64_shl       = 0x86,
	wbOp_i64_shr_s     = 0x87,
	wbOp_i64_shr_u     = 0x88,
	wbOp_i64_rotl      = 0x89,
	wbOp_i64_rotr      = 0x8a,

	wbOp_f32_abs       = 0x8b,
	wbOp_f32_neg       = 0x8c,
	wbOp_f32_ceil      = 0x8d,
	wbOp_f32_floor     = 0x8e,
	wbOp_f32_trunc     = 0x8f,
	wbOp_f32_nearest   = 0x90,
	wbOp_f32_sqrt      = 0x91,
	wbOp_f32_add       = 0x92,
	wbOp_f32_sub       = 0x93,
	wbOp_f32_mul       = 0x94,
	wbOp_f32_div       = 0x95,
	wbOp_f32_min       = 0x96,
	wbOp_f32_max       = 0x97,
	wbOp_f32_copysign  = 0x98,

	wbOp_f64_abs       = 0x99,
	wbOp_f64_neg       = 0x9a,
	wbOp_f64_ceil      = 0x9b,
	wbOp_f64_floor     = 0x9c,
	wbOp_f64_trunc     = 0x9d,
	wbOp_f64_nearest   = 0x9e,
	wbOp_f64_sqrt      = 0x9f,
	wbOp_f64_add       = 0xa0,
	wbOp_f64_sub       = 0xa1,
	wbOp_f64_mul       = 0xa2,
	wbOp_f64_div       = 0xa3,
	wbOp_f64_min       = 0xa4,
	wbOp_f64_max       = 0xa5,
	wbOp_f64_copysign  = 0xa6,

	wbOp_i32_wrap_i64        = 0xa7,
	wbOp_i32_trunc_f32_s     = 0xa8,
	wbOp_i32_trunc_f32_u     = 0xa9,
	wbOp_i32_trunc_f64_s     = 0xaa,
	wbOp_i32_trunc_f64_u     = 0xab,
	wbOp_i64_extend_i32_s    = 0xac,
	wbOp_i64_extend_i32_u    = 0xad,
	wbOp_i64_trunc_f32_s     = 0xae,
	wbOp_i64_trunc_f32_u     = 0xaf,
	wbOp_i64_trunc_f64_s     = 0xb0,
	wbOp_i64_trunc_f64_u     = 0xb1,
	wbOp_f32_convert_i32_s   = 0xb2,
	wbOp_f32_convert_i32_u   = 0xb3,
	wbOp_f32_convert_i64_s   = 0xb4,
	wbOp_f32_convert_i64_u   = 0xb5,
	wbOp_f32_demote_f64      = 0xb6,
	wbOp_f64_convert_i32_s   = 0xb7,
	wbOp_f64_convert_i32_u   = 0xb8,
	wbOp_f64_convert_i64_s   = 0xb9,
	wbOp_f64_convert_i64_u   = 0xba,
	wbOp_f64_promote_f32     = 0xbb,
	wbOp_i32_reinterpret_f32 = 0xbc,
	wbOp_i64_reinterpret_f64 = 0xbd,
	wbOp_f32_reinterpret_i32 = 0xbe,
	wbOp_f64_reinterpret_i64 = 0xbf,

	wbOp_i32_extend8_s       = 0xc0,
	wbOp_i32_extend16_s      = 0xc1,
	wbOp_i64_extend8_s       = 0xc2,
	wbOp_i64_extend16_s      = 0xc3,
	wbOp_i64_extend32_s      = 0xc4,
};

// Byte buffer with wasm-specific encoding helpers (see wasm_backend_emit.cpp)
struct wbBuffer {
	Array<u8> data;
};

struct wbFuncType {
	Array<wbValType> params;
	Array<wbValType> results;
};

enum wbValueKind : u8 {
	wbValue_Invalid,
	wbValue_Local, // value lives in wasm local `index`
	wbValue_Const, // scalar constant
};

// A scalar value produced by an expression
struct wbValue {
	wbValueKind kind;
	wbValType   vt;
	Type *      type; // Odin type
	u32         index; // wasm local index (wbValue_Local)
	union {
		i64 i;  // i32/i64 constant (wbValue_Const)
		f64 f;  // f32/f64 constant (wbValue_Const)
	};
};

struct wbLocal {
	wbValType vt;
	String    name; // for the `name` custom section, may be empty
};

// An open structured control flow label (block/loop/if) in a procedure body
struct wbLabel {
	Ast * label;         // Ast_Label of the owning statement, nullptr if unlabelled
	u32   break_depth;   // absolute label depth for `break`
	u32   continue_depth;// absolute label depth for `continue` (loops only)
	bool  is_loop;
};

struct wbModule;
struct wbProcedure;

// A `call` whose function index is not yet known (imports are numbered before
// defined functions, and new procedures are discovered while lowering).
struct wbCallReloc {
	isize         offset; // offset of the 5-byte padded ULEB in `code`
	wbProcedure * target;
};

struct wbProcedure {
	wbModule * module;
	Entity *   entity;
	String     name;      // link name
	Type *     type;      // Type_Proc
	Ast *      body;      // Ast_BlockStmt, nullptr for foreign procedures
	u32        type_index;
	u32        func_index; // final function index (imports come first)

	bool       is_foreign;
	bool       is_export;
	bool       failed;
	String     import_module;
	String     import_name;

	u32        param_count;    // number of wasm params (including context pointer)
	i32        context_local;  // local index of the context pointer, -1 if none
	Array<wbLocal> locals;     // all locals, params first
	Array<wbValType> results;
	PtrMap<Entity *, u32> entity_locals; // Odin variable -> wasm local

	Array<u32> result_locals; // locals for named results (empty otherwise)

	wbBuffer   code;      // instruction bytes (without local declarations)
	u32        depth;     // number of currently open labels
	Array<wbLabel> labels;
	Array<wbCallReloc> call_relocs;
};

struct wbModule {
	CheckerInfo *info;
	gbAllocator  allocator;

	Array<wbFuncType>     types;
	Array<wbProcedure *>  imports;    // foreign procedures
	Array<wbProcedure *>  procedures; // defined procedures, in function index order
	PtrMap<Entity *, wbProcedure *> procedure_map;
	Array<wbProcedure *>  work_queue; // procedures whose bodies still need lowering

	u32 stack_size;
	u32 memory_initial_pages;
	u32 global_stack_pointer; // global index

	i32 error_count;
};

gb_internal bool wb_generate_code(CheckerInfo *info);

#endif // WASM_BACKEND_HPP
