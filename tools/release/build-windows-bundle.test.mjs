import assert from "node:assert/strict";
import {
  readFileSync,
  mkdirSync,
  mkdtempSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import { buildPayloadFragment } from "./build-windows-bundle.mjs";

const repositoryRoot = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)),
  "..",
  "..",
);

test("Windows bundle payloads are deterministic and preserve suite media paths", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-windows-media-"));
  try {
    mkdirSync(path.join(root, "bin"), { recursive: true });
    mkdirSync(path.join(root, "release", "tuf"), { recursive: true });
    mkdirSync(path.join(root, "release", "payloads"), { recursive: true });
    writeFileSync(path.join(root, "bin", "arena-setup.exe"), "setup");
    writeFileSync(path.join(root, "bin", "libcurl.dll"), "runtime");
    writeFileSync(path.join(root, "release", "catalog.json"), "{}");
    writeFileSync(path.join(root, "release", "tuf", "root.json"), "{}");
    writeFileSync(path.join(root, "release", "payloads", "component.tar.gz"), "x");

    const first = buildPayloadFragment(root);
    const second = buildPayloadFragment(root);
    assert.equal(first, second);
    assert.match(first, /Name="release\\catalog\.json"/);
    assert.match(first, /Name="release\\tuf\\root\.json"/);
    assert.match(first, /Name="release\\payloads\\component\.tar\.gz"/);
    assert.match(first, /Name="libcurl\.dll"/);
    assert.doesNotMatch(first, /Name="bin\\libcurl\.dll"/);
    assert.doesNotMatch(first, /Name="bin\\arena-setup\.exe"/);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test("Windows bundle owns complete install, repair, and uninstall lifecycle", () => {
  const source = readFileSync(
    path.join(repositoryRoot, "packaging", "windows", "Bundle.wxs"),
    "utf8",
  );
  assert.match(source, /<MsiPackage[\s\S]*?Visible="no"/);
  assert.match(source, /<MsiPackage[\s\S]*?<ExePackage/);
  assert.match(source, /DetectCondition="WixBundleInstalled"/);
  assert.match(source, /Protocol="none"/);
  assert.match(source, /Name="UCI Arena Suite Setup"/);
  assert.doesNotMatch(source, /Name="\$\(var\.ProductName\)"/);
  assert.match(
    source,
    /--catalog &quot;\[WixBundleExecutePackageCacheFolder\]release\\catalog\.json&quot;/,
  );
  assert.match(
    source,
    /--bundle-owner &quot;\[WixBundleProviderKey\]&quot;/,
  );
  assert.match(
    source,
    /<MsiProperty[\s\S]*?Name="ARENA_DATA_ROOT"[\s\S]*?Value="\$\(var\.ArenaDataRoot\)"/,
  );
  assert.match(
    source,
    /InstallArguments="[^"]*--data-root &quot;\$\(var\.ArenaDataRoot\)&quot;[^"]*--restart-manager-service/,
  );
  assert.match(
    source,
    /RepairArguments="[^"]*--data-root &quot;\$\(var\.ArenaDataRoot\)&quot;[^"]*--restart-manager-service/,
  );
  assert.match(
    source,
    /UninstallArguments="[^"]*--data-root &quot;\$\(var\.ArenaDataRoot\)&quot;/,
  );
  assert.match(source, /RepairArguments="[\s\S]*\$\(var\.SetupRepairArguments\)"/);
  assert.match(source, /UninstallArguments="[\s\S]*\$\(var\.SetupUninstallArguments\)"/);
  assert.match(source, /Permanent="no"/);
  assert.doesNotMatch(source, /Permanent="yes"/);
});

test("Windows release workflow keeps channel labels out of the visible product name", () => {
  const source = readFileSync(
    path.join(repositoryRoot, ".github", "workflows", "release.yml"),
    "utf8",
  );
  assert.doesNotMatch(source, /ProductName=UCI Arena Suite Setup/);
  assert.doesNotMatch(source, /--catalog release\\catalog\.json/);
});

test("Windows Manager MSI patches the complete staged tree with service lifecycle", () => {
  const cmake = readFileSync(
    path.join(repositoryRoot, "CMakeLists.txt"),
    "utf8",
  );
  const patch = readFileSync(
    path.join(repositoryRoot, "packaging", "windows", "ServicePatch.xml"),
    "utf8",
  );
  assert.match(cmake, /CPACK_RESOURCE_FILE_LICENSE "\$\{CMAKE_CURRENT_BINARY_DIR\}\/LICENSE\.txt"/);
  assert.match(cmake, /CPACK_WIX_PATCH_FILE/);
  assert.match(patch, /Id="CM_CP_bin\.arena_service\.exe"/);
  assert.match(patch, /<ServiceInstall/);
  assert.match(patch, /Account="NT AUTHORITY\\LocalService"/);
  assert.match(patch, /<CPackWiXFragment Id="#PRODUCT">/);
  assert.match(patch, /<Property Id="ARENA_DATA_ROOT" Secure="yes"/);
  assert.match(patch, /--data-dir &quot;\[ARENA_DATA_ROOT\]&quot;/);
  assert.match(patch, /<ServiceControl/);
  assert.match(patch, /Remove="uninstall"/);
  assert.match(patch, /\[INSTALL_ROOT\]share\\uci-arena\\web/);
});
