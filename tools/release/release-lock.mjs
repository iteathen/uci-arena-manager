import { createHash } from "node:crypto";
import {
  createWriteStream,
  existsSync,
  mkdirSync,
  readFileSync,
  statSync,
} from "node:fs";
import path from "node:path";

const REQUIRED_PROVIDER_COMPONENTS = new Set([
  "uci_arena.engine",
  "book_forge.service",
  "uci_arena_evidence.service",
  "opening_book.standard",
  "lichess.adapter",
]);
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

export function validateReleaseLock(lock, { requireReady = true } = {}) {
  const errors = [];
  if (lock.schema !== "arena_suite_release_lock_v1") errors.push("wrong lock schema");
  if (!/^\d+\.\d+\.\d+(?:[-.][A-Za-z0-9.-]+)?$/.test(lock.suite_version ?? "")) {
    errors.push("suite_version must be immutable semver text");
  }
  if (lock.provider_repository !== "iteathen/UCI_Arena") {
    errors.push("provider_repository must be iteathen/UCI_Arena");
  }
  if (requireReady && lock.release_ready !== true) errors.push("release_ready is not true");
  if (!Array.isArray(lock.components)) errors.push("components must be an array");
  if (!Array.isArray(lock.managed_components)) {
    errors.push("managed_components must be an array");
  }
  const identities = new Set();
  const coverage = new Map([...TARGETS].map((target) => [target, new Set()]));
  const portableProviderCoverage = new Set();
  for (const component of lock.components ?? []) {
    const identity = `${component.component_id}:${component.target_triple}`;
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
    for (const key of ["artifact_sha256", "provider_result_sha256"]) {
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
    for (const key of ["artifact_name", "provider_result_name"]) {
      if (!component[key] || path.basename(component[key]) !== component[key]) {
        errors.push(`${identity} has unsafe ${key}`);
      }
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
      "provenance_sha256",
    ]) {
      if (!/^[0-9a-f]{64}$/.test(component[key] ?? "")) {
        errors.push(`${identity} has invalid ${key}`);
      }
    }
    for (const key of ["artifact_url", "provenance_url"]) {
      if (!/^https:\/\//.test(component[key] ?? "")) {
        errors.push(`${identity} has invalid ${key}`);
      }
    }
    if (!component.artifact_name
        || path.basename(component.artifact_name) !== component.artifact_name) {
      errors.push(`${identity} has unsafe artifact_name`);
    }
    if (!Number.isSafeInteger(component.artifact_size_bytes)
        || component.artifact_size_bytes <= 0) {
      errors.push(`${identity} has invalid artifact_size_bytes`);
    }
    if (component.component_id === "stockfish.engine"
        && !/^https:\/\//.test(component.source_offer_url ?? "")) {
      errors.push(`${identity} must include the Stockfish source offer`);
    }
  }
  if (requireReady) {
    for (const [target, components] of coverage) {
      for (const required of REQUIRED_PROVIDER_COMPONENTS) {
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
    throw new Error(`cannot resolve provider release ${tag}: HTTP ${releaseResponse.status}`);
  }
  const release = await releaseResponse.json();
  const asset = release.assets.find((candidate) => candidate.name === name);
  if (!asset) throw new Error(`provider release ${tag} is missing ${name}`);
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
    throw new Error(`cannot download provider asset ${name}: HTTP ${response.status}`);
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

async function downloadUrl(url, destination) {
  const response = await fetch(url, {
    headers: {"User-Agent": "uci-arena-manager-release"},
    redirect: "follow",
  });
  if (!response.ok || !response.body) {
    throw new Error(`cannot download managed component: HTTP ${response.status}`);
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
) {
  const isFileSet =
    (component.artifact_type ?? "archive") === "file_set";
  if (result.schema !== "arena_provider_release_result_v1"
      || result.release_authority !== "provider_component_only"
      || result.suite_installer_authority !== "iteathen/uci-arena-manager"
      || result.component_id !== component.component_id
      || result.target_triple !== component.target_triple
      || result.source_commit !== component.source_commit
      || (
        isFileSet
          ? result.artifact_kind !== "external_file_set_manifest"
          : result.artifact_kind !== undefined && result.artifact_kind !== "archive"
      )
      || result.artifact_sha256 !== component.artifact_sha256
      || result.artifact_size_bytes !== artifactSize) {
    throw new Error(`provider result mismatch for ${component.component_id}`);
  }
  if (isFileSet
      && (
        result.validation_evidence?.range_probe?.status !== "passed"
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

export async function fetchLockedComponents(lock, outputDirectory, token) {
  const validation = validateReleaseLock(lock);
  if (!validation.ok) throw new Error(validation.errors.join("\n"));
  if (!token) throw new Error("a short-lived provider read token is required");
  mkdirSync(outputDirectory, { recursive: true });
  const rows = [];
  for (const component of lock.components) {
    const artifactPath = path.join(outputDirectory, component.artifact_name);
    const resultPath = path.join(outputDirectory, component.provider_result_name);
    if (existsSync(artifactPath) || existsSync(resultPath)) {
      throw new Error(`refusing to overwrite provider assets for ${component.component_id}`);
    }
    await downloadAsset(
      lock.provider_repository,
      component.provider_release_tag,
      component.artifact_name,
      token,
      artifactPath,
    );
    await downloadAsset(
      lock.provider_repository,
      component.provider_release_tag,
      component.provider_result_name,
      token,
      resultPath,
    );
    if (sha256File(artifactPath) !== component.artifact_sha256
        || sha256File(resultPath) !== component.provider_result_sha256) {
      throw new Error(`locked digest mismatch for ${component.component_id}`);
    }
    const result = JSON.parse(readFileSync(resultPath, "utf8"));
    validateLockedProviderResult(
      component,
      result,
      statSync(artifactPath).size,
    );
    rows.push({ ...component, artifactPath, resultPath, result });
  }
  for (const component of lock.managed_components) {
    const artifactPath = path.join(outputDirectory, component.artifact_name);
    const provenanceName = `${component.artifact_name}.provenance.json`;
    const provenancePath = path.join(outputDirectory, provenanceName);
    if (existsSync(artifactPath) || existsSync(provenancePath)) {
      throw new Error(`refusing to overwrite managed assets for ${component.component_id}`);
    }
    await downloadUrl(component.artifact_url, artifactPath);
    await downloadUrl(component.provenance_url, provenancePath);
    if (sha256File(artifactPath) !== component.artifact_sha256
        || statSync(artifactPath).size !== component.artifact_size_bytes
        || sha256File(provenancePath) !== component.provenance_sha256) {
      throw new Error(`locked digest mismatch for managed ${component.component_id}`);
    }
    rows.push({
      ...component,
      artifactPath,
      provenancePath,
      result: {
        component_version: component.component_version,
        artifact_size_bytes: component.artifact_size_bytes,
      },
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
  "uci_arena_evidence.service": [
    "node_runtime.private",
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
        "uci_arena_evidence.service",
      ],
    },
    components: [manager, ...byIdentity.values()],
  };
}
