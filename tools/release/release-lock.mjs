import { createHash } from "node:crypto";
import {
  createWriteStream,
  existsSync,
  mkdirSync,
  readFileSync,
  statSync,
} from "node:fs";
import path from "node:path";

import {
  verifyManagedProvenance,
  verifyProviderProvenance,
} from "./sigstore-provenance.mjs";

const REQUIRED_PROVIDER_COMPONENTS = new Set([
  "uci_arena.engine",
  "book_forge.service",
  "uci_arena_evidence.service",
  "opening_book.standard",
  "lichess.adapter",
]);
const REQUIRED_V2_PROVIDER_COMPONENTS = new Set([
  ...REQUIRED_PROVIDER_COMPONENTS,
  "uci_arena_evidence.runtime",
]);
const TRUSTED_PROVIDERS = Object.freeze({
  uci_arena: Object.freeze({
    repository: "iteathen/UCI_Arena",
    workflowPath: ".github/workflows/component-release.yml",
    workflowName: "Provider component candidate",
    components: new Set([
      "uci_arena.engine",
      "uci_arena_evidence.runtime",
      "lichess.adapter",
      "syzygy.wdl.3-4-5",
      "syzygy.dtz.3-4-5",
    ]),
  }),
  book_forge: Object.freeze({
    repository: "iteathen/uci-arena-book-forge",
    workflowPath: ".github/workflows/component-release.yml",
    workflowName: "Book Forge component candidate",
    components: new Set([
      "book_forge.service",
      "opening_book.standard",
    ]),
  }),
  evidence_service: Object.freeze({
    repository: "iteathen/uci-arena-evidence-service",
    workflowPath: ".github/workflows/component-release.yml",
    workflowName: "Evidence Service component candidate",
    components: new Set(["uci_arena_evidence.service"]),
  }),
});
const TARGETS = new Set(["windows-x86_64", "linux-x86_64"]);
const REQUIRED_PORTABLE_PROVIDER_COMPONENTS = new Set([
  "syzygy.wdl.3-4-5",
  "syzygy.dtz.3-4-5",
]);
const REQUIRED_MANAGED_COMPONENTS = new Set([
  "node_runtime.private",
  "stockfish.engine",
]);

export function sha256File(filename) {
  return createHash("sha256").update(readFileSync(filename)).digest("hex");
}

function providerFor(lock, component, errors = []) {
  if (lock.schema === "arena_suite_release_lock_v1") {
    return TRUSTED_PROVIDERS.uci_arena;
  }
  const providerId = component.provider_id;
  const provider = TRUSTED_PROVIDERS[providerId];
  if (!provider) {
    errors.push(`${component.component_id ?? "<unknown>"} has an untrusted provider_id`);
    return null;
  }
  if (!provider.components.has(component.component_id)) {
    errors.push(
      `${component.component_id} is not owned by provider ${providerId}`,
    );
  }
  return provider;
}

export function resolveProviderAssetSource(lock, component) {
  const provider = providerFor(lock, component);
  if (!provider) {
    throw new Error(`component ${component.component_id} has no trusted provider`);
  }
  if (lock.schema === "arena_suite_release_lock_v2") {
    return {
      repository: lock.provider_asset_mirror?.repository,
      releaseTag: lock.provider_asset_mirror?.release_tag,
      tokenKind: "manager",
      providerRepository: provider.repository,
    };
  }
  return {
    repository: provider.repository,
    releaseTag: component.provider_release_tag,
    tokenKind: "provider",
    providerRepository: provider.repository,
  };
}

