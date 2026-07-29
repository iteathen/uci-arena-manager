#!/usr/bin/env node

import { spawnSync } from "node:child_process";
import {
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  statSync,
} from "node:fs";
import os from "node:os";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

import {
  sha256File,
  validateReleaseLock,
} from "./release-lock.mjs";

const ASSET_FIELDS = Object.freeze([
  Object.freeze({
    name: "artifact_name",
    sha256: "artifact_sha256",
  }),
  Object.freeze({
    name: "provider_result_name",
    sha256: "provider_result_sha256",
  }),
  Object.freeze({
    name: "provenance_bundle_name",
    sha256: "provenance_bundle_sha256",
  }),
]);

export function buildProviderMirrorPlan(lock) {
  const validation = validateReleaseLock(lock);
  if (!validation.ok) throw new Error(validation.errors.join("\n"));
  const mirror = lock.provider_asset_mirror;
  const assets = [];
  for (const component of lock.components) {
    const provider = lock.providers?.[component.provider_id];
    if (!provider) {
      throw new Error(
        `${component.component_id} has no provider repository for mirroring`,
      );
    }
    for (const field of ASSET_FIELDS) {
      assets.push({
        componentId: component.component_id,
        targetTriple: component.target_triple,
        sourceRepository: provider.repository,
        sourceReleaseTag: component.provider_release_tag,
        name: component[field.name],
        sha256: component[field.sha256],
      });
    }
  }
  assets.sort((left, right) => left.name.localeCompare(right.name));
  return {
    repository: mirror.repository,
    releaseTag: mirror.release_tag,
    suiteVersion: lock.suite_version,
    assets,
  };
}

function executeGh(args, { capture = false, allowNotFound = false } = {}) {
  const result = spawnSync("gh", args, {
    encoding: "utf8",
    stdio: capture ? "pipe" : "inherit",
    windowsHide: true,
  });
  if (result.error) throw result.error;
  if (result.status === 0) return result.stdout ?? "";
  const diagnostic = `${result.stderr ?? ""}\n${result.stdout ?? ""}`.trim();
  if (allowNotFound && /(?:release not found|HTTP 404)/iu.test(diagnostic)) {
    return null;
  }
  throw new Error(
    `gh ${args.slice(0, 3).join(" ")} failed with status ${result.status}`
    + (diagnostic ? `: ${diagnostic}` : ""),
  );
}

function releaseState(plan) {
  const output = executeGh(
    [
      "release",
      "view",
      plan.releaseTag,
      "--repo",
      plan.repository,
      "--json",
      "isDraft,isPrerelease,assets",
    ],
    { capture: true, allowNotFound: true },
  );
  return output === null ? null : JSON.parse(output);
}

function assertReleaseInventory(plan, release) {
  const expected = plan.assets
    .map((asset) => ({
      name: asset.name,
      size: statSync(asset.localPath).size,
    }))
    .sort((left, right) => left.name.localeCompare(right.name));
  const actual = (release.assets ?? [])
    .map((asset) => ({ name: asset.name, size: asset.size }))
    .sort((left, right) => left.name.localeCompare(right.name));
  if (JSON.stringify(actual) !== JSON.stringify(expected)) {
    throw new Error("provider mirror release inventory differs from the lock");
  }
}

function downloadAndVerifySourceAssets(plan, directory) {
  mkdirSync(directory, { recursive: true });
  const releases = new Map();
  for (const asset of plan.assets) {
    const key = `${asset.sourceRepository}\n${asset.sourceReleaseTag}`;
    const release = releases.get(key) ?? {
      repository: asset.sourceRepository,
      tag: asset.sourceReleaseTag,
      names: [],
    };
    release.names.push(asset.name);
    releases.set(key, release);
  }
  for (const release of releases.values()) {
    const patterns = release.names.flatMap((name) => ["--pattern", name]);
    executeGh([
      "release",
      "download",
      release.tag,
      "--repo",
      release.repository,
      ...patterns,
      "--dir",
      directory,
    ]);
  }
  for (const asset of plan.assets) {
    const localPath = path.join(directory, asset.name);
    if (sha256File(localPath) !== asset.sha256) {
      throw new Error(
        `source release digest mismatch for ${asset.componentId} ${asset.name}`,
      );
    }
    asset.localPath = localPath;
  }
}

