// A small code editor for the playground: a transparent <textarea> on top of a
// syntax highlighted <pre>, with a line number gutter.
//
// There is no bundler and no CDN on the playground, so this is plain JS with no
// dependencies. The textarea does all the real work (caret, selection, undo,
// input methods, accessibility), the <pre> behind it only paints the colours,
// which is why both have to use the exact same font metrics and padding.
//
// Only the lines around the viewport are highlighted and put into the DOM: the
// Karl2D sources are up to a few thousand lines and re-building tens of
// thousands of <span>s on every keystroke is what makes editors like this
// laggy. The tokenizer still runs from the start of the text (a comment or a
// raw string can begin anywhere above), but it only builds HTML for the window,
// which is the part that costs.
//
// createEditor(container) -> {getValue, setValue, setErrorLines, focus, onChange}

const EDITOR_LINE_HEIGHT = 20;  // px, must match the line-height in the CSS of .ed
const EDITOR_MARGIN_LINES = 60; // lines highlighted above and below the viewport

const ODIN_KEYWORDS = new Set([
	"package", "import", "proc", "struct", "union", "enum", "return", "if", "else",
	"for", "in", "not_in", "switch", "case", "break", "continue", "defer", "when",
	"do", "fallthrough", "using", "distinct", "dynamic", "map", "bit_set", "bit_field",
	"matrix", "or_else", "or_return", "or_break", "or_continue", "foreign", "cast",
	"transmute", "auto_cast", "where", "context", "asm", "inline", "no_inline",
	"nil", "true", "false",
]);

const ODIN_TYPES = new Set([
	"int", "i8", "i16", "i32", "i64", "i128", "uint", "u8", "u16", "u32", "u64", "u128",
	"i16le", "i32le", "i64le", "i128le", "i16be", "i32be", "i64be", "i128be",
	"u16le", "u32le", "u64le", "u128le", "u16be", "u32be", "u64be", "u128be",
	"f16", "f32", "f64", "f16le", "f32le", "f64le", "f16be", "f32be", "f64be",
	"complex32", "complex64", "complex128", "quaternion64", "quaternion128", "quaternion256",
	"bool", "b8", "b16", "b32", "b64", "string", "cstring", "rune", "rawptr", "byte",
	"uintptr", "any", "typeid",
]);

// One pass over the text: comments, strings, runes, numbers, directives and
// identifiers. Block comments nest in Odin, which a regex cannot express, so
// `/*` is only matched here and then scanned by hand below.
const ODIN_TOKEN = /\/\/[^\n]*|\/\*|"(?:\\.|[^"\\\n])*"?|`[^`]*`?|'(?:\\.|[^'\\\n])*'?|[#@][A-Za-z_]\w*|@\(|0[xXbBoOdD][0-9a-fA-F_]+|\d[\d_]*(?:\.\d[\d_]*)?(?:[eE][+-]?\d+)?[a-zA-Z]*|[A-Za-z_]\w*/g;

function editorEscape(text) {
	if (text.indexOf("&") === -1 && text.indexOf("<") === -1) {
		return text;
	}
	return text.replace(/&/g, "&amp;").replace(/</g, "&lt;");
}

// Classifies an identifier by what surrounds it: `foo(` and `foo ::` are
// procedures, `k2.` is a package prefix.
function editorIdentifierClass(text, start, end) {
	const word = text.slice(start, end);
	if (ODIN_KEYWORDS.has(word)) {
		return "ed-kw";
	}
	if (ODIN_TYPES.has(word)) {
		return "ed-ty";
	}
	let i = end;
	while (i < text.length && (text[i] === " " || text[i] === "\t")) {
		i++;
	}
	if (text[i] === "(") {
		return "ed-fn";
	}
	if (text[i] === ":" && text[i + 1] === ":") {
		return "ed-fn";
	}
	if (text[end] === "." && !/[\w.)\]]/.test(text[start - 1] || " ")) {
		return "ed-pkg";
	}
	return null;
}

