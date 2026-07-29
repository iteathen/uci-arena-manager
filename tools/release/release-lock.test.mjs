import assert from "node:assert/strict";
import test from "node:test";
import { fileURLToPath } from "node:url";
import {
  assembleCatalog,
  validateLockedManagedResult,
  validateLockedProviderResult,
  validateReleaseLock,
} from "./release-lock.mjs";

function row(component, target, options = {}) {
  const fileSet = options.artifactType === "file_set";
  const artifactName = fileSet
    ? `${component}-${target}.file-set.json`
    : `${component}-${target}.tar.gz`;
  return {
    component_id: component,
    component_version: "1.0.0",
    target_triple: target,
    source_commit: "1".repeat(40),
    provider_release_tag: `component/${component}/v1.0.0/${target}`,
    artifact_name: artifactName,
    artifact_type: options.artifactType ?? "archive",
    artifact_sha256: "2".repeat(64),
    provider_result_name: `${component}-${target}.provider-result.json`,
    provider_result_sha256: "3".repeat(64),
    provenance_bundle_name: `${artifactName}.sigstore.json`,
    provenance_bundle_sha256: "8".repeat(64),
    provenance_certificate_identity:
      "https://github.com/iteathen/UCI_Arena/"
      + ".github/workflows/component-release.yml@refs/heads/main",
    provenance_oidc_issuer: "https://token.actions.githubusercontent.com",
    provenance_workflow_name: "Provider component candidate",
    provenance_workflow_ref: "refs/heads/main",
    provenance_workflow_trigger: "workflow_dispatch",
    ...(fileSet ? { file_count: 4, installed_bytes: 4096 } : {}),
  };
}

function managed(component, target) {
  return {
    component_id: component,
    component_version: "1.0.0",
    target_triple: target,
    source_commit: "9".repeat(40),
    managed_release_tag:
      `managed/${component === "stockfish.engine" ? "stockfish" : "node-runtime"}/v1.0.0/${target}`,
    artifact_name: `${component}-${target}.tar.gz`,
    artifact_sha256: "4".repeat(64),
    artifact_size_bytes: 100,
    installed_bytes: 500,
    component_manifest_sha256: "5".repeat(64),
    licenses_sha256: "6".repeat(64),
    managed_result_name: `${component}-${target}.managed-result.json`,
    managed_result_sha256: "7".repeat(64),
    provenance_bundle_name: `${component}-${target}.tar.gz.sigstore.json`,
    provenance_bundle_sha256: "8".repeat(64),
    provenance_certificate_identity:
      "https://github.com/iteathen/uci-arena-manager/"
      + ".github/workflows/managed-component-release.yml@refs/heads/main",
    provenance_oidc_issuer: "https://token.actions.githubusercontent.com",
    provenance_workflow_name: "Managed component candidate",
    provenance_workflow_ref: "refs/heads/main",
    provenance_workflow_trigger: "workflow_dispatch",
    ...(component === "stockfish.engine"
      ? { source_offer_url: "https://example.invalid/stockfish-source.tar.gz" }
      : {}),
  };
}

test("release lock requires every provider component on both platforms", () => {
  const components = [];
  const managedComponents = [];
  for (const target of ["windows-x86_64", "linux-x86_64"]) {
    for (const component of [
      "uci_arena.engine",
      "book_forge.service",
      "uci_arena_evidence.service",
      "opening_book.standard",
      "lichess.adapter",
    ]) components.push(row(component, target));
    managedComponents.push(managed("node_runtime.private", target));
    managedComponents.push(managed("stockfish.engine", target));
  }
  components.push(row("syzygy.wdl.3-4-5", "any-any", { artifactType: "file_set" }));
  components.push(row("syzygy.dtz.3-4-5", "any-any", { artifactType: "file_set" }));
  const result = validateReleaseLock({
    schema: "arena_suite_release_lock_v1",
    suite_version: "1.0.0",
    release_ready: true,
    provider_repository: "iteathen/UCI_Arena",
    managed_repository: "iteathen/uci-arena-manager",
    components,
    managed_components: managedComponents,
  });
  assert.deepEqual(result, { ok: true, errors: [] });
});

