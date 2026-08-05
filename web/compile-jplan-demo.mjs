#!/usr/bin/env node

import {
  chmod,
  cp,
  mkdir,
  mkdtemp,
  readFile,
  readdir,
  rm,
  stat,
  writeFile,
} from "node:fs/promises";
import { tmpdir } from "node:os";
import { basename, dirname, join, resolve } from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(here, "..");
const [
  wispArg = "build/debug/bin/wisp",
  reaverArg = "reaver/src",
  outArg = "build/jplan-demo",
] = process.argv.slice(2);
const wisp = resolve(repoRoot, wispArg);
const reaver = resolve(repoRoot, reaverArg);
const source = resolve(here, "jplan-demo.rvr");
const outDir = resolve(repoRoot, outArg);
const outRoot = join(outDir, "jplan-demo.plan");
const outSnap = join(outDir, "jplan-demo-snap");

const temp = await mkdtemp(join(tmpdir(), "enki-jplan-"));
const work = join(temp, "src");
const textSnapshot = join(temp, "text-snapshot");

function run(args, options = {}) {
  return spawnSync(wisp, args, {
    cwd: work,
    encoding: "utf8",
    maxBuffer: 64 * 1024 * 1024,
    ...options,
  });
}

async function makeWritable(path) {
  const info = await stat(path);
  await chmod(path, info.mode | 0o200);
  if (!info.isDirectory()) return;
  for (const entry of await readdir(path))
    await makeWritable(join(path, entry));
}

try {
  await cp(reaver, work, { recursive: true });
  await makeWritable(work);
  await mkdir(join(work, "snap"), { recursive: true });
  await cp(source, join(work, "reaver", "jplan-demo.rvr"));

  const boot = run([
    "--file-root",
    work,
    join(work, "plan"),
    "reaver",
    "main",
  ]);
  if (boot.status !== 0)
    throw new Error(`could not boot Reaver:\n${boot.stderr || boot.stdout}`);

  const compileInput = `(#bind demo (#module jplan-demo))
(#import demo)
(Seq (Save (Pin main)) (Recv 0))
`;
  const compile = run(
    [
      "--export-text-snapshot",
      textSnapshot,
      "--file-root",
      work,
      "snap",
      "root",
      "_",
    ],
    { input: compileInput },
  );
  if (compile.status !== 1 || !compile.stderr.includes("deadlock"))
    throw new Error(
      `Reaver compilation did not stop at the snapshot barrier:\n${compile.stderr || compile.stdout}`,
    );

  const rootPath = join(textSnapshot, "root.plan");
  let rootManifest;
  try {
    rootManifest = await readFile(rootPath, "utf8");
  } catch (error) {
    const snapshots = await readdir(textSnapshot).catch(() => []);
    throw new Error(
      `Reaver stopped without publishing ${rootPath}\n` +
        `snapshots: ${snapshots.join(", ") || "<none>"}\n` +
        `stdout:\n${compile.stdout || "<empty>"}\n` +
        `stderr:\n${compile.stderr || "<empty>"}`,
      { cause: error },
    );
  }
  const rootLines = rootManifest.trim().split("\n");
  const rootMatch = rootLines.at(-1)?.match(/^@([1-9A-HJ-NP-Za-km-z]+)$/);
  if (!rootMatch) throw new Error("Reaver did not save a JPLAN root pin");
  const rootHash = rootMatch[1];

  const closure = new Map();
  const pending = [rootHash];
  while (pending.length > 0) {
    const hash = pending.pop();
    if (closure.has(hash)) continue;
    const text = await readFile(join(textSnapshot, `${hash}.plan`), "utf8");
    closure.set(hash, text);
    for (const match of text.matchAll(
      /^@([1-9A-HJ-NP-Za-km-z]+)(?:\s|$)/gm,
    ))
      pending.push(match[1]);
  }

  const rootText = closure.get(rootHash);
  if (![...closure.values()].some((text) => text.includes('#law "main"')))
    throw new Error(
      `saved JPLAN root is not the Reaver main function:\n${rootText.slice(0, 2048)}`,
    );

  await rm(outDir, { recursive: true, force: true });
  await mkdir(outSnap, { recursive: true });
  for (const [hash, text] of [...closure].sort(([a], [b]) =>
    a.localeCompare(b),
  ))
    await writeFile(join(outSnap, `${hash}.plan`), text, "utf8");
  await writeFile(outRoot, `@${rootHash}\n`, "utf8");

  const generated = (await readdir(outSnap)).length;
  console.log(
    `compiled ${basename(source)} to ${basename(outRoot)} with ${generated} snapshot pins`,
  );
} finally {
  await rm(temp, { recursive: true, force: true });
}
