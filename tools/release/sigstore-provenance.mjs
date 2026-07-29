#!/usr/bin/env node

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import {
  appendFileSync,
  closeSync,
  lstatSync,
  mkdirSync,
  openSync,
  readFileSync,
  readSync,
  readdirSync,
  writeFileSync,
} from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import { isDeepStrictEqual } from "node:util";

export const PROVIDER_PREDICATE_TYPE =
  "https://uci-arena.example/attestations/provider-release-result/v1";
export const MANAGED_PREDICATE_TYPE =
  "https://uci-arena.example/attestations/managed-component-release-result/v1";
export const MANAGER_PREDICATE_TYPE =
  "https://uci-arena.example/attestations/manager-release/v1";
export const INTOTO_STATEMENT_TYPE = "https://in-toto.io/Statement/v1";
export const SIGSTORE_BUNDLE_MEDIA_TYPE =
  "application/vnd.dev.sigstore.bundle.v0.3+json";

const MANAGER_REPOSITORY = "iteathen/uci-arena-manager";
const SHA256_PATTERN = /^[0-9a-f]{64}$/u;
const COMMIT_PATTERN = /^[0-9a-f]{40}$/u;
const VERSION_PATTERN = /^\d+\.\d+\.\d+(?:[-.][A-Za-z0-9.-]+)?$/u;
const SAFE_BUNDLE_NAME_PATTERN = /^[A-Za-z0-9][A-Za-z0-9._+-]*\.sigstore\.json$/u;

function parseArguments(argv) {
  const values = {};
  for (let index = 0; index < argv.length; index += 2) {
    const flag = argv[index];
    const value = argv[index + 1];
    if (!flag?.startsWith("--") || value === undefined) {
      throw new Error(`invalid argument near ${flag ?? "<end>"}`);
    }
    values[flag.slice(2)] = value;
  }
  return values;
}

function canonicalJson(value) {
  return `${JSON.stringify(value, null, 2)}\n`;
}

function readJson(filename) {
  return JSON.parse(readFileSync(filename, "utf8"));
}

function sha256File(filename) {
  const hash = createHash("sha256");
  const buffer = Buffer.allocUnsafe(1024 * 1024);
  const descriptor = openSync(filename, "r");
  try {
    for (;;) {
      const bytesRead = readSync(descriptor, buffer, 0, buffer.length, null);
      if (bytesRead === 0) break;
      hash.update(buffer.subarray(0, bytesRead));
    }
  } finally {
    closeSync(descriptor);
  }
  return hash.digest("hex");
}

function decodeCanonicalBase64(value) {
  if (typeof value !== "string"
      || value.length === 0
      || value.length % 4 !== 0
      || !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/u.test(value)) {
    throw new Error("Sigstore bundle DSSE payload is not canonical base64");
  }
  const decoded = Buffer.from(value, "base64");
  if (decoded.toString("base64") !== value) {
    throw new Error("Sigstore bundle DSSE payload is not canonical base64");
  }
  return decoded;
}

function readBundleStatement(bundlePath) {
  const bundle = readJson(bundlePath);
  if (bundle?.mediaType !== SIGSTORE_BUNDLE_MEDIA_TYPE) {
    throw new Error("unsupported Sigstore bundle media type");
  }
  if (typeof bundle?.verificationMaterial?.certificate?.rawBytes !== "string"
      || bundle.verificationMaterial.certificate.rawBytes.length === 0) {
    throw new Error("Sigstore bundle is missing its signing certificate");
  }
  const logEntries = bundle.verificationMaterial.tlogEntries;
  if (!Array.isArray(logEntries)
      || logEntries.length === 0
      || logEntries.some((entry) => !entry?.inclusionProof)) {
    throw new Error("Sigstore bundle is missing a transparency-log inclusion proof");
  }
  if (bundle?.dsseEnvelope?.payloadType !== "application/vnd.in-toto+json") {
    throw new Error("Sigstore bundle does not contain an in-toto DSSE payload");
  }
  if (!Array.isArray(bundle.dsseEnvelope.signatures)
      || bundle.dsseEnvelope.signatures.length !== 1) {
    throw new Error("Sigstore bundle must contain exactly one DSSE signature");
  }
  return JSON.parse(
    decodeCanonicalBase64(bundle.dsseEnvelope.payload).toString("utf8"),
  );
}

