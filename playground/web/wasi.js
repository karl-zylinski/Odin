// Minimal WASI preview 1 implementation over an in-memory file system, enough
// to run the Odin compiler (odin.wasm) in a browser. All files live in
// `WasiFileSystem`, paths are absolute and the single preopened directory is
// the root, so the compiler sees "/odin/base/...", "/src/main.odin" and so on.

const WASI_ESUCCESS = 0;
const WASI_EBADF    = 8;
const WASI_EEXIST   = 20;
const WASI_EINVAL   = 28;
const WASI_EIO      = 29;
const WASI_EISDIR   = 31;
const WASI_ENOENT   = 44;
const WASI_ENOTDIR  = 54;
const WASI_ENOTEMPTY = 55;
const WASI_ENOSYS   = 52;

const WASI_FILETYPE_CHARACTER_DEVICE = 2;
const WASI_FILETYPE_DIRECTORY        = 3;
const WASI_FILETYPE_REGULAR_FILE     = 4;

const WASI_OFLAGS_CREAT     = 1;
const WASI_OFLAGS_DIRECTORY = 2;
const WASI_OFLAGS_EXCL      = 4;
const WASI_OFLAGS_TRUNC     = 8;

class WasiExit {
	constructor(code) {
		this.code = code;
	}
}

// Paths are stored normalized: no leading or trailing slash, the root is "".
function wasiNormalizePath(path) {
	const parts = [];
	for (const part of path.split("/")) {
		if (part === "" || part === ".") {
			continue;
		}
		if (part === "..") {
			parts.pop();
			continue;
		}
		parts.push(part);
	}
	return parts.join("/");
}

function wasiParentPath(path) {
	const i = path.lastIndexOf("/");
	return i < 0 ? "" : path.slice(0, i);
}

class WasiFileSystem {
	constructor() {
		this.files = new Map(); // path -> {data: Uint8Array, size: number}
		this.dirs  = new Set([""]);
	}

	mkdirAll(path) {
		path = wasiNormalizePath(path);
		const parts = path === "" ? [] : path.split("/");
		let cur = "";
		for (const part of parts) {
			cur = cur === "" ? part : cur + "/" + part;
			this.dirs.add(cur);
		}
	}

	writeFile(path, bytes) {
		path = wasiNormalizePath(path);
		this.mkdirAll(wasiParentPath(path));
		this.files.set(path, {data: bytes, size: bytes.length});
	}

	readFile(path) {
		const f = this.files.get(wasiNormalizePath(path));
		return f ? f.data.subarray(0, f.size) : null;
	}

	// Loads a file made by playground/pack_root under `prefix`
	loadPack(buffer, prefix) {
		const bytes = new Uint8Array(buffer);
		const view = new DataView(buffer);
		const magic = new TextDecoder().decode(bytes.subarray(0, 8));
		if (magic !== "ODINPK01") {
			throw new Error("Bad pack file");
		}
		const decoder = new TextDecoder();
		const count = view.getUint32(8, true);
		let pos = 12;
		for (let i = 0; i < count; i++) {
			const pathLen = view.getUint32(pos, true); pos += 4;
			const path = decoder.decode(bytes.subarray(pos, pos+pathLen)); pos += pathLen;
			const dataLen = view.getUint32(pos, true); pos += 4;
			this.writeFile(prefix + "/" + path, bytes.subarray(pos, pos+dataLen)); pos += dataLen;
		}
	}
}

class Wasi {
	// stdout/stderr are callbacks taking a string
	constructor(fs, args, env, stdout, stderr) {
		this.fs = fs;
		this.args = args;
		this.env = env;
		this.stdout = stdout;
		this.stderr = stderr;
		this.memory = null;
		this.fds = new Map();
		this.nextFd = 4;
		this.fds.set(0, {kind: "stdin"});
		this.fds.set(1, {kind: "stdout"});
		this.fds.set(2, {kind: "stderr"});
		this.fds.set(3, {kind: "dir", path: ""});
		this.decoder = new TextDecoder();
		this.encoder = new TextEncoder();
		this.lineBuffers = ["", "", ""];
	}

	setMemory(memory) {
		this.memory = memory;
	}

	view() {
		return new DataView(this.memory.buffer);
	}

	bytes() {
		return new Uint8Array(this.memory.buffer);
	}

