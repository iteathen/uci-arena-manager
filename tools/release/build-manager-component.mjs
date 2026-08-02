#!/usr/bin/env node

import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
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

function parse(argv) {
  const result = { "passed-test": [] };
  for (let index = 0; index < argv.length; index += 2) {
    const flag = argv[index];
    const value = argv[index + 1];
    if (!flag?.startsWith("--") || value === undefined) {
      throw new Error(`invalid argument near ${flag ?? "<end>"}`);
    }
    const name = flag.slice(2);
    if (name === "passed-test") result[name].push(value);
    else if (Object.hasOwn(result, name)) throw new Error(`duplicate --${name}`);
    else result[name] = value;
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
      throw new Error(`symbolic links are forbidden in manager payloads: ${child}`);
    }
    if (status.isDirectory()) rows.push(...files(root, child));
    else if (status.isFile()) rows.push(child.replaceAll("\\", "/"));
    else throw new Error(`unsupported manager payload entry ${child}`);
  }
  return rows;
}

function git(repositoryRoot, args) {
  return execFileSync("git", ["-C", repositoryRoot, ...args], {
    encoding: "utf8",
    windowsHide: true,
  }).trim();
}

function sourceIdentity(repositoryRoot) {
  const commit = git(repositoryRoot, ["rev-parse", "HEAD"]);
  const status = git(repositoryRoot, [
    "status", "--porcelain", "--untracked-files=no",
  ]);
  const tree = git(repositoryRoot, ["ls-tree", "-r", "--full-tree", "HEAD"]);
  return {
    commit,
    clean: status.length === 0,
    treeSha256: sha256(Buffer.from(tree, "utf8")),
  };
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
    const executable = relative.startsWith("bin/")
      || (statSync(path.join(stage, relative)).mode & 0o111) !== 0;
    chunks.push(tarHeader(relative, bytes.length, executable ? 0o755 : 0o644, epoch));
    chunks.push(bytes);
    const padding = (512 - (bytes.length % 512)) % 512;
    if (padding) chunks.push(Buffer.alloc(padding));
  }
  chunks.push(Buffer.alloc(1024));
  writeFileSync(output, gzipSync(Buffer.concat(chunks), { level: 9, mtime: 0 }));
}

function exactDefinition(filename, target) {
  const definition = JSON.parse(readFileSync(filename, "utf8"));
  if (definition?.schema !== "arena_manager_component_definition_v1"
      || definition.component_id !== "manager.core"
      || definition.source_repository !== "iteathen/uci-arena-manager"
      || !Array.isArray(definition.tests)
      || definition.tests.length === 0
      || !definition.entrypoints_by_target?.[target]) {
    throw new Error("invalid Manager component definition");
  }
  return definition;
}