export function buildExpectedProviderStatement(providerResult) {
  return {
    _type: INTOTO_STATEMENT_TYPE,
    subject: [{
      name: providerResult.artifact_name,
      digest: { sha256: providerResult.artifact_sha256 },
    }],
    predicateType: PROVIDER_PREDICATE_TYPE,
    predicate: providerResult,
  };
}

export function buildExpectedManagedStatement(managedResult) {
  return {
    _type: INTOTO_STATEMENT_TYPE,
    subject: [{
      name: managedResult.artifact_name,
      digest: { sha256: managedResult.artifact_sha256 },
    }],
    predicateType: MANAGED_PREDICATE_TYPE,
    predicate: managedResult,
  };
}

export function prepareManagedProvenance({
  artifactPath,
  managedResultPath,
  statementDirectory,
}) {
  const artifact = path.resolve(artifactPath);
  const resultPath = path.resolve(managedResultPath);
  const managedResult = readJson(resultPath);
  if (managedResult.schema !== "arena_managed_component_release_result_v1"
      || managedResult.source_repository !== MANAGER_REPOSITORY
      || !COMMIT_PATTERN.test(managedResult.source_commit ?? "")
      || path.basename(artifact) !== managedResult.artifact_name
      || sha256File(artifact) !== managedResult.artifact_sha256) {
    throw new Error("managed result does not bind the exact managed artifact");
  }
  const statement = buildExpectedManagedStatement(managedResult);
  const statementRoot = path.resolve(statementDirectory);
  mkdirSync(statementRoot, { recursive: true });
  const bundlePath = `${artifact}.sigstore.json`;
  const statementPath = path.join(
    statementRoot,
    `${managedResult.artifact_name}.intoto.json`,
  );
  writeFileSync(statementPath, canonicalJson(statement), { flag: "wx" });
  return {
    artifactPath: artifact,
    bundlePath,
    predicateType: MANAGED_PREDICATE_TYPE,
    statement,
    statementPath,
  };
}

export function verifyProviderProvenance(
  component,
  providerResult,
  artifactPath,
  bundlePath,
  {
    cosignExecutable = "cosign",
    execute = execFileSync,
  } = {},
) {
  const actualStatement = readBundleStatement(bundlePath);
  const expectedStatement = buildExpectedProviderStatement(providerResult);
  if (!isDeepStrictEqual(actualStatement, expectedStatement)) {
    throw new Error(
      `signed provider predicate mismatch for ${component.component_id}`,
    );
  }
  const arguments_ = [
    "verify-blob-attestation",
    "--bundle",
    bundlePath,
    "--certificate-identity",
    component.provenance_certificate_identity,
    "--certificate-oidc-issuer",
    component.provenance_oidc_issuer,
    "--certificate-github-workflow-repository",
    "iteathen/UCI_Arena",
    "--certificate-github-workflow-sha",
    component.source_commit,
    "--certificate-github-workflow-name",
    component.provenance_workflow_name,
    "--certificate-github-workflow-ref",
    component.provenance_workflow_ref,
    "--certificate-github-workflow-trigger",
    component.provenance_workflow_trigger,
    "--type",
    PROVIDER_PREDICATE_TYPE,
    artifactPath,
  ];
  try {
    execute(cosignExecutable, arguments_, {
      encoding: "utf8",
      stdio: "pipe",
      windowsHide: true,
    });
  } catch (error) {
    throw new Error(
      `Sigstore verification failed for ${component.component_id}`,
      { cause: error },
    );
  }
  return actualStatement;
}

