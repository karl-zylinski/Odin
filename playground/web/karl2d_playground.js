// The Karl2D playground page: lets the user pick one of the Karl2D examples,
// edit it, compile it with the compiler worker and run it in the game iframe.
//
// The examples are listed in examples.json (made by playground/pack_root) and
// fetched one by one when selected, so that only the assets of the chosen
// example are downloaded. A compile writes the example into the compiler's
// file system as /odin/karl2d/examples/<dir>/ (the library is packed as
// /odin/karl2d so the examples' `import k2 "../.."` keeps working) together
// with Karl2D's web entry point in build/web/, which is the package built.

const sourceElement = document.getElementById("source");
const exampleSelect = document.getElementById("example");
const runButton = document.getElementById("run");
const statusElement = document.getElementById("status");
const consoleElement = document.getElementById("console");
const rightElement = document.getElementById("right");

let examples = [];          // examples.json
let exampleFiles = new Map(); // dir -> Map(path -> ArrayBuffer), fetched on demand
let entrySource = null;     // Karl2D's web entry point
let current = null;         // the selected example
let edited = new Map();     // dir -> edited main source, so switching examples keeps changes
let compilerReady = false;
let compiling = false;
let gameFrame = document.getElementById("game");

const worker = new Worker("compiler_worker.js");

worker.onmessage = (e) => {
	const msg = e.data;
	if (msg.type === "ready") {
		compilerReady = true;
		updateStatus("Ready");
		updateButtons();
	} else if (msg.type === "output") {
		consoleElement.textContent += msg.text;
	} else if (msg.type === "done") {
		compiling = false;
		updateButtons();
		if (msg.ok) {
			updateStatus("Compiled in " + Math.round(msg.ms) + " ms");
			runProgram(msg.wasm);
		} else {
			updateStatus("Compilation failed");
			testHook(false);
		}
	}
};

function updateStatus(text) {
	statusElement.textContent = text;
}

function updateButtons() {
	const ready = compilerReady && current !== null && !compiling;
	runButton.disabled = !ready;
	exampleSelect.disabled = examples.length === 0;
}

async function fetchBinary(url) {
	const response = await fetch(url);
	if (!response.ok) {
		throw new Error("Failed to fetch " + url + ": " + response.status);
	}
	return await response.arrayBuffer();
}

async function fetchExampleFiles(example) {
	if (exampleFiles.has(example.dir)) {
		return exampleFiles.get(example.dir);
	}
	const files = new Map();
	await Promise.all(example.files.map(async (path) => {
		files.set(path, await fetchBinary("examples/" + example.dir + "/" + path));
	}));
	exampleFiles.set(example.dir, files);
	return files;
}

async function selectExample(dir) {
	const example = examples.find((ex) => ex.dir === dir);
	if (!example) {
		return;
	}
	if (current !== null) {
		edited.set(current.dir, sourceElement.value);
	}
	current = example;
	exampleSelect.value = dir;
	if (edited.has(dir)) {
		sourceElement.value = edited.get(dir);
	} else {
		sourceElement.value = "";
		updateStatus("Loading " + dir + "...");
		try {
			const files = await fetchExampleFiles(example);
			sourceElement.value = new TextDecoder().decode(files.get(example.main));
			updateStatus(compilerReady ? "Ready" : "Loading compiler...");
		} catch (e) {
			updateStatus("" + e);
		}
	}
	updateButtons();
	history.replaceState(null, "", "?example=" + encodeURIComponent(dir));
}

async function compileAndRun() {
	if (compiling || !compilerReady || current === null) {
		return;
	}
	compiling = true;
	updateButtons();
	updateStatus("Compiling...");
	consoleElement.textContent = "";
	stopGame();

	const example = current;
	const files = await fetchExampleFiles(example);
	const root = "odin/karl2d/examples/" + example.dir + "/";
	const list = [];
	for (const [path, data] of files) {
		list.push({path: root + path, data: path === example.main ? sourceElement.value : data});
	}
	list.push({path: root + "build/web/entry.odin", data: entrySource});
	worker.postMessage({
		type: "compile",
		files: list,
		dir: "/" + root + "build/web",
		flags: ["-target:js_wasm32"],
	});
}

