#!/usr/bin/env node

import { createHash } from "node:crypto";
import { spawnSync } from "node:child_process";
import {
  copyFileSync,
  cpSync,
  existsSync,
  lstatSync,
  mkdirSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import path from "node:path";
import process from "node:process";

const repositoryRoot = path.resolve(import.meta.dirname, "../..");

function fail(message) {
  throw new Error(`complete development build: ${message}`);
}

function run(command, args, {
  capture = false,
  cwd = repositoryRoot,
  env = process.env,
} = {}) {
  const result = spawnSync(command, args, {
    cwd,
    env,
    encoding: "utf8",
    stdio: capture ? "pipe" : "inherit",
  });
  if (result.error) fail(`${command} could not start: ${result.error.message}`);
  if (result.status !== 0) {
    const detail = capture
      ? `: ${(result.stderr || result.stdout || "").trim()}`
      : "";
    fail(`${command} exited with status ${result.status}${detail}`);
  }
  return capture ? result.stdout.trim() : "";
}

function sha256(filename) {
  return createHash("sha256").update(readFileSync(filename)).digest("hex");
}

function parse(argv) {
  const result = {
    platform: "windows-x86_64",
    version: undefined,
    cosignExecutable: process.env.COSIGN_EXECUTABLE ?? "cosign",
  };
  for (let index = 0; index < argv.length; index += 1) {
    const flag = argv[index];
    if (!["--platform", "--version", "--cosign"].includes(flag)) {
      fail(`unknown argument ${flag}`);
    }
    const value = argv[index + 1];
    if (!value || value.startsWith("--")) fail(`${flag} requires a value`);
    index += 1;
    if (flag === "--platform") result.platform = value;
    if (flag === "--version") result.version = value;
    if (flag === "--cosign") result.cosignExecutable = value;
  }
  return result;
}

function exactFile(filename) {
  if (!existsSync(filename) || !lstatSync(filename).isFile() ||
      lstatSync(filename).size < 1) {
    fail(`required file is missing or empty: ${filename}`);
  }
  return filename;
}

function githubToken() {
  if (process.env.GH_MANAGER_TOKEN) return process.env.GH_MANAGER_TOKEN;
  const token = run("gh", ["auth", "token"], { capture: true });
  if (!token) fail("GitHub authentication is unavailable");
  return token;
}

function writeResult({
  outputDirectory,
  version,
  sourceCommit,
  lockPath,
  artifactNames,
}) {
  const artifacts = artifactNames.map((name) => {
    const filename = exactFile(path.join(outputDirectory, name));
    return {
      name,
      size_bytes: lstatSync(filename).size,
      sha256: sha256(filename),
    };
  });
  const result = {
    schema: "arena_clean_build_result_v1",
    schema_version: 1,
    suite_version: version,
    target_platform: "windows-x86_64",
    source_repository: "iteathen/uci-arena-manager",
    source_commit: sourceCommit,
    clean_tree: true,
    build_preset: "windows-release",
    installer_authority: "iteathen/uci-arena-manager",
    release_scope: "complete_arena_development",
    release_classification: "unsigned_complete_development_installer_candidate",
    provider_provenance: "digest_pinned_sigstore_verified",
    release_lock_sha256: sha256(lockPath),
    artifacts,
  };
  writeFileSync(
    path.join(outputDirectory, "clean-build-result.json"),
    `${JSON.stringify(result, null, 2)}\n`,
  );
  return result;
}

export function completeDevelopmentBuild({
  platform,
  version: explicitVersion,
  cosignExecutable,
}) {
  if (platform !== "windows-x86_64") {
    fail(
      "the native development bootstrapper is currently supported only for windows-x86_64",
    );
  }
  const lockPath = path.join(repositoryRoot, "release", "catalog.lock.json");
  const lock = JSON.parse(readFileSync(lockPath, "utf8"));
  const version = explicitVersion ?? lock.suite_version;
  if (!/^[0-9]+\.[0-9]+\.[0-9]+$/u.test(version)) {
    fail(`suite version is invalid: ${version}`);
  }
  if (version !== lock.suite_version || lock.release_ready !== true) {
    fail("version must match a release-ready reviewed catalog lock");
  }

  const sourceCommit = run("git", ["rev-parse", "HEAD"], { capture: true });
  const stagingRoot = path.join(
    repositoryRoot,
    "build",
    "complete-development",
    version,
  );
  const providerDirectory = path.join(stagingRoot, "provider");
  const candidateDirectory = path.join(stagingRoot, "candidate");
  const managerInstall = path.join(stagingRoot, "manager-install");
  const mediaRoot = path.join(stagingRoot, "windows-media");
  const fragment = path.join(stagingRoot, "SuiteMediaPayloads.wxs");
  rmSync(stagingRoot, { recursive: true, force: true });
  mkdirSync(stagingRoot, { recursive: true });

  run("node", [
    "tools/release/clean-build.mjs",
    "--manager-only",
    "--platform",
    platform,
    "--version",
    version,
  ]);
  run("cmake", [
    "--install",
    "build/windows-release",
    "--config",
    "Release",
    "--prefix",
    managerInstall,
  ]);
  const epoch = run(
    "git",
    ["show", "-s", "--format=%ct", "HEAD"],
    { capture: true },
  );
  const managerArchive = path.join(
    stagingRoot,
    `uci-arena-manager-${version}-${platform}.tar.gz`,
  );
  run("node", [
    "tools/release/build-manager-component.mjs",
    "--install-tree",
    managerInstall,
    "--version",
    version,
    "--target",
    platform,
    "--output",
    managerArchive,
    "--source-date-epoch",
    epoch,
  ]);

  run("node", [
    "tools/release/fetch-components.mjs",
    lockPath,
    providerDirectory,
  ], {
    env: {
      ...process.env,
      GH_MANAGER_TOKEN: githubToken(),
      COSIGN_EXECUTABLE: cosignExecutable,
    },
  });
  run("node", [
    "tools/release/assemble-catalog.mjs",
    "--lock",
    lockPath,
    "--provider-dir",
    providerDirectory,
    "--output-dir",
    candidateDirectory,
    "--manager-windows",
    managerArchive,
    "--target",
    platform,
  ]);

  mkdirSync(path.join(mediaRoot, "bin"), { recursive: true });
  mkdirSync(path.join(mediaRoot, "release"), { recursive: true });
  cpSync(path.join(managerInstall, "bin"), path.join(mediaRoot, "bin"), {
    recursive: true,
  });
  copyFileSync(
    path.join(candidateDirectory, "catalog.json"),
    path.join(mediaRoot, "release", "catalog.json"),
  );
  cpSync(
    path.join(candidateDirectory, "payloads"),
    path.join(mediaRoot, "release", "payloads"),
    { recursive: true },
  );
  run("node", [
    "tools/release/build-windows-bundle.mjs",
    "--media-root",
    mediaRoot,
    "--output",
    fragment,
    "--development",
    "true",
  ]);

  const outputDirectory = path.join(repositoryRoot, "dist", "windows");
  const managerMsiName =
    `UCI-Arena-Manager-${version}-windows-x64.msi`;
  const managerZipName =
    `UCI-Arena-Manager-${version}-windows-x64.zip`;
  const managerMsi = exactFile(path.join(outputDirectory, managerMsiName));
  const setup = exactFile(path.join(mediaRoot, "bin", "arena-setup.exe"));
  const buildBundle = (name, offline) => {
    const setupArguments = [
      "apply",
      "--preset",
      "complete_arena",
      ...(offline ? ["--offline"] : []),
      "--yes",
    ].join(" ");
    const repairArguments = [
      "repair",
      "--preset",
      "complete_arena",
      ...(offline ? ["--offline"] : []),
      "--yes",
    ].join(" ");
    run("wix", [
      "build",
      "-ext",
      "WixToolset.BootstrapperApplications.wixext",
      "-d",
      `ProductVersion=${version}`,
      "-d",
      `SuiteSetup=${setup}`,
      "-d",
      `ManagerMsi=${managerMsi}`,
      "-d",
      `SetupArguments=${setupArguments}`,
      "-d",
      `SetupRepairArguments=${repairArguments}`,
      "-d",
      "SetupUninstallArguments=uninstall --all --yes",
      "-d",
      "SetupTrustArguments=--allow-unsigned-development-catalog",
      "-out",
      path.join(outputDirectory, name),
      "packaging/windows/Bundle.wxs",
      fragment,
    ]);
  };
  const online =
    `UCI-Arena-Setup-${version}-windows-x64-online.exe`;
  const offline =
    `UCI-Arena-Setup-${version}-windows-x64-offline.exe`;
  buildBundle(online, false);
  buildBundle(offline, true);

  rmSync(path.join(outputDirectory, managerZipName), { force: true });
  return writeResult({
    outputDirectory,
    version,
    sourceCommit,
    lockPath,
    artifactNames: [online, offline, managerMsiName],
  });
}

function main() {
  const result = completeDevelopmentBuild(parse(process.argv.slice(2)));
  process.stdout.write(
    `complete development build: verified ${result.artifacts.length} ` +
    `${result.target_platform} installers at ${result.source_commit}\n`,
  );
}

if (process.argv[1] &&
    path.resolve(process.argv[1]) === path.resolve(import.meta.filename)) {
  try {
    main();
  } catch (error) {
    process.stderr.write(`${error.message}\n`);
    process.exitCode = 1;
  }
}
