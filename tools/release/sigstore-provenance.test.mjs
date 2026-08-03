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

test("only the manual Manager component workflow remains a release authority", () => {
  const workflow = readFileSync(
    new URL("../../.github/workflows/component-release.yml", import.meta.url),
    "utf8",
  );
  assert.match(workflow, /^name:\s*Manager component candidate$/mu);
  assert.match(workflow, /^on:\s*\n  workflow_dispatch:/mu);
  assert.doesNotMatch(workflow, /^ {2}(?:pull_request|push|merge_group|schedule):/mu);
  assert.match(workflow, /sigstore\/cosign-installer@6f9f17788090df1f26f669e9d70d6ae9567deba6/u);
  assert.match(workflow, /cosign-release:\s*v3\.0\.6/u);
  assert.match(workflow, /sigstore-provenance\.mjs prepare-provider/u);
  assert.match(workflow, /sigstore-provenance\.mjs verify-bundle/u);
  assert.doesNotMatch(workflow, /prepare-managed|prepare-manager|suite release/u);
});
