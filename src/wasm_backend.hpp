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
//   Aggregates (structs, arrays, slices, strings, ...) live in linear memory
//   and are referred to by address (wbValue_Memory). Scalar variables whose
//   address is never taken live in wasm locals, everything else gets a slot
//   in the procedure's stack frame (see "Memory layout" below).
//
// Memory layout:
//   [0, stack_size)          shadow stack, grows down from `stack_size`.
//                            `__stack_pointer` (global 0) holds the current top.
//   [stack_size, data_end)   global variables and constant data (one data segment)
//   [data_end, ...)          free for the runtime heap (memory.grow)
//
// Calling convention:
//   The ABI of the LLVM backend for wasm (lbAbiWasm), for every calling
//   convention, so that imports, exports and linked objects agree: scalar
//   parameters are passed directly, small aggregates of basic fields (and 128
//   bit integers) are flattened into one wasm value per field, anything else
//   is passed as an i32 pointer to a copy made by the caller (see
//   wb_abi_flatten). If the procedure has a single scalar result it is
//   returned directly; otherwise the caller passes a pointer to result storage
//   (laid out as the result tuple) as the first parameter. "odin" calling
//   convention procedures take the context pointer as the last parameter.
//   Procedure values are indices into the function table (0 is nil).
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

#define WB_NO_LOCAL 0xffffffffu

enum wbValueKind : u8 {
	wbValue_Invalid,
	wbValue_Local,  // scalar in wasm local `index`
	wbValue_Const,  // scalar constant
	wbValue_Memory, // aggregate at address local[index] + offset (absolute if index == WB_NO_LOCAL)
};

// The result of an expression
struct wbValue {
	wbValueKind kind;
	wbValType   vt;    // wasm type (i32 for wbValue_Memory)
	Type *      type;  // Odin type
	u32         index; // wasm local index
	i32         offset;
	union {
		i64 i;  // i32/i64 constant (wbValue_Const)
		f64 f;  // f32/f64 constant (wbValue_Const)
	};
};

enum wbAddrKind : u8 {
	wbAddr_Invalid,
	wbAddr_Local,  // scalar variable in wasm local `index`
	wbAddr_Memory, // local[index] + offset (absolute if index == WB_NO_LOCAL)
	wbAddr_Map,    // map element: local[index] points at the map, the key is at frame offset `offset`
	wbAddr_Swizzle, // `v.xyz` of an array at local[index] + offset; `type` is the swizzled array type
	wbAddr_BitField, // bits [bit_offset, bit_offset+bit_size) of the bit_field at local[index] + offset; `type` is the field type
	wbAddr_SoaVariable, // element `soa_index` of the #soa container at local[index] + offset; `type` is the element type
};

// An addressable location
struct wbAddr {
	wbAddrKind kind;
	Type *     type; // type of the stored value (for wbAddr_Map possibly the (value, ok) tuple)
	u32        index;
	i32        offset;
	Type *     map_type; // wbAddr_Map only
	u8         swizzle_count;      // wbAddr_Swizzle only
	u8         swizzle_indices[4]; // wbAddr_Swizzle only
	u8         bit_size;           // wbAddr_BitField only
	i32        bit_offset;         // wbAddr_BitField only
	bool       bit_field_in_local; // wbAddr_BitField only: the backing value is held in local[index] instead of memory
	Type *     soa_type;           // wbAddr_SoaVariable (and a wbAddr_Swizzle of an #soa element): the container type
	wbValue    soa_index;          // wbAddr_SoaVariable only: index of the element
	Ast *      soa_index_expr;     // wbAddr_SoaVariable only: for the bounds check (nullptr once checked)
};

struct wbLocal {
	wbValType vt;
	String    name; // for the `name` custom section, may be empty
};

// An open control flow construct that `break`/`continue`/`fallthrough` may target
struct wbLabel {
	Ast * label;          // Ast_Label of the owning statement, nullptr if unlabelled
	u32   break_depth;    // absolute label depth for `break`
	u32   continue_depth; // absolute label depth for `continue` (loops only)
	u32   fall_depth;     // absolute label depth for `fallthrough` (switch cases only)
	bool  is_loop;
	bool  is_switch;
	isize scope_index;    // scope depth when the construct was entered (for defers)
};

struct wbProcedure;
// A `defer` statement, or a call to a `@(deferred_*)` procedure (stmt == nullptr)
struct wbDefer {
	Ast * stmt;
	isize scope_index;
	isize context_stack_count; // context_stack.count when the defer was registered
	wbProcedure *   proc;      // deferred procedure and its arguments, kept in fresh storage
	Type *          proc_type;
	Array<wbValue>  proc_args;
};

// A `context` value visible in the current procedure (see wb_context_addr)
struct wbContextData {
	wbAddr addr;        // memory address of a Context
	isize  scope_index; // scope depth it was created at, -1 for the implicit parameter
	isize  uses;        // reads since creation; a write after a read must copy
};

