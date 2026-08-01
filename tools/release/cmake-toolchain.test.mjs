import assert from "node:assert/strict";
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { resolveCMakeTool } from "./cmake-toolchain.mjs";

function fakeCMake(root, version) {
  const bin = path.join(
    root,
    "build",
    "toolchain",
    `cmake-${version}`,
    `cmake-${version}-windows-x86_64`,
    "bin",
  );
  mkdirSync(bin, { recursive: true });
  for (const command of ["cmake", "cpack", "ctest"]) {
    writeFileSync(path.join(bin, `${command}.exe`), command);
  }
  return path.join(bin, "cmake.exe");
}

test("Windows clean builds prefer the newest supported repository CMake", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-cmake-toolchain-"));
  try {
    fakeCMake(root, "4.2.3");
    const cmake = fakeCMake(root, "4.3.4");
    assert.equal(resolveCMakeTool("cmake", {
      repositoryRoot: root,
      env: {},
      platform: "win32",
    }), cmake);
    assert.equal(resolveCMakeTool("cpack", {
      repositoryRoot: root,
      env: {},
      platform: "win32",
    }), path.join(path.dirname(cmake), "cpack.exe"));
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test("an explicit CMake executable override applies to the complete tool set", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-cmake-override-"));
  try {
    const cmake = fakeCMake(root, "5.0.0");
    assert.equal(resolveCMakeTool("ctest", {
      repositoryRoot: root,
      env: { ARENA_CMAKE_EXECUTABLE: cmake },
      platform: "win32",
    }), path.join(path.dirname(cmake), "ctest.exe"));
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test("tool resolution falls back to PATH when no supported local tool exists", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-cmake-path-"));
  try {
    fakeCMake(root, "4.2.3");
    assert.equal(resolveCMakeTool("cmake", {
      repositoryRoot: root,
      env: {},
      platform: "win32",
    }), "cmake");
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});
