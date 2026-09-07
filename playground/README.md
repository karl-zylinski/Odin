# Karl2D Playground

Runs the Odin compiler in the browser so that the [Karl2D](https://github.com/karl-zylinski/karl2d) examples can be edited, compiled and run on a web page: an example is picked from a dropdown, its source is shown in the editor, and Run compiles it and runs it in the pane next to it.

The compiler is built as a WebAssembly (WASI) module with `build_odin_wasi.sh` and generates code with the direct wasm backend (`-backend:wasm`), so no LLVM or linker is involved. Programs are compiled for `js_wasm32` and run with the normal Odin JS runtime (`core/sys/wasm/js/odin.js`) plus Karl2D's web audio backend.

## Building

Needs `./odin` (built with `build_odin.sh`), a Karl2D checkout (default `../karl2d`) and [wasi-sdk](https://github.com/WebAssembly/wasi-sdk) (default location `~/wasi-sdk`, override with `WASI_SDK=...`):

```
./playground/build_playground.sh [path/to/karl2d]
python3 -m http.server -d playground/dist 8000
```

Set `ODIN_WASM=path/to/odin.wasm` to reuse an already built compiler module instead of rebuilding it (which takes a few minutes).

`playground/dist` then contains:

- `odin.wasm`: the compiler (3.8 MB, 1.1 MB gzipped by the server)
- `odin_root.pack.gz`: the sources the compiler reads, packed by `playground/pack_root` and gzipped (9 MB becomes 2.3 MB). All of `base` and `core` except what can never be used in the browser (files whose name suffix or `#+build` tags exclude `js`/`wasm32`, the machine code library `core:rexcode`, the `core:sys` packages of other operating systems), the `vendor` packages Karl2D and `vendor:box2d` need together with their wasm objects, and the Karl2D library itself under `karl2d/`. The compiler reads them from an in-memory file system, `ODIN_ROOT` is `/odin`.
- `examples/`: the web capable Karl2D examples, one directory each, listed in `examples.json`. They are fetched when selected, so only the assets of the chosen example are downloaded.
- `web_entry.odin`: Karl2D's web entry point (`build_web/web_entry_templates/web_entry_template.odin`), compiled together with the example.
- `odin.js`, `audio_backend_web_audio.js`, `audio_backend_web_audio_processor.js`: the JS runtimes.
- `index.html`, `karl2d_playground.js`, `editor.js`, `game.html`, `compiler_worker.js`, `wasi.js`: the page. `editor.js` is the code editor (a transparent textarea over a syntax highlighted `<pre>`, no dependencies). `generic.html` + `playground.js` is a plain Odin playground (one source file, no Karl2D) on top of the same compiler worker.

## How it works

`compiler_worker.js` runs the compiler in a web worker. `wasi.js` implements the WASI preview 1 system calls the compiler uses (files, directories, arguments, environment, clocks, exit) over an in-memory file system. Every compile is a fresh instance of the compiler module with its own memory; the file system with the packed sources is shared.

For a run, the page writes the example's files (with the editor contents as the main file) to `/odin/karl2d/examples/<name>/` and the web entry point to `/odin/karl2d/examples/<name>/build/web/entry.odin`, so that the examples' `import k2 "../.."` and the entry's `import ex "../.."` resolve as they do in the Karl2D repository. The compiler is run as `odin build /odin/karl2d/examples/<name>/build/web -out:/out/main.wasm -backend:wasm -target:js_wasm32`. The result is posted to a fresh `game.html` iframe, which instantiates it the way Karl2D's own web template does (`odin.setupDefaultImports` plus `karl2dAudioJsImports`, `_start`, then `step` every frame until it returns false). A new iframe per run means nothing from the previous program lingers.

Opening `index.html?test=1&example=<name>` compiles and runs the example automatically, then posts the status and console text to `/log` and a capture of the game canvas to `/png`, which is what the headless browser test uses.
