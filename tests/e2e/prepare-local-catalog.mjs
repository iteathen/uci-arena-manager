#!/usr/bin/env node

import { createHash } from "node:crypto";
import {
  chmodSync,
  copyFileSync,
  lstatSync,
  mkdirSync,
  readFileSync,
  readdirSync,
  rmSync,
  statSync,
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

function sha256(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

function sha256File(filename) {
  return sha256(readFileSync(filename));
}

function files(root, relative = "") {
  const rows = [];
  for (const name of readdirSync(path.join(root, relative)).sort()) {
    const child = relative ? `${relative}/${name}` : name;
    const absolute = path.join(root, child);
    const status = lstatSync(absolute);
    if (status.isSymbolicLink()) {
      throw new Error(`symbolic links are forbidden in E2E payloads: ${child}`);
    }
    if (status.isDirectory()) rows.push(...files(root, child));
    else if (status.isFile()) rows.push(child.replaceAll("\\", "/"));
    else throw new Error(`unsupported E2E payload entry: ${child}`);
  }
  return rows;
}

function writeOctal(buffer, offset, length, value) {
  const encoded = Math.trunc(value).toString(8).padStart(length - 1, "0");
  if (encoded.length >= length) throw new Error(`tar value overflow: ${value}`);
  buffer.write(encoded, offset, length - 1, "ascii");
  buffer[offset + length - 1] = 0;
}

function splitTarPath(relative) {
  if (Buffer.byteLength(relative) <= 100) return { name: relative, prefix: "" };
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
  throw new Error(`E2E payload path is too long for ustar: ${relative}`);
}

function header(relative, size, mode, epoch) {
  const { name, prefix } = splitTarPath(relative);
  const value = Buffer.alloc(512);
  value.write(name, 0, 100, "utf8");
  writeOctal(value, 100, 8, mode);
  writeOctal(value, 108, 8, 0);
  writeOctal(value, 116, 8, 0);
  writeOctal(value, 124, 12, size);
  writeOctal(value, 136, 12, epoch);
  value.fill(0x20, 148, 156);
  value[156] = "0".charCodeAt(0);
  value.write("ustar\0", 257, 6, "ascii");
  value.write("00", 263, 2, "ascii");
  value.write("root", 265, 32, "ascii");
  value.write("root", 297, 32, "ascii");
  value.write(prefix, 345, 155, "utf8");
  const checksum = value.reduce((sum, byte) => sum + byte, 0);
  value.write(checksum.toString(8).padStart(6, "0"), 148, 6, "ascii");
  value[154] = 0;
  value[155] = 0x20;
  return value;
}

function archive(stage, output, epoch) {
  const chunks = [];
  for (const relative of files(stage)) {
    const bytes = readFileSync(path.join(stage, relative));
    const mode = relative.startsWith("bin/") ? 0o755 : 0o644;
    chunks.push(header(relative, bytes.length, mode, epoch), bytes);
    const padding = (512 - (bytes.length % 512)) % 512;
    if (padding) chunks.push(Buffer.alloc(padding));
  }
  chunks.push(Buffer.alloc(1024));
  writeFileSync(output, gzipSync(Buffer.concat(chunks), { level: 9, mtime: 0 }));
}

function copy(source, target) {
  mkdirSync(path.dirname(target), { recursive: true });
  copyFileSync(source, target);
  chmodSync(target, target.includes(`${path.sep}bin${path.sep}`) ? 0o755 : 0o644);
}

function managedComponent({
  output,
  componentId,
  version,
  target,
  payload,
  entrypoints,
  epoch,
}) {
  const artifactName = `${componentId}-${version}-${target}.tar.gz`;
  const artifact = path.join(output, "payloads", artifactName);
  const stage = path.join(output, `.stage-${componentId}`);
  rmSync(stage, { recursive: true, force: true });
  mkdirSync(stage, { recursive: true });
  try {
    for (const [source, relative] of payload) copy(source, path.join(stage, relative));
    const inventory = files(stage).map((relative) => ({
      path: relative,
      sha256: sha256File(path.join(stage, relative)),
      size_bytes: statSync(path.join(stage, relative)).size,
    }));
    writeFileSync(path.join(stage, "arena-component.json"), `${JSON.stringify({
      schema: "arena_install_component_v1",
      schema_version: 1,
      component_id: componentId,
      component_version: version,
      target_triple: target,
      entrypoints,
      capabilities: [],
      startup: {},
      data_paths: [],
      files: inventory,
    }, null, 2)}\n`);
    archive(stage, artifact, epoch);
    return {
      id: componentId,
      version,
      artifactName,
      artifact,
      installedBytes: files(stage).reduce(
        (sum, relative) => sum + statSync(path.join(stage, relative)).size,
        0,
      ),
    };
  } finally {
    rmSync(stage, { recursive: true, force: true });
  }
}

const args = parse(process.argv.slice(2));
for (const name of [
  "manager-archive",
  "provider-dir",
  "node",
  "node-license",
  "stockfish",
  "stockfish-license",
  "output",
  "source-date-epoch",
]) {
  if (!args[name]) throw new Error(`--${name} is required`);
}
const target = "windows-x86_64";
const epoch = Number(args["source-date-epoch"]);
if (!Number.isSafeInteger(epoch) || epoch < 0) {
  throw new Error("--source-date-epoch must be a nonnegative integer");
}
const output = path.resolve(args.output);
const payloads = path.join(output, "payloads");
rmSync(output, { recursive: true, force: true });
mkdirSync(payloads, { recursive: true });

const components = [];
function addArtifact({
  id,
  version,
  artifact,
  artifactName = path.basename(artifact),
  dependencies = [],
  workspaceReserveBytes = 0,
  installedBytes = statSync(artifact).size,
}) {
  const destination = path.join(payloads, artifactName);
  if (path.resolve(artifact) !== path.resolve(destination)) {
    copyFileSync(artifact, destination);
  }
  const size = statSync(artifact).size;
  components.push({
    id,
    version,
    dependencies,
    installed_bytes: installedBytes,
    staging_bytes: Math.max(installedBytes, size * 2),
    backup_bytes: installedBytes,
    workspace_reserve_bytes: workspaceReserveBytes,
    artifacts: {
      [target]: {
        source: `payloads/${artifactName}`,
        sha256: sha256File(artifact),
        size_bytes: size,
      },
    },
  });
}

addArtifact({
  id: "manager.core",
  version: "0.1.0",
  artifact: path.resolve(args["manager-archive"]),
});

const node = managedComponent({
  output,
  componentId: "node_runtime.private",
  version: process.version.slice(1),
  target,
  payload: [
    [path.resolve(args.node), "bin/node.exe"],
    [path.resolve(args["node-license"]), "licenses/LICENSE.txt"],
  ],
  entrypoints: { node: "bin/node.exe" },
  epoch,
});
addArtifact({ ...node, dependencies: ["manager.core"] });

const stockfish = managedComponent({
  output,
  componentId: "stockfish.engine",
  version: "2026.07.12",
  target,
  payload: [
    [path.resolve(args.stockfish), "bin/stockfish.exe"],
    [path.resolve(args["stockfish-license"]), "licenses/COPYING.txt"],
  ],
  entrypoints: { stockfish: "bin/stockfish.exe" },
  epoch,
});
addArtifact({ ...stockfish, dependencies: ["manager.core"] });

const providerDirectory = path.resolve(args["provider-dir"]);
const providerDependencies = {
  "uci_arena.engine": ["manager.core"],
  "book_forge.service": ["node_runtime.private", "stockfish.engine"],
  "opening_book.standard": [],
  "uci_arena_evidence.service": [
    "node_runtime.private",
    "uci_arena.engine",
    "stockfish.engine",
  ],
};
for (const id of [
  "uci_arena.engine",
  "book_forge.service",
  "opening_book.standard",
  "uci_arena_evidence.service",
]) {
  const resultName = readdirSync(providerDirectory)
    .filter(
      (name) => name.startsWith(`${id}-`) && name.endsWith(".provider-result.json"),
    )
    .sort((left, right) => left.localeCompare(right, "en", { numeric: true }))
    .at(-1);
  if (!resultName) throw new Error(`provider result is missing for ${id}`);
  const result = JSON.parse(readFileSync(path.join(providerDirectory, resultName), "utf8"));
  if (result.component_id !== id || result.target_triple !== target) {
    throw new Error(`provider result identity mismatch for ${id}`);
  }
  const artifact = path.join(providerDirectory, result.artifact_name);
  if (sha256File(artifact) !== result.artifact_sha256) {
    throw new Error(`provider artifact digest mismatch for ${id}`);
  }
  addArtifact({
    id,
    version: result.component_version,
    artifact,
    dependencies: providerDependencies[id],
    workspaceReserveBytes:
      id === "book_forge.service" || id === "uci_arena_evidence.service"
        ? 75 * 1024 ** 3
        : 0,
  });
}

const catalog = {
  schema: "arena_install_catalog_v1",
  schema_version: 1,
  suite_version: "0.1.0",
  channel: "development",
  presets: {
    manager_only: ["manager.core"],
    complete_arena: [
      "manager.core",
      "uci_arena.engine",
      "node_runtime.private",
      "stockfish.engine",
      "book_forge.service",
      "opening_book.standard",
      "uci_arena_evidence.service",
    ],
  },
  components,
};
writeFileSync(path.join(output, "catalog.json"), `${JSON.stringify(catalog, null, 2)}\n`);
process.stdout.write(`${JSON.stringify({
  schema: "arena_local_e2e_catalog_result_v1",
  catalog: path.join(output, "catalog.json"),
  component_count: components.length,
})}\n`);
