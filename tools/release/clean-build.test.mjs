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
  expectedInstallerNames,
  verifyInstallerSet,
  writeCleanBuildResult,
} from "./clean-build.mjs";

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
    assert.deepEqual(
      result.artifacts.map((artifact) => artifact.name),
      expectedInstallerNames("windows-x86_64", version),
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

test("release workflow delegates platform packaging to the canonical clean build", () => {
  const repositoryRoot = path.resolve(import.meta.dirname, "../..");
  const workflow = readFileSync(
    path.join(repositoryRoot, ".github", "workflows", "release.yml"),
    "utf8",
  );
  const managerJob = workflow.match(
    /^  build-manager:[\s\S]*?(?=^  compose-candidate:)/mu,
  )?.[0] ?? "";
  assert.match(
    managerJob,
    /node tools\/release\/clean-build\.mjs[\s\S]*--platform '\$\{\{ matrix\.target \}\}'/,
  );
  assert.doesNotMatch(managerJob, /^\s+cpack /mu);
});
