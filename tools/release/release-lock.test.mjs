import assert from "node:assert/strict";
import test from "node:test";
import { fileURLToPath } from "node:url";
import { assembleCatalog, validateReleaseLock } from "./release-lock.mjs";

function row(component, target) {
  return {
    component_id: component,
    component_version: "1.0.0",
    target_triple: target,
    source_commit: "1".repeat(40),
    provider_release_tag: `component/${component}/v1.0.0/${target}`,
    artifact_name: `${component}-${target}.tar.gz`,
    artifact_sha256: "2".repeat(64),
    provider_result_name: `${component}-${target}.provider-result.json`,
    provider_result_sha256: "3".repeat(64),
  };
}

function managed(component, target) {
  return {
    component_id: component,
    component_version: "1.0.0",
    target_triple: target,
    artifact_name: `${component}-${target}.tar.gz`,
    artifact_url: `https://example.invalid/${component}-${target}.tar.gz`,
    artifact_sha256: "4".repeat(64),
    artifact_size_bytes: 100,
    component_manifest_sha256: "5".repeat(64),
    licenses_sha256: "6".repeat(64),
    provenance_url: `https://example.invalid/${component}-${target}.provenance.json`,
    provenance_sha256: "7".repeat(64),
    ...(component === "stockfish.engine"
      ? { source_offer_url: "https://example.invalid/stockfish-source.tar.gz" }
      : {}),
  };
}

test("release lock requires every provider component on both platforms", () => {
  const components = [];
  const managedComponents = [managed("syzygy.wdl.3-4-5", "any")];
  for (const target of ["windows-x86_64", "linux-x86_64"]) {
    for (const component of [
      "uci_arena.engine",
      "book_forge.service",
      "uci_arena_evidence.service",
      "opening_book.standard",
    ]) components.push(row(component, target));
    managedComponents.push(managed("node_runtime.private", target));
    managedComponents.push(managed("stockfish.engine", target));
  }
  const result = validateReleaseLock({
    schema: "arena_suite_release_lock_v1",
    suite_version: "1.0.0",
    release_ready: true,
    provider_repository: "iteathen/UCI_Arena",
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
    components: [row("uci_arena.engine", "windows-x86_64")],
    managed_components: [],
  });
  assert.equal(result.ok, false);
  assert.ok(result.errors.some((error) => error.includes("suite_version")));
  assert.ok(result.errors.some((error) => error.includes("linux-x86_64 is missing")));
});

test("catalog assembly preserves provider digests and dependency ownership", () => {
  const locked = [];
  for (const component of [
    "uci_arena.engine",
    "book_forge.service",
    "opening_book.standard",
    "uci_arena_evidence.service",
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
  const syzygy = managed("syzygy.wdl.3-4-5", "any");
  syzygy.result = {
    component_version: "1.0.0",
    artifact_size_bytes: 100,
  };
  locked.push(syzygy);
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
  assert.equal(
    catalog.components.find((component) =>
      component.id === "book_forge.service")
      .artifacts["linux-x86_64"].sha256,
    locked.find((component) =>
      component.component_id === "book_forge.service").artifact_sha256,
  );
});