export function validateReleaseLock(lock, { requireReady = true } = {}) {
  const errors = [];
  if (![
    "arena_suite_release_lock_v1",
    "arena_suite_release_lock_v2",
  ].includes(lock.schema)) {
    errors.push("wrong lock schema");
  }
  if (!/^\d+\.\d+\.\d+(?:[-.][A-Za-z0-9.-]+)?$/.test(lock.suite_version ?? "")) {
    errors.push("suite_version must be immutable semver text");
  }
  if (lock.schema === "arena_suite_release_lock_v1"
      && lock.provider_repository !== "iteathen/UCI_Arena") {
    errors.push("provider_repository must be iteathen/UCI_Arena");
  }
  if (lock.schema === "arena_suite_release_lock_v2") {
    for (const [providerId, expected] of Object.entries(TRUSTED_PROVIDERS)) {
      const actual = lock.providers?.[providerId];
      if (actual?.repository !== expected.repository
          || actual?.workflow_path !== expected.workflowPath
          || actual?.workflow_name !== expected.workflowName) {
        errors.push(`provider ${providerId} does not match the trusted registry`);
      }
    }
    const actualIds = Object.keys(lock.providers ?? {}).sort();
    const expectedIds = Object.keys(TRUSTED_PROVIDERS).sort();
    if (JSON.stringify(actualIds) !== JSON.stringify(expectedIds)) {
      errors.push("providers must exactly match the trusted registry");
    }
    if (lock.provider_asset_mirror?.repository
          !== "iteathen/uci-arena-manager"
        || lock.provider_asset_mirror?.release_tag
          !== `provider-mirror/v${lock.suite_version}`
        || lock.provider_asset_mirror?.authority
          !== "digest_pinned_transport_only") {
      errors.push(
        "provider_asset_mirror must be the exact private Manager transport release",
      );
    }
  }
  if (lock.managed_repository !== "iteathen/uci-arena-manager") {
    errors.push("managed_repository must be iteathen/uci-arena-manager");
  }
  if (requireReady && lock.release_ready !== true) errors.push("release_ready is not true");
  if (!Array.isArray(lock.components)) errors.push("components must be an array");
  if (!Array.isArray(lock.managed_components)) {
    errors.push("managed_components must be an array");
  }
  const identities = new Set();
  const providerMirrorAssetNames = new Set();
  const coverage = new Map([...TARGETS].map((target) => [target, new Set()]));
  const portableProviderCoverage = new Set();
  for (const component of lock.components ?? []) {
    const identity = `${component.component_id}:${component.target_triple}`;
    const provider = providerFor(lock, component, errors);
    if (identities.has(identity)) errors.push(`duplicate component target ${identity}`);
    identities.add(identity);
    if (!/^[A-Za-z0-9._-]+$/.test(component.component_id ?? "")) {
      errors.push(`invalid component id ${component.component_id}`);
    }
    if (TARGETS.has(component.target_triple)) {
      coverage.get(component.target_triple).add(component.component_id);
    } else if (
      component.target_triple === "any-any" &&
      REQUIRED_PORTABLE_PROVIDER_COMPONENTS.has(component.component_id)
    ) {
      portableProviderCoverage.add(component.component_id);
    } else {
      errors.push(`unsupported target ${component.target_triple}`);
    }
    const artifactType = component.artifact_type ?? "archive";
    if (!["archive", "file_set"].includes(artifactType)) {
      errors.push(`${identity} has invalid artifact_type`);
    }
    if (artifactType === "file_set") {
      if (component.target_triple !== "any-any") {
        errors.push(`${identity} file_set must use target any-any`);
      }
      if (!component.artifact_name?.endsWith(".file-set.json")) {
        errors.push(`${identity} file_set must use a .file-set.json artifact`);
      }
      if (!Number.isSafeInteger(component.file_count) || component.file_count <= 0) {
        errors.push(`${identity} has invalid file_count`);
      }
      if (!Number.isSafeInteger(component.installed_bytes) || component.installed_bytes <= 0) {
        errors.push(`${identity} file_set must pin installed_bytes`);
      }
    } else if (!component.artifact_name?.endsWith(".tar.gz")) {
      errors.push(`${identity} archive must use a .tar.gz artifact`);
    }
    for (const key of [
      "artifact_sha256",
      "provider_result_sha256",
      "provenance_bundle_sha256",
    ]) {
      if (!/^[0-9a-f]{64}$/.test(component[key] ?? "")) {
        errors.push(`${identity} has invalid ${key}`);
      }
    }
    if (!/^[0-9a-f]{40}$/.test(component.source_commit ?? "")) {
      errors.push(`${identity} has invalid source_commit`);
    }
    if (!/^component\/[A-Za-z0-9._/-]+$/.test(component.provider_release_tag ?? "")) {
      errors.push(`${identity} has invalid provider_release_tag`);
    }
    for (const key of [
      "artifact_name",
      "provider_result_name",
      "provenance_bundle_name",
    ]) {
      if (!component[key] || path.basename(component[key]) !== component[key]) {
        errors.push(`${identity} has unsafe ${key}`);
      } else if (
        lock.schema === "arena_suite_release_lock_v2"
        && providerMirrorAssetNames.has(component[key])
      ) {
        errors.push(`${identity} reuses provider mirror asset ${component[key]}`);
      } else if (lock.schema === "arena_suite_release_lock_v2") {
        providerMirrorAssetNames.add(component[key]);
      }
    }
    if (component.provenance_bundle_name
        !== `${component.artifact_name}.sigstore.json`) {
      errors.push(`${identity} does not bind provenance to artifact_name`);
    }
    if (component.provenance_oidc_issuer
        !== "https://token.actions.githubusercontent.com") {
      errors.push(`${identity} has an untrusted provenance_oidc_issuer`);
    }
    if (provider
        && component.provenance_workflow_name !== provider.workflowName) {
      errors.push(`${identity} has an untrusted provenance_workflow_name`);
    }
    const workflowRef = component.provenance_workflow_ref ?? "";
    const workflowTrigger = component.provenance_workflow_trigger ?? "";
    if (workflowRef !== "refs/heads/main"
        || workflowTrigger !== "workflow_dispatch") {
      errors.push(`${identity} has an invalid provenance workflow ref/trigger pair`);
    }
    const expectedIdentity = provider
      ? `https://github.com/${provider.repository}/${provider.workflowPath}@${workflowRef}`
      : "";
    if (!provider
        || component.provenance_certificate_identity !== expectedIdentity) {
      errors.push(`${identity} has an untrusted provenance_certificate_identity`);
    }
  }
  const managedCoverage = new Map([...TARGETS].map((target) => [target, new Set()]));
  for (const component of lock.managed_components ?? []) {
    const identity = `${component.component_id}:${component.target_triple}`;
    if (identities.has(identity)) errors.push(`duplicate component target ${identity}`);
    identities.add(identity);
    if (!/^[A-Za-z0-9._-]+$/.test(component.component_id ?? "")) {
      errors.push(`invalid component id ${component.component_id}`);
    }
    if (TARGETS.has(component.target_triple)) {
      managedCoverage.get(component.target_triple).add(component.component_id);
    } else if (component.target_triple === "any") {
      // Portable Manager-owned archives remain permitted for future dependencies.
    } else {
      errors.push(`unsupported target ${component.target_triple}`);
    }
    for (const key of [
      "artifact_sha256",
      "component_manifest_sha256",
      "licenses_sha256",
      "managed_result_sha256",
      "provenance_bundle_sha256",
    ]) {
      if (!/^[0-9a-f]{64}$/.test(component[key] ?? "")) {
        errors.push(`${identity} has invalid ${key}`);
      }
    }
    if (!/^[0-9a-f]{40}$/.test(component.source_commit ?? "")) {
      errors.push(`${identity} has invalid source_commit`);
    }
    if (!/^managed\/[A-Za-z0-9._/-]+$/.test(component.managed_release_tag ?? "")) {
      errors.push(`${identity} has invalid managed_release_tag`);
    }
    for (const key of [
      "artifact_name",
      "managed_result_name",
      "provenance_bundle_name",
    ]) {
      if (!component[key] || path.basename(component[key]) !== component[key]) {
        errors.push(`${identity} has unsafe ${key}`);
      }
    }
    if (!component.artifact_name?.endsWith(".tar.gz")) {
      errors.push(`${identity} must use a .tar.gz artifact`);
    }
    if (component.provenance_bundle_name
        !== `${component.artifact_name}.sigstore.json`) {
      errors.push(`${identity} does not bind provenance to artifact_name`);
    }
    if (!Number.isSafeInteger(component.artifact_size_bytes)
        || component.artifact_size_bytes <= 0) {
      errors.push(`${identity} has invalid artifact_size_bytes`);
    }
    if (!Number.isSafeInteger(component.installed_bytes)
        || component.installed_bytes <= 0) {
      errors.push(`${identity} has invalid installed_bytes`);
    }
    if (component.component_id === "stockfish.engine"
        && !/^https:\/\//.test(component.source_offer_url ?? "")) {
      errors.push(`${identity} must include the Stockfish source offer`);
    }
    if (component.provenance_oidc_issuer
        !== "https://token.actions.githubusercontent.com") {
      errors.push(`${identity} has an untrusted provenance_oidc_issuer`);
    }
    if (component.provenance_workflow_name !== "Managed component candidate") {
      errors.push(`${identity} has an untrusted provenance_workflow_name`);
    }
    const workflowRef = component.provenance_workflow_ref ?? "";
    const workflowTrigger = component.provenance_workflow_trigger ?? "";
    if (workflowRef !== "refs/heads/main"
        || workflowTrigger !== "workflow_dispatch") {
      errors.push(`${identity} has an invalid provenance workflow ref/trigger pair`);
    }
    const expectedIdentity =
      `https://github.com/${lock.managed_repository}`
      + `/.github/workflows/managed-component-release.yml@${workflowRef}`;
    if (component.provenance_certificate_identity !== expectedIdentity) {
      errors.push(`${identity} has an untrusted provenance_certificate_identity`);
    }
  }
  if (requireReady) {
    const requiredProviderComponents =
      lock.schema === "arena_suite_release_lock_v2"
        ? REQUIRED_V2_PROVIDER_COMPONENTS
        : REQUIRED_PROVIDER_COMPONENTS;
    for (const [target, components] of coverage) {
      for (const required of requiredProviderComponents) {
        if (!components.has(required)) errors.push(`${target} is missing ${required}`);
      }
    }
    for (const [target, components] of managedCoverage) {
      for (const required of REQUIRED_MANAGED_COMPONENTS) {
        if (!components.has(required)) {
          errors.push(`${target} is missing managed ${required}`);
        }
      }
    }
    for (const required of REQUIRED_PORTABLE_PROVIDER_COMPONENTS) {
      if (!portableProviderCoverage.has(required)) {
        errors.push(`portable provider components are missing ${required}`);
      }
    }
  }
  return { ok: errors.length === 0, errors };
}

