#!/usr/bin/env node

import { createHash } from "node:crypto";
import { execFileSync } from "node:child_process";
import {
  createReadStream,
  createWriteStream,
  mkdirSync,
  readFileSync,
  readdirSync,
  rmSync,
  statSync,
  writeFileSync,
} from "node:fs";
import { Readable } from "node:stream";
import path from "node:path";
import process from "node:process";

function sha256File(filename) {
  return createHash("sha256").update(readFileSync(filename)).digest("hex");
}

function files(root, relative = "") {
  const rows = [];
  for (const name of readdirSync(path.join(root, relative)).sort()) {
    const child = relative ? `${relative}/${name}` : name;
    const status = statSync(path.join(root, child));
    if (status.isDirectory()) rows.push(...files(root, child));
    else if (status.isFile()) rows.push(child.replaceAll("\\", "/"));
    else throw new Error(`unsupported signing input ${child}`);
  }
  return rows;
}

async function oidcToken(audience) {
  const requestUrl = process.env.ACTIONS_ID_TOKEN_REQUEST_URL;
  const requestToken = process.env.ACTIONS_ID_TOKEN_REQUEST_TOKEN;
  if (!requestUrl || !requestToken) {
    throw new Error("GitHub Actions OIDC environment is unavailable");
  }
  const separator = requestUrl.includes("?") ? "&" : "?";
  const response = await fetch(
    `${requestUrl}${separator}audience=${encodeURIComponent(audience)}`,
    { headers: { Authorization: `Bearer ${requestToken}` } },
  );
  if (!response.ok) throw new Error(`OIDC token request failed: HTTP ${response.status}`);
  const result = await response.json();
  if (!result.value) throw new Error("OIDC token response is incomplete");
  return result.value;
}

async function streamResponse(response, filename) {
  if (!response.body) throw new Error("signing service returned no body");
  await new Promise((resolve, reject) => {
    const output = createWriteStream(filename, { flags: "wx", mode: 0o600 });
    output.on("error", reject);
    output.on("finish", resolve);
    Readable.fromWeb(response.body).pipe(output).on("error", reject);
  });
}

const input = path.resolve(process.argv[2] ?? "");
const output = path.resolve(process.argv[3] ?? "");
const endpoint = process.env.RELEASE_SIGNING_ENDPOINT?.replace(/\/+$/, "");
const audience = process.env.RELEASE_SIGNING_AUDIENCE;
if (!input || !output || !endpoint?.startsWith("https://") || !audience) {
  throw new Error(
    "input, output, HTTPS RELEASE_SIGNING_ENDPOINT, and RELEASE_SIGNING_AUDIENCE are required",
  );
}
const rows = files(input).map((relative) => ({
  path: relative,
  sha256: sha256File(path.join(input, relative)),
  size_bytes: statSync(path.join(input, relative)).size,
}));
const request = {
  schema: "arena_release_signing_request_v1",
  repository: process.env.GITHUB_REPOSITORY,
  source_commit: process.env.GITHUB_SHA,
  workflow_ref: process.env.GITHUB_WORKFLOW_REF,
  run_id: process.env.GITHUB_RUN_ID,
  artifacts: rows,
  requested_outputs: [
    "openpgp_packages_and_repository",
    "tuf_root_targets_snapshot_timestamp",
    "detached_portable_signatures",
  ],
};
const requestBytes = Buffer.from(`${JSON.stringify(request, null, 2)}\n`);
const requestSha256 = createHash("sha256").update(requestBytes).digest("hex");
writeFileSync(path.join(input, "signing-request.json"), requestBytes);
const requestArchive = path.join(path.dirname(output), "signing-request.tar");
const responseArchive = path.join(path.dirname(output), "signing-response.tar");
mkdirSync(path.dirname(output), { recursive: true });
rmSync(requestArchive, { force: true });
rmSync(responseArchive, { force: true });
execFileSync("cmake", ["-E", "tar", "cf", requestArchive, "--format=gnutar", "."], {
  cwd: input,
});
const token = await oidcToken(audience);
const response = await fetch(`${endpoint}/v1/sign-suite-release`, {
  method: "POST",
  headers: {
    Authorization: `Bearer ${token}`,
    "Content-Type": "application/x-tar",
    "X-Arena-Signing-Request-SHA256": requestSha256,
  },
  body: Readable.toWeb(createReadStream(requestArchive)),
  duplex: "half",
});
if (!response.ok) {
  throw new Error(`remote signing request failed: HTTP ${response.status}`);
}
await streamResponse(response, responseArchive);
rmSync(output, { recursive: true, force: true });
mkdirSync(output, { recursive: true });
execFileSync("cmake", ["-E", "tar", "xf", responseArchive], { cwd: output });
const result = JSON.parse(readFileSync(path.join(output, "signing-result.json"), "utf8"));
if (result.schema !== "arena_release_signing_result_v1"
    || result.request_sha256 !== requestSha256
    || result.status !== "signed") {
  throw new Error("remote signing result does not bind the approved request");
}
for (const artifact of result.artifacts ?? []) {
  const filename = path.join(output, artifact.path);
  if (!artifact.path || path.resolve(filename).startsWith(`${output}${path.sep}`) === false
      || sha256File(filename) !== artifact.sha256) {
    throw new Error(`remote signing output failed verification: ${artifact.path}`);
  }
}
rmSync(requestArchive, { force: true });
rmSync(responseArchive, { force: true });
process.stdout.write(`${JSON.stringify({
  schema: "arena_remote_signing_client_result_v1",
  request_sha256: requestSha256,
  signed_artifacts: result.artifacts.length,
})}\n`);
