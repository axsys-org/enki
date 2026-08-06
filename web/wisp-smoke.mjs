import { Buffer } from "node:buffer";
import { readFile } from "node:fs/promises";
import { resolve } from "node:path";
import { JPlanHost } from "./jplan-host.mjs";

const encoder = new TextEncoder();
const decoder = new TextDecoder();

const wasmPath = resolve(process.argv[2] ?? "build/wasm/browser/wisp.wasm");
const repoRoot = resolve(process.argv[3] ?? ".");

let instance;
const jplanHost = new JPlanHost(() => memory());
const wormholes = jplanHost.wormholes;

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
  return ptr && len ? decoder.decode(u8().slice(ptr, ptr + len)) : "";
}

function output(channel) {
  return readString(
    instance.exports.wisp_output_ptr(channel),
    instance.exports.wisp_output_len(channel),
  );
}

function errors() {
  return readString(
    instance.exports.wisp_error_ptr(),
    instance.exports.wisp_error_len(),
  );
}

function wasiImports() {
  return {
    enki: jplanHost.imports(),
    wasi_snapshot_preview1: {
      args_get: () => 0,
      args_sizes_get: (argc, argvBufSize) => {
        dv().setUint32(argc, 0, true);
        dv().setUint32(argvBufSize, 0, true);
        return 0;
      },
      environ_get: () => 0,
      environ_sizes_get: (count, size) => {
        dv().setUint32(count, 0, true);
        dv().setUint32(size, 0, true);
        return 0;
      },
      clock_time_get: (_id, _precision, out) => {
        dv().setBigUint64(out, BigInt(Date.now()) * 1000000n, true);
        return 0;
      },
      fd_close: () => 0,
      fd_fdstat_get: () => 8,
      fd_fdstat_set_flags: () => 0,
      fd_filestat_get: () => 8,
      fd_prestat_get: () => 8,
      fd_prestat_dir_name: () => 8,
      fd_read: () => 8,
      fd_seek: () => 8,
      fd_sync: () => 0,
      fd_write: (fd, iovs, iovsLen, nwritten) => {
        let written = 0;
        let text = "";
        for (let i = 0; i < iovsLen; i++) {
          const base = dv().getUint32(iovs + i * 8, true);
          const len = dv().getUint32(iovs + i * 8 + 4, true);
          written += len;
          text += decoder.decode(u8().slice(base, base + len));
        }
        if (text.length > 0) {
          if (fd === 2) process.stderr.write(text);
          else process.stdout.write(text);
        }
        dv().setUint32(nwritten, written, true);
        return 0;
      },
      proc_exit: (code) => {
        throw new Error(`WASI proc_exit(${code})`);
      },
      random_get: (ptr, len) => {
        crypto.getRandomValues(u8().subarray(ptr, ptr + len));
        return 0;
      },
      path_create_directory: () => 8,
      path_filestat_get: () => 8,
      path_open: () => 8,
      path_readlink: () => 8,
      path_remove_directory: () => 8,
      path_rename: () => 8,
      path_symlink: () => 8,
      path_unlink_file: () => 8,
    },
  };
}

async function load() {
  const wasm = await readFile(wasmPath);
  const result = await WebAssembly.instantiate(wasm, wasiImports());
  instance = result.instance;
}