// Highlights text[from:to], but tokenizes from the start so that the state of
// block comments and raw strings is right no matter where the window begins.
function editorHighlight(text, from, to) {
	const out = [];
	let pos = 0;
	let match;
	ODIN_TOKEN.lastIndex = 0;
	const push = (start, end, cls) => {
		if (end <= from || start >= to) {
			return;
		}
		const part = editorEscape(text.slice(Math.max(start, from), Math.min(end, to)));
		out.push(cls ? '<span class="' + cls + '">' + part + "</span>" : part);
	};
	while ((match = ODIN_TOKEN.exec(text)) !== null) {
		const start = match.index;
		if (start >= to) {
			break;
		}
		let end = start + match[0].length;
		let cls;
		const first = match[0][0];
		if (match[0] === "/*") {
			// Nested block comment: scan to the matching close
			let depth = 1;
			let i = start + 2;
			while (i < text.length && depth > 0) {
				if (text[i] === "/" && text[i + 1] === "*") {
					depth++;
					i += 2;
				} else if (text[i] === "*" && text[i + 1] === "/") {
					depth--;
					i += 2;
				} else {
					i++;
				}
			}
			end = i;
			ODIN_TOKEN.lastIndex = end;
			cls = "ed-com";
		} else if (first === "/") {
			cls = "ed-com";
		} else if (first === '"' || first === "`" || first === "'") {
			cls = "ed-str";
		} else if (first === "#" || first === "@") {
			cls = "ed-dir";
		} else if (first >= "0" && first <= "9") {
			cls = "ed-num";
		} else {
			cls = editorIdentifierClass(text, start, end);
		}
		push(pos, start, null);
		push(start, end, cls);
		pos = end;
	}
	push(pos, text.length, null);
	return out.join("");
}

