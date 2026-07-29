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
  assert.match(source, /RepairArguments="[^"]*\$\(var\.SetupRepairArguments\)[^"]*"/);
  assert.match(source, /UninstallArguments="[\s\S]*\$\(var\.SetupUninstallArguments\)"/);
  assert.match(source, /Permanent="no"/);
  assert.doesNotMatch(source, /Permanent="yes"/);
  for (const variable of [
    "InstallUciArenaEngine",
    "InstallBookForge",
    "InstallEvidenceService",
    "InstallLichessAdapter",
  ]) {
    assert.match(
      source,
      new RegExp(`Name="${variable}"[\\s\\S]*?Value="1"[\\s\\S]*?Persisted="yes"`),
    );
    assert.match(
      source,
      new RegExp(`--bundle-feature [^"]*=\\[${variable}\\]`),
    );
  }
  assert.match(source, /ThemeFile="packaging\/windows\/SuiteTheme\.xml"/);
  const theme = readFileSync(
    path.join(repositoryRoot, "packaging", "windows", "SuiteTheme.xml"),
    "utf8",
  );
  assert.match(theme, /Name="InstallUciArenaEngine"[\s\S]*recommended default/);
  assert.match(theme, /Name="ModifyOptionsButton"/);
  assert.match(theme, /Choose Components/);
  const setupSource = readFileSync(
    path.join(repositoryRoot, "apps", "setup", "main.cpp"),
    "utf8",
  );
  assert.match(setupSource, /bool manager_service_running\(\)/);
  assert.match(
    setupSource,
    /manager_was_running = manager_service_running\(\)/,
  );
  assert.match(
    setupSource,
    /manager_service_state_preserved/,
  );
});

test("Windows release workflow keeps channel labels out of the visible product name", () => {
  const source = readFileSync(
    path.join(repositoryRoot, ".github", "workflows", "release.yml"),
    "utf8",
  );
  assert.doesNotMatch(source, /ProductName=UCI Arena Suite Setup/);
  assert.doesNotMatch(source, /--catalog release\\catalog\.json/);
  assert.equal(
    source.match(/dotnet tool install --global wix --version 6\.0\.2/g)?.length,
    2,
  );
  assert.match(
    source,
    /wix extension add -g WixToolset\.BootstrapperApplications\.wixext\/6\.0\.2/,
  );
  assert.equal(
    source.match(/-ext WixToolset\.BootstrapperApplications\.wixext/g)?.length,
    2,
  );
  assert.doesNotMatch(source, /WixToolset\.Bal\.wixext/);
  assert.doesNotMatch(source, /wix --version 7\./);
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
  const packageTemplate = readFileSync(
    path.join(repositoryRoot, "packaging", "windows", "Package.wxs.in"),
    "utf8",
  );
  assert.match(cmake, /CPACK_RESOURCE_FILE_LICENSE "\$\{CMAKE_CURRENT_BINARY_DIR\}\/LICENSE\.txt"/);
  assert.match(cmake, /CPACK_WIX_TEMPLATE/);
  assert.match(cmake, /CPACK_WIX_PATCH_FILE/);
  assert.match(packageTemplate, /<MajorUpgrade/);
  assert.match(packageTemplate, /AllowSameVersionUpgrades="no"/);
  assert.doesNotMatch(packageTemplate, /AllowSameVersionUpgrades="yes"/);
  assert.match(patch, /Id="CM_CP_bin\.arena_service\.exe"/);
  assert.match(patch, /<ServiceInstall/);
  assert.match(patch, /Account="NT AUTHORITY\\LocalService"/);
  assert.match(patch, /<CPackWiXFragment Id="#PRODUCT">/);
  assert.match(patch, /<Property Id="ARENA_DATA_ROOT" Secure="yes"/);
  assert.match(patch, /--data-dir &quot;\[ARENA_DATA_ROOT\]&quot;/);
  assert.match(patch, /<ServiceControl/);
  assert.match(patch, /Remove="uninstall"/);
  assert.match(patch, /\[INSTALL_ROOT\]share\\uci-arena\\web/);
  assert.match(patch, /Id="CM_CP_bin\.arena\.exe"/);
  assert.match(patch, /Name="PATH"/);
  assert.match(patch, /Value="\[INSTALL_ROOT\]bin"/);
  assert.match(patch, /Permanent="no"/);
  assert.match(patch, /System="yes"/);
  assert.match(patch, /Id="CM_CP_bin\.uci_arena_manager\.exe"/);
  assert.match(patch, /Name="UCI Arena Manager"/);
  assert.match(patch, /Target="\[INSTALL_ROOT\]bin\\uci-arena-manager\.exe"/);
  assert.match(cmake, /CPACK_WIX_PRODUCT_ICON/);
});
