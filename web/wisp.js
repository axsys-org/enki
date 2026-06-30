const encoder = new TextEncoder();
const decoder = new TextDecoder();

const els = {
  status: document.querySelector("#status"),
  prompt: document.querySelector("#prompt"),
  bundleState: document.querySelector("#bundle-state"),
  fileState: document.querySelector("#file-state"),
  runState: document.querySelector("#run-state"),
  transcriptMeta: document.querySelector("#transcript-meta"),
  terminal: document.querySelector("#terminal"),
  form: document.querySelector("#repl-form"),
  input: document.querySelector("#repl-input"),
  send: document.querySelector("#send"),
  reset: document.querySelector("#reset"),
};

let instance;
let bundledSource = null;
let transcript = "";

const bootStdlibInput = "(#bind std (#module std))\n(#import std)\n";

function byteLabel(n) {
  return n === 1 ? "1 byte" : `${n} bytes`;
}

function setStatus(text, tone = "ok") {
  els.status.textContent = text;
  els.status.classList.toggle("ok", tone === "ok");
  els.status.classList.toggle("fail", tone === "fail");
}

function setRunState(text, tone = "") {
  els.runState.textContent = text;
  els.runState.classList.toggle("ok", tone === "ok");
  els.runState.classList.toggle("fail", tone === "fail");
}

function setBusy(busy) {
  els.input.disabled = busy;
  els.send.disabled = busy;
  els.reset.disabled = busy;
}

function appendTranscript(text, cls = "") {
  if (text.length === 0) return;
  const escaped = text
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;");
  const classAttr = cls.length === 0 ? "" : ` class="entry ${cls}"`;
  transcript += `<span${classAttr}>${escaped}</span>`;
  els.terminal.innerHTML = transcript;
  els.transcriptMeta.textContent = byteLabel(encoder.encode(els.terminal.textContent).length);
  els.terminal.scrollTop = els.terminal.scrollHeight;
}

function clearTranscript() {
  transcript = "";
  els.terminal.textContent = "";
  els.transcriptMeta.textContent = "0 bytes";
}

function memory() {
  return instance.exports.memory;
}

function u8() {
  return new Uint8Array(memory().buffer);
}

function dv() {
  return new DataView(memory().buffer);
}

function allocBytes(bytes, nul = false) {
  const ptr = instance.exports.wisp_alloc(bytes.length + (nul ? 1 : 0));
  u8().set(bytes, ptr);
  if (nul) u8()[ptr + bytes.length] = 0;
  return ptr;
}

function allocString(text) {
  return allocBytes(encoder.encode(text), true);
}

function free(ptr, len) {
  instance.exports.wisp_free(ptr, len);
}

function readString(ptr, len) {
  if (!ptr || len === 0) return "";
  return decoder.decode(u8().slice(ptr, ptr + len));
}

function callWithString(text, fn) {
  const bytes = encoder.encode(text);
  const ptr = allocBytes(bytes, true);
  try {
    return fn(ptr, bytes.length);
  } finally {
    free(ptr, bytes.length + 1);
  }
}

function base64Bytes(text) {
  const bin = atob(text);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  return bytes;
}

async function loadBundledSource() {
  try {
    const res = await fetch(new URL("reaver-src.json", import.meta.url), {
      cache: "no-store",
    });
    if (!res.ok) return null;
    const bundle = await res.json();
    return Array.isArray(bundle.files) ? bundle : null;
  } catch {
    return null;
  }
}

function mountVirtualFile(path, data, mtime) {
  const pathBytes = encoder.encode(path);
  const pathPtr = allocBytes(pathBytes);
  const dataPtr = allocBytes(data);
  try {
    const rc = instance.exports.wisp_mount_file(
      pathPtr,
      pathBytes.length,
      dataPtr,
      data.length,
      BigInt(mtime || 0),
    );
    if (rc !== 0) throw new Error(`invalid path: ${path}`);
  } finally {
    free(pathPtr, pathBytes.length);
    free(dataPtr, data.length);
  }
}

function mountBundledSource() {
  instance.exports.wisp_clear_files();
  let mounted = 0;
  if (bundledSource !== null) {
    for (const file of bundledSource.files) {
      mountVirtualFile(file.path, base64Bytes(file.base64), file.mtime);
      mounted++;
    }
  }
  els.bundleState.textContent =
    bundledSource === null ? "bundle: none" : `bundle: ${mounted}`;
  els.bundleState.classList.toggle("ok", bundledSource !== null);
}