function stopGame() {
	// Replacing the iframe stops the running program and everything it set up
	const fresh = document.createElement("iframe");
	fresh.id = "game";
	fresh.title = "Game";
	gameFrame.replaceWith(fresh);
	gameFrame = fresh;
}

function runProgram(bytes) {
	stopGame();
	const frame = gameFrame;
	const onReady = (e) => {
		if (e.source !== frame.contentWindow || !e.data || e.data.type !== "game-ready") {
			return;
		}
		window.removeEventListener("message", onReady);
		frame.contentWindow.postMessage({type: "run", wasm: bytes}, "*", [bytes]);
	};
	window.addEventListener("message", onReady);
	frame.src = "game.html";
}

window.addEventListener("message", (e) => {
	if (e.source !== gameFrame.contentWindow || !e.data) {
		return;
	}
	if (e.data.type === "game") {
		consoleElement.textContent += e.data.text;
	} else if (e.data.type === "game-started") {
		testHook(true);
	} else if (e.data.type === "game-crashed") {
		updateStatus("Program crashed");
		testHook(false);
	}
});

runButton.addEventListener("click", compileAndRun);
exampleSelect.addEventListener("change", () => selectExample(exampleSelect.value));

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

async function setup() {
	const params = new URLSearchParams(window.location.search);
	const [manifest, entry] = await Promise.all([fetch("examples/examples.json"), fetch("web_entry.odin")]);
	examples = await manifest.json();
	entrySource = await entry.text();
	for (const example of examples) {
		const option = document.createElement("option");
		option.value = example.dir;
		option.textContent = example.dir;
		exampleSelect.appendChild(option);
	}
	updateButtons();
	const wanted = params.get("example");
	await selectExample(examples.some((ex) => ex.dir === wanted) ? wanted : (examples.some((ex) => ex.dir === "basics") ? "basics" : examples[0].dir));
}

// Automated test: `?test=1` compiles and runs the selected example as soon as
// everything is loaded, then posts the result to `/log` and a capture of the
// game canvas (taken after three seconds) to `/png`.
const testParams = new URLSearchParams(window.location.search);
function testHook(ok) {
	if (!testParams.has("test")) {
		return;
	}
	const finish = (png) => {
		const text = statusElement.textContent + "\n" + consoleElement.textContent;
		fetch("/log", {method: "POST", body: (ok ? "OK\n" : "FAILED\n") + text}).then(() => fetch("/png", {method: "POST", body: png}));
	};
	if (!ok) {
		setTimeout(() => finish(""), 500);
		return;
	}
	const frameWindow = gameFrame.contentWindow;
	setTimeout(() => {
		// Runs right after the program's own frame callback, while the frame is still there
		frameWindow.requestAnimationFrame(() => {
			let png = "";
			try {
				const canvas = frameWindow.document.getElementById("webgl-canvas");
				const copy = document.createElement("canvas");
				copy.width = canvas.width;
				copy.height = canvas.height;
				copy.getContext("2d").drawImage(canvas, 0, 0);
				png = copy.toDataURL("image/png");
				consoleElement.textContent += "canvas: " + canvas.width + "x" + canvas.height + "\n";
			} catch (e) {
				consoleElement.textContent += "capture failed: " + e + "\n";
			}
			finish(png);
		});
	}, 3000);
}
if (testParams.has("test")) {
	window.onerror = (message) => {
		fetch("/log", {method: "POST", body: "ERROR " + message});
	};
	worker.onerror = (e) => {
		fetch("/log", {method: "POST", body: "WORKER ERROR " + e.message});
	};
	const waitForReady = setInterval(() => {
		if (compilerReady && current !== null && sourceElement.value !== "") {
			clearInterval(waitForReady);
			if (testParams.has("src")) {
				sourceElement.value = testParams.get("src");
			}
			compileAndRun();
		}
	}, 100);
}

setup().catch((e) => updateStatus("Failed to load: " + e));
