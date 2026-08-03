#!/usr/bin/env node

import { createHash } from "node:crypto";
import {
  appendFileSync,
  closeSync,
  mkdirSync,
  openSync,
  readFileSync,
  readSync,
  writeFileSync,
} from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import { isDeepStrictEqual } from "node:util";

export const PROVIDER_PREDICATE_TYPE =
  "https://uci-arena.example/attestations/provider-release-result/v1";
export const INTOTO_STATEMENT_TYPE = "https://in-toto.io/Statement/v1";
export const SIGSTORE_BUNDLE_MEDIA_TYPE =
  "application/vnd.dev.sigstore.bundle.v0.3+json";

const MANAGER_REPOSITORY = "iteathen/uci-arena-manager";
const SHA256_PATTERN = /^[0-9a-f]{64}$/u;
const COMMIT_PATTERN = /^[0-9a-f]{40}$/u;

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

export function prepareProviderProvenance({
  artifactPath,
  providerResultPath,
  statementDirectory,
}) {
  const artifact = path.resolve(artifactPath);
  const resultPath = path.resolve(providerResultPath);
  const providerResult = readJson(resultPath);
  if (providerResult.schema !== "arena_provider_release_result_v1"
      || providerResult.source_repository !== MANAGER_REPOSITORY
      || providerResult.component_id !== "manager.core"
      || providerResult.release_authority !== "provider_component_only"
      || providerResult.suite_installer_authority
        !== "iteathen/uci-arena-installer"
      || !COMMIT_PATTERN.test(providerResult.source_commit ?? "")
      || path.basename(artifact) !== providerResult.artifact_name
      || sha256File(artifact) !== providerResult.artifact_sha256) {
    throw new Error("provider result does not bind the exact Manager artifact");
  }
  const statement = buildExpectedProviderStatement(providerResult);
  const statementRoot = path.resolve(statementDirectory);
  mkdirSync(statementRoot, { recursive: true });
  const bundlePath = `${artifact}.sigstore.json`;
  const statementPath = path.join(
    statementRoot,
    `${providerResult.artifact_name}.intoto.json`,
  );
  writeFileSync(statementPath, canonicalJson(statement), { flag: "wx" });
  return {
    artifactPath: artifact,
    bundlePath,
    predicateType: PROVIDER_PREDICATE_TYPE,
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
  if (command === "prepare-provider") {
    const prepared = prepareProviderProvenance({
      artifactPath: args.artifact,
      providerResultPath: args.result,
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
    + "<prepare-provider|verify-bundle> [options]",
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