async function downloadAsset(repository, tag, name, token, destination) {
  const releaseResponse = await fetch(
    `https://api.github.com/repos/${repository}/releases/tags/${encodeURIComponent(tag)}`,
    {
      headers: {
        Accept: "application/vnd.github+json",
        Authorization: `Bearer ${token}`,
        "User-Agent": "uci-arena-manager-release",
        "X-GitHub-Api-Version": "2022-11-28",
      },
    },
  );
  if (!releaseResponse.ok) {
    throw new Error(`cannot resolve private release ${tag}: HTTP ${releaseResponse.status}`);
  }
  const release = await releaseResponse.json();
  const asset = release.assets.find((candidate) => candidate.name === name);
  if (!asset) throw new Error(`private release ${tag} is missing ${name}`);
  const response = await fetch(asset.url, {
    headers: {
      Accept: "application/octet-stream",
      Authorization: `Bearer ${token}`,
      "User-Agent": "uci-arena-manager-release",
      "X-GitHub-Api-Version": "2022-11-28",
    },
    redirect: "follow",
  });
  if (!response.ok || !response.body) {
    throw new Error(`cannot download private asset ${name}: HTTP ${response.status}`);
  }
  await new Promise((resolve, reject) => {
    const output = createWriteStream(destination, { flags: "wx", mode: 0o600 });
    output.on("error", reject);
    output.on("finish", resolve);
    response.body.pipeTo(new WritableStream({
      write(chunk) {
        return new Promise((writeResolve, writeReject) => {
          output.write(Buffer.from(chunk), (error) =>
            error ? writeReject(error) : writeResolve());
        });
      },
      close() {
        output.end();
      },
      abort(error) {
        output.destroy(error);
      },
    })).catch(reject);
  });
}

