// Default microarchitecture and target feature selection.
// Shared by all backends; only `-microarch:native` needs LLVM to query the host CPU.

gb_internal String get_default_microarchitecture() {
	String default_march = str_lit("generic");
	if (build_context.metrics.arch == TargetArch_amd64) {
		// NOTE(bill): x86-64-v2 is more than enough for everyone
		//
		// x86-64: CMOV, CMPXCHG8B, FPU, FXSR, MMX, FXSR, SCE, SSE, SSE2
		// x86-64-v2: (close to Nehalem) CMPXCHG16B, LAHF-SAHF, POPCNT, SSE3, SSE4.1, SSE4.2, SSSE3
		// x86-64-v3: (close to Haswell) AVX, AVX2, BMI1, BMI2, F16C, FMA, LZCNT, MOVBE, XSAVE
		// x86-64-v4: AVX512F, AVX512BW, AVX512CD, AVX512DQ, AVX512VL
		if (build_context.metrics.os == TargetOs_freestanding) {
			default_march = str_lit("x86-64");
		} else {
			default_march = str_lit("x86-64-v2");
		}
	} else if (build_context.metrics.arch == TargetArch_riscv64) {
		default_march = str_lit("generic-rv64");
	} else if (build_context.metrics.arch == TargetArch_arm32) {
		// The arm32 triple is `gnueabihf`, and the hard-float ABI passes floating point in the
		// VFP registers. `generic` has no FPU at all. LLVM cannot honor the ABI its own
		// triple asks for and quietly falls back to the soft-float convention.
		//
		// `arm1176jzf-s` is what clang picks by default for this same triple.
		default_march = str_lit("arm1176jzf-s");
	}

	return default_march;
}

gb_internal String get_final_microarchitecture() {
	BuildContext *bc = &build_context;

	String microarch = bc->microarch;
	if (microarch.len == 0) {
		microarch = get_default_microarchitecture();
	} else if (microarch == str_lit("native")) {
#if defined(ODIN_NO_LLVM)
		gb_printf_err("-microarch:native is not available in a compiler built without LLVM\n");
		gb_exit(1);
#else
		microarch = make_string_c(LLVMGetHostCPUName());
#endif
	}
	return microarch;
}

gb_internal String get_default_features() {
	BuildContext *bc = &build_context;

	if (bc->microarch == str_lit("native")) {
#if defined(ODIN_NO_LLVM)
		gb_printf_err("-microarch:native is not available in a compiler built without LLVM\n");
		gb_exit(1);
		String features = {};
#else
		String features = make_string_c(LLVMGetHostCPUFeatures());
#endif

		// Update the features string so LLVM uses it later.
		if (bc->target_features_string.len > 0) {
			bc->target_features_string = concatenate3_strings(permanent_allocator(), features, str_lit(","), bc->target_features_string);
		} else {
			bc->target_features_string = features;
		}

		return features;
	}

	int off = 0;
	for (int i = 0; i < bc->metrics.arch; i += 1) {
		off += target_microarch_counts[i];
	}

	String microarch = get_final_microarchitecture();

	// NOTE(laytan): for riscv64 to work properly with Odin, we need to enforce some features.
	// and we also overwrite the generic target to include more features so we don't default to
	// a potato feature set.
	if (bc->metrics.arch == TargetArch_riscv64) {
		if (microarch == str_lit("generic-rv64")) {
			// This is what clang does by default (on -march=rv64gc for General Computing), seems good to also default to.
			String features = str_lit("64bit,a,c,d,f,m,relax,zicsr,zifencei");

			// Update the features string so LLVM uses it later.
			if (bc->target_features_string.len > 0) {
				bc->target_features_string = concatenate3_strings(permanent_allocator(), features, str_lit(","), bc->target_features_string);
			} else {
				bc->target_features_string = features;
			}

			return features;
		}
	}

	for (int i = off; i < off+target_microarch_counts[bc->metrics.arch]; i += 1) {
		if (microarch_features_list[i].microarch == microarch) {
			return microarch_features_list[i].features;
		}
	}

	GB_PANIC("unknown microarch: %.*s", LIT(microarch));
	return {};
}
