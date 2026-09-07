package main

import "core:fmt"
import "core:reflect"
import "base:runtime"

Color :: enum {
	Red,
	Green,
	Blue = 10,
}

Point :: struct {
	x: int,
	y: int,
	tag: u8,
}

Named_Point :: distinct Point

Shape :: union {
	int,
	f64,
	Point,
}

print_type_info :: proc(ti: ^runtime.Type_Info) {
	switch v in ti.variant {
	case runtime.Type_Info_Named:
		fmt.println("named:", v.name)
	case runtime.Type_Info_Integer:
		fmt.println("integer signed:", v.signed, "size:", ti.size)
	case runtime.Type_Info_Rune:
		fmt.println("rune")
	case runtime.Type_Info_Float:
		fmt.println("float size:", ti.size)
	case runtime.Type_Info_Complex:
		fmt.println("complex")
	case runtime.Type_Info_Quaternion:
		fmt.println("quaternion")
	case runtime.Type_Info_String:
		fmt.println("string is_cstring:", v.is_cstring)
	case runtime.Type_Info_Boolean:
		fmt.println("boolean")
	case runtime.Type_Info_Any:
		fmt.println("any")
	case runtime.Type_Info_Type_Id:
		fmt.println("typeid")
	case runtime.Type_Info_Pointer:
		fmt.println("pointer elem nil:", v.elem == nil)
	case runtime.Type_Info_Multi_Pointer:
		fmt.println("multi pointer")
	case runtime.Type_Info_Procedure:
		fmt.println("procedure")
	case runtime.Type_Info_Array:
		fmt.println("array count:", v.count, "elem_size:", v.elem_size)
	case runtime.Type_Info_Enumerated_Array:
		fmt.println("enumerated array")
	case runtime.Type_Info_Dynamic_Array:
		fmt.println("dynamic array")
	case runtime.Type_Info_Slice:
		fmt.println("slice elem_size:", v.elem_size)
	case runtime.Type_Info_Parameters:
		fmt.println("parameters")
	case runtime.Type_Info_Struct:
		fmt.println("struct field_count:", v.field_count)
		for i in 0..<int(v.field_count) {
			fmt.println("  field", v.names[i], "offset", v.offsets[i], "type_size", v.types[i].size)
		}
	case runtime.Type_Info_Union:
		fmt.println("union variant_count:", len(v.variants), "no_nil:", v.no_nil)
	case runtime.Type_Info_Enum:
		fmt.println("enum names_count:", len(v.names))
		for name, i in v.names {
			fmt.println("  enum", name, "=", v.values[i])
		}
	case runtime.Type_Info_Map:
		fmt.println("map")
	case runtime.Type_Info_Bit_Set:
		fmt.println("bit_set")
	case runtime.Type_Info_Simd_Vector:
		fmt.println("simd_vector")
	case runtime.Type_Info_Matrix:
		fmt.println("matrix")
	case runtime.Type_Info_Soa_Pointer:
		fmt.println("soa_pointer")
	case runtime.Type_Info_Bit_Field:
		fmt.println("bit_field")
	case runtime.Type_Info_Fixed_Capacity_Dynamic_Array:
		fmt.println("fixed_capacity_dynamic_array")
	case:
		fmt.println("unknown variant")
	}
}

main :: proc() {
	fmt.println("size_of(int):", size_of(int))
	fmt.println("size_of(i8):", size_of(i8))
	fmt.println("size_of(i16):", size_of(i16))
	fmt.println("size_of(i32):", size_of(i32))
	fmt.println("size_of(i64):", size_of(i64))
	fmt.println("size_of(f32):", size_of(f32))
	fmt.println("size_of(f64):", size_of(f64))
	fmt.println("size_of(Point):", size_of(Point))
	fmt.println("size_of(Shape):", size_of(Shape))
	fmt.println("align_of(int):", align_of(int))
	fmt.println("align_of(Point):", align_of(Point))
	fmt.println("align_of(Shape):", align_of(Shape))

	fmt.println("--- typeid_of ---")
	tid_int := typeid_of(int)
	tid_point := typeid_of(Point)
	fmt.println("tid_int == tid_int:", tid_int == tid_int)
	fmt.println("tid_int == tid_point:", tid_int == tid_point)

	fmt.println("--- type_info_of variants ---")
	print_type_info(type_info_of(int))
	print_type_info(type_info_of(u32))
	print_type_info(type_info_of(f32))
	print_type_info(type_info_of(f64))
	print_type_info(type_info_of(bool))
	print_type_info(type_info_of(string))
	print_type_info(type_info_of(rawptr))
	print_type_info(type_info_of(^int))
	print_type_info(type_info_of([4]int))
	print_type_info(type_info_of([]int))
	print_type_info(type_info_of(Point))
	print_type_info(type_info_of(Named_Point))
	print_type_info(type_info_of(Color))
	print_type_info(type_info_of(Shape))

	fmt.println("--- reflect helpers ---")
	col := Color.Blue
	a_col: any = col
	fmt.println("enum_string:", reflect.enum_string(a_col))
	fmt.println("struct_field_names Point:", reflect.struct_field_names(Point))
	fmt.println("type_kind int:", reflect.type_kind(int))
	fmt.println("type_kind Point:", reflect.type_kind(Point))
	fmt.println("type_kind Color:", reflect.type_kind(Color))
	base_ti := reflect.type_info_base(type_info_of(Named_Point))
	fmt.println("type_info_base of Named_Point is struct:", base_ti != nil)
	if s, ok := base_ti.variant.(runtime.Type_Info_Struct); ok {
		fmt.println("base struct field_count:", s.field_count)
	}

	fmt.println("--- any + type assertion ---")
	x: int = 42
	v: any = x
	iv := v.(int)
	fmt.println("asserted int:", iv)
	iv2, ok2 := v.(int)
	fmt.println("comma-ok int:", iv2, ok2)
	sv2, oks := v.(string)
	fmt.println("comma-ok string (should fail):", sv2, oks)

	fmt.println("--- union type switch ---")
	// NOTE: constant array literals of union values are assigned at runtime
	// here (not as a literal) because the wasm backend cannot serialize
	// constant unions yet; see phase2_rtti_union_const_array(_struct).
	shapes: [3]Shape
	shapes[0] = 42
	shapes[1] = 3.5
	shapes[2] = Point{x = 1, y = 2, tag = 9}
	for sh in shapes {
		switch t in sh {
		case int:
			fmt.println("shape is int:", t)
		case f64:
			fmt.println("shape is f64:", t)
		case Point:
			fmt.println("shape is Point:", t.x, t.y, t.tag)
		case:
			fmt.println("shape is nil")
		}
	}

	// NOTE: `for c in Color` (ranging directly over an enum type) is tested
	// separately in phase2_rtti_enum_type_range because the wasm backend
	// does not support it yet ("unsupported range over a type").

	fmt.println("done")
}
