#!/usr/bin/env node

import { createHash } from "node:crypto";
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
import { gzipSync } from "node:zlib";

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

function writeOctal(buffer, offset, length, value) {
  const encoded = Math.trunc(value).toString(8).padStart(length - 1, "0");
  if (encoded.length >= length) {
    throw new Error(`tar numeric field overflow: ${value}`);
  }
  buffer.write(encoded, offset, length - 1, "ascii");
  buffer[offset + length - 1] = 0;
}

function splitTarPath(relative) {
  if (Buffer.byteLength(relative) <= 100) {
    return { name: relative, prefix: "" };
  }
  for (
    let index = relative.lastIndexOf("/");
    index > 0;
    index = relative.lastIndexOf("/", index - 1)
  ) {
    const prefix = relative.slice(0, index);
    const name = relative.slice(index + 1);
    if (Buffer.byteLength(prefix) <= 155 && Buffer.byteLength(name) <= 100) {
      return { name, prefix };
    }
  }
  throw new Error(`manager component path is too long for deterministic ustar: ${relative}`);
}

function tarHeader(relative, size, mode, epoch) {
  const { name, prefix } = splitTarPath(relative);
  const header = Buffer.alloc(512);
  header.write(name, 0, 100, "utf8");
  writeOctal(header, 100, 8, mode);
  writeOctal(header, 108, 8, 0);
  writeOctal(header, 116, 8, 0);
  writeOctal(header, 124, 12, size);
  writeOctal(header, 136, 12, epoch);
  header.fill(0x20, 148, 156);
  header[156] = "0".charCodeAt(0);
  header.write("ustar\0", 257, 6, "ascii");
  header.write("00", 263, 2, "ascii");
  header.write("root", 265, 32, "ascii");
  header.write("root", 297, 32, "ascii");
  header.write(prefix, 345, 155, "utf8");
  const checksum = header.reduce((sum, value) => sum + value, 0);
  header.write(checksum.toString(8).padStart(6, "0"), 148, 6, "ascii");
  header[154] = 0;
  header[155] = 0x20;
  return header;
}

function archive(stage, output, epoch) {
  const chunks = [];
  for (const relative of files(stage)) {
    const bytes = readFileSync(path.join(stage, relative));
    chunks.push(tarHeader(
      relative,
      bytes.length,
      relative.startsWith("bin/") ? 0o755 : 0o644,
      epoch,
    ));
    chunks.push(bytes);
    const padding = (512 - (bytes.length % 512)) % 512;
    if (padding) chunks.push(Buffer.alloc(padding));
  }
  chunks.push(Buffer.alloc(1024));
  writeFileSync(output, gzipSync(Buffer.concat(chunks), { level: 9, mtime: 0 }));
}

const args = parse(process.argv.slice(2));
for (const required of ["install-tree", "version", "target", "output", "source-date-epoch"]) {
  if (!args[required]) throw new Error(`--${required} is required`);
}
const source = path.resolve(args["install-tree"]);
const output = path.resolve(args.output);
const epoch = Number(args["source-date-epoch"]);
if (!Number.isSafeInteger(epoch) || epoch < 0) {
  throw new Error("--source-date-epoch must be a nonnegative integer");
}
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