	readString(ptr, len) {
		return this.decoder.decode(this.bytes().subarray(ptr, ptr+len));
	}

	resolvePath(dirFd, ptr, len) {
		const d = this.fds.get(dirFd);
		if (!d || d.kind !== "dir") {
			return null;
		}
		const rel = this.readString(ptr, len);
		if (rel.startsWith("/")) {
			return wasiNormalizePath(rel);
		}
		return wasiNormalizePath(d.path + "/" + rel);
	}

	writeStd(fdIndex, str) {
		// Emit whole lines; the compiler prints diagnostics piecewise
		let buf = this.lineBuffers[fdIndex] + str;
		const i = buf.lastIndexOf("\n");
		if (i >= 0) {
			(fdIndex === 1 ? this.stdout : this.stderr)(buf.slice(0, i+1));
			buf = buf.slice(i+1);
		}
		this.lineBuffers[fdIndex] = buf;
	}

	flush() {
		for (let i = 1; i <= 2; i++) {
			if (this.lineBuffers[i] !== "") {
				(i === 1 ? this.stdout : this.stderr)(this.lineBuffers[i]);
				this.lineBuffers[i] = "";
			}
		}
	}

	writeFilestat(buf, filetype, size) {
		const v = this.view();
		v.setBigUint64(buf+0,  0n, true);          // dev
		v.setBigUint64(buf+8,  0n, true);          // ino
		v.setUint8(buf+16, filetype);
		v.setBigUint64(buf+24, 1n, true);          // nlink
		v.setBigUint64(buf+32, BigInt(size), true);
		v.setBigUint64(buf+40, 0n, true);          // atim
		v.setBigUint64(buf+48, 0n, true);          // mtim
		v.setBigUint64(buf+56, 0n, true);          // ctim
	}

	statPath(path, buf) {
		if (this.fs.dirs.has(path)) {
			this.writeFilestat(buf, WASI_FILETYPE_DIRECTORY, 0);
			return WASI_ESUCCESS;
		}
		const f = this.fs.files.get(path);
		if (f) {
			this.writeFilestat(buf, WASI_FILETYPE_REGULAR_FILE, f.size);
			return WASI_ESUCCESS;
		}
		return WASI_ENOENT;
	}

	ensureCapacity(f, size) {
		if (size <= f.data.length) {
			return;
		}
		let cap = Math.max(f.data.length*2, 4096);
		while (cap < size) {
			cap *= 2;
		}
		const data = new Uint8Array(cap);
		data.set(f.data.subarray(0, f.size));
		f.data = data;
	}

	// Reads the iovs into `bytes` starting at `offset` of the file; returns the count read
	readIovs(f, iovs, iovsLen, offset) {
		const v = this.view();
		const mem = this.bytes();
		let total = 0;
		for (let i = 0; i < iovsLen; i++) {
			const ptr = v.getUint32(iovs + i*8, true);
			const len = v.getUint32(iovs + i*8 + 4, true);
			const n = Math.max(0, Math.min(len, f.size - offset));
			mem.set(f.data.subarray(offset, offset+n), ptr);
			offset += n;
			total += n;
			if (n < len) {
				break;
			}
		}
		return total;
	}

	writeIovs(f, iovs, iovsLen, offset) {
		const v = this.view();
		const mem = this.bytes();
		let total = 0;
		for (let i = 0; i < iovsLen; i++) {
			const ptr = v.getUint32(iovs + i*8, true);
			const len = v.getUint32(iovs + i*8 + 4, true);
			this.ensureCapacity(f, offset+len);
			f.data.set(mem.subarray(ptr, ptr+len), offset);
			offset += len;
			f.size = Math.max(f.size, offset);
			total += len;
		}
		return total;
	}

