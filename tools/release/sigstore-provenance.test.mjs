import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import {
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import {
  MANAGER_PREDICATE_TYPE,
  PROVIDER_PREDICATE_TYPE,
  SIGSTORE_BUNDLE_MEDIA_TYPE,
  buildExpectedProviderStatement,
  prepareManagerProvenance,
  verifyBundlePayload,
  verifyProviderProvenance,
} from "./sigstore-provenance.mjs";

function sha256(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

function writeBundle(filename, statement) {
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
  })}\n`);
}

function providerFixture(root) {
  const artifactPath = path.join(root, "uci-arena-1.0.0-linux-x86_64.tar.gz");
  const artifact = Buffer.from("provider artifact", "utf8");
  writeFileSync(artifactPath, artifact);
  const component = {
    component_id: "uci_arena.engine",
    source_commit: "1".repeat(40),
    provenance_certificate_identity:
      "https://github.com/iteathen/UCI_Arena/"
      + ".github/workflows/component-release.yml@refs/heads/main",
    provenance_oidc_issuer: "https://token.actions.githubusercontent.com",
    provenance_workflow_name: "Provider component candidate",
    provenance_workflow_ref: "refs/heads/main",
    provenance_workflow_trigger: "workflow_dispatch",
  };
  const result = {
    schema: "arena_provider_release_result_v1",
    artifact_name: path.basename(artifactPath),
    artifact_sha256: sha256(artifact),
  };
  const bundlePath = `${artifactPath}.sigstore.json`;
  writeBundle(bundlePath, buildExpectedProviderStatement(result));
  return { artifactPath, bundlePath, component, result };
}

test("provider verification pins every certificate claim and exact predicate", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-manager-provider-"));
  try {
    const fixture = providerFixture(root);
    const calls = [];
    const statement = verifyProviderProvenance(
      fixture.component,
      fixture.result,
      fixture.artifactPath,
      fixture.bundlePath,
      {
        cosignExecutable: "cosign-test",
        execute(executable, arguments_, options) {
          calls.push({ arguments_, executable, options });
          return "";
        },
      },
    );
    assert.deepEqual(statement, buildExpectedProviderStatement(fixture.result));
    assert.equal(calls.length, 1);
    assert.equal(calls[0].executable, "cosign-test");
    const arguments_ = calls[0].arguments_;
    for (const flag of [
      "--bundle",
      "--certificate-identity",
      "--certificate-oidc-issuer",
      "--certificate-github-workflow-repository",
      "--certificate-github-workflow-sha",
      "--certificate-github-workflow-name",
      "--certificate-github-workflow-ref",
      "--certificate-github-workflow-trigger",
      "--type",
    ]) assert.ok(arguments_.includes(flag), `missing ${flag}`);
    assert.ok(arguments_.includes(PROVIDER_PREDICATE_TYPE));
    assert.equal(arguments_.at(-1), fixture.artifactPath);
    assert.equal(
      arguments_.some((argument) => /ignore|check-claims=false/u.test(argument)),
      false,
    );
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test("provider verification rejects predicate substitution and Cosign failure", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-manager-provider-"));
  try {
    const fixture = providerFixture(root);
    assert.throws(
      () => verifyProviderProvenance(
        fixture.component,
        { ...fixture.result, artifact_name: "substituted.tar.gz" },
        fixture.artifactPath,
        fixture.bundlePath,
        { execute() { throw new Error("must not be reached"); } },
      ),
      /signed provider predicate mismatch/u,
    );
    assert.throws(
      () => verifyProviderProvenance(
        fixture.component,
        fixture.result,
        fixture.artifactPath,
        fixture.bundlePath,
        { execute() { throw new Error("invalid signature"); } },
      ),
      /Sigstore verification failed/u,
    );
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test("Manager statements cover exactly the selected release artifact set", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-manager-release-"));
  try {
    const artifacts = path.join(root, "dist");
    const statements = path.join(root, "statements");
    mkdirSync(path.join(artifacts, "windows"), { recursive: true });
    mkdirSync(path.join(artifacts, "linux"), { recursive: true });
    writeFileSync(path.join(artifacts, "manager.tar.gz"), "manager");
    writeFileSync(path.join(artifacts, "windows", "manager.msi"), "msi");
    writeFileSync(path.join(artifacts, "linux", "manager.deb"), "deb");
    writeFileSync(path.join(artifacts, "ignored.txt"), "not a Manager build artifact");

    const prepared = prepareManagerProvenance({
      artifactRoot: artifacts,
      bundleName: "manager-linux-x86_64.sigstore.json",
      scope: "manager-build",
      sourceCommit: "1".repeat(40),
      sourceRepository: "iteathen/uci-arena-manager",
      statementDirectory: statements,
      suiteVersion: "1.0.0",
      targetTriple: "linux-x86_64",
      workflowIdentity:
        "iteathen/uci-arena-manager/.github/workflows/release.yml"
        + "@refs/heads/main#123.1",
    });
    assert.equal(prepared.statement.predicateType, MANAGER_PREDICATE_TYPE);
    assert.equal(prepared.statement.predicate.artifact_count, 3);
    assert.deepEqual(
      prepared.statement.subject.map((subject) => subject.name),
      ["linux/manager.deb", "manager.tar.gz", "windows/manager.msi"],
    );
    writeBundle(prepared.bundlePath, prepared.statement);
    assert.deepEqual(
      verifyBundlePayload(prepared.bundlePath, prepared.statementPath),
      prepared.statement,
    );
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test("release workflow uses only pinned fail-closed Sigstore provenance", () => {
  const workflow = readFileSync(
    new URL("../../.github/workflows/release.yml", import.meta.url),
    "utf8",
  );
  const specification = readFileSync(
    new URL("../../specs/installation_contract_v1.yaml", import.meta.url),
    "utf8",
  );
  const continuousIntegration = readFileSync(
    new URL("../../.github/workflows/ci.yml", import.meta.url),
    "utf8",
  );
  assert.equal(
    workflow.match(
      /sigstore\/cosign-installer@6f9f17788090df1f26f669e9d70d6ae9567deba6/gu,
    )?.length,
    3,
  );
  assert.equal(workflow.match(/cosign-release:\s*v3\.0\.6/gu)?.length, 3);
  assert.equal(workflow.match(/cosign attest-blob/gu)?.length, 2);
  assert.equal(workflow.match(/cosign verify-blob-attestation/gu)?.length, 2);
  assert.equal(
    workflow.match(/sigstore-provenance\.mjs verify-bundle/gu)?.length,
    2,
  );
  assert.match(workflow, /Fetch exact provider candidates/u);
  assert.doesNotMatch(workflow, /actions\/attest-build-provenance/u);
  assert.doesNotMatch(workflow, /attestations:\s*write/u);
  assert.doesNotMatch(workflow, /insecure-ignore|check-claims=false|ignore-sct/u);
  assert.match(specification, /repository_visibility:\s*private/u);
  assert.match(specification, /Repository visibility is[\s\S]*never weakened/u);
  assert.match(
    continuousIntegration,
    /node --test tools\/release\/\*\.test\.mjs/u,
  );
});