function setInput(text) {
  const bytes = encoder.encode(text);
  const ptr = allocBytes(bytes);
  try {
    instance.exports.wisp_set_input(ptr, bytes.length);
  } finally {
    free(ptr, bytes.length);
  }
}

function setFileRoot() {
  callWithString("reaver/src", (ptr, len) => {
    instance.exports.wisp_set_file_root(ptr, len);
  });
}

function output(channel) {
  return readString(
    instance.exports.wisp_output_ptr(channel),
    instance.exports.wisp_output_len(channel),
  );
}

function errorText() {
  return readString(
    instance.exports.wisp_error_ptr(),
    instance.exports.wisp_error_len(),
  );
}

function setEmitTopLevel(enabled) {
  if (instance.exports.wisp_set_emit_top_level)
    instance.exports.wisp_set_emit_top_level(enabled ? 1 : 0);
}

function virtualFileExists(path) {
  const count = Number(instance.exports.wisp_file_count());
  for (let i = 0; i < count; i++) {
    const current = readString(
      instance.exports.wisp_file_path_ptr(i),
      instance.exports.wisp_file_path_len(i),
    );
    if (current === path) return true;
  }
  return false;
}

function writtenFileCount() {
  const count = Number(instance.exports.wisp_file_count());
  let written = 0;
  for (let i = 0; i < count; i++)
    if (instance.exports.wisp_file_written(i) === 1) written++;
  return written;
}

function refreshFileState() {
  const root = virtualFileExists("snap/root.plan") ? "ready" : "missing";
  els.fileState.textContent = `snap: ${root} / ${writtenFileCount()}`;
  els.fileState.classList.toggle("ok", root === "ready");
  els.fileState.classList.toggle("fail", root !== "ready");
}

function invokeWisp(srcDir, mod, fn, stdin, options = {}) {
  setFileRoot();
  setInput(stdin);
  setEmitTopLevel(options.emitTopLevel === true);

  const strings = [srcDir, mod, fn];
  const stringPtrs = strings.map(allocString);
  const argvPtr = instance.exports.wisp_alloc(4);
  try {
    instance.exports.wisp_set_now(BigInt(Math.floor(Date.now() / 1000)));
    return instance.exports.wisp_run(
      stringPtrs[0],
      stringPtrs[1],
      stringPtrs[2],
      0,
      argvPtr,
    );
  } finally {
    stringPtrs.forEach((ptr, i) => free(ptr, encoder.encode(strings[i]).length + 1));
    free(argvPtr, 4);
  }
}

function runReset() {
  return invokeWisp("reaver/src/plan", "reaver", "main", "");
}

function runResume(stdin) {
  return invokeWisp("snap", "root", "_", stdin);
}

function captureRun(label, rc, showOutput) {
  const stdout = output(1);
  const stderr = output(2);
  const errors = errorText();
  setRunState(`rc: ${rc}`, rc === 0 ? "ok" : "fail");
  refreshFileState();

  if (!showOutput) {
    if (rc !== 0)
      appendTranscript(`${label}: ${errors || stderr || `failed (${rc})`}\n`, "error");
    return;
  }

  if (stdout.length > 0) appendTranscript(stdout.endsWith("\n") ? stdout : `${stdout}\n`);
  if (stderr.length > 0)
    appendTranscript(stderr.endsWith("\n") ? stderr : `${stderr}\n`, "stderr");
  if (errors.length > 0)
    appendTranscript(errors.endsWith("\n") ? errors : `${errors}\n`, "error");
}

async function ensureBooted(force = false) {
  setBusy(true);
  setStatus(force ? "resetting" : "booting");
  els.prompt.textContent = "x/boot";
  if (force) mountBundledSource();

  try {
    if (force || !virtualFileExists("snap/root.plan")) {
      let rc = runReset();
      captureRun("x/boot reset", rc, false);
      if (rc !== 0) throw new Error(errorText() || "x/boot reset failed");

      rc = runResume(bootStdlibInput);
      captureRun("x/boot load stdlib", rc, false);
      if (rc !== 0) throw new Error(errorText() || "x/boot load stdlib failed");
    }
    setStatus("ready");
    els.prompt.textContent = "snap root _";
  } finally {
    setBusy(false);
    els.input.focus();
  }
}

