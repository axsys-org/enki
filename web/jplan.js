import { JPlanHost } from "./jplan-host.mjs";

const encoder = new TextEncoder();
const decoder = new TextDecoder();

let instance;
let dispatchTail = Promise.resolve();

const environment = {
  root: document.body,
  state: null,
  pendingAction: "",
  pendingPayload: "",
  dispatch(action, payload = "") {
    dispatchTail = dispatchTail.then(() => runAction(action, payload));
    return dispatchTail;
  },
};

const jplanHost = new JPlanHost(() => instance.exports.memory);
const wormholes = jplanHost.wormholes;

function bytes() {
  return new Uint8Array(instance.exports.memory.buffer);
}

function view() {
  return new DataView(instance.exports.memory.buffer);
}

function allocate(data, nul = false) {
  const size = data.length + (nul ? 1 : 0);
  const ptr = instance.exports.wisp_alloc(size || 1);
  bytes().set(data, ptr);
  if (nul) bytes()[ptr + data.length] = 0;
  return { ptr, size: size || 1 };
}

function free(allocation) {
  instance.exports.wisp_free(allocation.ptr, allocation.size);
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

function mountFile(path, data, mtime = 0) {
  const pathAllocation = allocate(encoder.encode(path));
  const dataAllocation = allocate(data);
  try {
    const rc = instance.exports.wisp_mount_file(
      pathAllocation.ptr,
      encoder.encode(path).length,
      dataAllocation.ptr,
      data.length,
      BigInt(mtime),
    );
    if (rc !== 0) throw new Error(`could not mount ${path}`);
  } finally {
    free(pathAllocation);
    free(dataAllocation);
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
  const allocation = allocate(root);
  try {
    instance.exports.wisp_set_file_root(allocation.ptr, root.length);
  } finally {
    free(allocation);
  }
}

function runAction(action, payload) {
  environment.pendingAction = action;
  environment.pendingPayload = payload;

  const environmentToken = wormholes.adopt(environment);
  const actionBytes = encoder.encode(action);
  const payloadBytes = encoder.encode(payload);
  const actionAllocation = allocate(actionBytes);
  const payloadAllocation = allocate(payloadBytes);
  try {
    const rc = instance.exports.wisp_jplan_dispatch(
      environmentToken,
      actionAllocation.ptr,
      actionBytes.length,
      payloadAllocation.ptr,
      payloadBytes.length,
    );
    if (rc !== 0) throw new Error(errorText() || `runtime failed (${rc})`);
    if (wormholes.size !== 0)
      throw new Error(`${wormholes.size} wormhole table entries leaked`);
  } finally {
    free(actionAllocation);
    free(payloadAllocation);
  }
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
      fd_write: (_fd, iovs, iovsLen, written) => {
        let count = 0;
        for (let i = 0; i < iovsLen; i++)
          count += view().getUint32(iovs + i * 8 + 4, true);
        view().setUint32(written, count, true);
        return 0;
      },
      proc_exit: (code) => {
        throw new Error(`WASI proc_exit(${code})`);
      },
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
  const wasm = await WebAssembly.instantiateStreaming(
    fetch(new URL("wisp.wasm", import.meta.url)),
    wasiImports(),
  );
  instance = wasm.instance;
  await mountBundle();
  await environment.dispatch("mount");
}

load().catch((error) => {
  console.error(error);
  document.body.textContent = error instanceof Error ? error.message : String(error);
});
