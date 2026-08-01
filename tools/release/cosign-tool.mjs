import { existsSync, lstatSync } from "node:fs";
import path from "node:path";
import process from "node:process";

const pinnedCosignVersion = "3.0.6";

function regularFile(filename) {
  return existsSync(filename) && lstatSync(filename).isFile();
}

export function resolveCosignExecutable(executable = "cosign", {
  repositoryRoot,
  platform = process.platform,
} = {}) {
  if (executable !== "cosign") {
    if (path.isAbsolute(executable) && !regularFile(executable)) {
      throw new Error(`configured Cosign executable is unavailable: ${executable}`);
    }
    return executable;
  }
  if (platform !== "win32" || !repositoryRoot) return executable;
  const localExecutable = path.join(
    repositoryRoot,
    "build",
    "tooling",
    `cosign-${pinnedCosignVersion}`,
    "cosign-windows-amd64.exe",
  );
  return regularFile(localExecutable) ? localExecutable : executable;
}