async function submitInput() {
  const text = els.input.value;
  if (text.trim().length === 0) return;
  els.input.value = "";
  appendTranscript(`> ${text.replace(/\s+$/, "")}\n`, "input");

  setBusy(true);
  setStatus("running");
  try {
    await ensureBooted();
    const stdin = text.endsWith("\n") ? text : `${text}\n`;
    const rc = runResume(stdin);
    captureRun("repl", rc, true);
    setStatus(rc === 0 ? "ready" : `failed (${rc})`, rc === 0 ? "ok" : "fail");
  } catch (err) {
    setStatus("failed", "fail");
    appendTranscript(`${err instanceof Error ? err.message : String(err)}\n`, "error");
  } finally {
    setBusy(false);
    els.input.focus();
  }
}

function wasiImports() {
  const errnoSuccess = 0;
  const errnoBadf = 8;
  return {
    wasi_snapshot_preview1: {
      args_get: () => errnoSuccess,
      args_sizes_get: (argc, argvBufSize) => {
        dv().setUint32(argc, 0, true);
        dv().setUint32(argvBufSize, 0, true);
        return errnoSuccess;
      },
      environ_get: () => errnoSuccess,
      environ_sizes_get: (count, size) => {
        dv().setUint32(count, 0, true);
        dv().setUint32(size, 0, true);
        return errnoSuccess;
      },
      clock_time_get: (_id, _precision, out) => {
        dv().setBigUint64(out, BigInt(Date.now()) * 1000000n, true);
        return errnoSuccess;
      },
      fd_close: () => errnoSuccess,
      fd_fdstat_get: () => errnoBadf,
      fd_fdstat_set_flags: () => errnoSuccess,
      fd_filestat_get: () => errnoBadf,
      fd_prestat_get: () => errnoBadf,
      fd_prestat_dir_name: () => errnoBadf,
      fd_read: () => errnoBadf,
      fd_seek: () => errnoBadf,
      fd_sync: () => errnoSuccess,
      fd_write: (fd, iovs, iovsLen, nwritten) => {
        let written = 0;
        let text = "";
        for (let i = 0; i < iovsLen; i++) {
          const base = dv().getUint32(iovs + i * 8, true);
          const len = dv().getUint32(iovs + i * 8 + 4, true);
          written += len;
          text += decoder.decode(u8().slice(base, base + len));
        }
        if (fd === 1) console.log(text);
        else if (fd === 2) console.error(text);
        dv().setUint32(nwritten, written, true);
        return errnoSuccess;
      },
      proc_exit: (code) => {
        throw new Error(`WASI proc_exit(${code})`);
      },
      random_get: (ptr, len) => {
        crypto.getRandomValues(u8().subarray(ptr, ptr + len));
        return errnoSuccess;
      },
      path_create_directory: () => errnoBadf,
      path_filestat_get: () => errnoBadf,
      path_open: () => errnoBadf,
      path_readlink: () => errnoBadf,
      path_remove_directory: () => errnoBadf,
      path_rename: () => errnoBadf,
      path_symlink: () => errnoBadf,
      path_unlink_file: () => errnoBadf,
    },
  };
}

async function load() {
  const wasm = await WebAssembly.instantiateStreaming(
    fetch(new URL("wisp.wasm", import.meta.url)),
    wasiImports(),
  );
  instance = wasm.instance;
  bundledSource = await loadBundledSource();
  mountBundledSource();
  refreshFileState();
  await ensureBooted();
}

setBusy(true);
updateStartupUi();

function updateStartupUi() {
  setRunState("rc: --");
  els.transcriptMeta.textContent = "0 bytes";
}

els.form.addEventListener("submit", (event) => {
  event.preventDefault();
  submitInput();
});

els.input.addEventListener("keydown", (event) => {
  if (event.key === "Enter" && !event.shiftKey) {
    event.preventDefault();
    submitInput();
  }
});

els.reset.addEventListener("click", async () => {
  clearTranscript();
  try {
    await ensureBooted(true);
  } catch (err) {
    setStatus("failed", "fail");
    appendTranscript(`${err instanceof Error ? err.message : String(err)}\n`, "error");
  }
});

load().catch((err) => {
  setStatus("failed to load wasm", "fail");
  els.bundleState.textContent = "bundle: failed";
  els.bundleState.classList.add("fail");
  appendTranscript(`${err instanceof Error ? err.stack || err.message : String(err)}\n`, "error");
  setBusy(false);
});
