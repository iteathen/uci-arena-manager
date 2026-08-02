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
import { tmpdir } from "node:os";
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
  "build-manager-component.mjs",
);

function sha256(filename) {
  return createHash("sha256").update(readFileSync(filename)).digest("hex");
}

test("Manager component archives accept a source epoch and remain reproducible", async () => {
  const root = mkdtempSync(path.join(tmpdir(), "arena-manager-component-"));
  try {
    const installTree = path.join(root, "install");
    const bin = path.join(installTree, "bin");
    mkdirSync(bin, { recursive: true });
    for (const name of [
      "arena-service.exe",
      "arena.exe",
      "arena-broker.exe",
      "arena-setup.exe",
      "uci-arena-manager.exe",
    ]) {
      writeFileSync(path.join(bin, name), `${name}\n`);
    }
    const licenses = path.join(installTree, "share", "licenses", "manager");
    mkdirSync(licenses, { recursive: true });
    writeFileSync(path.join(licenses, "LICENSE"), "synthetic test license\n");
    const first = path.join(root, "first.tar.gz");
    const second = path.join(root, "second.tar.gz");
    const common = [
      script,
      "--install-tree",
      installTree,
      "--definition",
      path.join(repositoryRoot, "release", "components", "manager-core.json"),
      "--version",
      "0.1.0",
      "--target",
      "windows-x86_64",
      "--source-date-epoch",
      "1700000000",
      "--workflow-identity",
      "iteathen/uci-arena-manager/.github/workflows/component-release.yml@refs/heads/main#1.1",
      "--passed-test",
      "manager_spec_web_release_checks",
      "--passed-test",
      "manager_native_tests",
      "--passed-test",
      "manager_component_install_tree",
      "--allow-dirty",
      "true",
    ];
    execFileSync(process.execPath, [...common, "--output", first]);
    await new Promise((resolve) => setTimeout(resolve, 1100));
    execFileSync(process.execPath, [...common, "--output", second]);
    assert.equal(sha256(first), sha256(second));
    const result = JSON.parse(readFileSync(
      path.join(root, "first.provider-result.json"),
      "utf8",
    ));
    assert.equal(result.component_id, "manager.core");
    assert.equal(result.suite_installer_authority, "iteathen/uci-arena-installer");
    assert.equal(result.tests.length, 3);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});
