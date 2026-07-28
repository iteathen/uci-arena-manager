import assert from "node:assert/strict";
import {
  mkdirSync,
  mkdtempSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { buildPayloadFragment } from "./build-windows-bundle.mjs";

test("Windows bundle payloads are deterministic and preserve suite media paths", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-windows-media-"));
  try {
    mkdirSync(path.join(root, "bin"), { recursive: true });
    mkdirSync(path.join(root, "release", "tuf"), { recursive: true });
    mkdirSync(path.join(root, "release", "payloads"), { recursive: true });
    writeFileSync(path.join(root, "bin", "arena-setup.exe"), "setup");
    writeFileSync(path.join(root, "release", "catalog.json"), "{}");
    writeFileSync(path.join(root, "release", "tuf", "root.json"), "{}");
    writeFileSync(path.join(root, "release", "payloads", "component.tar.gz"), "x");

    const first = buildPayloadFragment(root);
    const second = buildPayloadFragment(root);
    assert.equal(first, second);
    assert.match(first, /Name="release\\catalog\.json"/);
    assert.match(first, /Name="release\\tuf\\root\.json"/);
    assert.match(first, /Name="release\\payloads\\component\.tar\.gz"/);
    assert.doesNotMatch(first, /Name="bin\\arena-setup\.exe"/);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});
