// The playground page: sends the editor contents to the compiler worker and
// runs the resulting js_wasm32 module with the normal Odin JS runtime.

const DEFAULT_SOURCE = `package main

import "core:fmt"

main :: proc() {
	fmt.println("Hellope!")

	for i in 1..=5 {
		fmt.printfln("%d squared is %d", i, i*i)
	}
}
`;

const sourceElement = document.getElementById("source");
const flagsElement = document.getElementById("flags");
const runButton = document.getElementById("run");
const statusElement = document.getElementById("status");
const compilerOutput = document.getElementById("compiler-output");
const programOutput = document.getElementById("program-output");
const canvas = document.getElementById("webgl-canvas");

sourceElement.value = DEFAULT_SOURCE;

const worker = new Worker("compiler_worker.js");
let compiling = false;
let runGeneration = 0;

worker.onmessage = (e) => {
	const msg = e.data;
	if (msg.type === "ready") {
		statusElement.textContent = "Ready";
		runButton.disabled = false;
	} else if (msg.type === "output") {
		compilerOutput.textContent += msg.text;
	} else if (msg.type === "done") {
		compiling = false;
		runButton.disabled = false;
		if (msg.ok) {
			statusElement.textContent = "Compiled in " + Math.round(msg.ms) + " ms";
			runProgram(msg.wasm);
		} else {
			statusElement.textContent = "Compilation failed";
			if (window.playgroundTestHook) {
				window.playgroundTestHook(false);
			}
		}
	}
};

function compileAndRun() {
	if (compiling) {
		return;
	}
	compiling = true;
	runGeneration++;
	runButton.disabled = true;
	statusElement.textContent = "Compiling...";
	compilerOutput.textContent = "";
	programOutput.textContent = "";
	canvas.style.display = "none";
	const flags = flagsElement.value.split(/\s+/).filter((f) => f.length > 0);
	worker.postMessage({type: "compile", source: sourceElement.value, flags: flags});
}

async function runProgram(bytes) {
	const generation = ++runGeneration;
	const memoryInterface = new odin.WasmMemoryInterface();
	memoryInterface.setIntSize(4);
	const imports = odin.setupDefaultImports(memoryInterface, null, memoryInterface.memory);
	imports.odin_env.write = (fd, ptr, len) => {
		programOutput.textContent += memoryInterface.loadString(ptr, len);
	};
	let exports;
	try {
		const wasm = await WebAssembly.instantiate(bytes, imports);
		exports = wasm.instance.exports;
		memoryInterface.setExports(exports);
		if (exports.memory) {
			memoryInterface.setMemory(exports.memory);
		}
		if (exports._start) {
			exports._start();
		}
	} catch (e) {
		programOutput.textContent += "\nProgram crashed: " + e + "\n";
		if (window.playgroundTestHook) {
			window.playgroundTestHook(false);
		}
		return;
	}

	if (exports.step) {
		canvas.style.display = "block";
		const odin_ctx = exports.default_context_ptr();
		let prevTimeStamp = undefined;
		const step = (currTimeStamp) => {
			if (generation !== runGeneration) {
				return;
			}
			if (prevTimeStamp == undefined) {
				prevTimeStamp = currTimeStamp;
			}
			const dt = (currTimeStamp - prevTimeStamp)*0.001;
			prevTimeStamp = currTimeStamp;
			let keepGoing = false;
			try {
				keepGoing = exports.step(dt, odin_ctx);
			} catch (e) {
				programOutput.textContent += "\nProgram crashed: " + e + "\n";
				return;
			}
			if (!keepGoing) {
				if (exports._end) {
					exports._end();
				}
				return;
			}
			window.requestAnimationFrame(step);
		};
		window.requestAnimationFrame(step);
	} else if (exports._end) {
		exports._end();
	}
	if (window.playgroundTestHook) {
		window.playgroundTestHook(true);
	}
}

runButton.addEventListener("click", compileAndRun);

document.addEventListener("keydown", (e) => {
	if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) {
		e.preventDefault();
		compileAndRun();
	}
});

sourceElement.addEventListener("keydown", (e) => {
	if (e.key === "Tab") {
		e.preventDefault();
		const start = sourceElement.selectionStart;
		const end = sourceElement.selectionEnd;
		sourceElement.setRangeText("\t", start, end, "end");
	}
});

// Automated test: `?test=1` compiles and runs the default program as soon as
// the compiler is ready and posts the console contents to `/log`.
const testParams = new URLSearchParams(window.location.search);
if (testParams.has("test")) {
	if (testParams.has("src")) {
		sourceElement.value = testParams.get("src");
	}
	window.playgroundTestHook = (ok) => {
		setTimeout(() => {
			const text = statusElement.textContent + "\n" + document.getElementById("console").textContent;
			fetch("/log", {method: "POST", body: (ok ? "OK\n" : "FAILED\n") + text});
		}, 200);
	};
	window.onerror = (message) => {
		fetch("/log", {method: "POST", body: "ERROR " + message});
	};
	worker.onerror = (e) => {
		fetch("/log", {method: "POST", body: "WORKER ERROR " + e.message});
	};
	const waitForReady = setInterval(() => {
		if (!runButton.disabled) {
			clearInterval(waitForReady);
			compileAndRun();
		}
	}, 100);
}
