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
  MANAGED_PREDICATE_TYPE,
  PROVIDER_PREDICATE_TYPE,
  SIGSTORE_BUNDLE_MEDIA_TYPE,
  buildExpectedManagedStatement,
  buildExpectedProviderStatement,
  prepareManagedProvenance,
  prepareManagerProvenance,
  verifyBundlePayload,
  verifyManagedProvenance,
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
    assert.ok(arguments_.includes("iteathen/UCI_Arena"));
    assert.equal(arguments_.at(-1), fixture.artifactPath);
    assert.equal(
      arguments_.some((argument) => /ignore|check-claims=false/u.test(argument)),
      false,
    );
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test("provider verification pins the selected private provider repository", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-manager-provider-"));
  try {
    const fixture = providerFixture(root);
    const calls = [];
    verifyProviderProvenance(
      fixture.component,
      fixture.result,
      fixture.artifactPath,
      fixture.bundlePath,
      {
        providerRepository: "iteathen/uci-arena-book-forge",
        execute(executable, arguments_) {
          calls.push({ executable, arguments_ });
        },
      },
    );
    assert.equal(calls.length, 1);
    const repositoryFlag = calls[0].arguments_.indexOf(
      "--certificate-github-workflow-repository",
    );
    assert.equal(
      calls[0].arguments_[repositoryFlag + 1],
      "iteathen/uci-arena-book-forge",
    );
    assert.equal(calls[0].arguments_.includes("iteathen/UCI_Arena"), false);
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

test("managed verification pins the Manager workflow and exact predicate", () => {
  const root = mkdtempSync(path.join(os.tmpdir(), "arena-manager-managed-"));
  try {
    const artifactPath = path.join(
      root,
      "node_runtime.private-24.15.0-linux-x86_64.tar.gz",
    );
    writeFileSync(artifactPath, "managed artifact");
    const result = {
      schema: "arena_managed_component_release_result_v1",
      source_repository: "iteathen/uci-arena-manager",
      source_commit: "9".repeat(40),
      artifact_name: path.basename(artifactPath),
      artifact_sha256: sha256("managed artifact"),
    };
    const resultPath = path.join(root, "node.managed-result.json");
    writeFileSync(resultPath, `${JSON.stringify(result)}\n`);
    const prepared = prepareManagedProvenance({
      artifactPath,
      managedResultPath: resultPath,
      statementDirectory: path.join(root, "statements"),
    });
    assert.equal(prepared.predicateType, MANAGED_PREDICATE_TYPE);
    assert.deepEqual(prepared.statement, buildExpectedManagedStatement(result));
    writeBundle(prepared.bundlePath, prepared.statement);
    const calls = [];
    const component = {
      component_id: "node_runtime.private",
      source_commit: result.source_commit,
      provenance_certificate_identity:
        "https://github.com/iteathen/uci-arena-manager/"
        + ".github/workflows/managed-component-release.yml@refs/heads/main",
      provenance_oidc_issuer: "https://token.actions.githubusercontent.com",
      provenance_workflow_name: "Managed component candidate",
      provenance_workflow_ref: "refs/heads/main",
      provenance_workflow_trigger: "workflow_dispatch",
    };
    verifyManagedProvenance(
      component,
      result,
      artifactPath,
      prepared.bundlePath,
      {
        execute(executable, arguments_) {
          calls.push({ executable, arguments_ });
        },
      },
    );
    assert.equal(calls.length, 1);
    assert.ok(calls[0].arguments_.includes(MANAGED_PREDICATE_TYPE));
    assert.ok(calls[0].arguments_.includes("iteathen/uci-arena-manager"));
    assert.equal(
      calls[0].arguments_.some((argument) =>
        /ignore|check-claims=false/u.test(argument)),
      false,
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
  const automationPolicy = readFileSync(
    new URL("../check-github-automation.mjs", import.meta.url),
    "utf8",
  );
  const packageManifest = readFileSync(
    new URL("../../package.json", import.meta.url),
    "utf8",
  );
  const catalogAssembler = readFileSync(
    new URL("./assemble-catalog.mjs", import.meta.url),
    "utf8",
  );
  const managedWorkflow = readFileSync(
    new URL(
      "../../.github/workflows/managed-component-release.yml",
      import.meta.url,
    ),
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
  assert.match(workflow, /Fetch exact private component candidates/u);
  assert.match(workflow, /GH_MANAGER_TOKEN:\s*\$\{\{ github\.token \}\}/u);
  assert.doesNotMatch(workflow, /actions\/attest-build-provenance/u);
  assert.doesNotMatch(workflow, /attestations:\s*write/u);
  assert.doesNotMatch(workflow, /insecure-ignore|check-claims=false|ignore-sct/u);
  assert.match(specification, /repository_visibility:\s*private/u);
  assert.match(specification, /Repository visibility is[\s\S]*never weakened/u);
  assert.match(workflow, /npm audit --omit=dev/u);
  assert.match(packageManifest, /npm audit --omit=dev --prefix web/u);
  assert.match(automationPolicy, /"pull_request"/u);
  assert.match(automationPolicy, /"push"/u);
  assert.match(automationPolicy, /"merge_group"/u);
  assert.match(automationPolicy, /"schedule"/u);
  assert.match(automationPolicy, /workflow_dispatch/u);
  assert.match(catalogAssembler, /validateLockedManagedResult/u);
  assert.match(catalogAssembler, /managed_result_sha256/u);
  assert.match(catalogAssembler, /provenance_bundle_sha256/u);
  assert.doesNotMatch(
    catalogAssembler,
    /provenance_sha256|\.provenance\.json/u,
  );
  assert.match(managedWorkflow, /^name:\s*Managed component candidate$/mu);
  assert.match(
    managedWorkflow,
    /sigstore\/cosign-installer@6f9f17788090df1f26f669e9d70d6ae9567deba6/u,
  );
  assert.match(managedWorkflow, /cosign-release:\s*v3\.0\.6/u);
  assert.match(managedWorkflow, /cosign attest-blob/u);
  assert.match(managedWorkflow, /cosign verify-blob-attestation/u);
  assert.match(managedWorkflow, /sigstore-provenance\.mjs verify-bundle/u);
  assert.match(
    managedWorkflow,
    /cc5149eabd53779ce1e7bdc5401643622d0c7e6800ade18928a767e940bb0e62/u,
  );
  assert.match(
    managedWorkflow,
    /d4272657905319328294355973faee40a8c28e3eecb0e7b266ed34ff33383b76/u,
  );
  assert.match(
    managedWorkflow,
    /ad9b423546714137916bd38978af6fd68d7b8951bef25ff76bf43da72d6cb786/u,
  );
  assert.match(managedWorkflow, /\$version = '15\.1'/u);
  assert.match(
    managedWorkflow,
    /export PATH=\/c\/mingw64\/bin:`\$PATH; make[\s\S]*COMP=gcc COMPCXX=g\+\+/u,
  );
  assert.doesNotMatch(managedWorkflow, /PATH=\/mingw64\/bin/u);
  assert.match(managedWorkflow, /make -C '\$windowsPath\/src'/u);
  assert.doesNotMatch(managedWorkflow, /cygpath -u/u);
  assert.doesNotMatch(
    managedWorkflow,
    /insecure-ignore|check-claims=false|ignore-sct|curl\s+-[^ \n]*k/u,
  );
});
