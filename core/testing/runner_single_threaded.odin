#+private
package testing

/*
	(c) Copyright 2024 Feoramund <rune@swevencraft.org>.
	Made available under Odin's license.

	List of contributors:
		Ginger Bill: Initial implementation.
		Feoramund:   Total rewrite.
		Karl Zylinski: Single-threaded runner.
*/

import "base:runtime"
import "core:fmt"
import "core:io"
import "core:log"
import "core:math/rand"
import "core:os"
import "core:strings"
import "core:time"

// A minimal test runner for targets without threads (WebAssembly).
//
// Tests run one after another in the runner's own thread, so there is no
// isolation: a failed assertion, panic or bounds check aborts the whole run
// after reporting which test was running. Log messages are written straight to
// STDERR instead of going through a channel.
runner_single_threaded :: proc(internal_tests: []Internal_Test) -> bool {
	stdout := os.to_stream(os.stdout)
	stderr := os.to_stream(os.stderr)

	global_log_colors_disabled = true

	selected := select_tests_by_name(internal_tests, TEST_NAMES, stderr)
	defer delete(selected)

	when SHARED_RANDOM_SEED == 0 {
		shared_random_seed := rand.uint64()
	} else {
		shared_random_seed := SHARED_RANDOM_SEED
	}

	fmt.wprintfln(stdout, "Starting single-threaded test runner with %i test%s (random seed: %i).",
		len(selected), "" if len(selected) == 1 else "s", shared_random_seed)

	failed_tests: [dynamic]string
	defer delete(failed_tests)

	total_start := time.tick_now()

	for it, i in selected {
		fmt.wprintfln(stdout, "[%i/%i] %s.%s", i+1, len(selected), it.pkg, it.name)

		t: T
		t.seed = shared_random_seed
		t._log_allocator = context.allocator

		local_test_expected_failures = {}
		local_test_assertion_raised = {}

		context.assertion_failure_proc = single_threaded_assertion_failure_proc

		context.logger = {
			procedure = single_threaded_logger_proc,
			data = &t,
			lowest_level = get_log_level(),
			options = Default_Test_Logger_Opts - {.Terminal_Color},
		}

		random_generator_state: rand.Xoshiro256_Random_State
		context.random_generator = {
			procedure = rand.xoshiro256_random_generator_proc,
			data = &random_generator_state,
		}
		rand.reset(t.seed)

		free_all(context.temp_allocator)

		it.p(&t)

		end_t(&t)

		if failed(&t) {
			fmt.wprintfln(stdout, "  FAIL %s.%s (%i error%s)", it.pkg, it.name, t.error_count, "" if t.error_count == 1 else "s")
			append(&failed_tests, fmt.aprintf("%s.%s", it.pkg, it.name))
		}
	}

	total_duration := time.tick_since(total_start)

	fmt.wprintfln(stdout, "Finished %i test%s in %v: %i passed, %i failed.",
		len(selected), "" if len(selected) == 1 else "s",
		total_duration,
		len(selected) - len(failed_tests), len(failed_tests))

	if len(failed_tests) > 0 {
		fmt.wprintln(stdout, "Failed tests:")
		for name in failed_tests {
			fmt.wprintfln(stdout, "\t%s", name)
			delete(name)
		}
	}

	return len(failed_tests) == 0
}

// Select the tests named in a comma-separated list of `name` or `pkg.name`
// entries; an empty list selects every test.
select_tests_by_name :: proc(internal_tests: []Internal_Test, test_names: string, stderr: io.Writer) -> [dynamic]Internal_Test {
	selected: [dynamic]Internal_Test

	if test_names == "" {
		append(&selected, ..internal_tests)
		return selected
	}

	index_list := test_names
	for selector in strings.split_iterator(&index_list, ",") {
		pkg, _, name := strings.partition(selector, ".")
		if name == "" {
			name, pkg = pkg, ""
		}

		found := false
		for it in internal_tests {
			if it.name == name && (pkg == "" || it.pkg == pkg) {
				found = true
				append(&selected, it)
				break
			}
		}

		if !found {
			fmt.wprintfln(stderr, "No test found for the name: %q", selector)
		}
	}

	return selected
}

single_threaded_logger_proc :: proc(logger_data: rawptr, level: runtime.Logger_Level, text: string, options: runtime.Logger_Options, location := #caller_location) {
	t := cast(^T)logger_data

	if level >= .Error {
		t.error_count += 1
	}

	formatted := format_log_text(level, text, options, location, time.now(), context.temp_allocator)
	fmt.eprintln(formatted)
}

// There is no way to recover from a trap, so a failed assertion ends the run.
single_threaded_assertion_failure_proc :: proc(prefix, message: string, loc: runtime.Source_Code_Location) -> ! {
	if local_test_expected_failures.message_count + local_test_expected_failures.location_count > 0 {
		log.debugf("%s\n\tmessage: %q\n\tlocation: %w", prefix, message, loc)
	} else {
		log.fatalf("%s: %s", prefix, message, location = loc)
	}
	fmt.eprintln("The single-threaded test runner cannot recover from a failed assertion; stopping.")
	os.exit(1)
}
