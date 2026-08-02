import assert from "node:assert/strict";
import {
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import test from "node:test";
import {
  cleanBuild,
  expectedInstallerNames,
  verifyInstallerSet,
  writeCleanBuildResult,
} from "./clean-build.mjs";

test("clean build requires an explicit Manager-Core scope locally", () => {
  assert.throws(
    () => cleanBuild({
      platform: "windows-x86_64",
      version: "1.2.3",
    }),
    /scope is required.*protected suite release workflow/,
  );
});

function fixture(platform, version = "1.2.3") {
  const root = mkdtempSync(path.join(tmpdir(), "arena-clean-build-"));
  mkdirSync(root, { recursive: true });
  for (const name of expectedInstallerNames(platform, version)) {
    writeFileSync(path.join(root, name), Buffer.from(`fixture:${name}`, "utf8"));
  }
  return { root, version };
}

test("Windows clean-build result binds the complete exact installer set", () => {
  const { root, version } = fixture("windows-x86_64");
  try {
    const result = writeCleanBuildResult({
      platform: "windows-x86_64",
      version,
      outputDirectory: root,
      sourceCommit: "1".repeat(40),
    });
    assert.equal(result.schema, "arena_clean_build_result_v1");
    assert.equal(result.installer_authority, "iteathen/uci-arena-manager");
    assert.equal(result.release_scope, "manager_core_development");
    assert.deepEqual(
      result.artifacts.map((artifact) => artifact.name),
      expectedInstallerNames("windows-x86_64", version),
    );
    assert.deepEqual(
      result.artifacts.map((artifact) => artifact.name),
      [
        `UCI-Arena-Manager-${version}-windows-x64-user.msi`,
        `UCI-Arena-Manager-${version}-windows-x64-machine.msi`,
        `UCI-Arena-Manager-${version}-windows-x64.zip`,
      ],
    );
    assert.ok(result.artifacts.every((artifact) =>
      /^[0-9a-f]{64}$/u.test(artifact.sha256)));
    assert.deepEqual(
      JSON.parse(readFileSync(path.join(root, "clean-build-result.json"), "utf8")),
      result,
    );
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test("installer verification rejects missing, empty, and stale outputs", () => {
  const missing = fixture("linux-x86_64");
  const expected = expectedInstallerNames("linux-x86_64", missing.version);
  try {
    rmSync(path.join(missing.root, expected[0]));
    assert.throws(
      () => verifyInstallerSet({
        platform: "linux-x86_64",
        version: missing.version,
        outputDirectory: missing.root,
      }),
      /required installers missing/,
    );
  } finally {
    rmSync(missing.root, { recursive: true, force: true });
  }

  const empty = fixture("linux-x86_64");
  try {
    writeFileSync(path.join(empty.root, expected[1]), Buffer.alloc(0));
    assert.throws(
      () => verifyInstallerSet({
        platform: "linux-x86_64",
        version: empty.version,
        outputDirectory: empty.root,
      }),
      /installer is empty/,
    );
  } finally {
    rmSync(empty.root, { recursive: true, force: true });
  }

  const stale = fixture("linux-x86_64");
  try {
    writeFileSync(path.join(stale.root, "uci-arena-manager-old.tar.gz"), "stale");
    assert.throws(
      () => verifyInstallerSet({
        platform: "linux-x86_64",
        version: stale.version,
        outputDirectory: stale.root,
      }),
      /unexpected or stale outputs/,
    );
  } finally {
    rmSync(stale.root, { recursive: true, force: true });
  }
});

test("same-version replacement is explicit and development-only", () => {
  const repositoryRoot = path.resolve(import.meta.dirname, "../..");
  const cleanBuild = readFileSync(
    path.join(repositoryRoot, "tools", "release", "clean-build.mjs"),
    "utf8",
  );
  const cmake = readFileSync(path.join(repositoryRoot, "CMakeLists.txt"), "utf8");
  const wix = readFileSync(
    path.join(repositoryRoot, "packaging", "windows", "Package.wxs.in"),
    "utf8",
  );
  assert.match(
    cleanBuild,
    /ARENA_DEVELOPMENT_SAME_VERSION_UPGRADES=\$\{/u,
  );
  assert.match(
    cleanBuild,
    /developmentSameVersionUpgrade \? "ON" : "OFF"/u,
  );
  assert.match(
    cmake,
    /option\(\s*ARENA_DEVELOPMENT_SAME_VERSION_UPGRADES[\s\S]*OFF\s*\)/u,
  );
  assert.match(
    wix,
    /AllowSameVersionUpgrades="@CPACK_WIX_ALLOW_SAME_VERSION_UPGRADES@"/u,
  );
});

test("Windows clean build validates both fixed-scope MSI databases", () => {
  const repositoryRoot = path.resolve(import.meta.dirname, "../..");
  const source = readFileSync(
    path.join(repositoryRoot, "tools", "release", "clean-build.mjs"),
    "utf8",
  );
  assert.match(
    source,
    /expectedInstallerNames\(platform, version\)[\s\S]*name\.endsWith\("\.msi"\)[\s\S]*run\("wix", \["msi", "validate"/u,
  );
  assert.doesNotMatch(source, /-sice:ICE38/u);
});