	imports() {
		const self = this;
		return {
			args_sizes_get(argcPtr, argvBufSizePtr) {
				const v = self.view();
				let size = 0;
				for (const a of self.args) {
					size += self.encoder.encode(a).length + 1;
				}
				v.setUint32(argcPtr, self.args.length, true);
				v.setUint32(argvBufSizePtr, size, true);
				return WASI_ESUCCESS;
			},
			args_get(argvPtr, argvBuf) {
				return self.writeStringList(self.args, argvPtr, argvBuf);
			},
			environ_sizes_get(countPtr, bufSizePtr) {
				const v = self.view();
				let size = 0;
				for (const e of self.env) {
					size += self.encoder.encode(e).length + 1;
				}
				v.setUint32(countPtr, self.env.length, true);
				v.setUint32(bufSizePtr, size, true);
				return WASI_ESUCCESS;
			},
			environ_get(envPtr, envBuf) {
				return self.writeStringList(self.env, envPtr, envBuf);
			},
			clock_res_get(id, resPtr) {
				self.view().setBigUint64(resPtr, 1000n, true);
				return WASI_ESUCCESS;
			},
			clock_time_get(id, precision, timePtr) {
				let ns;
				if (id === 0) {
					ns = BigInt(Date.now()) * 1000000n;
				} else {
					ns = BigInt(Math.round(performance.now() * 1000)) * 1000n;
				}
				self.view().setBigUint64(timePtr, ns, true);
				return WASI_ESUCCESS;
			},
			fd_close(fd) {
				if (!self.fds.has(fd)) {
					return WASI_EBADF;
				}
				if (fd > 3) {
					self.fds.delete(fd);
				}
				return WASI_ESUCCESS;
			},
			fd_fdstat_get(fd, buf) {
				const d = self.fds.get(fd);
				if (!d) {
					return WASI_EBADF;
				}
				const v = self.view();
				let filetype = WASI_FILETYPE_CHARACTER_DEVICE;
				if (d.kind === "dir") {
					filetype = WASI_FILETYPE_DIRECTORY;
				} else if (d.kind === "file") {
					filetype = WASI_FILETYPE_REGULAR_FILE;
				}
				v.setUint8(buf, filetype);
				v.setUint16(buf+2, 0, true);
				v.setBigUint64(buf+8, 0xFFFFFFFFFFFFFFFFn, true);
				v.setBigUint64(buf+16, 0xFFFFFFFFFFFFFFFFn, true);
				return WASI_ESUCCESS;
			},
			fd_filestat_get(fd, buf) {
				const d = self.fds.get(fd);
				if (!d) {
					return WASI_EBADF;
				}
				if (d.kind === "dir") {
					self.writeFilestat(buf, WASI_FILETYPE_DIRECTORY, 0);
				} else if (d.kind === "file") {
					self.writeFilestat(buf, WASI_FILETYPE_REGULAR_FILE, d.file.size);
				} else {
					self.writeFilestat(buf, WASI_FILETYPE_CHARACTER_DEVICE, 0);
				}
				return WASI_ESUCCESS;
			},
			fd_filestat_set_size(fd, size) {
				const d = self.fds.get(fd);
				if (!d || d.kind !== "file") {
					return WASI_EBADF;
				}
				size = Number(size);
				self.ensureCapacity(d.file, size);
				if (size > d.file.size) {
					d.file.data.fill(0, d.file.size, size);
				}
				d.file.size = size;
				return WASI_ESUCCESS;
			},
			fd_prestat_get(fd, buf) {
				if (fd !== 3) {
					return WASI_EBADF;
				}
				const v = self.view();
				v.setUint8(buf, 0); // preopentype dir
				v.setUint32(buf+4, 1, true); // name length of "/"
				return WASI_ESUCCESS;
			},
			fd_prestat_dir_name(fd, pathPtr, pathLen) {
				if (fd !== 3) {
					return WASI_EBADF;
				}
				if (pathLen >= 1) {
					self.bytes()[pathPtr] = "/".charCodeAt(0);
				}
				return WASI_ESUCCESS;
			},
			fd_read(fd, iovs, iovsLen, nreadPtr) {
				const d = self.fds.get(fd);
				if (!d) {
					return WASI_EBADF;
				}
				if (d.kind === "stdin") {
					self.view().setUint32(nreadPtr, 0, true);
					return WASI_ESUCCESS;
				}
				if (d.kind !== "file") {
					return WASI_EBADF;
				}
				const n = self.readIovs(d.file, iovs, iovsLen, d.offset);
				d.offset += n;
				self.view().setUint32(nreadPtr, n, true);
				return WASI_ESUCCESS;
			},
			fd_pread(fd, iovs, iovsLen, offset, nreadPtr) {
				const d = self.fds.get(fd);
				if (!d || d.kind !== "file") {
					return WASI_EBADF;
				}
				const n = self.readIovs(d.file, iovs, iovsLen, Number(offset));
				self.view().setUint32(nreadPtr, n, true);
				return WASI_ESUCCESS;
			},
			fd_write(fd, iovs, iovsLen, nwrittenPtr) {
				const d = self.fds.get(fd);
				if (!d) {
					return WASI_EBADF;
				}
				if (d.kind === "stdout" || d.kind === "stderr") {
					const v = self.view();
					let total = 0;
					for (let i = 0; i < iovsLen; i++) {
						const ptr = v.getUint32(iovs + i*8, true);
						const len = v.getUint32(iovs + i*8 + 4, true);
						self.writeStd(fd, self.readString(ptr, len));
						total += len;
					}
					v.setUint32(nwrittenPtr, total, true);
					return WASI_ESUCCESS;
				}
				if (d.kind !== "file") {
					return WASI_EBADF;
				}
				const n = self.writeIovs(d.file, iovs, iovsLen, d.offset);
				d.offset += n;
				self.view().setUint32(nwrittenPtr, n, true);
				return WASI_ESUCCESS;
			},
			fd_pwrite(fd, iovs, iovsLen, offset, nwrittenPtr) {
				const d = self.fds.get(fd);
				if (!d || d.kind !== "file") {
					return WASI_EBADF;
				}
				const n = self.writeIovs(d.file, iovs, iovsLen, Number(offset));
				self.view().setUint32(nwrittenPtr, n, true);
				return WASI_ESUCCESS;
			},
			fd_seek(fd, offset, whence, newOffsetPtr) {
				const d = self.fds.get(fd);
				if (!d) {
					return WASI_EBADF;
				}
				if (d.kind !== "file") {
					return WASI_EBADF;
				}
				offset = Number(offset);
				let base = 0;
				if (whence === 1) {
					base = d.offset;
				} else if (whence === 2) {
					base = d.file.size;
				} else if (whence !== 0) {
					return WASI_EINVAL;
				}
				if (base + offset < 0) {
					return WASI_EINVAL;
				}
				d.offset = base + offset;
				self.view().setBigUint64(newOffsetPtr, BigInt(d.offset), true);
				return WASI_ESUCCESS;
			},
			fd_readdir(fd, buf, bufLen, cookie, bufUsedPtr) {
				const d = self.fds.get(fd);
				if (!d || d.kind !== "dir") {
					return WASI_EBADF;
				}
				if (!d.entries) {
					d.entries = self.listDir(d.path);
				}
				const v = self.view();
				const mem = self.bytes();
				let pos = 0;
				for (let i = Number(cookie); i < d.entries.length; i++) {
					const e = d.entries[i];
					const name = self.encoder.encode(e.name);
					const entry = new Uint8Array(24 + name.length);
					const ev = new DataView(entry.buffer);
					ev.setBigUint64(0, BigInt(i+1), true);
					ev.setBigUint64(8, BigInt(i+1), true);
					ev.setUint32(16, name.length, true);
					ev.setUint8(20, e.isDir ? WASI_FILETYPE_DIRECTORY : WASI_FILETYPE_REGULAR_FILE);
					entry.set(name, 24);
					const n = Math.min(entry.length, bufLen - pos);
					mem.set(entry.subarray(0, n), buf + pos);
					pos += n;
					if (n < entry.length) {
						break;
					}
				}
				v.setUint32(bufUsedPtr, pos, true);
				return WASI_ESUCCESS;
			},
			path_create_directory(fd, pathPtr, pathLen) {
				const path = self.resolvePath(fd, pathPtr, pathLen);
				if (path === null) {
					return WASI_EBADF;
				}
				if (self.fs.dirs.has(path) || self.fs.files.has(path)) {
					return WASI_EEXIST;
				}
				if (!self.fs.dirs.has(wasiParentPath(path))) {
					return WASI_ENOENT;
				}
				self.fs.dirs.add(path);
				return WASI_ESUCCESS;
			},
			path_filestat_get(fd, flags, pathPtr, pathLen, buf) {
				const path = self.resolvePath(fd, pathPtr, pathLen);
				if (path === null) {
					return WASI_EBADF;
				}
				return self.statPath(path, buf);
			},
			path_open(dirFd, dirFlags, pathPtr, pathLen, oflags, rightsBase, rightsInheriting, fdFlags, fdPtr) {
				const path = self.resolvePath(dirFd, pathPtr, pathLen);
				if (path === null) {
					return WASI_EBADF;
				}
				const v = self.view();
				if (self.fs.dirs.has(path)) {
					if ((oflags & WASI_OFLAGS_EXCL) && (oflags & WASI_OFLAGS_CREAT)) {
						return WASI_EEXIST;
					}
					const fd = self.nextFd++;
					self.fds.set(fd, {kind: "dir", path: path});
					v.setUint32(fdPtr, fd, true);
					return WASI_ESUCCESS;
				}
				if (oflags & WASI_OFLAGS_DIRECTORY) {
					return self.fs.files.has(path) ? WASI_ENOTDIR : WASI_ENOENT;
				}
				let file = self.fs.files.get(path);
				if (file) {
					if ((oflags & WASI_OFLAGS_EXCL) && (oflags & WASI_OFLAGS_CREAT)) {
						return WASI_EEXIST;
					}
					if (oflags & WASI_OFLAGS_TRUNC) {
						file.size = 0;
					}
				} else {
					if (!(oflags & WASI_OFLAGS_CREAT)) {
						return WASI_ENOENT;
					}
					if (!self.fs.dirs.has(wasiParentPath(path))) {
						return WASI_ENOENT;
					}
					file = {data: new Uint8Array(0), size: 0};
					self.fs.files.set(path, file);
				}
				const fd = self.nextFd++;
				// fdflags append = 1
				self.fds.set(fd, {kind: "file", path: path, file: file, offset: (fdFlags & 1) ? file.size : 0});
				v.setUint32(fdPtr, fd, true);
				return WASI_ESUCCESS;
			},
			path_remove_directory(fd, pathPtr, pathLen) {
				const path = self.resolvePath(fd, pathPtr, pathLen);
				if (path === null) {
					return WASI_EBADF;
				}
				if (!self.fs.dirs.has(path)) {
					return self.fs.files.has(path) ? WASI_ENOTDIR : WASI_ENOENT;
				}
				if (self.listDir(path).length > 0) {
					return WASI_ENOTEMPTY;
				}
				self.fs.dirs.delete(path);
				return WASI_ESUCCESS;
			},
			path_unlink_file(fd, pathPtr, pathLen) {
				const path = self.resolvePath(fd, pathPtr, pathLen);
				if (path === null) {
					return WASI_EBADF;
				}
				if (!self.fs.files.has(path)) {
					return self.fs.dirs.has(path) ? WASI_EISDIR : WASI_ENOENT;
				}
				self.fs.files.delete(path);
				return WASI_ESUCCESS;
			},
			proc_exit(code) {
				throw new WasiExit(code);
			},
			random_get(ptr, len) {
				crypto.getRandomValues(self.bytes().subarray(ptr, ptr+len));
				return WASI_ESUCCESS;
			},
			sched_yield() {
				return WASI_ESUCCESS;
			},
			poll_oneoff() {
				return WASI_ENOSYS;
			},
		};
	}

