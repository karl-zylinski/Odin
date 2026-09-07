// Web worker that runs the Odin compiler (odin.wasm). The compiler is a
// normal command line program, so every compile is a fresh instance with a
// fresh linear memory: the module is compiled once, the file system with the
// packed ODIN_ROOT sources is shared between runs.
//
// Messages in:  {type: "compile", files: [{path, data}], dir, flags}
//   `files` are written into the shared file system before compiling (paths
//   without a leading slash, `data` is a string or an ArrayBuffer), `dir` is
//   the package directory to build, `flags` extra compiler arguments.
// Messages out: {type: "ready"}, {type: "output", stream, text},
//               {type: "done", ok, ms, wasm?}

importScripts("wasi.js");

let compilerModule = null;
let rootFs = null;

async function setup() {
	const [moduleResponse, packResponse] = await Promise.all([fetch("odin.wasm"), fetch("odin_root.pack.gz")]);
	const compiled = WebAssembly.compileStreaming(moduleResponse);
	// The pack is gzipped on disk so that it is small no matter how it is served
	const unpacked = new Response(packResponse.body.pipeThrough(new DecompressionStream("gzip")));
	rootFs = new WasiFileSystem();
	rootFs.loadPack(await unpacked.arrayBuffer(), "/odin");
	compilerModule = await compiled;
	postMessage({type: "ready"});
}

// Files written by earlier compiles, removed before the next one
let writtenPaths = [];

function compile(msg) {
	const fs = rootFs;
	for (const path of writtenPaths) {
		fs.files.delete(path);
	}
	writtenPaths = [];
	for (const file of msg.files) {
		const data = typeof file.data === "string" ? new TextEncoder().encode(file.data) : new Uint8Array(file.data);
		fs.writeFile(file.path, data);
		writtenPaths.push(wasiNormalizePath(file.path));
	}
	fs.mkdirAll("out");

	const args = ["odin", "build", msg.dir, "-out:/out/main.wasm", "-backend:wasm"].concat(msg.flags);
	const env = ["ODIN_ROOT=/odin"];
	const wasi = new Wasi(fs, args, env,
		(text) => postMessage({type: "output", stream: "stdout", text: text}),
		(text) => postMessage({type: "output", stream: "stderr", text: text}));

	const started = performance.now();
	let code;
	try {
		code = wasiRun(compilerModule, wasi);
	} catch (e) {
		postMessage({type: "output", stream: "stderr", text: "Compiler crashed: " + e + "\n"});
		postMessage({type: "done", ok: false, ms: performance.now() - started});
		return;
	}
	const ms = performance.now() - started;
	const output = fs.readFile("out/main.wasm");
	fs.files.delete("out/main.wasm");
	if (code !== 0 || !output) {
		postMessage({type: "done", ok: false, ms: ms});
		return;
	}
	const bytes = new Uint8Array(output).slice().buffer;
	postMessage({type: "done", ok: true, ms: ms, wasm: bytes}, [bytes]);
}

onmessage = (e) => {
	if (e.data.type === "compile") {
		compile(e.data);
	}
};

setup().catch((e) => {
	postMessage({type: "output", stream: "stderr", text: "Failed to load the compiler: " + e + "\n"});
});
