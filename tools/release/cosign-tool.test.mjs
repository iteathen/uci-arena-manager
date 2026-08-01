import assert from "node:assert/strict";
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { resolveCosignExecutable } from "./cosign-tool.mjs";

test("Windows composition resolves the pinned repository Cosign verifier", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-cosign-tool-"));
  try {
    const executable = path.join(
      root,
      "build",
      "tooling",
      "cosign-3.0.6",
      "cosign-windows-amd64.exe",
    );
    mkdirSync(path.dirname(executable), { recursive: true });
    writeFileSync(executable, "cosign");
    assert.equal(resolveCosignExecutable("cosign", {
      repositoryRoot: root,
      platform: "win32",
    }), executable);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test("an explicit Cosign executable remains authoritative", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-cosign-override-"));
  try {
    const executable = path.join(root, "custom-cosign.exe");
    writeFileSync(executable, "cosign");
    assert.equal(resolveCosignExecutable(executable, {
      repositoryRoot: root,
      platform: "win32",
    }), executable);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test("Cosign resolution falls back to PATH when no pinned local tool exists", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-cosign-path-"));
  try {
    assert.equal(resolveCosignExecutable("cosign", {
      repositoryRoot: root,
      platform: "win32",
    }), "cosign");
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});
