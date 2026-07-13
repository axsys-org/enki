#!/usr/bin/env node
import { createReadStream } from "node:fs";
import { access, readdir, readFile, stat } from "node:fs/promises";
import { createServer } from "node:http";
import { extname, join, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

const here = resolve(fileURLToPath(new URL(".", import.meta.url)));
const repoRoot = resolve(here, "..");

const args = process.argv.slice(2);
let host = "127.0.0.1";
let port = 8787;
let root = "";
let wasmPath = "";
let jplanDir = resolve(repoRoot, "build/jplan-demo");

function takeValue(flag, i) {
  if (i + 1 >= args.length) throw new Error(`${flag} needs a value`);
  return args[i + 1];
}

for (let i = 0; i < args.length; i++) {
  const arg = args[i];
  if (arg === "--host") host = takeValue(arg, i++);
  else if (arg === "--port") port = Number(takeValue(arg, i++));
  else if (arg === "--root") root = takeValue(arg, i++);
  else if (arg === "--wasm") wasmPath = takeValue(arg, i++);
  else if (arg === "--jplan-dir")
    jplanDir = resolve(takeValue(arg, i++));
  else if (arg === "-h" || arg === "--help") {
    console.log(
      "usage: node web/wisp-devserver.mjs [--host HOST] [--port PORT] [--root DIR] [--wasm FILE] [--jplan-dir DIR]",
    );
    process.exit(0);
  } else {
    throw new Error(`unknown argument: ${arg}`);
  }
}

if (!Number.isInteger(port) || port < 1 || port > 65535)
  throw new Error("--port must be an integer from 1 to 65535");

async function exists(path) {
  try {
    await access(path);
    return true;
  } catch {
    return false;
  }
}

async function collectFiles(dir) {
  const entries = await readdir(dir, { withFileTypes: true });
  const out = [];
  for (const entry of entries.sort((a, b) => a.name.localeCompare(b.name))) {
    const path = resolve(dir, entry.name);
    if (entry.isDirectory()) out.push(...(await collectFiles(path)));
    else if (entry.isFile()) out.push(path);
  }
  return out;
}

async function dynamicReaverBundle() {
  const srcRoot = resolve(repoRoot, "reaver/src");
  const files = [];
  for (const file of await collectFiles(srcRoot)) {
    const info = await stat(file);
    const rel = file.slice(srcRoot.length + 1).replaceAll("\\", "/");
    files.push({
      path: `reaver/src/${rel}`,
      mtime: Math.floor(info.mtimeMs / 1000),
      base64: (await readFile(file)).toString("base64"),
    });
  }
  const jplanDemo = resolve(jplanDir, "jplan-demo.plan");
  const jplanInfo = await stat(jplanDemo);
  files.push({
    path: "reaver/src/plan/jplan-demo.plan",
    mtime: Math.floor(jplanInfo.mtimeMs / 1000),
    base64: (await readFile(jplanDemo)).toString("base64"),
  });
  const jplanSource = resolve(here, "jplan-demo.rvr");
  const jplanSourceInfo = await stat(jplanSource);
  files.push({
    path: "reaver/src/reaver/jplan-demo.rvr",
    mtime: Math.floor(jplanSourceInfo.mtimeMs / 1000),
    base64: (await readFile(jplanSource)).toString("base64"),
  });
  const jplanSnap = resolve(jplanDir, "jplan-demo-snap");
  for (const file of await collectFiles(jplanSnap)) {
    const info = await stat(file);
    const rel = file.slice(jplanSnap.length + 1).replaceAll("\\", "/");
    files.push({
      path: `reaver/src/plan/${rel}`,
      mtime: Math.floor(info.mtimeMs / 1000),
      base64: (await readFile(file)).toString("base64"),
    });
  }
  return `${JSON.stringify({ root: "reaver/src", files })}\n`;
}

const packagedRoot = resolve(repoRoot, "result/share/enki/browser");
root = resolve(root || ((await exists(packagedRoot)) ? packagedRoot : here));
wasmPath = wasmPath ? resolve(wasmPath) : resolve(root, "wisp.wasm");
if (!(await exists(wasmPath)))
  wasmPath = resolve(repoRoot, "build/wasm/browser/wisp.wasm");

const mime = new Map([
  [".html", "text/html; charset=utf-8"],
  [".js", "text/javascript; charset=utf-8"],
  [".mjs", "text/javascript; charset=utf-8"],
  [".css", "text/css; charset=utf-8"],
  [".wasm", "application/wasm"],
  [".json", "application/json; charset=utf-8"],
  [".txt", "text/plain; charset=utf-8"],
]);

function headersFor(path) {
  return {
    "Content-Type": mime.get(extname(path)) ?? "application/octet-stream",
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Embedder-Policy": "require-corp",
    "Cross-Origin-Resource-Policy": "same-origin",
    "Origin-Agent-Cluster": "?1",
  };
}

function send(res, status, body) {
  res.writeHead(status, {
    ...headersFor(".txt"),
    "Cache-Control": "no-store",
  });
  res.end(body);
}

function sendJson(res, body) {
  res.writeHead(200, {
    ...headersFor(".json"),
    "Cache-Control": "no-store",
    "Content-Length": Buffer.byteLength(body),
  });
  res.end(body);
}

function resolveRequest(url) {
  const parsed = new URL(url, "http://localhost");
  let path = decodeURIComponent(parsed.pathname);
  if (path === "/") path = "/wisp.html";
  if (path === "/wisp.wasm") return wasmPath;

  const target = resolve(root, `.${path}`);
  const inside = target === root || target.startsWith(root + sep);
  return inside ? target : "";
}

const server = createServer(async (req, res) => {
  if (req.method !== "GET" && req.method !== "HEAD") {
    send(res, 405, "method not allowed\n");
    return;
  }

  const target = resolveRequest(req.url ?? "/");
  if (!target) {
    send(res, 403, "forbidden\n");
    return;
  }

  try {
    const parsed = new URL(req.url ?? "/", "http://localhost");
    if (parsed.pathname === "/reaver-src.json") {
      const packaged = resolve(root, "reaver-src.json");
      if (!(await exists(packaged))) {
        sendJson(res, await dynamicReaverBundle());
        return;
      }
    }

    const info = await stat(target);
    const file = info.isDirectory() ? join(target, "wisp.html") : target;
    const fileInfo = await stat(file);
    if (!fileInfo.isFile()) {
      send(res, 404, "not found\n");
      return;
    }

    res.writeHead(200, {
      ...headersFor(file),
      "Content-Length": fileInfo.size,
      "Cache-Control": "no-store",
    });
    if (req.method === "HEAD") res.end();
    else createReadStream(file).pipe(res);
  } catch {
    send(res, 404, "not found\n");
  }
});

server.on("error", (err) => {
  console.error(`wisp-devserver: ${err.message}`);
  process.exit(1);
});

server.listen(port, host, () => {
  console.log(`Wisp browser devserver: http://${host}:${port}/`);
  console.log(`root: ${root}`);
  console.log(`wasm: ${wasmPath}`);
});