test("release lock rejects mutable and incomplete composition", () => {
  const result = validateReleaseLock({
    schema: "arena_suite_release_lock_v1",
    suite_version: "latest",
    release_ready: true,
    provider_repository: "iteathen/UCI_Arena",
    managed_repository: "iteathen/uci-arena-manager",
    components: [row("uci_arena.engine", "windows-x86_64")],
    managed_components: [],
  });
  assert.equal(result.ok, false);
  assert.ok(result.errors.some((error) => error.includes("suite_version")));
  assert.ok(result.errors.some((error) => error.includes("linux-x86_64 is missing")));
});

test("release lock rejects provenance that is not bound to the trusted workflow", () => {
  const component = row("uci_arena.engine", "windows-x86_64");
  component.provenance_bundle_name = "substituted.sigstore.json";
  component.provenance_certificate_identity =
    "https://github.com/iteathen/UCI_Arena/"
    + ".github/workflows/other.yml@refs/heads/main";
  component.provenance_workflow_trigger = "push";
  const validation = validateReleaseLock({
    schema: "arena_suite_release_lock_v1",
    suite_version: "1.0.0",
    release_ready: false,
    provider_repository: "iteathen/UCI_Arena",
    managed_repository: "iteathen/uci-arena-manager",
    components: [component],
    managed_components: [],
  }, { requireReady: false });
  assert.equal(validation.ok, false);
  assert.ok(validation.errors.some((error) =>
    error.includes("does not bind provenance to artifact_name")));
  assert.ok(validation.errors.some((error) =>
    error.includes("invalid provenance workflow ref/trigger pair")));
  assert.ok(validation.errors.some((error) =>
    error.includes("untrusted provenance_certificate_identity")));
});

test("managed result requires exact build, smoke, source, and workflow evidence", () => {
  const locked = managed("stockfish.engine", "linux-x86_64");
  const result = {
    schema: "arena_managed_component_release_result_v1",
    release_authority: "manager_managed_component_only",
    suite_installer_authority: "iteathen/uci-arena-manager",
    source_repository: "iteathen/uci-arena-manager",
    source_commit: locked.source_commit,
    component_id: locked.component_id,
    component_version: locked.component_version,
    target_triple: locked.target_triple,
    artifact_name: locked.artifact_name,
    artifact_sha256: locked.artifact_sha256,
    artifact_size_bytes: locked.artifact_size_bytes,
    installed_bytes: locked.installed_bytes,
    component_manifest_sha256: locked.component_manifest_sha256,
    licenses_sha256: locked.licenses_sha256,
    upstream_inputs: [{
      role: "source",
      url: "https://example.invalid/stockfish-source.tar.gz",
      sha256: "a".repeat(64),
    }, {
      role: "evaluation_network",
      url: "https://example.invalid/nn-ad9b42354671.nnue",
      sha256: "b".repeat(64),
    }],
    source_offer_url: locked.source_offer_url,
    validation_evidence: {
      executable_smoke: {
        status: "passed",
        protocol: "uci_startup",
      },
    },
    workflow_identity:
      "iteathen/uci-arena-manager/"
      + ".github/workflows/managed-component-release.yml@refs/heads/main#123.1",
  };
  assert.doesNotThrow(() =>
    validateLockedManagedResult(locked, result, 100));
  assert.throws(
    () => validateLockedManagedResult(
      locked,
      {
        ...result,
        validation_evidence: {
          executable_smoke: { status: "passed", protocol: "node_version" },
        },
      },
      100,
    ),
    /smoke evidence mismatch/u,
  );
  assert.throws(
    () => validateLockedManagedResult(
      locked,
      { ...result, source_offer_url: "https://example.invalid/substitute" },
      100,
    ),
    /source offer mismatch/u,
  );
  assert.throws(
    () => validateLockedManagedResult(
      locked,
      { ...result, upstream_inputs: result.upstream_inputs.slice(0, 1) },
      100,
    ),
    /upstream input mismatch/u,
  );
});

