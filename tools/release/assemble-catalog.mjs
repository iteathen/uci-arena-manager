#!/usr/bin/env node

import {
  copyFileSync,
  mkdirSync,
  readFileSync,
  writeFileSync,
} from "node:fs";
import path from "node:path";
import process from "node:process";
import {
  assembleCatalog,
  sha256File,
  validateReleaseLock,
} from "./release-lock.mjs";

function parse(argv) {
  const values = {};
  for (let index = 0; index < argv.length; index += 2) {
    if (!argv[index]?.startsWith("--") || argv[index + 1] === undefined) {
      throw new Error(`invalid argument near ${argv[index] ?? "<end>"}`);
    }
    const key = argv[index].slice(2);
    if (values[key]) {
      values[key] = Array.isArray(values[key])
        ? [...values[key], argv[index + 1]]
        : [values[key], argv[index + 1]];
    } else values[key] = argv[index + 1];
  }
  return values;
}

const args = parse(process.argv.slice(2));
for (const required of ["lock", "provider-dir", "output-dir", "manager-windows", "manager-linux"]) {
  if (!args[required]) throw new Error(`--${required} is required`);
}
const lock = JSON.parse(readFileSync(args.lock, "utf8"));
const validation = validateReleaseLock(lock);
if (!validation.ok) throw new Error(validation.errors.join("\n"));
const providerDirectory = path.resolve(args["provider-dir"]);
const providerRows = lock.components.map((component) => {
  const artifactPath = path.join(providerDirectory, component.artifact_name);
  const resultPath = path.join(providerDirectory, component.provider_result_name);
  if (sha256File(artifactPath) !== component.artifact_sha256
      || sha256File(resultPath) !== component.provider_result_sha256) {
    throw new Error(`provider bytes do not match lock for ${component.component_id}`);
  }
  return {
    ...component,
    artifactPath,
    result: JSON.parse(readFileSync(resultPath, "utf8")),
  };
});
const managedRows = lock.managed_components.map((component) => {
  const artifactPath = path.join(providerDirectory, component.artifact_name);
  const provenancePath = path.join(
    providerDirectory,
    `${component.artifact_name}.provenance.json`,
  );
  if (sha256File(artifactPath) !== component.artifact_sha256
      || sha256File(provenancePath) !== component.provenance_sha256) {
    throw new Error(`managed bytes do not match lock for ${component.component_id}`);
  }
  return {
    ...component,
    artifactPath,
    provenancePath,
    managed: true,
    result: {
      component_version: component.component_version,
      artifact_size_bytes: component.artifact_size_bytes,
    },
  };
});
const rows = [...providerRows, ...managedRows];
const managerArtifacts = [
  { target_triple: "windows-x86_64", path: path.resolve(args["manager-windows"]) },
  { target_triple: "linux-x86_64", path: path.resolve(args["manager-linux"]) },
];
const output = path.resolve(args["output-dir"]);
const payloads = path.join(output, "payloads");
mkdirSync(payloads, { recursive: true });
for (const row of rows) {
  copyFileSync(row.artifactPath, path.join(payloads, row.artifact_name));
}
const provenance = path.join(output, "provenance");
mkdirSync(provenance, { recursive: true });
for (const row of providerRows) {
  copyFileSync(
    path.join(providerDirectory, row.provider_result_name),
    path.join(provenance, row.provider_result_name),
  );
}
for (const row of managedRows) {
  copyFileSync(
    row.provenancePath,
    path.join(provenance, path.basename(row.provenancePath)),
  );
}
for (const artifact of managerArtifacts) {
  copyFileSync(artifact.path, path.join(payloads, path.basename(artifact.path)));
}
const catalog = assembleCatalog(lock, rows, managerArtifacts);
writeFileSync(path.join(output, "catalog.json"), `${JSON.stringify(catalog, null, 2)}\n`);
process.stdout.write(`${JSON.stringify({
  schema: "arena_catalog_assembly_result_v1",
  suite_version: catalog.suite_version,
  component_count: catalog.components.length,
  catalog_sha256: sha256File(path.join(output, "catalog.json")),
})}\n`);
