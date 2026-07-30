import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import path from "node:path";
import test from "node:test";

const root = path.resolve(import.meta.dirname, "../..");

test("installed launcher embeds the paired dashboard without opening a browser", () => {
  const launcher = readFileSync(
    path.join(root, "apps", "launcher", "main.cpp"),
    "utf8",
  );
  assert.match(launcher, /webview::webview window/);
  assert.match(launcher, /ui --no-open --desktop-host/);
  assert.match(launcher, /WEBVIEW2_USER_DATA_FOLDER/);
  assert.match(launcher, /UCIArenaManagerInteractiveLauncher/);
  assert.match(launcher, /find_available_picker_port/);
  assert.match(launcher, /--picker-port/);
  assert.doesNotMatch(launcher, /ShellExecute/);
  assert.doesNotMatch(launcher, /xdg-open/);
});

test("native host setup authority is narrow, typed, and unavailable to normal browser launch", () => {
  const cli = readFileSync(path.join(root, "apps", "cli", "main.cpp"), "utf8");
  const helper = readFileSync(
    path.join(root, "apps", "cli", "ui_picker.cpp"),
    "utf8",
  );
  const setup = readFileSync(path.join(root, "apps", "setup", "main.cpp"), "utf8");
  assert.match(cli, /if \(desktop_host\)[\s\S]*setup=1/u);
  assert.match(helper, /arena_ui_setup_request_v1/);
  assert.match(helper, /component id is not available to desktop setup/);
  assert.match(helper, /\/v1\/setup-plan/);
  assert.match(helper, /\/v1\/setup-apply/);
  assert.match(helper, /runas/);
  assert.doesNotMatch(helper, /input\.at\("executable"\)/);
  assert.doesNotMatch(helper, /input\.at\("argv"\)/);
  assert.doesNotMatch(helper, /input\.at\("catalog"\)/);
  assert.match(setup, /--result-file/);
});

test("desktop helper keeps scratch state out of the install directory", () => {
  const helper = readFileSync(
    path.join(root, "apps", "cli", "ui_picker.cpp"),
    "utf8",
  );
  const launcher = readFileSync(
    path.join(root, "apps", "launcher", "main.cpp"),
    "utf8",
  );
  assert.match(helper, /temp_directory_path\(\)/);
  assert.match(helper, /uci-arena-manager-ui-/);
  assert.match(helper, /setUploadPath\(scratch\.path\(\)\.string\(\)\)/);
  assert.match(helper, /remove_all\(path_, error\)/);
  assert.match(launcher, /scratch_path_/);
  assert.match(launcher, /remove_all\(scratch_path_, error\)/);
});

test("desktop WebView dependencies are immutable and their licenses are packaged", () => {
  const cmake = readFileSync(path.join(root, "CMakeLists.txt"), "utf8");
  assert.match(
    cmake,
    /GIT_TAG 3ab4b5d722438fc8a13e6ca830c5e2372d19a01d/,
  );
  assert.match(cmake, /WEBVIEW_MSWEBVIEW2_VERSION "1\.0\.4078\.44"/);
  assert.match(cmake, /share\/licenses\/webview/);
  assert.match(cmake, /share\/licenses\/microsoft-webview2/);
});
