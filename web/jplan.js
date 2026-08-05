import { JPlanHost } from "./jplan-host.mjs";

const encoder = new TextEncoder();
const decoder = new TextDecoder();

const els = {
  status: document.querySelector("#status"),
  objectList: document.querySelector("#object-list"),
  source: document.querySelector("#source"),
  doIt: document.querySelector("#do-it"),
  inspect: document.querySelector("#inspect"),
  reset: document.querySelector("#reset"),
  resultKind: document.querySelector("#result-kind"),
  resultValue: document.querySelector("#result-value"),
  properties: document.querySelector("#properties"),
  propertyCount: document.querySelector("#property-count"),
  transcript: document.querySelector("#transcript"),
  handleCount: document.querySelector("#handle-count"),
};

const defaultSource = `const counter = objects[0];
counter.value += 1;
environment.print(\`counter is now \${counter.value}\`);
return {
  message: "Hello from JPLAN",
  count: counter.value,
  updatedAt: new Date(),
};`;

let instance;
let counter;
let workspaceState;
let namedObjects;
let selected;
let lastResult;

const environment = {
  acceptResult(value) {
    lastResult = value;
  },
  print(value) {
    appendTranscript(String(value));
  },
};

const jplanHost = new JPlanHost(() => instance.exports.memory);
const wormholes = jplanHost.wormholes;

function memory() {
  return instance.exports.memory;
}

function bytes() {
  return new Uint8Array(memory().buffer);
}

function view() {
  return new DataView(memory().buffer);
}

function allocate(data, nul = false) {
  const ptr = instance.exports.wisp_alloc(data.length + (nul ? 1 : 0));
  bytes().set(data, ptr);
  if (nul) bytes()[ptr + data.length] = 0;
  return ptr;
}

function free(ptr, size) {
  instance.exports.wisp_free(ptr, size);
}

function readString(ptr, len) {
  return ptr && len ? decoder.decode(bytes().slice(ptr, ptr + len)) : "";
}

function errorText() {
  return readString(
    instance.exports.wisp_error_ptr(),
    instance.exports.wisp_error_len(),
  );
}

function setStatus(text, tone = "") {
  els.status.textContent = text;
  els.status.className = `status ${tone}`.trim();
}

function setBusy(busy) {
  els.doIt.disabled = busy;
  els.inspect.disabled = busy;
  els.reset.disabled = busy;
  els.source.disabled = busy;
}

function appendTranscript(text) {
  const stamp = new Date().toLocaleTimeString([], {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  });
  els.transcript.textContent += `[${stamp}] ${text}\n`;
  els.transcript.scrollTop = els.transcript.scrollHeight;
}

function shortValue(value) {
  if (value instanceof Error) return `${value.name}: ${value.message}`;
  if (typeof value === "string") return JSON.stringify(value);
  if (typeof value === "function") return `[Function ${value.name || "anonymous"}]`;
  if (typeof value === "symbol") return String(value);
  if (value === null || typeof value !== "object") return String(value);
  const tag = Object.prototype.toString.call(value).slice(8, -1);
  try {
    const json = JSON.stringify(value);
    return json && json.length <= 140 ? json : `[${tag}]`;
  } catch {
    return `[${tag}]`;
  }
}

function inspectProperties(value) {
  els.resultKind.textContent =
    value instanceof Error
      ? value.name
      : value === null
        ? "null"
        : typeof value;
  els.resultValue.textContent = shortValue(value);
  els.properties.replaceChildren();

  let keys = [];
  if (value !== null && (typeof value === "object" || typeof value === "function")) {
    try {
      keys = Reflect.ownKeys(value);
    } catch (error) {
      keys = [`<inspection failed: ${error}>`];
    }
  }

  for (const key of keys) {
    const row = document.createElement("div");
    row.className = "property";
    const name = document.createElement("span");
    name.textContent = typeof key === "symbol" ? String(key) : key;
    const rendered = document.createElement("span");
    try {
      const descriptor = Object.getOwnPropertyDescriptor(value, key);
      rendered.textContent = descriptor && "value" in descriptor
        ? shortValue(descriptor.value)
        : "<accessor>";
    } catch (error) {
      rendered.textContent = `<unavailable: ${error}>`;
    }
    row.append(name, rendered);
    els.properties.append(row);
  }
  els.propertyCount.textContent = `${keys.length} ${keys.length === 1 ? "property" : "properties"}`;
}

function renderObjects() {
  els.objectList.replaceChildren();
  namedObjects.forEach((entry, index) => {
    const item = document.createElement("li");
    const button = document.createElement("button");
    button.type = "button";
    button.className = `object-row${selected.has(index) ? " selected" : ""}`;
    button.innerHTML = `<span class="slot">${selected.has(index) ? `#${[...selected].indexOf(index)}` : "—"}</span><span><span class="object-name"></span><span class="object-type"></span></span>`;
    button.querySelector(".object-name").textContent = entry.name;
    button.querySelector(".object-type").textContent = shortValue(entry.value);
    button.addEventListener("click", () => {
      if (selected.has(index)) selected.delete(index);
      else selected.add(index);
      renderObjects();
    });
    item.append(button);
    els.objectList.append(item);
  });
}

function selectedObjects() {
  return [...selected].sort((a, b) => a - b).map((index) => namedObjects[index].value);
}

function mountFile(path, data, mtime = 0) {
  const pathBytes = encoder.encode(path);
  const pathPtr = allocate(pathBytes);
  const dataPtr = allocate(data);
  try {
    const rc = instance.exports.wisp_mount_file(
      pathPtr,
      pathBytes.length,
      dataPtr,
      data.length,
      BigInt(mtime),
    );
    if (rc !== 0) throw new Error(`could not mount ${path}`);
  } finally {
    free(pathPtr, pathBytes.length);
    free(dataPtr, data.length);
  }
}

