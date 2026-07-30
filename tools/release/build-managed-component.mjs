#!/usr/bin/env node

import { spawnSync } from "node:child_process";
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
  utimesSync,
  writeFileSync,
} from "node:fs";
import path from "node:path";
import process from "node:process";
import { gzipSync } from "node:zlib";

const COMPONENTS = Object.freeze({
  "node_runtime.private": {
    executable: { windows: "node.exe", linux: "node" },
    entrypoint: "node",
    licenseName: "node-LICENSE",
    capability: "private_node_runtime",
    smokeProtocol: "node_version",
  },
  "stockfish.engine": {
    executable: { windows: "stockfish.exe", linux: "stockfish" },
    entrypoint: "stockfish",
    licenseName: "stockfish-Copying.txt",
    capability: "uci_engine",
    smokeProtocol: "uci_startup",
  },
});
const TARGETS = new Set(["windows-x86_64", "linux-x86_64"]);
const SHA256_PATTERN = /^[0-9a-f]{64}$/u;
const COMMIT_PATTERN = /^[0-9a-f]{40}$/u;
const VERSION_PATTERN = /^\d+\.\d+(?:\.\d+)?(?:[-.][A-Za-z0-9.-]+)?$/u;

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

function canonicalJson(value) {
  return `${JSON.stringify(value, null, 2)}\n`;
}

function sha256File(filename) {
  return createHash("sha256").update(readFileSync(filename)).digest("hex");
}

function files(root, relative = "") {
  const rows = [];
  for (const name of readdirSync(path.join(root, relative)).sort()) {
    const child = relative ? `${relative}/${name}` : name;
    const absolute = path.join(root, child);
    const status = lstatSync(absolute);
    if (status.isSymbolicLink()) {
      throw new Error(`symbolic links are forbidden in managed payloads: ${child}`);
    }
    if (status.isDirectory()) rows.push(...files(root, child));
    else if (status.isFile()) rows.push(child.replaceAll("\\", "/"));
    else throw new Error(`unsupported managed payload entry ${child}`);
  }
  return rows;
}