	writeStringList(list, ptrs, buf) {
		const view = this.view();
		const mem = this.bytes();
		let pos = buf;
		for (let i = 0; i < list.length; i++) {
			const bytes = this.encoder.encode(list[i]);
			view.setUint32(ptrs + i*4, pos, true);
			mem.set(bytes, pos);
			mem[pos + bytes.length] = 0;
			pos += bytes.length + 1;
		}
		return WASI_ESUCCESS;
	}

	listDir(path) {
		const entries = [];
		const prefix = path === "" ? "" : path + "/";
		for (const d of this.fs.dirs) {
			if (d !== "" && d.startsWith(prefix) && d.indexOf("/", prefix.length) < 0 && d !== path) {
				entries.push({name: d.slice(prefix.length), isDir: true});
			}
		}
		for (const f of this.fs.files.keys()) {
			if (f.startsWith(prefix) && f.indexOf("/", prefix.length) < 0) {
				entries.push({name: f.slice(prefix.length), isDir: false});
			}
		}
		entries.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
		return entries;
	}
}

// Instantiates `module` with the WASI imports and runs its `_start`.
// Returns the exit code.
function wasiRun(module, wasi) {
	const instance = new WebAssembly.Instance(module, {wasi_snapshot_preview1: wasi.imports()});
	wasi.setMemory(instance.exports.memory);
	let code = 0;
	try {
		instance.exports._start();
	} catch (e) {
		if (e instanceof WasiExit) {
			code = e.code;
		} else {
			wasi.flush();
			throw e;
		}
	}
	wasi.flush();
	return code;
}