function decodeBase64(text) {
  const binary = atob(text);
  return Uint8Array.from(binary, (char) => char.charCodeAt(0));
}

async function mountBundle() {
  const response = await fetch(new URL("reaver-src.json", import.meta.url), {
    cache: "no-store",
  });
  if (!response.ok) throw new Error("could not load reaver-src.json");
  const bundle = await response.json();
  instance.exports.wisp_clear_files();
  for (const file of bundle.files)
    mountFile(file.path, decodeBase64(file.base64), file.mtime);

  const root = encoder.encode("reaver/src");
  const rootPtr = allocate(root);
  try {
    instance.exports.wisp_set_file_root(rootPtr, root.length);
  } finally {
    free(rootPtr, root.length);
  }
}

function runJplan(source, objects) {
  const environmentToken = wormholes.adopt(environment);
  const objectTokens = objects.map((object) => wormholes.adopt(object));
  const tokenBytes = objectTokens.length * 8 || 8;
  const tokensPtr = instance.exports.wisp_alloc(tokenBytes);
  const sourceBytes = encoder.encode(source);
  const sourcePtr = allocate(sourceBytes);
  try {
    objectTokens.forEach((token, index) =>
      view().setBigUint64(tokensPtr + index * 8, token, true),
    );
    lastResult = undefined;
    return instance.exports.wisp_jplan_run(
      environmentToken,
      tokensPtr,
      objectTokens.length,
      sourcePtr,
      sourceBytes.length,
    );
  } finally {
    free(tokensPtr, tokenBytes);
    free(sourcePtr, sourceBytes.length);
    els.handleCount.textContent = `${wormholes.size} live handles`;
  }
}

async function perform(label, source) {
  setBusy(true);
  setStatus("running");
  appendTranscript(`${label} → PLAN/JPLAN`);
  try {
    const objects = selectedObjects();
    const rc = runJplan(source, objects);
    if (rc !== 0) throw new Error(errorText() || `runtime failed (${rc})`);
    if (wormholes.size !== 0)
      throw new Error(`${wormholes.size} wormhole table entries leaked`);
    inspectProperties(lastResult);
    renderObjects();
    appendTranscript(`${label} ← ${shortValue(lastResult)}`);
    setStatus("ready", "ready");
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    appendTranscript(`${label} ! ${message}`);
    setStatus("failed", "fail");
  } finally {
    setBusy(false);
    els.source.focus();
  }
}

function resetWorkspace() {
  counter = { value: 0, step: 1 };
  workspaceState = { name: "JPLAN workspace", runs: 0 };
  namedObjects = [
    { name: "counter", value: counter },
    { name: "workspace", value: workspaceState },
    { name: "document.body", value: document.body },
  ];
  selected = new Set([0]);
  lastResult = undefined;
  els.source.value = defaultSource;
  els.transcript.textContent = "";
  inspectProperties(undefined);
  renderObjects();
  appendTranscript("workspace reset");
}

function wasiImports() {
  const badf = 8;
  return {
    enki: jplanHost.imports(),
    wasi_snapshot_preview1: {
      args_get: () => 0,
      args_sizes_get: (argc, size) => {
        view().setUint32(argc, 0, true);
        view().setUint32(size, 0, true);
        return 0;
      },
      environ_get: () => 0,
      environ_sizes_get: (count, size) => {
        view().setUint32(count, 0, true);
        view().setUint32(size, 0, true);
        return 0;
      },
      clock_time_get: (_id, _precision, out) => {
        view().setBigUint64(out, BigInt(Date.now()) * 1000000n, true);
        return 0;
      },
      fd_close: () => 0,
      fd_fdstat_get: () => badf,
      fd_fdstat_set_flags: () => 0,
      fd_filestat_get: () => badf,
      fd_prestat_get: () => badf,
      fd_prestat_dir_name: () => badf,
      fd_read: () => badf,
      fd_seek: () => badf,
      fd_sync: () => 0,
      fd_write: (_fd, _iovs, _iovsLen, written) => {
        view().setUint32(written, 0, true);
        return 0;
      },
      proc_exit: (code) => { throw new Error(`WASI proc_exit(${code})`); },
      random_get: (ptr, len) => {
        crypto.getRandomValues(bytes().subarray(ptr, ptr + len));
        return 0;
      },
      path_create_directory: () => badf,
      path_filestat_get: () => badf,
      path_open: () => badf,
      path_readlink: () => badf,
      path_remove_directory: () => badf,
      path_rename: () => badf,
      path_symlink: () => badf,
      path_unlink_file: () => badf,
    },
  };
}

async function load() {
  setBusy(true);
  const wasm = await WebAssembly.instantiateStreaming(
    fetch(new URL("wisp.wasm", import.meta.url)),
    wasiImports(),
  );
  instance = wasm.instance;
  await mountBundle();
  resetWorkspace();
  setBusy(false);
  setStatus("ready", "ready");
}

els.doIt.addEventListener("click", () => {
  workspaceState.runs++;
  perform("Do It", els.source.value);
});

els.inspect.addEventListener("click", () => {
  workspaceState.runs++;
  perform("Inspect", "return objects[0];");
});

els.reset.addEventListener("click", () => {
  resetWorkspace();
  setStatus("ready", "ready");
});

load().catch((error) => {
  setBusy(false);
  setStatus("failed to load", "fail");
  appendTranscript(error instanceof Error ? error.stack || error.message : String(error));
});
