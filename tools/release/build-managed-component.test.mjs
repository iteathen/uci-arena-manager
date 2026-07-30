import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import {
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const repositoryRoot = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)),
  "../..",
);
const script = path.join(
  repositoryRoot,
  "tools",
  "release",
  "build-managed-component.mjs",
);

function sha256(filename) {
  return createHash("sha256").update(readFileSync(filename)).digest("hex");
}

test("managed Node candidate is native-smoked, licensed, and reproducible", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-managed-component-"));
  try {
    const license = path.join(root, "LICENSE");
    writeFileSync(license, "test license\n");
    const version = process.version.slice(1);
    const common = [
      script,
      "--component-id", "node_runtime.private",
      "--version", version,
      "--target", process.platform === "win32"
        ? "windows-x86_64"
        : "linux-x86_64",
      "--executable", process.execPath,
      "--license", license,
      "--source-url", "https://nodejs.org/test-input",
      "--source-sha256", "a".repeat(64),
      "--source-commit", "9".repeat(40),
      "--workflow-identity",
      "iteathen/uci-arena-manager/"
        + ".github/workflows/managed-component-release.yml"
        + "@refs/heads/main#123.1",
      "--source-date-epoch", "1700000000",
    ];
    const first = path.join(root, "first");
    const second = path.join(root, "second");
    mkdirSync(first);
    mkdirSync(second);
    execFileSync(process.execPath, [...common, "--output-dir", first]);
    execFileSync(process.execPath, [...common, "--output-dir", second]);
    const base = `node_runtime.private-${version}-${process.platform === "win32"
      ? "windows-x86_64"
      : "linux-x86_64"}`;
    const firstArtifact = path.join(first, `${base}.tar.gz`);
    const secondArtifact = path.join(second, `${base}.tar.gz`);
    assert.equal(sha256(firstArtifact), sha256(secondArtifact));
    const result = JSON.parse(
      readFileSync(path.join(first, `${base}.managed-result.json`), "utf8"),
    );
    assert.equal(result.artifact_sha256, sha256(firstArtifact));
    assert.equal(result.validation_evidence.executable_smoke.status, "passed");
    assert.equal(result.validation_evidence.executable_smoke.protocol, "node_version");
    const manifest = JSON.parse(execFileSync(
      "tar",
      ["-xOzf", firstArtifact, "arena-component.json"],
      { encoding: "utf8" },
    ));
    assert.deepEqual(manifest.entrypoints, {
      node: `bin/${process.platform === "win32" ? "node.exe" : "node"}`,
    });
    assert.deepEqual(result.upstream_inputs, [{
      role: "source",
      url: "https://nodejs.org/test-input",
      sha256: "a".repeat(64),
    }]);
    assert.ok(result.installed_bytes > result.artifact_size_bytes);
    assert.match(result.component_manifest_sha256, /^[0-9a-f]{64}$/u);
    assert.equal(result.licenses_sha256, sha256(license));
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});
