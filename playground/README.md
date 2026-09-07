# Odin Playground

Runs the Odin compiler in the browser. The compiler is built as a WebAssembly (WASI) module with `build_odin_wasi.sh` and generates code with the direct wasm backend (`-backend:wasm`), so no LLVM or linker is involved. Programs are compiled for `js_wasm32` and run on the page with the normal Odin JS runtime (`core/sys/wasm/js/odin.js`).

## Building

Needs `./odin` (built with `build_odin.sh`) and [wasi-sdk](https://github.com/WebAssembly/wasi-sdk) (default location `~/wasi-sdk`, override with `WASI_SDK=...`):

```
./playground/build_playground.sh
python3 -m http.server -d playground/dist 8000
```

`playground/dist` then contains:

- `odin.wasm`: the compiler
- `odin_root.pack`: the `base`, `core` and `vendor` sources and the wasm objects vendor packages link against, packed by `playground/pack_root`. The compiler reads them from an in-memory file system, `ODIN_ROOT` is `/odin`.
- `odin.js`, `index.html`, `playground.js`, `compiler_worker.js`, `wasi.js`: the page

## How it works

`compiler_worker.js` runs the compiler in a web worker. `wasi.js` implements the WASI preview 1 system calls the compiler uses (files, directories, arguments, environment, clocks, exit) over an in-memory file system. Every compile is a fresh instance of the compiler module with its own memory; the file system with the packed sources is shared. The editor contents are written to `/src/main.odin`, the compiler is run as `odin build /src -out:/out/main.wasm -backend:wasm <flags>` and the result is posted back to the page, which instantiates it with `odin.setupDefaultImports` and calls `_start` (and `step` every frame if the program exports one).

Opening `index.html?test=1` compiles and runs the default program automatically and posts the console contents to `/log`, which is what the headless browser test uses.
