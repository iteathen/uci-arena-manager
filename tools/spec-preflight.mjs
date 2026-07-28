#!/usr/bin/env node

import { readFile } from "node:fs/promises";
import path from "node:path";
import process from "node:process";

function argumentsOf(argv) {
  const parsed = { task: "", changedFiles: [] };
  for (let index = 0; index < argv.length; index += 1) {
    if (argv[index] === "--task") {
      parsed.task = argv[index + 1] ?? "";
      index += 1;
    } else if (argv[index] === "--changed-files") {
      parsed.changedFiles = argv.slice(index + 1);
      break;
    }
  }
  return parsed;
}

const args = argumentsOf(process.argv.slice(2));
const root = path.resolve(import.meta.dirname, "..");
const indexText = await readFile(path.join(root, "specs", "index.yaml"), "utf8");

const entries = [];
let current = null;
for (const line of indexText.split(/\r?\n/)) {
  const id = line.match(/^\s*-\s+spec_id:\s*(.+)$/);
  if (id) {
    current = { specId: id[1].trim(), specPath: "", appliesTo: [] };
    entries.push(current);
    continue;
  }
  if (!current) continue;
  const specPath = line.match(/^\s+path:\s*(.+)$/);
  if (specPath) current.specPath = specPath[1].trim();
  const apply = line.match(/^\s+-\s+(.+)$/);
  if (apply && !apply[1].includes(":")) {
    current.appliesTo.push(apply[1].replaceAll('"', "").trim());
  }
}

const normalizedFiles = args.changedFiles.map((file) =>
  path.relative(root, path.resolve(root, file)).replaceAll("\\", "/"),
);
const required = new Set(["specs/00_READ_FIRST.yaml"]);

for (const entry of entries) {
  if (
    entry.appliesTo.includes("*") ||
    normalizedFiles.some((file) =>
      entry.appliesTo.some(
        (prefix) =>
          file === prefix ||
          file.startsWith(`${prefix}/`) ||
          prefix.startsWith(`${file}/`),
      ),
    )
  ) {
    required.add(entry.specPath);
  }
}

const warnings = normalizedFiles
  .filter(
    (file) =>
      !entries.some(
        (entry) =>
          entry.appliesTo.includes("*") ||
          entry.appliesTo.some(
            (prefix) =>
              file === prefix ||
              file.startsWith(`${prefix}/`) ||
              prefix.startsWith(`${file}/`),
          ),
      ),
  )
  .map((file) => `No path-specific spec matched ${file}`);

process.stdout.write(
  `${JSON.stringify(
    {
      schema: "arena_spec_preflight_v1",
      task: args.task,
      changed_files: normalizedFiles,
      required_specs: [...required],
      warnings,
    },
    null,
    2,
  )}\n`,
);