function createEditor(container) {
	container.classList.add("ed");
	container.innerHTML =
		'<div class="ed-gutter"><div class="ed-numbers"></div></div>' +
		'<div class="ed-wrap">' +
			'<div class="ed-errors"></div>' +
			'<pre class="ed-code" aria-hidden="true"></pre>' +
			'<textarea class="ed-input" spellcheck="false" autocomplete="off" autocorrect="off" autocapitalize="off" wrap="off"></textarea>' +
		"</div>";
	const gutter = container.querySelector(".ed-gutter");
	const numbers = container.querySelector(".ed-numbers");
	const wrap = container.querySelector(".ed-wrap");
	const errorLayer = container.querySelector(".ed-errors");
	const code = container.querySelector(".ed-code");
	const input = container.querySelector(".ed-input");

	let lineStarts = [0];   // offset of every line, for mapping lines to the text
	let renderedFrom = -1;  // the line window currently in the DOM
	let renderedTo = -1;
	let errorLines = [];
	let changeCallbacks = [];

	function measureLines() {
		const text = input.value;
		lineStarts = [0];
		for (let i = text.indexOf("\n"); i !== -1; i = text.indexOf("\n", i + 1)) {
			lineStarts.push(i + 1);
		}
	}

	// Puts the highlighted window and the line numbers in the DOM and moves both
	// to where the textarea is scrolled to.
	function render(force) {
		const scrollTop = input.scrollTop;
		const count = lineStarts.length;
		const visible = Math.ceil(input.clientHeight / EDITOR_LINE_HEIGHT) + 1;
		const firstVisible = Math.floor(scrollTop / EDITOR_LINE_HEIGHT);
		if (force || firstVisible < renderedFrom || firstVisible + visible > renderedTo) {
			const from = Math.max(0, firstVisible - EDITOR_MARGIN_LINES);
			const to = Math.min(count, firstVisible + visible + EDITOR_MARGIN_LINES);
			const text = input.value;
			code.innerHTML = editorHighlight(text, lineStarts[from], to < count ? lineStarts[to] : text.length);
			const parts = [];
			for (let line = from; line < to; line++) {
				const number = line + 1;
				parts.push(errorLines.indexOf(number) === -1 ? number : '<span class="ed-errnum">' + number + "</span>");
			}
			numbers.innerHTML = parts.join("\n");
			renderedFrom = from;
			renderedTo = to;
			code.style.top = from * EDITOR_LINE_HEIGHT + "px";
			numbers.style.top = from * EDITOR_LINE_HEIGHT + "px";
			gutter.style.width = Math.max(2, String(count).length) + "ch";
		}
		code.style.transform = "translate(" + -input.scrollLeft + "px, " + -scrollTop + "px)";
		numbers.style.transform = "translateY(" + -scrollTop + "px)";
		errorLayer.style.transform = "translateY(" + -scrollTop + "px)";
	}

	function renderErrors() {
		errorLayer.innerHTML = errorLines.map((line) =>
			'<div class="ed-errline" style="top: ' + (line - 1) * EDITOR_LINE_HEIGHT + 'px"></div>').join("");
	}

	// execCommand keeps the browser's own undo stack alive, which setRangeText
	// would throw away; it is deprecated but there is no replacement for that.
	function replaceSelection(text, selectionStart, selectionEnd) {
		if (selectionStart !== undefined) {
			input.setSelectionRange(selectionStart, selectionEnd);
		}
		let ok = false;
		try {
			ok = document.execCommand("insertText", false, text);
		} catch (e) {
			ok = false;
		}
		if (!ok) {
			input.setRangeText(text, input.selectionStart, input.selectionEnd, "end");
			input.dispatchEvent(new Event("input", {bubbles: true}));
		}
	}

	function lineStartOffset(offset) {
		return input.value.lastIndexOf("\n", offset - 1) + 1;
	}

	// Tab/Shift+Tab over a selection that covers more than one line indents or
	// dedents whole lines, like every other editor does.
	function indentSelection(dedent) {
		const text = input.value;
		const selectionStart = input.selectionStart;
		const selectionEnd = input.selectionEnd;
		const start = lineStartOffset(selectionStart);
		let end = text.indexOf("\n", selectionEnd);
		end = end === -1 ? text.length : end;
		let firstDelta = 0; // how much the first line moved, to keep a lone caret in place
		const changed = text.slice(start, end).split("\n").map((line, index) => {
			let delta = 0;
			let result = line;
			if (!dedent) {
				if (line !== "") {
					result = "\t" + line;
					delta = 1;
				}
			} else {
				let strip = 0;
				if (line.startsWith("\t")) {
					strip = 1;
				} else {
					while (strip < 4 && line[strip] === " ") {
						strip++;
					}
				}
				result = line.slice(strip);
				delta = -strip;
			}
			if (index === 0) {
				firstDelta = delta;
			}
			return result;
		}).join("\n");
		if (changed === text.slice(start, end)) {
			return;
		}
		replaceSelection(changed, start, end);
		if (selectionStart === selectionEnd) {
			const caret = Math.max(start, selectionStart + firstDelta);
			input.setSelectionRange(caret, caret);
		} else {
			input.setSelectionRange(start, start + changed.length);
		}
	}

	function onKeyDown(e) {
		// Ctrl+Enter (run) and every other shortcut stays with the page, and
		// while an input method is composing the keys are not ours either
		if (e.ctrlKey || e.metaKey || e.altKey || e.isComposing) {
			return;
		}
		if (e.key === "Tab") {
			e.preventDefault();
			const text = input.value;
			const multiLine = text.slice(input.selectionStart, input.selectionEnd).indexOf("\n") !== -1;
			if (e.shiftKey || multiLine) {
				indentSelection(e.shiftKey);
			} else {
				replaceSelection("\t");
			}
		} else if (e.key === "Enter") {
			e.preventDefault();
			const text = input.value;
			const start = lineStartOffset(input.selectionStart);
			const line = text.slice(start, input.selectionStart);
			const indent = (/^[\t ]*/.exec(line) || [""])[0];
			// A line that opens a block indents the next one
			const opens = /[{(\[]\s*$/.test(line.replace(/\/\/.*$/, ""));
			replaceSelection("\n" + indent + (opens ? "\t" : ""));
		}
	}

	function onInput() {
		measureLines();
		if (errorLines.length > 0) {
			// The reported lines have moved as soon as the text changes
			errorLines = [];
			renderErrors();
		}
		render(true);
		for (const callback of changeCallbacks) {
			callback();
		}
	}

	input.addEventListener("input", onInput);
	input.addEventListener("keydown", onKeyDown);
	input.addEventListener("scroll", () => render(false));
	if (window.ResizeObserver) {
		new ResizeObserver(() => render(true)).observe(wrap);
	}

	measureLines();
	render(true);

	return {
		getValue() {
			return input.value;
		},
		setValue(text) {
			input.value = text;
			input.scrollTop = 0;
			input.scrollLeft = 0;
			errorLines = [];
			renderErrors();
			measureLines();
			render(true);
		},
		setErrorLines(lines) {
			errorLines = lines.slice();
			renderErrors();
			render(true);
		},
		focus() {
			input.focus();
		},
		onChange(callback) {
			changeCallbacks.push(callback);
		},
	};
}