export function validateLockedProviderResult(
  component,
  result,
  artifactSize,
  provider = TRUSTED_PROVIDERS.uci_arena,
) {
  const isFileSet =
    (component.artifact_type ?? "archive") === "file_set";
  if (result.schema !== "arena_provider_release_result_v1"
      || result.release_authority !== "provider_component_only"
      || result.suite_installer_authority !== "iteathen/uci-arena-manager"
      || result.source_repository !== provider.repository
      || result.component_id !== component.component_id
      || result.component_version !== component.component_version
      || result.target_triple !== component.target_triple
      || result.source_commit !== component.source_commit
      || result.artifact_name !== component.artifact_name
      || (
        isFileSet
          ? result.artifact_kind !== "external_file_set_manifest"
          : result.artifact_kind !== undefined && result.artifact_kind !== "archive"
      )
      || result.artifact_sha256 !== component.artifact_sha256
      || result.artifact_size_bytes !== artifactSize) {
    throw new Error(`provider result mismatch for ${component.component_id}`);
  }
  const workflowIdentityPrefix =
    `${provider.repository}/${provider.workflowPath}@`
    + `${component.provenance_workflow_ref}#`;
  if (typeof result.workflow_identity !== "string"
      || !result.workflow_identity.startsWith(workflowIdentityPrefix)
      || !/^[1-9][0-9]*\.[1-9][0-9]*$/.test(
        result.workflow_identity.slice(workflowIdentityPrefix.length),
      )) {
    throw new Error(`provider workflow identity mismatch for ${component.component_id}`);
  }
  if (isFileSet
      && (
        result.validation_evidence?.range_probe?.status !== "availability_verified"
        || result.validation_evidence.range_probe.content_verified !== false
        || result.validation_evidence.range_probe.files !== component.file_count
        || result.validation_evidence.range_probe.bytes_transferred
          !== component.file_count
      )) {
    throw new Error(
      `provider range evidence mismatch for ${component.component_id}`,
    );
  }
  if (isFileSet
      && (
        (result.external_file_count ?? result.file_set_file_count)
          !== component.file_count
        || (result.external_size_bytes ?? result.file_set_total_size_bytes)
          !== component.installed_bytes
      )) {
    throw new Error(
      `provider file-set totals mismatch for ${component.component_id}`,
    );
  }
}