export function verifyManagedProvenance(
  component,
  managedResult,
  artifactPath,
  bundlePath,
  {
    cosignExecutable = "cosign",
    execute = execFileSync,
  } = {},
) {
  const actualStatement = readBundleStatement(bundlePath);
  const expectedStatement = buildExpectedManagedStatement(managedResult);
  if (!isDeepStrictEqual(actualStatement, expectedStatement)) {
    throw new Error(
      `signed managed predicate mismatch for ${component.component_id}`,
    );
  }
  const arguments_ = [
    "verify-blob-attestation",
    "--bundle",
    bundlePath,
    "--certificate-identity",
    component.provenance_certificate_identity,
    "--certificate-oidc-issuer",
    component.provenance_oidc_issuer,
    "--certificate-github-workflow-repository",
    MANAGER_REPOSITORY,
    "--certificate-github-workflow-sha",
    component.source_commit,
    "--certificate-github-workflow-name",
    component.provenance_workflow_name,
    "--certificate-github-workflow-ref",
    component.provenance_workflow_ref,
    "--certificate-github-workflow-trigger",
    component.provenance_workflow_trigger,
    "--type",
    MANAGED_PREDICATE_TYPE,
    artifactPath,
  ];
  try {
    execute(cosignExecutable, arguments_, {
      encoding: "utf8",
      stdio: "pipe",
      windowsHide: true,
    });
  } catch (error) {
    throw new Error(
      `Sigstore verification failed for ${component.component_id}`,
      { cause: error },
    );
  }
  return actualStatement;
}

function normalizedRelative(root, filename) {
  const relative = path.relative(root, filename).replaceAll("\\", "/");
  const parts = relative.split("/");
  if (!relative
      || path.isAbsolute(relative)
      || parts.some((part) => part === "" || part === "." || part === "..")) {
    throw new Error(`unsafe release artifact path: ${filename}`);
  }
  return relative;
}

function listFiles(root, relative = "") {
  const rows = [];
  const directory = path.join(root, relative);
  for (const name of readdirSync(directory).sort()) {
    const childRelative = relative ? `${relative}/${name}` : name;
    const absolute = path.join(root, childRelative);
    const entry = lstatSync(absolute);
    if (entry.isSymbolicLink()) {
      throw new Error(`symbolic links are forbidden in release artifacts: ${absolute}`);
    }
    if (entry.isDirectory()) rows.push(...listFiles(root, childRelative));
    else if (entry.isFile()) rows.push(absolute);
    else throw new Error(`unsupported release artifact: ${absolute}`);
  }
  return rows;
}

function managerArtifactPaths(root, scope) {
  const files = listFiles(root).filter((filename) =>
    !filename.endsWith(".sigstore.json"));
  if (scope === "suite-release") return files;
  if (scope === "manager-build") {
    return files.filter((filename) => {
      const relative = normalizedRelative(root, filename);
      return (
        (!relative.includes("/") && relative.endsWith(".tar.gz"))
        || relative.startsWith("windows/")
        || relative.startsWith("linux/")
      );
    });
  }
  throw new Error(`unsupported Manager provenance scope: ${scope}`);
}

export function prepareManagerProvenance({
  artifactRoot,
  bundleName,
  sourceCommit,
  sourceRepository,
  statementDirectory,
  suiteVersion,
  targetTriple,
  workflowIdentity,
  scope,
}) {
  const root = path.resolve(artifactRoot);
  if (sourceRepository !== MANAGER_REPOSITORY) {
    throw new Error(`source repository must be ${MANAGER_REPOSITORY}`);
  }
  if (!COMMIT_PATTERN.test(sourceCommit ?? "")) {
    throw new Error("Manager provenance requires an exact source commit");
  }
  if (!VERSION_PATTERN.test(suiteVersion ?? "")) {
    throw new Error("Manager provenance requires immutable suite version text");
  }
  if (!["windows-x86_64", "linux-x86_64", "any"].includes(targetTriple)
      || (scope === "manager-build" && targetTriple === "any")
      || (scope === "suite-release" && targetTriple !== "any")) {
    throw new Error("Manager provenance has an unsupported target triple");
  }
  if (typeof workflowIdentity !== "string"
      || !/^iteathen\/uci-arena-manager\/\.github\/workflows\/release\.yml@refs\/heads\/main#[1-9][0-9]*\.[1-9][0-9]*$/u.test(
        workflowIdentity,
      )) {
    throw new Error("Manager provenance has an invalid workflow identity");
  }
  if (!SAFE_BUNDLE_NAME_PATTERN.test(bundleName ?? "")
      || path.basename(bundleName) !== bundleName) {
    throw new Error("Manager provenance bundle name is unsafe");
  }
  const artifactPaths = managerArtifactPaths(root, scope);
  if (artifactPaths.length === 0) {
    throw new Error(`Manager provenance scope ${scope} contains no artifacts`);
  }
  const subjects = artifactPaths.map((filename) => ({
    name: normalizedRelative(root, filename),
    digest: { sha256: sha256File(filename) },
  }));
  const statement = {
    _type: INTOTO_STATEMENT_TYPE,
    subject: subjects,
    predicateType: MANAGER_PREDICATE_TYPE,
    predicate: {
      schema: "arena_manager_release_provenance_v1",
      artifact_scope: scope,
      artifact_count: subjects.length,
      source_repository: sourceRepository,
      source_commit: sourceCommit,
      suite_version: suiteVersion,
      target_triple: targetTriple,
      workflow_identity: workflowIdentity,
    },
  };
  const statementRoot = path.resolve(statementDirectory);
  mkdirSync(statementRoot, { recursive: true });
  const statementPath = path.join(statementRoot, `${bundleName}.intoto.json`);
  const bundlePath = path.join(root, bundleName);
  writeFileSync(statementPath, canonicalJson(statement), { flag: "wx" });
  return {
    anchorArtifactPath: artifactPaths[0],
    bundlePath,
    predicateType: MANAGER_PREDICATE_TYPE,
    statement,
    statementPath,
  };
}