test("file-set provider results require exact one-byte range evidence", () => {
  const locked = row(
    "syzygy.wdl.3-4-5",
    "any-any",
    { artifactType: "file_set" },
  );
  const result = {
    schema: "arena_provider_release_result_v1",
    release_authority: "provider_component_only",
    suite_installer_authority: "iteathen/uci-arena-manager",
    source_repository: "iteathen/UCI_Arena",
    component_id: locked.component_id,
    component_version: locked.component_version,
    target_triple: locked.target_triple,
    source_commit: locked.source_commit,
    artifact_name: locked.artifact_name,
    artifact_kind: "external_file_set_manifest",
    artifact_sha256: locked.artifact_sha256,
    artifact_size_bytes: 100,
    workflow_identity:
      "iteathen/UCI_Arena/.github/workflows/component-release.yml"
      + "@refs/heads/main#123.1",
    external_file_count: 4,
    external_size_bytes: 4096,
    validation_evidence: {
      range_probe: {
        status: "availability_verified",
        content_verified: false,
        files: 4,
        bytes_transferred: 4,
      },
    },
  };
  assert.doesNotThrow(() =>
    validateLockedProviderResult(locked, result, 100));
  assert.throws(
    () => validateLockedProviderResult(
      locked,
      { ...result, release_authority: "suite_installer" },
      100,
    ),
    /provider result mismatch/,
  );
  assert.throws(
    () => validateLockedProviderResult(
      locked,
      {
        ...result,
        validation_evidence: {
          range_probe: {
            status: "not_requested",
            content_verified: false,
            files: 0,
            bytes_transferred: 0,
          },
        },
      },
      100,
    ),
    /range evidence mismatch/,
  );
  assert.throws(
    () => validateLockedProviderResult(
      locked,
      {
        ...result,
        validation_evidence: {
          range_probe: {
            ...result.validation_evidence.range_probe,
            content_verified: true,
          },
        },
      },
      100,
    ),
    /range evidence mismatch/,
  );
});

test("catalog assembly preserves provider digests and dependency ownership", () => {
  const locked = [];
  for (const component of [
    "uci_arena.engine",
    "book_forge.service",
    "opening_book.standard",
    "uci_arena_evidence.service",
    "lichess.adapter",
  ]) {
    const provider = row(component, "linux-x86_64");
    provider.result = {
      component_version: "1.0.0",
      artifact_size_bytes: 100,
    };
    locked.push(provider);
  }
  for (const component of ["node_runtime.private", "stockfish.engine"]) {
    const suite = managed(component, "linux-x86_64");
    suite.result = {
      component_version: "1.0.0",
      artifact_size_bytes: 100,
    };
    locked.push(suite);
  }
  const syzygy = row(
    "syzygy.wdl.3-4-5",
    "any-any",
    { artifactType: "file_set" },
  );
  syzygy.result = {
    component_version: "1.0.0",
    artifact_size_bytes: 100,
    artifact_kind: "external_file_set_manifest",
    external_file_count: 4,
    external_size_bytes: 4096,
  };
  locked.push(syzygy);
  const syzygyDtz = row(
    "syzygy.dtz.3-4-5",
    "any-any",
    { artifactType: "file_set" },
  );
  syzygyDtz.result = {
    component_version: "1.0.0",
    artifact_size_bytes: 100,
    artifact_kind: "external_file_set_manifest",
    external_file_count: 4,
    external_size_bytes: 4096,
  };
  locked.push(syzygyDtz);
  const catalog = assembleCatalog(
    { suite_version: "1.0.0" },
    locked,
    [{
      target_triple: "linux-x86_64",
      path: fileURLToPath(import.meta.url),
    }],
  );
  assert.equal(catalog.schema, "arena_install_catalog_v1");
  assert.deepEqual(
    catalog.components.find((component) =>
      component.id === "book_forge.service").dependencies,
    ["node_runtime.private", "stockfish.engine"],
  );
  assert.deepEqual(
    catalog.components.find((component) =>
      component.id === "opening_book.standard").dependencies,
    [],
  );
  assert.deepEqual(
    catalog.components.find((component) =>
      component.id === "lichess.adapter").dependencies,
    ["node_runtime.private", "uci_arena.engine", "opening_book.standard"],
  );
  assert.ok(catalog.presets.complete_arena.includes("lichess.adapter"));
  assert.equal(
    catalog.components.find((component) =>
      component.id === "book_forge.service")
      .artifacts["linux-x86_64"].sha256,
    locked.find((component) =>
      component.component_id === "book_forge.service").artifact_sha256,
  );
  assert.deepEqual(
    catalog.components.find((component) =>
      component.id === "syzygy.wdl.3-4-5").artifacts.any,
    {
      type: "file_set",
      source: `payloads/${syzygy.artifact_name}`,
      sha256: syzygy.artifact_sha256,
      size_bytes: 100,
      file_count: 4,
      total_size_bytes: 4096,
    },
  );
});
