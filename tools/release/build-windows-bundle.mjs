#!/usr/bin/env node

import {
  lstatSync,
  mkdirSync,
  readdirSync,
  writeFileSync,
} from "node:fs";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

function parse(argv) {
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

function xml(value) {
  return value
    .replaceAll("&", "&amp;")
    .replaceAll('"', "&quot;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;");
}

function files(root, relative = "") {
  const rows = [];
  for (const name of readdirSync(path.join(root, relative)).sort()) {
    const child = relative ? path.join(relative, name) : name;
    const status = lstatSync(path.join(root, child));
    if (status.isSymbolicLink()) {
      throw new Error(`suite media contains a symbolic link: ${child}`);
    }
    if (status.isDirectory()) {
      rows.push(...files(root, child));
    } else if (status.isFile()) {
      rows.push(child);
    } else {
      throw new Error(`suite media contains an unsupported entry: ${child}`);
    }
  }
  return rows;
}

function payloadName(relative) {
  const normalized = path.normalize(relative);
  const segments = normalized.split(path.sep);
  if (segments.length === 2 && segments[0].toLowerCase() === "bin") {
    return segments[1];
  }
  return normalized;
}

export function buildPayloadFragment(mediaRoot) {
  const root = path.resolve(mediaRoot);
  const setup = path.normalize(path.join("bin", "arena-setup.exe"));
  const payloads = files(root)
    .filter((relative) => path.normalize(relative) !== setup);
  if (!lstatSync(path.join(root, setup)).isFile()) {
    throw new Error("suite media is missing bin/arena-setup.exe");
  }
  if (!payloads.some((relative) =>
    path.normalize(relative) === path.normalize("release/catalog.json"))) {
    throw new Error("suite media is missing release/catalog.json");
  }
  if (!payloads.some((relative) =>
    path.normalize(relative) === path.normalize("release/tuf/root.json"))) {
    throw new Error("suite media is missing release/tuf/root.json");
  }
  const lines = [
    '<?xml version="1.0" encoding="UTF-8"?>',
    '<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">',
    '  <Fragment>',
    '    <PayloadGroup Id="SuiteMediaPayloads">',
  ];
  payloads.forEach((relative, index) => {
    const source = path.join(root, relative);
    const name = payloadName(relative).replaceAll("/", "\\");
    lines.push(
      `      <Payload Id="SuiteMedia${String(index + 1).padStart(4, "0")}" ` +
      `SourceFile="${xml(source)}" Name="${xml(name)}" />`,
    );
  });
  lines.push(
    "    </PayloadGroup>",
    "  </Fragment>",
    "</Wix>",
    "",
  );
  return lines.join("\n");
}

export function main(argv = process.argv.slice(2)) {
  const args = parse(argv);
  if (!args["media-root"] || !args.output) {
    throw new Error("--media-root and --output are required");
  }
  const output = path.resolve(args.output);
  mkdirSync(path.dirname(output), { recursive: true });
  writeFileSync(output, buildPayloadFragment(args["media-root"]), "utf8");
}

if (path.resolve(process.argv[1] ?? "") === fileURLToPath(import.meta.url)) {
  try {
    main();
  } catch (error) {
    process.stderr.write(`build-windows-bundle: ${error.message}\n`);
    process.exitCode = 1;
  }
}