function writeOctal(buffer, offset, length, value) {
  const encoded = Math.trunc(value).toString(8).padStart(length - 1, "0");
  if (encoded.length >= length) throw new Error(`tar numeric field overflow: ${value}`);
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
  throw new Error(`managed component path is too long for ustar: ${relative}`);
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

function requireRegularFile(filename, label) {
  const status = lstatSync(filename);
  if (!status.isFile() || status.isSymbolicLink()) {
    throw new Error(`${label} must be a regular file`);
  }
}

function smokeExecutable(componentId, version, executable) {
  if (componentId === "node_runtime.private") {
    const result = spawnSync(executable, ["--version"], {
      encoding: "utf8",
      timeout: 30_000,
      windowsHide: true,
    });
    if (result.error || result.status !== 0 || result.stdout.trim() !== `v${version}`) {
      throw new Error(`Node.js smoke failed: ${result.error?.message ?? result.stderr}`);
    }
    return { status: "passed", protocol: "node_version", observed_version: `v${version}` };
  }
  const result = spawnSync(executable, [], {
    encoding: "utf8",
    input: "uci\nisready\nposition startpos\ngo depth 1\nquit\n",
    maxBuffer: 16 * 1024 * 1024,
    timeout: 30_000,
    windowsHide: true,
  });
  const output = result.stdout ?? "";
  const validBestMove = /^bestmove [a-h][1-8][a-h][1-8][qrbn]?(?:\s|$)/mu.test(output);
  if (result.error
      || result.status !== 0
      || !output.includes(`id name Stockfish ${version}`)
      || !/^uciok$/mu.test(output)
      || !/^readyok$/mu.test(output)
      || !validBestMove) {
    throw new Error(`Stockfish UCI smoke failed: ${result.error?.message ?? result.stderr}`);
  }
  return {
    status: "passed",
    protocol: "uci_startup",
    observed_version: `Stockfish ${version}`,
    uciok: true,
    readyok: true,
    valid_bestmove: true,
  };
}

const args = parse(process.argv.slice(2));
for (const required of [
  "component-id",
  "version",
  "target",
  "executable",
  "license",
  "source-url",
  "source-sha256",
  "source-commit",
  "workflow-identity",
  "output-dir",
  "source-date-epoch",
]) {
  if (!args[required]) throw new Error(`--${required} is required`);
}
const componentId = args["component-id"];
const definition = COMPONENTS[componentId];
if (!definition) throw new Error(`unsupported managed component ${componentId}`);
if (!VERSION_PATTERN.test(args.version)) throw new Error("--version is invalid");
if (!TARGETS.has(args.target)) throw new Error("--target is invalid");
if (!/^https:\/\//u.test(args["source-url"])) throw new Error("--source-url must use HTTPS");
if (!SHA256_PATTERN.test(args["source-sha256"])) {
  throw new Error("--source-sha256 must be lowercase SHA-256");
}
if (!COMMIT_PATTERN.test(args["source-commit"])) {
  throw new Error("--source-commit must be an exact commit");
}
if (!/^iteathen\/uci-arena-manager\/\.github\/workflows\/managed-component-release\.yml@refs\/heads\/main#[1-9][0-9]*\.[1-9][0-9]*$/u.test(
  args["workflow-identity"],
)) {
  throw new Error("--workflow-identity must name the protected managed workflow");
}
if (componentId === "stockfish.engine"
    && !/^https:\/\//u.test(args["source-offer-url"] ?? "")) {
  throw new Error("Stockfish requires --source-offer-url");
}
if (componentId === "stockfish.engine"
    && (!/^https:\/\//u.test(args["network-url"] ?? "")
      || !SHA256_PATTERN.test(args["network-sha256"] ?? ""))) {
  throw new Error("Stockfish requires an exact evaluation-network input");
}
const executable = path.resolve(args.executable);
const license = path.resolve(args.license);
requireRegularFile(executable, "managed executable");
requireRegularFile(license, "managed license");
const validationEvidence = smokeExecutable(componentId, args.version, executable);
const epoch = Number(args["source-date-epoch"]);
if (!Number.isSafeInteger(epoch) || epoch < 0) {
  throw new Error("--source-date-epoch must be a nonnegative integer");
}

const outputDirectory = path.resolve(args["output-dir"]);
mkdirSync(outputDirectory, { recursive: true });
const baseName = `${componentId}-${args.version}-${args.target}`;
const artifactName = `${baseName}.tar.gz`;
const resultName = `${baseName}.managed-result.json`;
const artifactPath = path.join(outputDirectory, artifactName);
const resultPath = path.join(outputDirectory, resultName);
const stage = path.join(outputDirectory, `${baseName}.stage`);
for (const filename of [artifactPath, resultPath, stage]) {
  if (lstatExists(filename)) throw new Error(`refusing to overwrite ${filename}`);
}
mkdirSync(path.join(stage, "bin"), { recursive: true });
mkdirSync(path.join(stage, "licenses"), { recursive: true });
const platform = args.target.startsWith("windows") ? "windows" : "linux";
const executableRelative = `bin/${definition.executable[platform]}`;
const licenseRelative = `licenses/${definition.licenseName}`;
copyFileSync(executable, path.join(stage, executableRelative));
copyFileSync(license, path.join(stage, licenseRelative));
chmodSync(path.join(stage, executableRelative), 0o755);
chmodSync(path.join(stage, licenseRelative), 0o644);
utimesSync(path.join(stage, executableRelative), epoch, epoch);
utimesSync(path.join(stage, licenseRelative), epoch, epoch);
const payloadFiles = files(stage).map((relative) => ({
  path: relative,
  sha256: sha256File(path.join(stage, relative)),
  size_bytes: statSync(path.join(stage, relative)).size,
}));
const manifestPath = path.join(stage, "arena-component.json");
writeFileSync(manifestPath, canonicalJson({
  schema: "arena_install_component_v1",
  schema_version: 1,
  component_id: componentId,
  component_version: args.version,
  target_triple: args.target,
  entrypoints: { [definition.entrypoint]: executableRelative },
  capabilities: [definition.capability],
  startup: {},
  data_paths: [],
  files: payloadFiles,
}));
chmodSync(manifestPath, 0o644);
utimesSync(manifestPath, epoch, epoch);
archive(stage, artifactPath, epoch);
const repeatPath = `${artifactPath}.repeat`;
archive(stage, repeatPath, epoch);
if (sha256File(artifactPath) !== sha256File(repeatPath)) {
  throw new Error("managed component archive is not reproducible");
}
rmSync(repeatPath, { force: true });
const result = {
  schema: "arena_managed_component_release_result_v1",
  release_authority: "manager_managed_component_only",
  suite_installer_authority: "iteathen/uci-arena-manager",
  source_repository: "iteathen/uci-arena-manager",
  source_commit: args["source-commit"],
  component_id: componentId,
  component_version: args.version,
  target_triple: args.target,
  artifact_name: artifactName,
  artifact_sha256: sha256File(artifactPath),
  artifact_size_bytes: statSync(artifactPath).size,
  installed_bytes: files(stage).reduce(
    (total, relative) => total + statSync(path.join(stage, relative)).size,
    0,
  ),
  component_manifest_sha256: sha256File(manifestPath),
  licenses_sha256: sha256File(path.join(stage, licenseRelative)),
  upstream_inputs: [
    {
      role: "source",
      url: args["source-url"],
      sha256: args["source-sha256"],
    },
    ...(componentId === "stockfish.engine"
      ? [{
          role: "evaluation_network",
          url: args["network-url"],
          sha256: args["network-sha256"],
        }]
      : []),
  ],
  ...(args["source-offer-url"] ? { source_offer_url: args["source-offer-url"] } : {}),
  validation_evidence: { executable_smoke: validationEvidence },
  workflow_identity: args["workflow-identity"],
};
writeFileSync(resultPath, canonicalJson(result), { flag: "wx" });
rmSync(stage, { recursive: true, force: true });
process.stdout.write(canonicalJson({
  schema: "arena_managed_component_build_v1",
  artifact_path: artifactPath,
  result_path: resultPath,
  result,
}));

function lstatExists(filename) {
  try {
    lstatSync(filename);
    return true;
  } catch (error) {
    if (error?.code === "ENOENT") return false;
    throw error;
  }
}