export function validateLockedProviderResultForLock(
  lock,
  component,
  result,
  artifactSize,
) {
  const errors = [];
  const provider = providerFor(lock, component, errors);
  if (!provider || errors.length > 0) {
    throw new Error(
      errors.join("\n")
        || `component ${component.component_id} has no trusted provider`,
    );
  }
  validateLockedProviderResult(component, result, artifactSize, provider);
}

export function validateLockedManagedResult(component, result, artifactSize) {
  if (result.schema !== "arena_managed_component_release_result_v1"
      || result.release_authority !== "manager_managed_component_only"
      || result.suite_installer_authority !== "iteathen/uci-arena-manager"
      || result.source_repository !== "iteathen/uci-arena-manager"
      || result.component_id !== component.component_id
      || result.component_version !== component.component_version
      || result.target_triple !== component.target_triple
      || result.source_commit !== component.source_commit
      || result.artifact_name !== component.artifact_name
      || result.artifact_sha256 !== component.artifact_sha256
      || result.artifact_size_bytes !== artifactSize
      || result.artifact_size_bytes !== component.artifact_size_bytes
      || result.installed_bytes !== component.installed_bytes
      || result.component_manifest_sha256 !== component.component_manifest_sha256
      || result.licenses_sha256 !== component.licenses_sha256
      || result.validation_evidence?.executable_smoke?.status !== "passed") {
    throw new Error(`managed result mismatch for ${component.component_id}`);
  }
  const upstreamInputs = result.upstream_inputs;
  const expectedRoles = component.component_id === "stockfish.engine"
    ? ["source", "evaluation_network"]
    : ["source"];
  if (!Array.isArray(upstreamInputs)
      || upstreamInputs.length !== expectedRoles.length
      || upstreamInputs.some((input, index) =>
        input?.role !== expectedRoles[index]
        || !/^https:\/\//.test(input.url ?? "")
        || !/^[0-9a-f]{64}$/.test(input.sha256 ?? ""))) {
    throw new Error(`managed upstream input mismatch for ${component.component_id}`);
  }
  const expectedProtocol = component.component_id === "stockfish.engine"
    ? "uci_startup"
    : "node_version";
  if (result.validation_evidence.executable_smoke.protocol !== expectedProtocol) {
    throw new Error(`managed smoke evidence mismatch for ${component.component_id}`);
  }
  if (component.component_id === "stockfish.engine"
      && result.source_offer_url !== component.source_offer_url) {
    throw new Error(`managed source offer mismatch for ${component.component_id}`);
  }
  const workflowIdentityPrefix =
    "iteathen/uci-arena-manager/"
    + ".github/workflows/managed-component-release.yml@"
    + `${component.provenance_workflow_ref}#`;
  if (typeof result.workflow_identity !== "string"
      || !result.workflow_identity.startsWith(workflowIdentityPrefix)
      || !/^[1-9][0-9]*\.[1-9][0-9]*$/.test(
        result.workflow_identity.slice(workflowIdentityPrefix.length),
      )) {
    throw new Error(`managed workflow identity mismatch for ${component.component_id}`);
  }
}

