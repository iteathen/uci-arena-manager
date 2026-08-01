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

test("development bundle permits no synthetic TUF root but keeps the catalog", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-dev-media-"));
  try {
    mkdirSync(path.join(root, "bin"), { recursive: true });
    mkdirSync(path.join(root, "release"), { recursive: true });
    writeFileSync(path.join(root, "bin", "arena-setup.exe"), "setup");
    writeFileSync(path.join(root, "release", "catalog.json"), "{}");
    const xml = buildPayloadFragment(root, { development: true });
    assert.match(xml, /release\\catalog\.json/);
    assert.doesNotMatch(xml, /release\\tuf\\root\.json/);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test("Windows per-machine bundle owns complete elevated lifecycle", () => {
  const source = readFileSync(
    path.join(repositoryRoot, "packaging", "windows", "Bundle.wxs"),
    "utf8",
  );
  assert.match(source, /<MsiPackage[\s\S]*?Visible="no"/);
  assert.match(source, /<MsiPackage[\s\S]*?<ExePackage/);
  assert.match(source, /DetectCondition="WixBundleInstalled"/);
  assert.match(source, /Protocol="none"/);
  assert.match(source, /Name="UCI Arena Suite Setup \(all users\)"/);
  assert.doesNotMatch(source, /Name="\$\(var\.ProductName\)"/);
  assert.match(
    source,
    /--catalog &quot;\[WixBundleExecutePackageCacheFolder\]release\\catalog\.json&quot;/,
  );
  assert.match(
    source,
    /--bundle-owner &quot;\[WixBundleProviderKey\]&quot;/,
  );
  assert.match(source, /--install-scope per-machine/);
  assert.doesNotMatch(source, /ArenaWorkspaceRoot|--workspace-root/);
  for (const operation of ["install", "repair", "uninstall"]) {
    assert.match(
      source,
      new RegExp(
        `--result-file &quot;\\[WixBundleLog_SuiteComponents\\]\\.${operation}\\.json&quot;`,
      ),
    );
  }
  for (const root of [
    "--install-root",
    "--data-root",
    "--cache-root",
    "--knowledge-root",
  ]) {
    assert.match(source, new RegExp(root));
  }
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
  assert.match(setupSource, /user_profile \/ "UCI Arena Workspaces"/);
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

test("Windows per-user bundle remains fixed-scope and non-elevated", () => {
  const source = readFileSync(
    path.join(repositoryRoot, "packaging", "windows", "Bundle.User.wxs"),
    "utf8",
  );
  assert.match(source, /Name="UCI Arena Suite Setup \(per-user\)"/);
  assert.match(source, /<MsiPackage[\s\S]*?Visible="no"/);
  assert.match(source, /ArenaDataRoot="\[LocalAppDataFolder\]UCI Arena Manager"/);
  assert.match(source, /<MsiProperty[^>]*Name="ARENA_DATA_ROOT"[^>]*ArenaDataRoot/);
  assert.match(source, /PerMachine="no"/);
  assert.doesNotMatch(source, /PerMachine="yes"/);
  assert.doesNotMatch(source, /--restart-manager-service/);
  assert.match(source, /--install-scope per-user/);
  assert.doesNotMatch(source, /ArenaWorkspaceRoot|--workspace-root/);
  for (const operation of ["install", "repair", "uninstall"]) {
    assert.match(
      source,
      new RegExp(
        `--result-file &quot;\\[WixBundleLog_SuiteComponents\\]\\.${operation}\\.json&quot;`,
      ),
    );
  }
  assert.match(source, /--install-root[^\n]*ArenaInstallRoot/);
  assert.match(source, /--cache-root[^\n]*ArenaCacheRoot/);
  assert.match(source, /--knowledge-root[^\n]*ArenaKnowledgeRoot/);
  for (const variable of [
    "InstallUciArenaEngine",
    "InstallBookForge",
    "InstallEvidenceService",
    "InstallLichessAdapter",
  ]) {
    assert.match(
      source,
      new RegExp(`Name="${variable}"[^>]*Value="1"[^>]*Persisted="yes"`),
    );
    assert.match(source, new RegExp(`--bundle-feature [^"]*=\\[${variable}\\]`));
  }
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
  assert.match(source, /Template = 'packaging\/windows\/Bundle\.User\.wxs'/);
  assert.match(source, /Template = 'packaging\/windows\/Bundle\.wxs'/);
  assert.match(
    source,
    /UCI-Arena-Setup-\$version-windows-x64-\$\(\$scope\.Name\)-\$mode\.exe/,
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
  const userPatch = readFileSync(
    path.join(repositoryRoot, "packaging", "windows", "UserPatch.xml"),
    "utf8",
  );
  const packageTemplate = readFileSync(
    path.join(repositoryRoot, "packaging", "windows", "Package.wxs.in"),
    "utf8",
  );
  const setupEngine = readFileSync(
    path.join(
      repositoryRoot,
      "src",
      "application",
      "installer",
      "setup.cpp",
    ),
    "utf8",
  );
  assert.match(cmake, /CPACK_RESOURCE_FILE_LICENSE "\$\{CMAKE_CURRENT_BINARY_DIR\}\/LICENSE\.txt"/);
  assert.match(cmake, /CPACK_WIX_TEMPLATE/);
  assert.match(cmake, /CPACK_WIX_PATCH_FILE/);
  assert.match(packageTemplate, /<MajorUpgrade/);
  assert.match(
    packageTemplate,
    /AllowSameVersionUpgrades="@CPACK_WIX_ALLOW_SAME_VERSION_UPGRADES@"/,
  );
  assert.match(
    cmake,
    /option\(\s*ARENA_DEVELOPMENT_SAME_VERSION_UPGRADES[\s\S]*OFF\s*\)/,
  );
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
  assert.doesNotMatch(userPatch, /<ServiceInstall/);
  assert.doesNotMatch(userPatch, /<ServiceControl/);
  assert.match(userPatch, /LocalAppDataFolder/);
  assert.match(userPatch, /System="no"/);
  assert.doesNotMatch(userPatch, /Advertise="yes"/);
  assert.match(userPatch, /Target="\[INSTALL_ROOT\]bin\\uci-arena-manager\.exe"/);
  assert.match(userPatch, /ARENA_MACHINE_SCOPE_INSTALLED/);
  assert.match(userPatch, /Installed OR NOT ARENA_MACHINE_SCOPE_INSTALLED/);
  assert.match(userPatch, /Id="#PRODUCTFEATURE"/);
  assert.match(cmake, /packaging\/windows\/UserFolders\.wxs/);
  const userFolders = readFileSync(
    path.join(repositoryRoot, "packaging", "windows", "UserFolders.wxs"),
    "utf8",
  );
  const releaseWorkflow = readFileSync(
    path.join(repositoryRoot, ".github", "workflows", "release.yml"),
    "utf8",
  );
  assert.match(userFolders, /Id="ArenaUserProgramsFolderCleanup"/);
  assert.match(userFolders, /Directory="INSTALL_PREFIX_1"/);
  assert.match(userFolders, /Root="HKCU"/);
  assert.match(patch, /Id="CM_CP_bin\.uci_arena_manager\.exe"/);
  assert.match(patch, /Name="UCI Arena Manager"/);
  assert.match(patch, /Advertise="yes"/);
  assert.doesNotMatch(patch, /Target="\[INSTALL_ROOT\]/);
  assert.match(patch, /ARENA_USER_SCOPE_INSTALLED/);
  assert.match(patch, /Installed OR NOT ARENA_USER_SCOPE_INSTALLED/);
  assert.match(cmake, /CPACK_WIX_PRODUCT_ICON/);
  assert.match(cmake, /ARENA_WINDOWS_PER_USER_INSTALL/);
  assert.match(cmake, /install-scope\.txt\.in/);
  assert.doesNotMatch(cmake, /ARENA_WINDOWS_PER_USER_INSTALL=1/);
  const scopeReader = readFileSync(
    path.join(
      repositoryRoot,
      "include",
      "arena",
      "platform",
      "windows_install_scope.hpp",
    ),
    "utf8",
  );
  assert.match(scopeReader, /value\.back\(\) == '\\r'/);
  assert.match(scopeReader, /return WindowsInstallScope::per_machine/);
  assert.match(cmake, /CPACK_WIX_INSTALL_SCOPE "perUser"/);
  assert.match(cmake, /CPACK_WIX_INSTALL_SCOPE "perMachine"/);
  assert.match(cmake, /CPACK_WIX_ROOT_FOLDER_ID "LocalAppDataFolder"/);
  assert.match(cmake, /CPACK_PACKAGE_INSTALL_DIRECTORY "Programs\/UCI Arena Manager"/);
  assert.match(cmake, /CMAKE_VERSION VERSION_LESS 4\.3/);
  assert.match(cmake, /CPACK_WIX_COMPONENT_KEYS_REGISTRY_PATH/);
  assert.match(releaseWorkflow, /\$version = '4\.3\.4'/);
  assert.match(
    releaseWorkflow,
    /86e5fcafb38bdf58346a78b187c7b6b4f252ae5242cffe24c463a92bbd2e77d1/,
  );
  assert.match(cmake, /packaging\/windows\/UserPatch\.xml/);
  const startupStop = setupEngine.search(
    /invoke_provider_startup\(\s*paths,\s*previous_receipt,\s*"uninstall",\s*true\)/,
  );
  const payloadRename = setupEngine.indexOf(
    "rename_managed_component_tree_with_retry(target, replaced, id)",
  );
  const configurationRollback = setupEngine.indexOf(
    'action->contains("active_model_pointer_path")',
  );
  const startupRestore = setupEngine.search(
    /action->at\("previous_startup_receipt"\),\s*"install",\s*true\)/,
  );
  assert.ok(startupStop >= 0 && startupStop < payloadRename);
  assert.match(
    setupEngine,
    /trusted_existing_receipt_workspace && receipt\.contains\("startup"\)/,
  );
  assert.ok(
    configurationRollback >= 0 && configurationRollback < startupRestore,
  );
});

test("Windows service transitions are bounded and startup-stop safe", () => {
  const service = readFileSync(
    path.join(repositoryRoot, "apps", "service", "main.cpp"),
    "utf8",
  );
  const control = readFileSync(
    path.join(
      repositoryRoot,
      "src",
      "platform",
      "windows",
      "service_control.cpp",
    ),
    "utf8",
  );
  assert.match(service, /std::atomic_bool service_stop_requested/);
  assert.match(service, /registerBeginningAdvice/);
  assert.match(service, /service_status\.dwCurrentState = SERVICE_START_PENDING/);
  assert.match(service, /service_stop_requested\.load/);
  assert.match(control, /transition_timeout_ms = 30'000/);
  assert.match(control, /wait_for_start_resolution/);
  assert.match(
    control,
    /wait_for_service_state\(\s*service, SERVICE_STOPPED,[\s\S]*?"stop"\)/,
  );
  assert.match(
    control,
    /wait_for_service_state\(\s*service, SERVICE_RUNNING,[\s\S]*?"start"\)/,
  );
});
