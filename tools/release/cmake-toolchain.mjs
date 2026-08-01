import { existsSync, lstatSync, readdirSync } from "node:fs";
import path from "node:path";
import process from "node:process";

const cmakeTools = new Set(["cmake", "cpack", "ctest"]);
const minimumWindowsVersion = [4, 3, 0];

function regularFile(filename) {
  return existsSync(filename) && lstatSync(filename).isFile();
}

function compareVersions(left, right) {
  for (let index = 0; index < left.length; index += 1) {
    if (left[index] !== right[index]) return left[index] - right[index];
  }
  return 0;
}

function siblingTool(cmakeExecutable, command, platform) {
  const extension = platform === "win32" ? ".exe" : "";
  return path.join(path.dirname(cmakeExecutable), `${command}${extension}`);
}

export function resolveCMakeTool(command, {
  repositoryRoot,
  env = process.env,
  platform = process.platform,
} = {}) {
  if (!cmakeTools.has(command)) return command;
  const override = env.ARENA_CMAKE_EXECUTABLE?.trim();
  if (override) {
    const cmakeExecutable = path.resolve(override);
    const executable = command === "cmake"
      ? cmakeExecutable
      : siblingTool(cmakeExecutable, command, platform);
    if (!regularFile(executable)) {
      throw new Error(`configured CMake tool is unavailable: ${executable}`);
    }
    return executable;
  }
  if (platform !== "win32" || !repositoryRoot) return command;

  const toolchainRoot = path.join(repositoryRoot, "build", "toolchain");
  if (!existsSync(toolchainRoot)) return command;
  const candidates = [];
  for (const entry of readdirSync(toolchainRoot, { withFileTypes: true })) {
    const match = /^cmake-(\d+)\.(\d+)\.(\d+)$/u.exec(entry.name);
    if (!entry.isDirectory() || !match) continue;
    const version = match.slice(1).map(Number);
    if (compareVersions(version, minimumWindowsVersion) < 0) continue;
    const cmakeExecutable = path.join(
      toolchainRoot,
      entry.name,
      `${entry.name}-windows-x86_64`,
      "bin",
      "cmake.exe",
    );
    const executable = command === "cmake"
      ? cmakeExecutable
      : siblingTool(cmakeExecutable, command, platform);
    if (regularFile(cmakeExecutable) && regularFile(executable)) {
      candidates.push({ executable, version });
    }
  }
  candidates.sort((left, right) => compareVersions(right.version, left.version));
  return candidates[0]?.executable ?? command;
}
