#!/usr/bin/env node

import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import {
  chmodSync,
  copyFileSync,
  mkdirSync,
  readFileSync,
  readdirSync,
  rmSync,
  statSync,
  utimesSync,
  writeFileSync,
} from "node:fs";
import path from "node:path";
import process from "node:process";

function parse(argv) {
  const result = {};
  for (let index = 0; index < argv.length; index += 2) {
    if (!argv[index]?.startsWith("--") || argv[index + 1] === undefined) {
      throw new Error(`invalid argument near ${argv[index] ?? "<end>"}`);
    }
    result[argv[index].slice(2)] = argv[index + 1];
  }
  return result;
}

function sha256File(filename) {
  return createHash("sha256").update(readFileSync(filename)).digest("hex");
}

function files(root, relative = "") {
  const rows = [];
  for (const name of readdirSync(path.join(root, relative)).sort()) {
    const child = relative ? `${relative}/${name}` : name;
    const absolute = path.join(root, child);
    const status = statSync(absolute);
    if (status.isDirectory()) rows.push(...files(root, child));
    else if (status.isFile()) rows.push(child.replaceAll("\\", "/"));
    else throw new Error(`unsupported manager payload entry ${child}`);
  }
  return rows;
}

function archive(stage, output, epoch) {
  execFileSync(
    "cmake",
    ["-E", "tar", "cfz", output, "--format=gnutar", `--mtime=${epoch}`, "."],
    { cwd: stage, windowsHide: true },
  );
}

const args = parse(process.argv.slice(2));
for (const required of ["install-tree", "version", "target", "output", "source-date-epoch"]) {
  if (!args[required]) throw new Error(`--${required} is required`);
}
const source = path.resolve(args["install-tree"]);
const output = path.resolve(args.output);
const epoch = Number(args["source-date-epoch"]);
const stage = `${output}.stage`;
rmSync(stage, { recursive: true, force: true });
mkdirSync(stage, { recursive: true });
for (const relative of files(source)) {
  const destination = path.join(stage, relative);
  mkdirSync(path.dirname(destination), { recursive: true });
  copyFileSync(path.join(source, relative), destination);
  chmodSync(destination, relative.startsWith("bin/") ? 0o755 : 0o644);
  utimesSync(destination, epoch, epoch);
}
const payloadFiles = files(stage).map((relative) => ({
  path: relative,
  sha256: sha256File(path.join(stage, relative)),
  size_bytes: statSync(path.join(stage, relative)).size,
}));
const manifestPath = path.join(stage, "arena-component.json");
writeFileSync(manifestPath, `${JSON.stringify({
  schema: "arena_install_component_v1",
  schema_version: 1,
  component_id: "manager.core",
  component_version: args.version,
  target_triple: args.target,
  entrypoints: {
    service: args.target.startsWith("windows") ? "bin/arena-service.exe" : "bin/arena-service",
    cli: args.target.startsWith("windows") ? "bin/arena.exe" : "bin/arena",
    broker: args.target.startsWith("windows") ? "bin/arena-broker.exe" : "bin/arena-broker",
    setup: args.target.startsWith("windows") ? "bin/arena-setup.exe" : "bin/arena-setup"
  },
  capabilities: ["control_plane", "setup", "broker"],
  startup: {},
  data_paths: [],
  files: payloadFiles,
}, null, 2)}\n`);
utimesSync(manifestPath, epoch, epoch);
mkdirSync(path.dirname(output), { recursive: true });
const second = `${output}.repeat`;
archive(stage, output, epoch);
archive(stage, second, epoch);
if (sha256File(output) !== sha256File(second)) {
  throw new Error("manager component archive is not reproducible");
}
rmSync(second, { force: true });
rmSync(stage, { recursive: true, force: true });
process.stdout.write(`${JSON.stringify({
  schema: "arena_manager_component_build_v1",
  artifact: output,
  sha256: sha256File(output),
})}\n`);