export function buildManagerComponent({
  installTree,
  definitionPath,
  repositoryRoot,
  version,
  target,
  output,
  sourceDateEpoch,
  workflowIdentity,
  passedTests,
  allowDirty = false,
}) {
  if (!/^[0-9]+\.[0-9]+\.[0-9]+(?:[-.][A-Za-z0-9.-]+)?$/u.test(version)) {
    throw new Error("version must be immutable semver text");
  }
  if (!/^(windows|linux)-x86_64$/u.test(target)) {
    throw new Error("unsupported Manager component target");
  }
  if (typeof workflowIdentity !== "string" || !workflowIdentity.includes("@")) {
    throw new Error("workflow identity is required");
  }
  const definition = exactDefinition(definitionPath, target);
  if (passedTests.length !== definition.tests.length
      || passedTests.some((name, index) => name !== definition.tests[index])) {
    throw new Error("passed tests do not match the exact Manager component test set");
  }
  const identity = sourceIdentity(repositoryRoot);
  if (!identity.clean && !allowDirty) {
    throw new Error("Manager component releases require a clean tracked worktree");
  }
  const source = path.resolve(installTree);
  const artifactPath = path.resolve(output);
  const epoch = Number(sourceDateEpoch);
  if (!Number.isSafeInteger(epoch) || epoch < 0) {
    throw new Error("source date epoch must be a nonnegative integer");
  }
  const stage = `${artifactPath}.stage`;
  rmSync(stage, { recursive: true, force: true });
  mkdirSync(stage, { recursive: true });
  try {
    const excluded = new Set(definition.exclude_by_target?.[target] ?? []);
    for (const relative of files(source)) {
      if (excluded.has(relative)) continue;
      const destination = path.join(stage, relative);
      mkdirSync(path.dirname(destination), { recursive: true });
      copyFileSync(path.join(source, relative), destination);
      chmodSync(destination, relative.startsWith("bin/") ? 0o755 : 0o644);
      utimesSync(destination, epoch, epoch);
    }
    const entrypoints = definition.entrypoints_by_target[target];
    const stagedFiles = new Set(files(stage));
    for (const [name, relative] of Object.entries(entrypoints)) {
      if (!stagedFiles.has(relative)) {
        throw new Error(`Manager entrypoint ${name} is missing: ${relative}`);
      }
    }
    const payloadFiles = files(stage).map((relative) => ({
      path: relative,
      sha256: sha256File(path.join(stage, relative)),
      size_bytes: statSync(path.join(stage, relative)).size,
    }));
    const licenseFiles = payloadFiles.filter((file) =>
      /(^|\/)(license|copying|copyright)(\.|$)/iu.test(file.path));
    if (licenseFiles.length === 0) {
      throw new Error("Manager component must include license evidence");
    }
    const sbom = {
      spdxVersion: "SPDX-2.3",
      dataLicense: "CC0-1.0",
      SPDXID: "SPDXRef-DOCUMENT",
      name: `manager.core-${version}-${target}`,
      documentNamespace: `https://uci-arena.example/spdx/${identity.commit}/manager.core/${target}`,
      creationInfo: {
        created: new Date(epoch * 1000).toISOString(),
        creators: ["Tool: UCI-Arena-Manager-component-release/1"],
      },
      files: payloadFiles.map((file, index) => ({
        fileName: `./${file.path}`,
        SPDXID: `SPDXRef-File-${index + 1}`,
        checksums: [{ algorithm: "SHA256", checksumValue: file.sha256 }],
      })),
    };
    const sbomPath = path.join(stage, "sbom.spdx.json");
    writeFileSync(sbomPath, `${JSON.stringify(sbom, null, 2)}\n`);
    const testReceipt = {
      schema: "arena_provider_test_receipt_v1",
      component_id: "manager.core",
      target_triple: target,
      source_commit: identity.commit,
      workflow_identity: workflowIdentity,
      tests: passedTests.map((name) => ({ name, status: "passed" })),
    };
    const testReceiptPath = path.join(stage, "component-test-receipt.json");
    writeFileSync(testReceiptPath, `${JSON.stringify(testReceipt, null, 2)}\n`);
    const componentFiles = files(stage).map((relative) => ({
      path: relative,
      sha256: sha256File(path.join(stage, relative)),
      size_bytes: statSync(path.join(stage, relative)).size,
    }));
    const componentManifest = {
      schema: "arena_install_component_v1",
      schema_version: 1,
      component_id: "manager.core",
      component_version: version,
      target_triple: target,
      entrypoints,
      dependencies: definition.dependencies,
      capabilities: definition.capabilities,
      startup: definition.startup,
      data_paths: definition.data_paths,
      files: componentFiles,
    };
    const manifestPath = path.join(stage, "arena-component.json");
    writeFileSync(manifestPath, `${JSON.stringify(componentManifest, null, 2)}\n`);
    mkdirSync(path.dirname(artifactPath), { recursive: true });
    const repeat = `${artifactPath}.repeat`;
    archive(stage, artifactPath, epoch);
    archive(stage, repeat, epoch);
    if (sha256File(artifactPath) !== sha256File(repeat)) {
      throw new Error("Manager component archive is not reproducible");
    }
    rmSync(repeat, { force: true });
    const result = {
      schema: "arena_provider_release_result_v1",
      component_id: "manager.core",
      component_version: version,
      target_triple: target,
      artifact_name: path.basename(artifactPath),
      artifact_sha256: sha256File(artifactPath),
      artifact_size_bytes: statSync(artifactPath).size,
      component_manifest_sha256: sha256File(manifestPath),
      source_repository: definition.source_repository,
      source_commit: identity.commit,
      source_tree_sha256: identity.treeSha256,
      clean_tree: identity.clean,
      release_authority: "provider_component_only",
      suite_installer_authority: "iteathen/uci-arena-installer",
      workflow_identity: workflowIdentity,
      toolchain_identity: {
        node: process.version,
        platform: process.platform,
        architecture: process.arch,
      },
      dependency_lock_identities: definition.dependency_lock_identities,
      build_flags: definition.build_flags_by_target[target],
      sbom_sha256: sha256File(sbomPath),
      licenses_sha256: sha256(Buffer.concat(
        licenseFiles.map((file) => readFileSync(path.join(stage, file.path))),
      )),
      test_receipt_sha256: sha256File(testReceiptPath),
      tests: testReceipt.tests,
    };
    const resultPath = path.join(
      path.dirname(artifactPath),
      `${path.basename(artifactPath, ".tar.gz")}.provider-result.json`,
    );
    writeFileSync(resultPath, `${JSON.stringify(result, null, 2)}\n`);
    return { artifactPath, resultPath, result };
  } finally {
    rmSync(stage, { recursive: true, force: true });
  }
}

function main() {
  const args = parse(process.argv.slice(2));
  for (const required of [
    "install-tree", "definition", "version", "target", "output",
    "source-date-epoch", "workflow-identity",
  ]) {
    if (!args[required]) throw new Error(`--${required} is required`);
  }
  if (args["passed-test"].length === 0) {
    throw new Error("at least one --passed-test is required");
  }
  const built = buildManagerComponent({
    installTree: args["install-tree"],
    definitionPath: path.resolve(args.definition),
    repositoryRoot: path.resolve(args["repository-root"] ?? process.cwd()),
    version: args.version,
    target: args.target,
    output: args.output,
    sourceDateEpoch: args["source-date-epoch"],
    workflowIdentity: args["workflow-identity"],
    passedTests: args["passed-test"],
    allowDirty: args["allow-dirty"] === "true",
  });
  process.stdout.write(`${JSON.stringify({
    schema: "arena_manager_component_build_v2",
    artifact: built.artifactPath,
    result: built.resultPath,
    sha256: built.result.artifact_sha256,
  })}\n`);
}

if (process.argv[1] && path.resolve(process.argv[1]) === path.resolve(import.meta.filename)) {
  try {
    main();
  } catch (error) {
    process.stderr.write(`build-manager-component: ${error.message}\n`);
    process.exitCode = 1;
  }
}