export function verifyBundlePayload(bundlePath, statementPath) {
  const actualStatement = readBundleStatement(bundlePath);
  const expectedStatement = readJson(statementPath);
  if (!isDeepStrictEqual(actualStatement, expectedStatement)) {
    throw new Error("Sigstore bundle payload does not match the expected statement");
  }
  return actualStatement;
}

function writeGitHubOutputs(filename, values) {
  for (const [name, value] of Object.entries(values)) {
    const text = String(value);
    if (/[\u0000-\u001f\u007f]/u.test(text)) {
      throw new Error(`unsafe GitHub output value for ${name}`);
    }
    appendFileSync(filename, `${name}=${text}\n`, "utf8");
  }
}

function main(argv) {
  const [command, ...rest] = argv;
  const args = parseArguments(rest);
  if (command === "prepare-manager") {
    const prepared = prepareManagerProvenance({
      artifactRoot: args["artifact-root"],
      bundleName: args["bundle-name"],
      scope: args.scope,
      sourceCommit: args["source-commit"],
      sourceRepository: args["source-repository"],
      statementDirectory: args["statement-directory"],
      suiteVersion: args["suite-version"],
      targetTriple: args["target-triple"],
      workflowIdentity: args["workflow-identity"],
    });
    if (args["github-output"]) {
      writeGitHubOutputs(args["github-output"], {
        anchor_artifact_path: prepared.anchorArtifactPath,
        bundle_path: prepared.bundlePath,
        predicate_type: prepared.predicateType,
        statement_path: prepared.statementPath,
      });
    } else {
      process.stdout.write(canonicalJson(prepared));
    }
    return;
  }
  if (command === "prepare-managed") {
    const prepared = prepareManagedProvenance({
      artifactPath: args.artifact,
      managedResultPath: args.result,
      statementDirectory: args["statement-directory"],
    });
    if (args["github-output"]) {
      writeGitHubOutputs(args["github-output"], {
        artifact_path: prepared.artifactPath,
        bundle_path: prepared.bundlePath,
        predicate_type: prepared.predicateType,
        statement_path: prepared.statementPath,
      });
    } else {
      process.stdout.write(canonicalJson(prepared));
    }
    return;
  }
  if (command === "verify-bundle") {
    verifyBundlePayload(args.bundle, args.statement);
    process.stdout.write("sigstore-provenance: bundle payload verified\n");
    return;
  }
  throw new Error(
    "usage: sigstore-provenance.mjs "
    + "<prepare-managed|prepare-manager|verify-bundle> [options]",
  );
}

if (process.argv[1]
    && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url))) {
  try {
    main(process.argv.slice(2));
  } catch (error) {
    process.stderr.write(`sigstore-provenance: ${error.message}\n`);
    process.exitCode = 1;
  }
}
