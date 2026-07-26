#!/usr/bin/env node

import { access, readFile } from "node:fs/promises";
import path from "node:path";
import process from "node:process";

const root = path.resolve(import.meta.dirname, "..");
const indexPath = path.join(root, "specs", "index.yaml");
const indexText = await readFile(indexPath, "utf8");
const referenced = [...indexText.matchAll(/^\s+path:\s*(.+)$/gm)].map((match) =>
  match[1].trim(),
);

const failures = [];
for (const relative of referenced) {
  try {
    await access(path.join(root, relative));
  } catch {
    failures.push(`missing spec: ${relative}`);
  }
}

for (const schema of [
  "specs/schemas/command-envelope.schema.json",
  "specs/schemas/event-envelope.schema.json",
  "specs/schemas/service-profile.schema.json",
]) {
  try {
    JSON.parse(await readFile(path.join(root, schema), "utf8"));
  } catch (error) {
    failures.push(`invalid JSON schema ${schema}: ${error.message}`);
  }
}

if (!indexText.includes("arena_foundation_v1")) {
  failures.push("foundation spec is not registered");
}

if (failures.length > 0) {
  process.stderr.write(`${failures.join("\n")}\n`);
  process.exit(1);
}

process.stdout.write(`spec-check: ${referenced.length} registered specs valid\n`);