function verifyPublishedMirror(plan, directory) {
  mkdirSync(directory, { recursive: true });
  const patterns = plan.assets.flatMap((asset) => [
    "--pattern",
    asset.name,
  ]);
  executeGh([
    "release",
    "download",
    plan.releaseTag,
    "--repo",
    plan.repository,
    ...patterns,
    "--dir",
    directory,
  ]);
  for (const asset of plan.assets) {
    if (sha256File(path.join(directory, asset.name)) !== asset.sha256) {
      throw new Error(`published mirror digest mismatch for ${asset.name}`);
    }
  }
}

export function mirrorProviderAssets(lock) {
  const plan = buildProviderMirrorPlan(lock);
  executeGh(["auth", "status"]);
  const temporaryRoot = mkdtempSync(
    path.join(os.tmpdir(), "uci-arena-provider-mirror-"),
  );
  try {
    const sourceDirectory = path.join(temporaryRoot, "source");
    const mirrorDirectory = path.join(temporaryRoot, "mirror");
    downloadAndVerifySourceAssets(plan, sourceDirectory);

    let release = releaseState(plan);
    if (release && !release.isDraft) {
      assertReleaseInventory(plan, release);
      verifyPublishedMirror(plan, mirrorDirectory);
      return {
        schema: "arena_provider_asset_mirror_result_v1",
        status: "already_verified",
        repository: plan.repository,
        release_tag: plan.releaseTag,
        assets: plan.assets.length,
      };
    }
    if (!release) {
      executeGh([
        "release",
        "create",
        plan.releaseTag,
        "--repo",
        plan.repository,
        "--target",
        "main",
        "--draft",
        "--title",
        `UCI Arena provider mirror ${plan.suiteVersion}`,
        "--notes",
        "Private digest-pinned transport mirror. Original provider Sigstore provenance remains authoritative.",
      ]);
    }
    executeGh([
      "release",
      "upload",
      plan.releaseTag,
      ...plan.assets.map((asset) => asset.localPath),
      "--repo",
      plan.repository,
      "--clobber",
    ]);
    release = releaseState(plan);
    assertReleaseInventory(plan, release);
    executeGh([
      "release",
      "edit",
      plan.releaseTag,
      "--repo",
      plan.repository,
      "--draft=false",
      "--prerelease=true",
    ]);
    release = releaseState(plan);
    if (release.isDraft || !release.isPrerelease) {
      throw new Error("provider mirror did not become a private prerelease");
    }
    assertReleaseInventory(plan, release);
    return {
      schema: "arena_provider_asset_mirror_result_v1",
      status: "published",
      repository: plan.repository,
      release_tag: plan.releaseTag,
      assets: plan.assets.length,
    };
  } finally {
    rmSync(temporaryRoot, { recursive: true, force: true });
  }
}

function main() {
  const args = process.argv.slice(2);
  const dryRun = args.includes("--dry-run");
  const lockArgument = args.find((argument) => argument !== "--dry-run")
    ?? "release/catalog.lock.json";
  const lockPath = path.resolve(lockArgument);
  const lock = JSON.parse(readFileSync(lockPath, "utf8"));
  let result;
  if (dryRun) {
    const plan = buildProviderMirrorPlan(lock);
    result = {
      schema: "arena_provider_asset_mirror_plan_v1",
      status: "planned",
      ...plan,
      assets: plan.assets.map((asset) => ({
        component_id: asset.componentId,
        target_triple: asset.targetTriple,
        source_repository: asset.sourceRepository,
        source_release_tag: asset.sourceReleaseTag,
        name: asset.name,
        sha256: asset.sha256,
      })),
    };
  } else {
    result = mirrorProviderAssets(lock);
  }
  process.stdout.write(`${JSON.stringify(result, null, 2)}\n`);
}

if (process.argv[1]
    && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main();
}
