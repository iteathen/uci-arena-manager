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
    ]) {
      writeFileSync(path.join(bin, name), `${name}\n`);
    }
    const first = path.join(root, "first.tar.gz");
    const second = path.join(root, "second.tar.gz");
    const common = [
      script,
      "--install-tree",
      installTree,
      "--version",
      "0.1.0",
      "--target",
      "windows-x86_64",
      "--source-date-epoch",
      "1700000000",
    ];
    execFileSync(process.execPath, [...common, "--output", first]);
    await new Promise((resolve) => setTimeout(resolve, 1100));
    execFileSync(process.execPath, [...common, "--output", second]);
    assert.equal(sha256(first), sha256(second));
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});
