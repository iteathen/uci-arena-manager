import assert from "node:assert/strict";
import {
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import test from "node:test";
import {
  cleanBuild,
  expectedInstallerNames,
  verifyInstallerSet,
  writeCleanBuildResult,
} from "./clean-build.mjs";

test("clean build requires an explicit Manager-Core scope locally", () => {
  assert.throws(
    () => cleanBuild({
      platform: "windows-x86_64",
      version: "1.2.3",
    }),
    /scope is required.*protected suite release workflow/,
  );
});

function fixture(platform, version = "1.2.3") {
  const root = mkdtempSync(path.join(tmpdir(), "arena-clean-build-"));
  mkdirSync(root, { recursive: true });
  for (const name of expectedInstallerNames(platform, version)) {
    writeFileSync(path.join(root, name), Buffer.from(`fixture:${name}`, "utf8"));
  }
  return { root, version };
}

test("Windows clean-build result binds the complete exact installer set", () => {
  const { root, version } = fixture("windows-x86_64");
  try {
    const result = writeCleanBuildResult({
      platform: "windows-x86_64",
      version,
      outputDirectory: root,
      sourceCommit: "1".repeat(40),
    });
    assert.equal(result.schema, "arena_clean_build_result_v1");
    assert.equal(result.installer_authority, "iteathen/uci-arena-manager");
    assert.equal(result.release_scope, "manager_core_development");
    assert.deepEqual(
      result.artifacts.map((artifact) => artifact.name),
      expectedInstallerNames("windows-x86_64", version),
    );
    assert.ok(result.artifacts.every((artifact) =>
      /^[0-9a-f]{64}$/u.test(artifact.sha256)));
    assert.deepEqual(
      JSON.parse(readFileSync(path.join(root, "clean-build-result.json"), "utf8")),
      result,
    );
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test("installer verification rejects missing, empty, and stale outputs", () => {
  const missing = fixture("linux-x86_64");
  const expected = expectedInstallerNames("linux-x86_64", missing.version);
  try {
    rmSync(path.join(missing.root, expected[0]));
    assert.throws(
      () => verifyInstallerSet({
        platform: "linux-x86_64",
        version: missing.version,
        outputDirectory: missing.root,
      }),
      /required installers missing/,
    );
  } finally {
    rmSync(missing.root, { recursive: true, force: true });
  }

  const empty = fixture("linux-x86_64");
  try {
    writeFileSync(path.join(empty.root, expected[1]), Buffer.alloc(0));
    assert.throws(
      () => verifyInstallerSet({
        platform: "linux-x86_64",
        version: empty.version,
        outputDirectory: empty.root,
      }),
      /installer is empty/,
    );
  } finally {
    rmSync(empty.root, { recursive: true, force: true });
  }

  const stale = fixture("linux-x86_64");
  try {
    writeFileSync(path.join(stale.root, "uci-arena-manager-old.tar.gz"), "stale");
    assert.throws(
      () => verifyInstallerSet({
        platform: "linux-x86_64",
        version: stale.version,
        outputDirectory: stale.root,
      }),
      /unexpected or stale outputs/,
    );
  } finally {
    rmSync(stale.root, { recursive: true, force: true });
  }
});

test("manual release delegates platform packaging to the canonical clean build", () => {
  const repositoryRoot = path.resolve(import.meta.dirname, "../..");
  const releaseWorkflow = readFileSync(
    path.join(repositoryRoot, ".github", "workflows", "release.yml"),
    "utf8",
  );
  const managerJob = releaseWorkflow.match(
    /^  build-manager:[\s\S]*?(?=^  compose-candidate:)/mu,
  )?.[0] ?? "";
  assert.match(
    managerJob,
    /'tools\/release\/clean-build\.mjs'[\s\S]*'--manager-only'[\s\S]*'--platform', '\$\{\{ matrix\.target \}\}'/,
  );
  assert.match(
    managerJob,
    /'\$\{\{ inputs\.publish \}\}' -eq 'true'[\s\S]*TUF_ROOT_SHA256/u,
  );
  assert.match(
    managerJob,
    /if \(\$rootSha256 -match '\^\[0-9a-f\]\{64\}\$'\)[\s\S]*--tuf-root-sha256/u,
  );
  assert.match(
    managerJob,
    /sudo apt-get install --yes acl autoconf autoconf-archive automake libtool ninja-build lintian rpm rpmlint libwebkit2gtk-4\.1-dev/u,
  );
  assert.match(
    managerJob,
    /WixToolset\.BootstrapperApplications\.wixext\/6\.0\.2/u,
  );
  assert.ok(
    managerJob.indexOf(
      "name: Install Linux native build prerequisites and package validators",
    ) < managerJob.indexOf("uses: lukka/run-vcpkg@"),
    "Linux native prerequisites must be installed before vcpkg bootstrap",
  );
  assert.doesNotMatch(managerJob, /^\s+cpack /mu);

  assert.equal(
    existsSync(path.join(repositoryRoot, ".github", "workflows", "ci.yml")),
    false,
    "automatic CI workflow must remain absent",
  );
  assert.match(releaseWorkflow, /^on:\s+workflow_dispatch:/mu);
  assert.doesNotMatch(
    releaseWorkflow,
    /^ {2}(?:pull_request|push|merge_group|schedule):/mu,
  );
});
