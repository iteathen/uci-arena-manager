import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";
import path from "node:path";
import test from "node:test";

const repositoryRoot = path.resolve(import.meta.dirname, "../..");

function repositoryFile(relativePath) {
  return readFileSync(path.join(repositoryRoot, relativePath), "utf8");
}

test("Linux packages declare policy metadata and reproducible documentation", () => {
  const cmake = repositoryFile("CMakeLists.txt");

  assert.match(
    cmake,
    /CPACK_PACKAGE_CONTACT[\s\S]*iteathen@users\.noreply\.github\.com/u,
  );
  assert.match(cmake, /CPACK_DEBIAN_PACKAGE_DESCRIPTION/u);
  assert.match(cmake, /libwebkit2gtk-4\.1-0/u);
  assert.match(cmake, /webkit2gtk4\.1/u);
  assert.match(cmake, /passwd, acl, libwebkit2gtk/u);
  assert.match(cmake, /systemd, acl, webkit2gtk/u);
  assert.match(cmake, /zenity/u);
  assert.match(cmake, /CPACK_RPM_PACKAGE_LICENSE "AGPL-3\.0-or-later"/u);
  assert.match(cmake, /set\(CPACK_STRIP_FILES TRUE\)/u);
  assert.match(cmake, /gzip REQUIRED/u);
  assert.match(cmake, /ARENA_GZIP_EXECUTABLE\}" -9 -n -c/u);
  assert.match(cmake, /arena\.1\.gz/u);
  assert.doesNotMatch(cmake, /arena-setup\.1\.gz/u);
  assert.match(cmake, /changelog\.gz/u);
  assert.match(cmake, /packaging\/linux\/copyright/u);
  assert.match(cmake, /share\/doc\/uci-arena-manager\/third-party/u);
  assert.match(cmake, /share\/lintian\/overrides/u);
  assert.match(
    cmake,
    /packaging\/linux\/debian\/postinst[\s\S]*packaging\/linux\/debian\/prerm[\s\S]*packaging\/linux\/debian\/postrm/u,
  );

  assert.match(
    repositoryFile("packaging/linux/changelog.in"),
    /uci-arena-manager \(@PROJECT_VERSION@\)/u,
  );
  assert.match(
    repositoryFile("packaging/linux/copyright"),
    /License: AGPL-3\+/u,
  );
});

test("Linux maintainer scripts are executable and distro-specific", () => {
  const scripts = [
    "packaging/linux/debian/postinst",
    "packaging/linux/debian/prerm",
    "packaging/linux/debian/postrm",
    "packaging/linux/postinst",
    "packaging/linux/prerm",
    "packaging/linux/postrm",
  ];
  for (const script of scripts) {
    const indexEntry = execFileSync(
      "git",
      ["ls-files", "--stage", "--", script],
      { cwd: repositoryRoot, encoding: "utf8" },
    );
    assert.match(indexEntry, /^100755 /u, `${script} must be executable`);
  }

  for (const script of scripts.slice(0, 3)) {
    const body = repositoryFile(script);
    assert.doesNotMatch(body, /\bsystemctl\b/u);
  }
  assert.match(
    repositoryFile("packaging/linux/debian/postinst"),
    /deb-systemd-helper[\s\S]*deb-systemd-invoke/u,
  );
});

test("Lintian exceptions are exact, reviewed, and cannot mask new tags", () => {
  const override = repositoryFile(
    "packaging/linux/uci-arena-manager.lintian-overrides",
  );
  const activeLines = override
    .split(/\r?\n/u)
    .map((line) => line.trim())
    .filter((line) => line && !line.startsWith("#"));
  const expected = [
    "uci-arena-manager binary: embedded-library bzip2 [usr/bin/arena-service]",
    "uci-arena-manager binary: embedded-library curl [usr/bin/arena-service]",
    "uci-arena-manager binary: embedded-library curl [usr/bin/arena]",
    "uci-arena-manager binary: embedded-library zlib [usr/bin/arena-service]",
    "uci-arena-manager binary: embedded-library zlib [usr/bin/arena]",
    "uci-arena-manager binary: no-manual-page [usr/bin/arena-broker]",
    "uci-arena-manager binary: no-manual-page [usr/bin/arena-service]",
    "uci-arena-manager binary: no-manual-page [usr/bin/uci-arena-manager]",
  ];
  assert.deepEqual(activeLines, expected);
  assert.doesNotMatch(override, /embedded-library\s*$/mu);

  const releaseWorkflow = repositoryFile(".github/workflows/component-release.yml");
  assert.match(
    releaseWorkflow,
    /lintian dist\/linux\/\*\.deb\s+rpmlint dist\/linux\/\*\.rpm/u,
  );
  assert.doesNotMatch(
    releaseWorkflow,
    /(?:lintian|rpmlint)[^\n]*(?:\|\|\s*true|continue-on-error)/u,
  );
});
