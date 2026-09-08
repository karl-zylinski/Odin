// Web worker that runs the Odin compiler (odin.wasm). The compiler is a
// normal command line program, so every compile is a fresh instance with a
// fresh linear memory: the module is compiled once, the file system with the
// ODIN_ROOT sources is shared between runs.
//
// The sources are not one download. `packs/manifest.json` lists every package
// with its file names, their sizes and what it imports, which is enough for
// the compiler to look around the file system; the contents come one gzipped
// pack per package, fetched when a program's imports say they are needed. A
// program that imports nothing from `core:crypto` never downloads it.
//
// Messages in:  {type: "compile", files: [{path, data}], dir, flags}
//   `files` are written into the shared file system before compiling (paths
//   without a leading slash, `data` is a string or an ArrayBuffer), `dir` is
//   the package directory to build, `flags` extra compiler arguments.
//               {type: "prefetch", files: [{path, data}]}
//   the sources of the example that was just opened, so that the packs they
//   need are on their way before the compile is asked for.
// Messages out: {type: "ready"}, {type: "output", stream, text},
//               {type: "done", ok, ms, wasm?}

importScripts("wasi.js");

let compilerModule = null;
let rootFs = null;
let packages = null;                 // package directory -> {files, imports}
const packFetches = new Map();       // package directory -> promise of its contents

// The packages every program in this playground uses: fetched while the
// compiler module is still being downloaded and compiled
const PREFETCH_AT_STARTUP = ["karl2d", "base/runtime", "core/fmt", "core/os"];

async function setup() {
	const [moduleResponse, manifestResponse] = await Promise.all([
		fetch("odin.wasm"),
		fetch("packs/manifest.json"),
	]);
	const compiled = WebAssembly.compileStreaming(moduleResponse);
	if (!manifestResponse.ok) {
		throw new Error("Failed to fetch packs/manifest.json: " + manifestResponse.status);
	}
	packages = (await manifestResponse.json()).packages;
	rootFs = new WasiFileSystem();
	rootFs.loadManifest(packages, "/odin");
	prefetch(PREFETCH_AT_STARTUP);
	compilerModule = await compiled;
	postMessage({type: "ready"});
}

// Fetches one package's pack, once
function fetchPack(dir) {
	let pending = packFetches.get(dir);
	if (pending === undefined) {
		pending = (async () => {
			const response = await fetch("packs/" + dir + ".pack.gz");
			if (!response.ok) {
				throw new Error("Failed to fetch packs/" + dir + ".pack.gz: " + response.status);
			}
			// Gzipped on disk so that it is small no matter how it is served
			const unpacked = new Response(response.body.pipeThrough(new DecompressionStream("gzip")));
			rootFs.loadPack(await unpacked.arrayBuffer(), "/odin");
		})();
		packFetches.set(dir, pending);
	}
	return pending;
}

// The packages `roots` need: what they import, what those import, and so on
function importClosure(roots) {
	const needed = new Set();
	const queue = roots.slice();
	while (queue.length > 0) {
		const dir = queue.pop();
		if (needed.has(dir) || packages[dir] === undefined) {
			continue; // not a packed package (an example's own directory, say)
		}
		needed.add(dir);
		for (const imported of packages[dir].imports) {
			queue.push(imported);
		}
	}
	return needed;
}

function prefetch(roots) {
	return Promise.all([...importClosure(roots)].map(fetchPack));
}

// Resolves a path as it appears in an `import` against the directory of the
// file it appears in, the way `source_imports` in playground/pack_root does
function importedPackage(path, dir) {
	const colon = path.indexOf(":");
	if (colon >= 0) {
		const collection = path.slice(0, colon), name = path.slice(colon+1);
		if (collection !== "base" && collection !== "core" && collection !== "vendor") {
			return null;
		}
		return name === "" ? collection : collection + "/" + name;
	}
	return wasiNormalizePath(dir + "/" + path);
}

// `@(require) import _ "vendor:libc-shim"` counts too
const IMPORT_PATTERN = /^(?:@\([^)]*\)\s*)*import\s+(?:[A-Za-z_]\w*\s+)?"([^"]+)"/;

// The packages the given sources import, by their pack directories
function importsOfSources(files) {
	const roots = [];
	for (const file of files) {
		if (!file.path.endsWith(".odin")) {
			continue;
		}
		// An edited file arrives as text, the rest as the bytes that were fetched
		const text = typeof file.data === "string" ? file.data : new TextDecoder().decode(new Uint8Array(file.data));
		// Paths arrive as "odin/karl2d/examples/basics/basics.odin": the
		// directory the imports of the file are relative to is inside the root
		const path = wasiNormalizePath(file.path);
		const dir = wasiParentPath(path).replace(/^odin\/?/, "");
		for (const line of text.split("\n")) {
			const match = IMPORT_PATTERN.exec(line.trim());
			if (match === null) {
				continue;
			}
			const dependency = importedPackage(match[1], dir);
			if (dependency !== null) {
				roots.push(dependency);
			}
		}
	}
	return roots;
}

// Files written by earlier compiles, removed before the next one
let writtenPaths = [];

async function compile(msg) {
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

	const started = performance.now();
	// What the sources say they need, plus whatever the compile turns out to
	// read that they did not say (a missing pack aborts the run, and the
	// compile starts over on a fresh instance once it has arrived)
	await prefetch(importsOfSources(msg.files));
	let code;
	for (let retries = 0; ; retries++) {
		const wasi = new Wasi(fs, args, env,
			(text) => postMessage({type: "output", stream: "stdout", text: text}),
			(text) => postMessage({type: "output", stream: "stderr", text: text}));
		try {
			code = wasiRun(compilerModule, wasi);
			break;
		} catch (e) {
			if (e instanceof WasiMissingPack && retries < 64) {
				await fetchPack(e.pack);
				continue;
			}
			postMessage({type: "output", stream: "stderr", text: "Compiler crashed: " + e + "\n"});
			postMessage({type: "done", ok: false, ms: performance.now() - started});
			return;
		}
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
		compile(e.data).catch((error) => {
			postMessage({type: "output", stream: "stderr", text: "Compiler failed: " + error + "\n"});
			postMessage({type: "done", ok: false, ms: 0});
		});
	} else if (e.data.type === "prefetch") {
		if (packages !== null) {
			prefetch(importsOfSources(e.data.files)).catch(() => {}); // the compile reports what it cannot get
		}
	}
};

setup().catch((e) => {
	postMessage({type: "output", stream: "stderr", text: "Failed to load the compiler: " + e + "\n"});
});
