#!/usr/bin/env node

import { createHash } from "node:crypto";
import { spawnSync } from "node:child_process";
import {
  copyFileSync,
  existsSync,
  lstatSync,
  mkdirSync,
  readFileSync,
  readdirSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import path from "node:path";
import process from "node:process";

const repositoryRoot = path.resolve(import.meta.dirname, "../..");
const manifestName = "clean-build-result.json";
const platformDefinitions = {
  "windows-x86_64": {
    preset: "windows-release",
    outputDirectory: path.join("dist", "windows"),
    artifacts(version) {
      const stem = `UCI-Arena-Manager-${version}-windows-x64`;
      return [`${stem}.msi`, `${stem}.zip`];
    },
    generators: [
      ["-C", "Release", "-G", "WIX"],
      ["-C", "Release", "-G", "ZIP"],
    ],
  },
  "linux-x86_64": {
    preset: "linux-release",
    outputDirectory: path.join("dist", "linux"),
    artifacts(version) {
      return [
        `uci-arena-manager_${version}_amd64.deb`,
        `uci-arena-manager-${version}-1.x86_64.rpm`,
        `uci-arena-manager-${version}-linux-x86_64.tar.gz`,
      ];
    },
    generators: [
      ["-G", "DEB"],
      ["-G", "RPM"],
      ["-G", "TGZ"],
    ],
  },
};

function fail(message) {
  throw new Error(`clean build: ${message}`);
}

function sha256(filename) {
  return createHash("sha256").update(readFileSync(filename)).digest("hex");
}

function run(command, args, options = {}) {
  let executable = command;
  let commandArguments = args;
  if (process.platform === "win32" && command === "npm") {
    const npmCli = path.join(
      path.dirname(process.execPath),
      "node_modules",
      "npm",
      "bin",
      "npm-cli.js",
    );
    if (!existsSync(npmCli)) fail(`npm CLI not found beside Node.js: ${npmCli}`);
    executable = process.execPath;
    commandArguments = [npmCli, ...args];
  }
  const result = spawnSync(executable, commandArguments, {
    cwd: options.cwd ?? repositoryRoot,
    env: process.env,
    stdio: options.capture ? "pipe" : "inherit",
    encoding: "utf8",
  });
  if (result.error) fail(`${command} could not start: ${result.error.message}`);
  if (result.status !== 0) {
    const detail = options.capture
      ? `: ${(result.stderr || result.stdout || "").trim()}`
      : "";
    fail(`${command} exited with status ${result.status}${detail}`);
  }
  return options.capture ? result.stdout.trim() : "";
}

function parseArguments(argv) {
  const parsed = {
    platform: process.platform === "win32"
      ? "windows-x86_64"
      : "linux-x86_64",
    version: undefined,
    tufRootSha256: "",
    verifyOnly: false,
  };
  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    if (argument === "--verify-only") {
      parsed.verifyOnly = true;
      continue;
    }
    if (!["--platform", "--version", "--tuf-root-sha256"].includes(argument)) {
      fail(`unknown argument ${argument}`);
    }
    const value = argv[index + 1];
    if (!value || value.startsWith("--")) fail(`${argument} requires a value`);
    index += 1;
    if (argument === "--platform") parsed.platform = value;
    if (argument === "--version") parsed.version = value;
    if (argument === "--tuf-root-sha256") parsed.tufRootSha256 = value;
  }
  return parsed;
}

function suiteVersion(explicitVersion) {
  const version = explicitVersion ?? JSON.parse(readFileSync(
    path.join(repositoryRoot, "release", "catalog.lock.json"),
    "utf8",
  )).suite_version;
  if (!/^[0-9]+\.[0-9]+\.[0-9]+$/u.test(version)) {
    fail(`suite version must be a three-field semantic version, received ${version}`);
  }
  return version;
}

function definitionFor(platform) {
  const definition = platformDefinitions[platform];
  if (!definition) {
    fail(`unsupported platform ${platform}; expected ${Object.keys(
      platformDefinitions,
    ).join(" or ")}`);
  }
  return definition;
}

function assertCanonicalOutputDirectory(platform, outputDirectory) {
  const expected = path.resolve(
    repositoryRoot,
    definitionFor(platform).outputDirectory,
  );
  const resolved = path.resolve(outputDirectory);
  if (resolved !== expected) {
    fail(`output directory must be ${expected}`);
  }
  return resolved;
}

function sourceIdentity() {
  const status = run(
    "git",
    ["status", "--porcelain=v1", "--untracked-files=no"],
    { capture: true },
  );
  if (status) fail(`tracked worktree is not clean:\n${status}`);
  const commit = run("git", ["rev-parse", "HEAD"], { capture: true });
  if (!/^[0-9a-f]{40}$/u.test(commit)) fail("source commit is not a SHA-1");
  return { commit, clean: true };
}

function resetOutputDirectory(platform) {
  const outputDirectory = assertCanonicalOutputDirectory(
    platform,
    path.join(repositoryRoot, definitionFor(platform).outputDirectory),
  );
  rmSync(outputDirectory, { recursive: true, force: true });
  mkdirSync(outputDirectory, { recursive: true });
  return outputDirectory;
}

