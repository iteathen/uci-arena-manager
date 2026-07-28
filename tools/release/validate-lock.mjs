#!/usr/bin/env node

import { readFileSync } from "node:fs";
import path from "node:path";
import process from "node:process";
import { validateReleaseLock } from "./release-lock.mjs";

const filename = path.resolve(process.argv[2] ?? "release/catalog.lock.json");
const lock = JSON.parse(readFileSync(filename, "utf8"));
const result = validateReleaseLock(lock);
if (!result.ok) {
  process.stderr.write(`${result.errors.join("\n")}\n`);
  process.exit(1);
}
process.stdout.write(`${JSON.stringify({
  schema: "arena_suite_release_lock_validation_v1",
  status: "pass",
  suite_version: lock.suite_version,
  components: lock.components.length,
})}\n`);