export async function fetchLockedComponents(
  lock,
  outputDirectory,
  providerToken,
  {
    managerToken,
    cosignExecutable = "cosign",
  } = {},
) {
  const validation = validateReleaseLock(lock);
  if (!validation.ok) throw new Error(validation.errors.join("\n"));
  if (lock.schema !== "arena_suite_release_lock_v2" && !providerToken) {
    throw new Error("a short-lived provider read token is required");
  }
  if (!managerToken) throw new Error("a short-lived Manager read token is required");
  mkdirSync(outputDirectory, { recursive: true });
  const rows = [];
  for (const component of lock.components) {
    const provider = providerFor(lock, component);
    if (!provider) {
      throw new Error(`component ${component.component_id} has no trusted provider`);
    }
    const assetSource = resolveProviderAssetSource(lock, component);
    const assetToken = assetSource.tokenKind === "manager"
      ? managerToken
      : providerToken;
    const artifactPath = path.join(outputDirectory, component.artifact_name);
    const resultPath = path.join(outputDirectory, component.provider_result_name);
    const bundlePath = path.join(outputDirectory, component.provenance_bundle_name);
    if (existsSync(artifactPath) || existsSync(resultPath) || existsSync(bundlePath)) {
      throw new Error(`refusing to overwrite provider assets for ${component.component_id}`);
    }
    await downloadAsset(
      assetSource.repository,
      assetSource.releaseTag,
      component.artifact_name,
      assetToken,
      artifactPath,
    );
    await downloadAsset(
      assetSource.repository,
      assetSource.releaseTag,
      component.provider_result_name,
      assetToken,
      resultPath,
    );
    await downloadAsset(
      assetSource.repository,
      assetSource.releaseTag,
      component.provenance_bundle_name,
      assetToken,
      bundlePath,
    );
    if (sha256File(artifactPath) !== component.artifact_sha256
        || sha256File(resultPath) !== component.provider_result_sha256
        || sha256File(bundlePath) !== component.provenance_bundle_sha256) {
      throw new Error(`locked digest mismatch for ${component.component_id}`);
    }
    const result = JSON.parse(readFileSync(resultPath, "utf8"));
    validateLockedProviderResult(
      component,
      result,
      statSync(artifactPath).size,
      provider,
    );
    verifyProviderProvenance(
      component,
      result,
      artifactPath,
      bundlePath,
      {
        cosignExecutable,
        providerRepository: provider.repository,
      },
    );
    rows.push({
      ...component,
      artifactPath,
      bundlePath,
      resultPath,
      result,
    });
  }
  for (const component of lock.managed_components) {
    const artifactPath = path.join(outputDirectory, component.artifact_name);
    const resultPath = path.join(outputDirectory, component.managed_result_name);
    const bundlePath = path.join(outputDirectory, component.provenance_bundle_name);
    if (existsSync(artifactPath) || existsSync(resultPath) || existsSync(bundlePath)) {
      throw new Error(`refusing to overwrite managed assets for ${component.component_id}`);
    }
    await downloadAsset(
      lock.managed_repository,
      component.managed_release_tag,
      component.artifact_name,
      managerToken,
      artifactPath,
    );
    await downloadAsset(
      lock.managed_repository,
      component.managed_release_tag,
      component.managed_result_name,
      managerToken,
      resultPath,
    );
    await downloadAsset(
      lock.managed_repository,
      component.managed_release_tag,
      component.provenance_bundle_name,
      managerToken,
      bundlePath,
    );
    if (sha256File(artifactPath) !== component.artifact_sha256
        || statSync(artifactPath).size !== component.artifact_size_bytes
        || sha256File(resultPath) !== component.managed_result_sha256
        || sha256File(bundlePath) !== component.provenance_bundle_sha256) {
      throw new Error(`locked digest mismatch for managed ${component.component_id}`);
    }
    const result = JSON.parse(readFileSync(resultPath, "utf8"));
    validateLockedManagedResult(component, result, statSync(artifactPath).size);
    verifyManagedProvenance(
      component,
      result,
      artifactPath,
      bundlePath,
      { cosignExecutable },
    );
    rows.push({
      ...component,
      artifactPath,
      bundlePath,
      resultPath,
      result,
      managed: true,
    });
  }
  return rows;
}

