#!/usr/bin/env node
import { mkdir, readdir, readFile, stat, writeFile } from "node:fs/promises";
import { dirname, relative, resolve } from "node:path";

const [
  srcArg = "reaver/src",
  outArg = "build/wasm/browser/reaver-src.json",
  jplanArg = "build/jplan-demo",
] = process.argv.slice(2);
const srcRoot = resolve(srcArg);
const outPath = resolve(outArg);
const jplanRoot = resolve(jplanArg);
const jplanDemoPath = resolve(jplanRoot, "jplan-demo.plan");
const jplanSourcePath = new URL("./jplan-demo.rvr", import.meta.url);
const jplanSnapRoot = resolve(jplanRoot, "jplan-demo-snap");

async function collect(dir) {
  const entries = await readdir(dir, { withFileTypes: true });
  const out = [];
  for (const entry of entries.sort((a, b) => a.name.localeCompare(b.name))) {
    const path = resolve(dir, entry.name);
    if (entry.isDirectory()) out.push(...(await collect(path)));
    else if (entry.isFile()) out.push(path);
  }
  return out;
}

const files = [];
for (const file of await collect(srcRoot)) {
  const info = await stat(file);
  const rel = relative(srcRoot, file).replaceAll("\\", "/");
  files.push({
    path: `reaver/src/${rel}`,
    mtime: Math.floor(info.mtimeMs / 1000),
    base64: (await readFile(file)).toString("base64"),
  });
}

const jplanInfo = await stat(jplanDemoPath);
files.push({
  path: "reaver/src/plan/jplan-demo.plan",
  mtime: Math.floor(jplanInfo.mtimeMs / 1000),
  base64: (await readFile(jplanDemoPath)).toString("base64"),
});

const jplanSourceInfo = await stat(jplanSourcePath);
files.push({
  path: "reaver/src/reaver/jplan-demo.rvr",
  mtime: Math.floor(jplanSourceInfo.mtimeMs / 1000),
  base64: (await readFile(jplanSourcePath)).toString("base64"),
});

for (const file of await collect(jplanSnapRoot)) {
  const info = await stat(file);
  files.push({
    path: `reaver/src/plan/${relative(jplanSnapRoot, file).replaceAll("\\", "/")}`,
    mtime: Math.floor(info.mtimeMs / 1000),
    base64: (await readFile(file)).toString("base64"),
  });
}

await mkdir(dirname(outPath), { recursive: true });
await writeFile(
  outPath,
  `${JSON.stringify({ root: "reaver/src", files })}\n`,
  "utf8",
);
