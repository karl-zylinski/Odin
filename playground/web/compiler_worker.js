// Web worker that runs the Odin compiler (odin.wasm). The compiler is a
// normal command line program, so every compile is a fresh instance with a
// fresh linear memory: the module is compiled once, the file system with the
// packed `base`, `core` and `vendor` sources is shared between runs.

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

function compile(msg) {
	const fs = rootFs;
	// Files from the previous compile in /src and /out are replaced
	for (const path of Array.from(fs.files.keys())) {
		if (path.startsWith("src/") || path.startsWith("out/")) {
			fs.files.delete(path);
		}
	}
	fs.mkdirAll("src");
	fs.mkdirAll("out");
	fs.writeFile("src/main.odin", new TextEncoder().encode(msg.source));

	const args = ["odin", "build", "/src", "-out:/out/main.wasm", "-backend:wasm"].concat(msg.flags);
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