const DEPENDENCIES = Object.freeze({
  "manager.core": [],
  "uci_arena.engine": ["manager.core"],
  "node_runtime.private": ["manager.core"],
  "stockfish.engine": ["manager.core"],
  "book_forge.service": ["node_runtime.private", "stockfish.engine"],
  "opening_book.standard": [],
  "uci_arena_evidence.runtime": [
    "node_runtime.private",
    "uci_arena.engine",
    "stockfish.engine",
  ],
  "uci_arena_evidence.service": [
    "node_runtime.private",
    "uci_arena_evidence.runtime",
    "uci_arena.engine",
    "stockfish.engine",
  ],
  "lichess.adapter": [
    "node_runtime.private",
    "uci_arena.engine",
    "opening_book.standard",
  ],
  "syzygy.wdl.3-4-5": ["uci_arena.engine"],
  "syzygy.dtz.3-4-5": ["syzygy.wdl.3-4-5"],
});

export function assembleCatalog(lock, lockedRows, managerArtifacts) {
  const byIdentity = new Map();
  for (const row of lockedRows) {
    const key = row.component_id;
    const version = row.component_version ?? row.result.component_version;
    if (!byIdentity.has(key)) {
      const artifactType = row.artifact_type ?? "archive";
      const installedBytes =
        row.installed_bytes ??
        row.result.external_size_bytes ??
        row.result.file_set_total_size_bytes ??
        row.result.artifact_size_bytes;
      byIdentity.set(key, {
        id: key,
        version,
        dependencies: row.dependencies ?? DEPENDENCIES[key] ?? [],
        installed_bytes: installedBytes,
        staging_bytes: row.staging_bytes ?? installedBytes,
        backup_bytes: row.backup_bytes ?? installedBytes,
        workspace_reserve_bytes: row.workspace_reserve_bytes ?? 0,
        artifacts: {},
      });
    } else if (byIdentity.get(key).version !== version) {
      throw new Error(`component ${key} has inconsistent platform versions`);
    }
    const artifactTarget =
      row.target_triple === "any-any" ? "any" : row.target_triple;
    if (byIdentity.get(key).artifacts[artifactTarget]) {
      throw new Error(`component ${key} duplicates ${artifactTarget}`);
    }
    const artifact = {
      type: row.artifact_type ?? "archive",
      source: `payloads/${row.artifact_name}`,
      sha256: row.artifact_sha256,
      size_bytes: row.result.artifact_size_bytes,
    };
    if (artifact.type === "file_set") {
      artifact.file_count =
        row.file_count ??
        row.result.external_file_count ??
        row.result.file_set_file_count;
      artifact.total_size_bytes =
        row.installed_bytes ??
        row.result.external_size_bytes ??
        row.result.file_set_total_size_bytes;
    }
    byIdentity.get(key).artifacts[artifactTarget] = artifact;
  }
  const manager = {
    id: "manager.core",
    version: lock.suite_version,
    dependencies: [],
    installed_bytes: 0,
    staging_bytes: 0,
    backup_bytes: 0,
    workspace_reserve_bytes: 0,
    artifacts: {},
  };
  for (const artifact of managerArtifacts) {
    manager.artifacts[artifact.target_triple] = {
      source: `payloads/${path.basename(artifact.path)}`,
      sha256: sha256File(artifact.path),
      size_bytes: statSync(artifact.path).size,
    };
    manager.installed_bytes = Math.max(manager.installed_bytes, statSync(artifact.path).size);
    manager.staging_bytes = Math.max(manager.staging_bytes, statSync(artifact.path).size * 2);
    manager.backup_bytes = Math.max(manager.backup_bytes, statSync(artifact.path).size);
  }
  for (const required of [
    "uci_arena.engine",
    "node_runtime.private",
    "stockfish.engine",
    "book_forge.service",
    "opening_book.standard",
    "uci_arena_evidence.runtime",
    "uci_arena_evidence.service",
    "lichess.adapter",
    "syzygy.wdl.3-4-5",
    "syzygy.dtz.3-4-5",
  ]) {
    if (!byIdentity.has(required)) {
      throw new Error(`locked composition is missing catalog component ${required}`);
    }
  }
  return {
    schema: "arena_install_catalog_v1",
    schema_version: 1,
    suite_version: lock.suite_version,
    channel: "candidate",
    presets: {
      manager_only: ["manager.core"],
      complete_arena: [
        "manager.core",
        "uci_arena.engine",
        "node_runtime.private",
        "stockfish.engine",
        "book_forge.service",
        "opening_book.standard",
        "uci_arena_evidence.runtime",
        "uci_arena_evidence.service",
        "lichess.adapter",
      ],
    },
    components: [manager, ...byIdentity.values()],
  };
}
