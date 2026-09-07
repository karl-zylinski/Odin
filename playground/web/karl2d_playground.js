// The Karl2D playground page: lets the user pick one of the Karl2D examples,
// edit it, compile it with the compiler worker and run it in the game iframe.
//
// The examples are listed in examples.json (made by playground/pack_root) and
// fetched one by one when selected, so that only the assets of the chosen
// example are downloaded. A compile writes the example into the compiler's
// file system as /odin/karl2d/examples/<dir>/ (the library is packed as
// /odin/karl2d so the examples' `import k2 "../.."` keeps working) together
// with Karl2D's web entry point in build/web/, which is the package built.
//
// All the files of the example are listed below the editor; the .odin ones can
// be opened in the editor (some examples are made of several files). Edits are
// kept per file in `edited` so that switching file or example never loses them.

const editor = createEditor(document.getElementById("editor"));
const exampleSelect = document.getElementById("example");
const runButton = document.getElementById("run");
const statusElement = document.getElementById("status");
const consoleElement = document.getElementById("console");
const filesElement = document.getElementById("files");

let examples = [];          // examples.json
let exampleFiles = new Map(); // dir -> Map(path -> ArrayBuffer), fetched on demand
let entrySource = null;     // Karl2D's web entry point
let current = null;         // the selected example
let currentPath = null;     // the file of it that is open in the editor
let edited = new Map();     // dir + "/" + path -> edited source
let thumbnailUrls = [];     // object URLs of the file list, revoked when it is rebuilt
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
			markErrorLines();
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

// The compiler reports `path(line:column) Error: ...`; the lines of the file
// that is open are marked in the editor.
function markErrorLines() {
	if (currentPath === null) {
		return;
	}
	const lines = [];
	const pattern = /([^\s()]+)\((\d+):\d+\)\s+(?:Syntax )?Error/g;
	let match;
	while ((match = pattern.exec(consoleElement.textContent)) !== null) {
		if (match[1].endsWith("/" + currentPath) && lines.indexOf(+match[2]) === -1) {
			lines.push(+match[2]);
		}
	}
	editor.setErrorLines(lines);
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

function fileKey(dir, path) {
	return dir + "/" + path;
}

function isSource(path) {
	return path.endsWith(".odin");
}

// Keeps what is in the editor, so that opening another file or example and
// coming back shows the edits again.
function saveEditor() {
	if (current !== null && currentPath !== null) {
		edited.set(fileKey(current.dir, currentPath), editor.getValue());
	}
}

function openFile(path) {
	saveEditor();
	const files = exampleFiles.get(current.dir);
	const key = fileKey(current.dir, path);
	currentPath = path;
	editor.setValue(edited.has(key) ? edited.get(key) : new TextDecoder().decode(files.get(path)));
	updateFileList();
}

const IMAGE_TYPES = {png: "image/png", jpg: "image/jpeg", jpeg: "image/jpeg", gif: "image/gif", bmp: "image/bmp", webp: "image/webp"};

function fileExtension(path) {
	const name = path.slice(path.lastIndexOf("/") + 1);
	const dot = name.lastIndexOf(".");
	return dot <= 0 ? "" : name.slice(dot + 1).toLowerCase();
}

function formatSize(bytes) {
	if (bytes < 1024) {
		return bytes + " B";
	}
	if (bytes < 1024 * 1024) {
		return Math.round(bytes / 1024) + " kB";
	}
	return (bytes / (1024 * 1024)).toFixed(1) + " MB";
}

// Builds one entry of the file list. It only needs the bytes, so that files
// dropped onto the page (which is what this list is meant for next: adding new
// textures, sounds and fonts to an example) can be added the same way.
function makeFileEntry(path, data) {
	const extension = fileExtension(path);
	const size = formatSize(data.byteLength);
	const entry = document.createElement("div");
	entry.className = "file" + (isSource(path) ? " source" : "") + (path === currentPath ? " open" : "");
	entry.title = path + "\n" + size;

	const thumb = document.createElement("div");
	thumb.className = "file-thumb";
	if (IMAGE_TYPES[extension]) {
		const url = URL.createObjectURL(new Blob([data], {type: IMAGE_TYPES[extension]}));
		thumbnailUrls.push(url);
		const image = document.createElement("img");
		image.src = url;
		image.alt = path;
		thumb.appendChild(image);
	} else {
		// Everything the browser cannot show (tga, wav, ttf, json, ...) gets the
		// extension and the size instead of a thumbnail
		const label = document.createElement("div");
		label.innerHTML = '<span class="file-ext"></span><span class="file-size"></span>';
		label.querySelector(".file-ext").textContent = extension === "" ? "FILE" : extension.toUpperCase();
		label.querySelector(".file-size").textContent = size;
		thumb.appendChild(label);
	}
	entry.appendChild(thumb);

	const name = document.createElement("span");
	name.className = "file-name";
	name.textContent = path.slice(path.lastIndexOf("/") + 1);
	entry.appendChild(name);

	if (isSource(path)) {
		entry.addEventListener("click", () => openFile(path));
	}
	return entry;
}

// The main file first, then the other sources, then everything else
function sortedFiles(example) {
	const rank = (path) => path === example.main ? 0 : (isSource(path) ? 1 : 2);
	return example.files.slice().sort((a, b) => rank(a) - rank(b) || a.localeCompare(b));
}

function updateFileList() {
	for (const url of thumbnailUrls) {
		URL.revokeObjectURL(url);
	}
	thumbnailUrls = [];
	filesElement.textContent = "";
	if (current === null || !exampleFiles.has(current.dir)) {
		return;
	}
	const files = exampleFiles.get(current.dir);
	for (const path of sortedFiles(current)) {
		filesElement.appendChild(makeFileEntry(path, files.get(path)));
	}
}

async function selectExample(dir) {
	const example = examples.find((ex) => ex.dir === dir);
	if (!example) {
		return;
	}
	saveEditor();
	current = example;
	currentPath = null;
	exampleSelect.value = dir;
	editor.setValue("");
	updateFileList();
	if (!exampleFiles.has(dir)) {
		updateStatus("Loading " + dir + "...");
	}
	try {
		await fetchExampleFiles(example);
		if (current !== example) {
			return; // another example was picked while this one was loading
		}
		openFile(example.main);
		updateStatus(compilerReady ? "Ready" : "Loading compiler...");
	} catch (e) {
		updateStatus("" + e);
	}
	updateButtons();
	history.replaceState(null, "", "?example=" + encodeURIComponent(dir));
}

async function compileAndRun() {
	if (compiling || !compilerReady || current === null) {
		return;
	}
	compiling = true;
	saveEditor();
	editor.setErrorLines([]);
	updateButtons();
	updateStatus("Compiling...");
	consoleElement.textContent = "";
	stopGame();

	const example = current;
	const files = await fetchExampleFiles(example);
	const root = "odin/karl2d/examples/" + example.dir + "/";
	const list = [];
	for (const [path, data] of files) {
		// Every edited source is sent as text, the rest as the bytes fetched
		const key = fileKey(example.dir, path);
		list.push({path: root + path, data: edited.has(key) ? edited.get(key) : data});
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
		if (compilerReady && current !== null && editor.getValue() !== "") {
			clearInterval(waitForReady);
			if (testParams.has("src")) {
				editor.setValue(testParams.get("src"));
			}
			compileAndRun();
		}
	}, 100);
}

setup().catch((e) => updateStatus("Failed to load: " + e));
