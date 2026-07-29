#!/usr/bin/env node

import { createHash } from "node:crypto";
import { createWriteStream, existsSync, rmSync } from "node:fs";
import path from "node:path";
import process from "node:process";

const args = Object.fromEntries(
  Array.from({ length: process.argv.slice(2).length / 2 }, (_, index) => {
    const flag = process.argv[2 + index * 2];
    const value = process.argv[3 + index * 2];
    if (!flag?.startsWith("--") || value === undefined) {
      throw new Error(`invalid argument near ${flag ?? "<end>"}`);
    }
    return [flag.slice(2), value];
  }),
);
if (!/^https:\/\//u.test(args.url ?? "")) throw new Error("--url must use HTTPS");
if (!/^[0-9a-f]{64}$/u.test(args.sha256 ?? "")) {
  throw new Error("--sha256 must be lowercase SHA-256");
}
if (!args.output) throw new Error("--output is required");
const output = path.resolve(args.output);
if (existsSync(output)) throw new Error(`refusing to overwrite ${output}`);
const response = await fetch(args.url, {
  headers: { "User-Agent": "uci-arena-manager-release" },
  redirect: "follow",
});
if (!response.ok || !response.body) {
  throw new Error(`pinned input download failed: HTTP ${response.status}`);
}
const hash = createHash("sha256");
const stream = createWriteStream(output, { flags: "wx", mode: 0o600 });
try {
  for await (const chunk of response.body) {
    hash.update(chunk);
    if (!stream.write(chunk)) {
      await new Promise((resolve) => stream.once("drain", resolve));
    }
  }
  await new Promise((resolve, reject) => {
    stream.once("error", reject);
    stream.end(resolve);
  });
  const actual = hash.digest("hex");
  if (actual !== args.sha256) {
    throw new Error(`pinned input digest mismatch: expected ${args.sha256}, got ${actual}`);
  }
} catch (error) {
  stream.destroy();
  rmSync(output, { force: true });
  throw error;
}
process.stdout.write(`download-pinned-input: verified ${args.sha256}\n`);
