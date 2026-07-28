#!/usr/bin/env node

import { readFileSync } from "node:fs";
import path from "node:path";
import process from "node:process";
import { fetchLockedComponents } from "./release-lock.mjs";

const lockPath = path.resolve(process.argv[2] ?? "release/catalog.lock.json");
const output = path.resolve(process.argv[3] ?? "dist/provider");
const lock = JSON.parse(readFileSync(lockPath, "utf8"));
const rows = await fetchLockedComponents(lock, output, process.env.GH_PROVIDER_TOKEN);
process.stdout.write(`${JSON.stringify({
  schema: "arena_suite_component_fetch_result_v1",
  components: rows.map((row) => ({
    component_id: row.component_id,
    target_triple: row.target_triple,
    artifact_sha256: row.artifact_sha256,
  })),
})}\n`);
