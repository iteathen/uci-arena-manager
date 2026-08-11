import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import {
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import {
  PROVIDER_PREDICATE_TYPE,
  SIGSTORE_BUNDLE_MEDIA_TYPE,
  buildExpectedProviderStatement,
  prepareProviderProvenance,
  verifyBundlePayload,
} from "./sigstore-provenance.mjs";

function sha256(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

function writeBundle(filename, statement, overrides = {}) {
  writeFileSync(filename, `${JSON.stringify({
    mediaType: SIGSTORE_BUNDLE_MEDIA_TYPE,
    verificationMaterial: {
      certificate: { rawBytes: "test-certificate" },
      tlogEntries: [{ inclusionProof: { checkpoint: "test-checkpoint" } }],
    },
    dsseEnvelope: {
      payload: Buffer.from(JSON.stringify(statement), "utf8").toString("base64"),
      payloadType: "application/vnd.in-toto+json",
      signatures: [{ sig: "test-signature" }],
    },
    ...overrides,
  })}\n`);
}

function fixture(root) {
  const artifactPath = path.join(root, "manager.core-0.3.8-windows-x86_64.tar.gz");
  const artifact = Buffer.from("manager component", "utf8");
  writeFileSync(artifactPath, artifact);
  const result = {
    schema: "arena_provider_release_result_v1",
    component_id: "manager.core",
    artifact_name: path.basename(artifactPath),
    artifact_sha256: sha256(artifact),
    source_repository: "iteathen/uci-arena-manager",
    source_commit: "2".repeat(40),
    release_authority: "provider_component_only",
    suite_installer_authority: "iteathen/uci-arena-installer",
  };
  const resultPath = path.join(root, "manager.provider-result.json");
  writeFileSync(resultPath, `${JSON.stringify(result)}\n`);
  return { artifactPath, result, resultPath };
}

test("Manager provider preparation binds one exact Installer-consumed candidate", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-manager-provider-prepare-"));
  try {
    const { artifactPath, result, resultPath } = fixture(root);
    const prepared = prepareProviderProvenance({
      artifactPath,
      providerResultPath: resultPath,
      statementDirectory: path.join(root, "statements"),
    });
    assert.equal(prepared.artifactPath, artifactPath);
    assert.equal(prepared.predicateType, PROVIDER_PREDICATE_TYPE);
    assert.deepEqual(prepared.statement, buildExpectedProviderStatement(result));
    assert.equal(prepared.bundlePath, `${artifactPath}.sigstore.json`);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test("Manager provider preparation rejects artifact and authority substitution", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-manager-provider-reject-"));
  try {
    const { artifactPath, result, resultPath } = fixture(root);
    writeFileSync(resultPath, `${JSON.stringify({
      ...result,
      suite_installer_authority: "iteathen/uci-arena-manager",
    })}\n`);
    assert.throws(
      () => prepareProviderProvenance({
        artifactPath,
        providerResultPath: resultPath,
        statementDirectory: path.join(root, "statements-authority"),
      }),
      /does not bind the exact Manager artifact/u,
    );

    writeFileSync(resultPath, `${JSON.stringify(result)}\n`);
    writeFileSync(artifactPath, "substituted component");
    assert.throws(
      () => prepareProviderProvenance({
        artifactPath,
        providerResultPath: resultPath,
        statementDirectory: path.join(root, "statements-artifact"),
      }),
      /does not bind the exact Manager artifact/u,
    );
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test("bundle payload verification requires certificate log proof and exact statement", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-manager-provider-bundle-"));
  try {
    const { artifactPath, result, resultPath } = fixture(root);
    const prepared = prepareProviderProvenance({
      artifactPath,
      providerResultPath: resultPath,
      statementDirectory: path.join(root, "statements"),
    });
    writeBundle(prepared.bundlePath, prepared.statement);
    assert.deepEqual(
      verifyBundlePayload(prepared.bundlePath, prepared.statementPath),
      prepared.statement,
    );
    writeBundle(prepared.bundlePath, { ...prepared.statement, subject: [] });
    assert.throws(
      () => verifyBundlePayload(prepared.bundlePath, prepared.statementPath),
      /does not match/u,
    );
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test("only the manual Manager component workflow uploads signed candidate assets", () => {
  const workflow = readFileSync(
    new URL("../../.github/workflows/component-release.yml", import.meta.url),
    "utf8",
  );
  const cmake = readFileSync(
    new URL("../../CMakeLists.txt", import.meta.url),
    "utf8",
  );
  assert.match(workflow, /^name:\s*Manager component candidate$/mu);
  assert.match(workflow, /^on:\s*\n  workflow_dispatch:/mu);
  assert.doesNotMatch(workflow, /^ {2}(?:pull_request|push|merge_group|schedule):/mu);
  assert.match(workflow, /default:\s*github-hosted/u);
  assert.match(
    workflow,
    /\["self-hosted","Windows","X64","ephemeral","uci-suite","manager-release"\]/u,
  );
  assert.match(workflow, /ephemeral self-hosted release is Windows-only/u);
  assert.match(workflow, /\('cl\.exe', 'gh\.exe', 'git\.exe', 'msbuild\.exe'\)/u);
  assert.match(workflow, /npm run check/u);
  assert.match(workflow, /\$LASTEXITCODE -ne 0.*Manager repository checks failed/u);
  assert.doesNotMatch(workflow, /npm run naming:check\s+npm test --prefix web/u);
  assert.match(workflow, /cmake --preset \$preset -G Ninja/u);
  assert.match(workflow, /Microsoft\\CMake\\Ninja\\ninja\.exe/u);
  assert.match(workflow, /name: Map exact ephemeral MSVC runtime directory/u);
  assert.match(workflow, /\$env:VCToolsRedistDir/u);
  assert.match(workflow, /Microsoft\.VC\*\.CRT/u);
  assert.match(workflow, /\$runtimeCandidates\.Count -ne 1/u);
  assert.match(workflow, /ARENA_MANAGER_MSVC_RUNTIME_DIRECTORY=\$runtimeDirectory/u);
  assert.match(workflow, /Add-Content -LiteralPath \$env:GITHUB_ENV/u);
  assert.match(workflow, /-DARENA_MANAGER_MSVC_RUNTIME_DIRECTORY=/u);
  assert.ok(workflow.indexOf("Map exact ephemeral MSVC runtime directory")
    < workflow.indexOf("Configure exact Manager source"));
  assert.match(cmake, /set\(ARENA_MANAGER_MSVC_RUNTIME_DIRECTORY "" CACHE PATH/u);
  assert.match(cmake, /list\(APPEND ARENA_MANAGER_RUNTIME_SEARCH_DIRECTORIES/u);
  assert.match(workflow, /ephemeral-self-hosted-windows'\) \{ 1 \} else \{ 4 \}/u);
  assert.doesNotMatch(workflow, /self-hosted.*linux/iu);
  assert.match(workflow, /sigstore\/cosign-installer@6f9f17788090df1f26f669e9d70d6ae9567deba6/u);
  assert.match(workflow, /cosign-release:\s*v3\.0\.6/u);
  assert.match(workflow, /sigstore-provenance\.mjs prepare-provider/u);
  assert.match(workflow, /sigstore-provenance\.mjs verify-bundle/u);
  assert.match(workflow, /contents:\s*write/u);
  assert.doesNotMatch(workflow, /actions\/(?:upload|download)-artifact/u);
  assert.match(workflow, /Expected exactly three candidate assets/u);
  assert.match(workflow, /gh release view \$tag/u);
  assert.match(workflow, /\$release\.targetCommitish -ne \$env:GITHUB_SHA/u);
  assert.match(workflow, /\$release\.assets\)\.Count -ne 0/u);
  assert.match(workflow, /gh release upload \$tag @assetPaths/u);
  assert.doesNotMatch(workflow, /gh release create/u);
  assert.ok(workflow.indexOf("sigstore-provenance.mjs verify-bundle")
    < workflow.indexOf("gh release upload"));
  assert.doesNotMatch(workflow, /prepare-managed|prepare-manager|suite release/u);
});