// What the body of a procedure is generated from
enum wbProcGen : u8 {
	wbProcGen_Body,           // an Ast_BlockStmt
	wbProcGen_StartupRuntime, // `__$startup_runtime`: global initializers, then @(init) procedures
	wbProcGen_CleanupRuntime, // `__$cleanup_runtime`: @(fini) procedures
	wbProcGen_Hasher,         // `__$hasher$$T`: map key hasher for `gen_type` (see wb_hasher_proc_for_type)
	wbProcGen_Equal,          // `__$equal$$T`: map key equality for `gen_type`
	wbProcGen_TestMain,       // `_start` for `odin test`: runs the runtime startup, then testing.runner
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
	Entity *   entity;    // nullptr for generated procedures
	String     name;      // link name
	Type *     type;      // Type_Proc, nullptr for generated procedures
	Ast *      body;      // Ast_BlockStmt, nullptr for foreign/generated procedures
	u32        type_index;
	u32        func_index;  // final function index (imports come first)
	u32        table_index; // index in the function table, 0 if not referenced as a value

	bool       is_foreign;
	bool       is_llvm_intrinsic; // foreign `llvm.*` procedure, lowered at the call site
	bool       is_export;
	bool       is_raw_body;  // linked from an object file: `code` holds the complete function body
	bool       failed;
	wbProcedure *alias;      // foreign procedure resolved to a defined function by the linker
	wbProcGen  gen;
	Type *     gen_type;    // the type a hasher/equal procedure is generated for
	Ast *      curr_stmt;   // statement being lowered (for #caller_location of implicit runtime calls)
	u16        state_flags; // #no_bounds_check / #no_type_assert etc. of the enclosing statements
	String     import_module;
	String     import_name;

	u32        param_count;    // number of wasm params (including sret and context pointers)
	i32        context_local;  // local index of the context pointer, -1 if none
	i32        sret_local;     // local index of the result pointer, -1 if none
	u32        fp_local;       // frame pointer (lowest address of the frame)
	u32        old_sp_local;   // stack pointer on entry
	u32        frame_size;     // bytes of stack frame, known after lowering
	Array<wbLocal> locals;     // all locals, params first
	Array<wbValType> results;

	PtrMap<Entity *, wbAddr> variables; // Odin variable -> storage
	PtrMap<Ast *, wbValue> selector_values; // `x->f(..)` is `x.f(x, ..)`: x evaluated once (StateFlag_SelectorCallExpr)
	PtrMap<Ast *, wbAddr>  selector_addrs;
	PtrSet<Entity *> addressed;         // variables whose address is taken
	Array<wbAddr> result_addrs;         // named results (empty otherwise)

	Array<wbDefer> defers;
	Array<isize>   scopes; // defers.count when each open scope was entered
	Array<wbContextData> context_stack;

	wbBuffer   prologue;  // stack frame setup, generated after the body
	wbBuffer   code;      // instruction bytes (without local declarations)
	u32        depth;     // number of currently open wasm labels
	Array<wbLabel> labels;
	Array<wbCallReloc> call_relocs;
};

// A global variable whose initializer runs in the start function
struct wbGlobalInit {
	Entity *entity;
	Ast *   init_expr;
};

struct wbObject;

struct wbModule {
	CheckerInfo *info;
	gbAllocator  allocator;

	Array<wbFuncType>     types;
	Array<wbProcedure *>  imports;    // foreign procedures
	Array<wbProcedure *>  procedures; // defined procedures, in function index order
	PtrMap<Entity *, wbProcedure *> procedure_map;
	Array<wbProcedure *>  work_queue; // procedures whose bodies still need lowering
	Array<wbProcedure *>  table;      // function table, index 0 is reserved for nil
	Array<wbObject *>     objects;    // linked object files (wasm_backend_link.cpp)
	Array<wbProcedure *>  aliased;    // foreign procedures resolved to defined functions
	wbProcedure *         startup;    // runs non-constant global initializers (wasm start function, only without an entry point)
	wbProcedure *         startup_runtime; // `__$startup_runtime`, generated when referenced
	wbProcedure *         cleanup_runtime; // `__$cleanup_runtime`, generated when referenced
	isize                 global_inits_emitted; // prefix of global_init_queue already lowered

	u32 stack_size;
	u32 memory_initial_pages;
	u32 global_stack_pointer; // global index

	// Global variables and constant data, one data segment at `data_base`
	Array<u8> data;
	u32       data_base;
	PtrMap<Entity *, u32> globals;       // global variable -> absolute address
	StringMap<wbProcedure *> libm_imports; // host math procedures (`env` module) backing `llvm.*` intrinsics
	StringMap<u32>        string_bytes;  // interned NUL-terminated string data
	StringMap<u32>        string_values; // interned `string` {data, len} constants
	StringMap<u32>        string16_values; // interned `string16` {data, len} constants (keyed by UTF-8 source)
	StringMap<u32>        string16_bytes;  // interned NUL-terminated UTF-16 string data (keyed by UTF-8 source)
	StringMap<wbProcedure *> gen_procs;      // generated hasher/equal procedures by canonical name (wasm_backend_map.cpp)
	StringMap<u32>        map_cell_infos;    // Map_Cell_Info constants by canonical type name
	StringMap<u32>        map_infos;         // Map_Info constants by canonical map type name
	Array<wbGlobalInit>   global_init_queue; // globals with non-constant initializers
	Array<u32>            type_info_addrs;   // type info table entry -> absolute address (0 = empty slot)

	i32 error_count;
};

gb_internal bool wb_generate_code(CheckerInfo *info);

#endif // WASM_BACKEND_HPP