function mount(path, text, mtime = 1n) {
  const pathBytes = encoder.encode(path);
  const dataBytes = encoder.encode(text);
  const pathPtr = allocBytes(pathBytes);
  const dataPtr = allocBytes(dataBytes);
  try {
    const rc = instance.exports.wisp_mount_file(
      pathPtr,
      pathBytes.length,
      dataPtr,
      dataBytes.length,
      mtime,
    );
    if (rc !== 0) throw new Error(`mount failed: ${path}`);
  } finally {
    free(pathPtr, pathBytes.length);
    free(dataPtr, dataBytes.length);
  }
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

function setFileRoot(path) {
  const bytes = encoder.encode(path);
  const ptr = allocBytes(bytes);
  try {
    instance.exports.wisp_set_file_root(ptr, bytes.length);
  } finally {
    free(ptr, bytes.length);
  }
}

function run(srcDir, mod, fn = "", args = []) {
  const strings = [srcDir, mod, fn, ...args];
  const ptrs = strings.map(allocString);
  const argvPtr = instance.exports.wisp_alloc(args.length * 4 || 4);
  try {
    args.forEach((_, i) => dv().setUint32(argvPtr + i * 4, ptrs[3 + i], true));
    instance.exports.wisp_set_now(1234n);
    return instance.exports.wisp_run(ptrs[0], ptrs[1], ptrs[2], args.length, argvPtr);
  } finally {
    ptrs.forEach((ptr, i) => free(ptr, encoder.encode(strings[i]).length + 1));
    free(argvPtr, args.length * 4 || 4);
  }
}

function runJplan(environment, objects, source) {
  const environmentToken = wormholes.adopt(environment);
  const objectTokens = objects.map((object) => wormholes.adopt(object));
  const objectTokensPtr = instance.exports.wisp_alloc(
    objectTokens.length * 8 || 8,
  );
  const sourceBytes = encoder.encode(source);
  const sourcePtr = allocBytes(sourceBytes);
  try {
    objectTokens.forEach((token, i) =>
      dv().setBigUint64(objectTokensPtr + i * 8, token, true),
    );
    return instance.exports.wisp_jplan_run(
      environmentToken,
      objectTokensPtr,
      objectTokens.length,
      sourcePtr,
      sourceBytes.length,
    );
  } finally {
    free(objectTokensPtr, objectTokens.length * 8 || 8);
    free(sourcePtr, sourceBytes.length);
  }
}

function runJplanDispatch(environment, action, payload = "") {
  environment.pendingAction = action;
  environment.pendingPayload = payload;
  const environmentToken = wormholes.adopt(environment);
  const actionBytes = encoder.encode(action);
  const payloadBytes = encoder.encode(payload);
  const actionPtr = instance.exports.wisp_alloc(actionBytes.length || 1);
  const payloadPtr = instance.exports.wisp_alloc(payloadBytes.length || 1);
  u8().set(actionBytes, actionPtr);
  u8().set(payloadBytes, payloadPtr);
  try {
    return instance.exports.wisp_jplan_dispatch(
      environmentToken,
      actionPtr,
      actionBytes.length,
      payloadPtr,
      payloadBytes.length,
    );
  } finally {
    free(actionPtr, actionBytes.length || 1);
    free(payloadPtr, payloadBytes.length || 1);
  }
}

class FakeNode {
  constructor(tagName) {
    this.tagName = tagName;
    this.children = [];
    this.dataset = {};
    this.className = "";
    this.textContent = "";
    this.innerHTML = "";
    this.value = "";
    this.id = "";
    this.listeners = new Map();
  }

  append(...children) {
    for (const child of children) {
      if (child.tagName === "#fragment") this.children.push(...child.children);
      else this.children.push(child);
    }
  }

  replaceChildren(...children) {
    this.children = [];
    this.append(...children);
  }

  addEventListener(name, listener) {
    this.listeners.set(name, listener);
  }

  setAttribute(name, value) {
    this[name] = value;
  }

  querySelector(selector) {
    if (selector.startsWith("#") && this.id === selector.slice(1)) return this;
    for (const child of this.children) {
      const found = child.querySelector(selector);
      if (found) return found;
    }
    return null;
  }
}

function findTestId(node, testId) {
  if (node.dataset.testid === testId) return node;
  for (const child of node.children) {
    const found = findTestId(child, testId);
    if (found) return found;
  }
  return null;
}

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function written(path) {
  const count = Number(instance.exports.wisp_file_count());
  for (let i = 0; i < count; i++) {
    const p = readString(
      instance.exports.wisp_file_path_ptr(i),
      instance.exports.wisp_file_path_len(i),
    );
    if (p === path) return instance.exports.wisp_file_written(i) === 1;
  }
  return false;
}

function fileText(path) {
  const count = Number(instance.exports.wisp_file_count());
  for (let i = 0; i < count; i++) {
    const p = readString(
      instance.exports.wisp_file_path_ptr(i),
      instance.exports.wisp_file_path_len(i),
    );
    if (p !== path) continue;
    const ptr = instance.exports.wisp_file_data_ptr(i);
    const len = Number(instance.exports.wisp_file_data_len(i));
    return decoder.decode(u8().slice(ptr, ptr + len));
  }
  return null;
}

await load();

const held = { kind: "js-object" };
const heldToken = wormholes.adopt(held);
instance.exports.wisp_wormhole_heap_new();
const heldSlot = instance.exports.wisp_wormhole_adopt(heldToken);
assert(wormholes.get(heldToken) === held, "wormhole lost adopted object");
instance.exports.wisp_wormhole_collect();
assert(wormholes.refCount(heldToken) === 1, "live wormhole was released");
const cloneSlot = instance.exports.wisp_wormhole_clone(heldSlot);
assert(wormholes.refCount(heldToken) === 2, "wormhole clone did not retain");
instance.exports.wisp_wormhole_drop(heldSlot);
instance.exports.wisp_wormhole_collect();
assert(wormholes.refCount(heldToken) === 1, "dead wormhole was not released");
instance.exports.wisp_wormhole_close(cloneSlot);
instance.exports.wisp_wormhole_close(cloneSlot);
assert(!wormholes.has(heldToken), "closed wormhole retained its JS object");

const teardownToken = wormholes.adopt({ kind: "heap-teardown" });
instance.exports.wisp_wormhole_adopt(teardownToken);
instance.exports.wisp_wormhole_heap_dispose();
assert(!wormholes.has(teardownToken), "heap teardown retained its JS object");

instance.exports.wisp_clear_files();
setFileRoot("files");
setInput("");
mount(
  "src/simple.plan",
  `"top-level"\n` +
    `(#bind Print (#pin (#law "Print" (Print x) ((#pin "R") ("Print" x)))))\n` +
    `(#bind main (#pin (#law "main" (main args) ((#pin "R") ("Print" "ok")))))\n`,
);
let rc = run("src", "simple", "main");
assert(rc === 0, `simple failed: ${errors()}`);
assert(output(1) === "ok", `Print stdout mismatch: ${output(1)}`);
assert(output(2).includes("top-level"), `top-level stderr missing: ${output(2)}`);

instance.exports.wisp_clear_files();
setFileRoot("reaver/src");
mount(
  "src/save1.plan",
  `(#bind Save (#pin (#law "Save" (Save x) ((#pin "B") ("Save" x)))))\n` +
    `(#bind prog (#pin (#law "prog" (prog args) 0)))\n` +
    `(Save prog)\n`,
);
rc = run("src", "save1");
const rootSnapshot = fileText("snap/root.plan");
assert(rc === 0, `Save failed: ${errors()}`);
assert(written("snap/root.plan"), "Save did not write snap/root.plan");
assert(rootSnapshot !== null && rootSnapshot.includes("@"), "Save root snapshot is empty");

instance.exports.wisp_clear_files();
setFileRoot("files");
mount(
  "snap/root.plan",
  `(#bind Output (#pin (#law "Output" (Output x) ((#pin "R") ("Output" x)))))\n` +
    `(#bind Input (#pin (#law "Input" (Input x) ((#pin "R") ("Input" x)))))\n` +
    `(#bind ReadFile (#pin (#law "ReadFile" (ReadFile x) ((#pin "R") ("ReadFile" x)))))\n` +
    `(#bind WriteFile (#pin (#law "WriteFile" (WriteFile p b) ((#pin "R") ("WriteFile" p b)))))\n` +
    `(#bind inside (#pin (#law "inside" (inside args) ((#pin "R") ("Output" ((#pin "R") ("ReadFile" "allowed.txt")))))))\n` +
    `(#bind escape (#pin (#law "escape" (escape args) ((#pin "R") ("Output" ((#pin "R") ("ReadFile" "../secret.txt")))))))\n` +
    `(#bind write (#pin (#law "write" (write args) ((#pin "R") ("WriteFile" "out.txt" ((#pin "R") ("Input" 64)))))))\n` +
    `(#bind read-written (#pin (#law "read-written" (read-written args) ((#pin "R") ("Output" ((#pin "R") ("ReadFile" "out.txt")))))))\n`,
);
mount("files/allowed.txt", "inside-ok");
mount("secret.txt", "outside-secret");
rc = run("snap", "root", "inside");
assert(rc === 0 && output(1) === "inside-ok", `ReadFile failed: ${output(1)} ${errors()}`);
rc = run("snap", "root", "escape");
assert(rc === 0 && !output(1).includes("outside-secret"), "ReadFile escaped root");
setInput("x");
rc = run("snap", "root", "write");
assert(rc === 0 && written("files/out.txt"), `WriteFile did not write: ${errors()}`);
setInput("");
rc = run("snap", "root", "read-written");
assert(rc === 0 && output(1) === "x", `written file did not persist: ${output(1)} ${errors()}`);

instance.exports.wisp_clear_files();
setFileRoot("");
mount("tests/plan/actors.plan", await readFile(resolve(repoRoot, "tests/plan/actors.plan"), "utf8"));
for (const [fn, want] of [
  ["ping", "hello-via-echo"],
  ["count", "3"],
  ["tryrecv", "tm"],
]) {
  rc = run("tests/plan", "actors", fn);
  assert(rc === 0, `${fn} failed: ${errors()}`);
  assert(output(1) === want, `${fn} stdout mismatch: ${output(1)}`);
}
rc = run("tests/plan", "actors", "stuck");
assert(rc !== 0 && errors().includes("deadlock"), "stuck did not report deadlock");

instance.exports.wisp_clear_files();
setFileRoot("reaver/src");
const browserBundle = JSON.parse(
  await readFile(resolve(wasmPath, "..", "reaver-src.json"), "utf8"),
);
for (const file of browserBundle.files)
  mount(
    file.path,
    decoder.decode(Buffer.from(file.base64, "base64")),
    BigInt(file.mtime || 0),
  );
const environment = {
  result: undefined,
  acceptResult(value) {
    this.result = value;
  },
};
const model = { value: 40, label: "π" };
rc = runJplan(
  environment,
  [model],
  `if (this !== environment) throw new Error("wrong this");
objects[0].value += 2;
return { value: objects[0].value, label: objects[0].label };`,
);
assert(rc === 0, `JPLAN demo failed: ${errors()}`);
assert(environment.result.value === 42, "JPLAN result was not published");
assert(environment.result.label === "π", "JPLAN UTF-8 source/object mismatch");
assert(wormholes.size === 0, "JPLAN demo leaked wormhole table entries");

rc = runJplan(environment, [], `throw new TypeError("demo boom");`);
assert(rc === 0, `JPLAN Error result failed: ${errors()}`);
assert(environment.result instanceof TypeError, "JPLAN did not publish an Error value");
assert(environment.result.message === "demo boom", "JPLAN Error message mismatch");
assert(wormholes.size === 0, "JPLAN Error result leaked wormholes");

rc = runJplan(environment, [], "return 7;");
assert(rc === 0 && environment.result === 7, "JPLAN primitive result failed");
assert(wormholes.size === 0, "JPLAN primitive result leaked wormholes");

const document = {
  body: new FakeNode("body"),
  createElement: (tagName) => new FakeNode(tagName),
  createDocumentFragment: () => new FakeNode("#fragment"),
};
globalThis.document = document;
const uiEnvironment = {
  root: document.body,
  pendingAction: "",
  pendingPayload: "",
  dispatched: null,
  dispatch(action, payload) {
    this.dispatched = { action, payload };
  },
};

rc = runJplanDispatch(uiEnvironment, "mount");
assert(rc === 0, `JPLAN Reaver mount failed: ${errors()}`);
assert(
  findTestId(document.body, "reaver-app") !== null,
  "Reaver did not instantiate the UI tree",
);
assert(
  findTestId(document.body, "status")?.textContent.includes("Reaver"),
  "Reaver status component was not rendered",
);
assert(wormholes.size === 0, "Reaver mount leaked wormholes");

const demoSource = findTestId(document.body, "source").value;
findTestId(document.body, "do-it").listeners.get("click")();
assert(
  uiEnvironment.dispatched.action === "do-it" &&
    uiEnvironment.dispatched.payload === demoSource,
  "Reaver did not wire the Do It event through its dispatch bridge",
);
rc = runJplanDispatch(uiEnvironment, "do-it", demoSource);
assert(rc === 0, `JPLAN Reaver Do It failed: ${errors()}`);
assert(uiEnvironment.state.counter.value === 1, "Reaver Do It did not mutate the model");
assert(
  findTestId(document.body, "result-value").textContent.includes("count"),
  "Reaver did not publish the Do It result",
);
assert(
  findTestId(document.body, "transcript").textContent.includes("counter is now 1"),
  "Reaver transcript did not receive environment.print",
);
assert(wormholes.size === 0, "Reaver Do It leaked wormholes");

// Exercise the browser entry point itself, including its WASI shims, queued
// dispatch bridge, packaged source bundle, and event listener.  The direct
// calls above intentionally remain lower-level runtime tests.
const bootstrapDocument = {
  body: new FakeNode("body"),
  createElement: (tagName) => new FakeNode(tagName),
  createDocumentFragment: () => new FakeNode("#fragment"),
};
globalThis.document = bootstrapDocument;
const realFetch = globalThis.fetch;
globalThis.fetch = async (input) => {
  const url = String(input);
  if (url.endsWith("/wisp.wasm"))
    return new Response(await readFile(wasmPath), {
      headers: { "content-type": "application/wasm" },
    });
  if (url.endsWith("/reaver-src.json"))
    return new Response(
      await readFile(resolve(wasmPath, "..", "reaver-src.json")),
      { headers: { "content-type": "application/json" } },
    );
  return realFetch(input);
};

await import(new URL("./jplan.js?smoke", import.meta.url));
for (
  let i = 0;
  i < 200 && findTestId(bootstrapDocument.body, "reaver-app") === null;
  i++
)
  await new Promise((done) => setTimeout(done, 10));
assert(
  findTestId(bootstrapDocument.body, "reaver-app") !== null,
  `JPLAN browser bootstrap did not render: ${bootstrapDocument.body.textContent}`,
);

findTestId(bootstrapDocument.body, "do-it").listeners.get("click")();
for (let i = 0; i < 200; i++) {
  const text =
    findTestId(bootstrapDocument.body, "transcript")?.textContent ?? "";
  if (text.includes("counter is now 1")) break;
  await new Promise((done) => setTimeout(done, 10));
}
assert(
  findTestId(bootstrapDocument.body, "transcript").textContent.includes(
    "counter is now 1",
  ),
  "JPLAN browser bootstrap click did not dispatch and render",
);

findTestId(bootstrapDocument.body, "object-1").listeners.get("click")();
for (let i = 0; i < 200; i++) {
  if (
    findTestId(bootstrapDocument.body, "object-1")?.className.includes(
      "selected",
    )
  )
    break;
  await new Promise((done) => setTimeout(done, 10));
}
assert(
  findTestId(bootstrapDocument.body, "object-1").className.includes("selected"),
  "JPLAN browser object selection did not rerender",
);

findTestId(bootstrapDocument.body, "inspect").listeners.get("click")();
for (let i = 0; i < 200; i++) {
  const text =
    findTestId(bootstrapDocument.body, "transcript")?.textContent ?? "";
  if (text.includes("Inspect")) break;
  await new Promise((done) => setTimeout(done, 10));
}
assert(
  findTestId(bootstrapDocument.body, "transcript").textContent.includes("Inspect"),
  "JPLAN browser Inspect click did not dispatch and render",
);

findTestId(bootstrapDocument.body, "reset").listeners.get("click")();
for (let i = 0; i < 200; i++) {
  const text =
    findTestId(bootstrapDocument.body, "transcript")?.textContent ?? "";
  if (text === "workspace mounted by Reaver") break;
  await new Promise((done) => setTimeout(done, 10));
}
assert(
  findTestId(bootstrapDocument.body, "transcript").textContent ===
    "workspace mounted by Reaver",
  "JPLAN browser Reset click did not restore the workspace",
);
globalThis.fetch = realFetch;

console.log("wisp browser smoke: OK");