function resetPackagingDirectory(buildDirectory) {
  const expectedRoot = path.resolve(repositoryRoot, "build");
  const packagingDirectory = path.resolve(
    buildDirectory,
    "clean-build-packages",
  );
  const relative = path.relative(expectedRoot, packagingDirectory);
  if (!relative || relative.startsWith("..") || path.isAbsolute(relative)) {
    fail("packaging staging directory escaped the repository build root");
  }
  rmSync(packagingDirectory, { recursive: true, force: true });
  mkdirSync(packagingDirectory, { recursive: true });
  return packagingDirectory;
}

function copyInstallerOutputs({
  platform,
  version,
  packagingDirectory,
  outputDirectory,
}) {
  for (const name of expectedInstallerNames(platform, version)) {
    const source = path.join(packagingDirectory, name);
    let stat;
    try {
      stat = lstatSync(source);
    } catch {
      fail(`CPack did not emit required installer: ${name}`);
    }
    if (!stat.isFile() || stat.size < 1) {
      fail(`CPack installer is not a nonempty regular file: ${name}`);
    }
    copyFileSync(source, path.join(outputDirectory, name));
  }
}

export function expectedInstallerNames(platform, version) {
  return definitionFor(platform).artifacts(version);
}

export function verifyInstallerSet({
  platform,
  version,
  outputDirectory,
}) {
  const expected = expectedInstallerNames(platform, version);
  const allowed = new Set([...expected, manifestName]);
  const entries = readdirSync(outputDirectory, { withFileTypes: true });
  const unexpected = entries
    .filter((entry) => !allowed.has(entry.name))
    .map((entry) => entry.name)
    .sort();
  if (unexpected.length > 0) {
    fail(`unexpected or stale outputs: ${unexpected.join(", ")}`);
  }
  const names = new Set(entries.map((entry) => entry.name));
  const missing = expected.filter((name) => !names.has(name));
  if (missing.length > 0) fail(`required installers missing: ${missing.join(", ")}`);

  const caseFolded = entries.map((entry) => entry.name.toLowerCase());
  if (new Set(caseFolded).size !== caseFolded.length) {
    fail("ambiguous case-insensitive output names");
  }

  return expected.map((name) => {
    const filename = path.join(outputDirectory, name);
    const stat = lstatSync(filename);
    if (!stat.isFile()) fail(`installer is not a regular file: ${name}`);
    if (stat.size < 1) fail(`installer is empty: ${name}`);
    return {
      name,
      size_bytes: stat.size,
      sha256: sha256(filename),
    };
  });
}

export function writeCleanBuildResult({
  platform,
  version,
  outputDirectory,
  sourceCommit,
}) {
  const definition = definitionFor(platform);
  const result = {
    schema: "arena_clean_build_result_v1",
    schema_version: 1,
    suite_version: version,
    target_platform: platform,
    source_repository: "iteathen/uci-arena-manager",
    source_commit: sourceCommit,
    clean_tree: true,
    build_preset: definition.preset,
    installer_authority: "iteathen/uci-arena-manager",
    release_classification: "unsigned_platform_installer_candidate",
    artifacts: verifyInstallerSet({ platform, version, outputDirectory }),
  };
  writeFileSync(
    path.join(outputDirectory, manifestName),
    `${JSON.stringify(result, null, 2)}\n`,
  );
  return result;
}

export function cleanBuild({
  platform,
  version,
  tufRootSha256 = "",
  verifyOnly = false,
}) {
  const definition = definitionFor(platform);
  if (tufRootSha256 && !/^[0-9a-f]{64}$/u.test(tufRootSha256)) {
    fail("TUF root SHA-256 must be empty or exactly 64 lowercase hexadecimal characters");
  }
  const identity = sourceIdentity();
  const outputDirectory = path.resolve(repositoryRoot, definition.outputDirectory);

  if (!verifyOnly) {
    resetOutputDirectory(platform);
    run("node", ["tools/spec-check.mjs"]);
    run("npm", ["run", "naming:check"]);
    run("npm", ["test", "--prefix", "web"]);
    run("npm", ["run", "build", "--prefix", "web"]);
    run("cmake", [
      "--preset",
      definition.preset,
      `-DARENA_VERSION=${version}`,
      `-DARENA_TUF_ROOT_SHA256=${tufRootSha256}`,
    ]);
    run("cmake", ["--build", "--preset", definition.preset, "--clean-first"]);
    run("ctest", ["--preset", definition.preset]);
    const buildDirectory = path.join(repositoryRoot, "build", definition.preset);
    const packagingDirectory = resetPackagingDirectory(buildDirectory);
    try {
      for (const generator of definition.generators) {
        run("cpack", [...generator, "-B", packagingDirectory], {
          cwd: buildDirectory,
        });
      }
      copyInstallerOutputs({
        platform,
        version,
        packagingDirectory,
        outputDirectory,
      });
    } finally {
      rmSync(packagingDirectory, { recursive: true, force: true });
    }
  }

  return writeCleanBuildResult({
    platform,
    version,
    outputDirectory,
    sourceCommit: identity.commit,
  });
}

function main() {
  const args = parseArguments(process.argv.slice(2));
  const version = suiteVersion(args.version);
  const result = cleanBuild({ ...args, version });
  process.stdout.write(
    `clean build: verified ${result.artifacts.length} ${result.target_platform} installers at ${result.source_commit}\n`,
  );
}

if (process.argv[1] && path.resolve(process.argv[1]) === path.resolve(import.meta.filename)) {
  try {
    main();
  } catch (error) {
    process.stderr.write(`${error.message}\n`);
    process.exitCode = 1;
  }
}
