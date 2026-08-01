import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const repositoryRoot = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)),
  "..",
  "..",
);
const runner = readFileSync(
  path.join(repositoryRoot, "tools", "release", "windows-installer-cycle.ps1"),
  "utf8",
);
const releaseReadme = readFileSync(
  path.join(repositoryRoot, "release", "README.md"),
  "utf8",
);

test("Windows installer cycle makes MSI invocation single-shot and resumable", () => {
  assert.match(
    runner,
    /ValidateSet\("prepare", "install", "adopt", "smoke", "status", "restore"\)/,
  );
  assert.match(runner, /ValidateSet\("per-user", "per-machine"\)/);
  assert.match(runner, /\[string\]\$Scope = "per-user"/);
  assert.match(runner, /phase -ne "prepared"/);
  assert.match(runner, /install\.invocation_count -ne 0/);
  assert.match(runner, /phase = "installing"/);
  assert.match(runner, /install\.invocation_count = 1/);
  assert.match(runner, /Save-Cycle \$cycle[\s\S]*?Start-Process -FilePath "msiexec\.exe"/);
  assert.match(runner, /MSI candidate digest changed after cycle preparation/);
  assert.match(runner, /CandidateSourceCommit must be a full Git commit SHA/);
  assert.match(runner, /runner_commit = \$sourceCommit/);
  assert.match(runner, /source_commit = \$CandidateSourceCommit\.ToLowerInvariant\(\)/);
  assert.match(runner, /if \(\$RestartService\)[\s\S]*?Restart-Service/);
  assert.match(runner, /RequirePlanningEvidence/);
  assert.match(runner, /suppliedPath\.Split\(",", \[StringSplitOptions\]::RemoveEmptyEntries\)/);
  assert.match(runner, /Probe engine path is unavailable/);
  assert.match(runner, /bin\\uci-arena-manager\.exe/);
  assert.match(runner, /GetEnvironmentVariable\("Path", \$pathScope\)/);
  assert.match(runner, /if \(\$Scope -eq "per-machine"\)[\s\S]*?Assert-Administrator/);
  assert.match(runner, /Start-Process -FilePath \$servicePath[\s\S]*?-WindowStyle Hidden/);
  assert.match(runner, /Per-user MSI generation requires CMake 4\.3 or newer/);
  assert.match(runner, /\*-windows-x64-\$artifactScope\.msi/);
  assert.equal(runner.match(/Assert-MsiCandidateScope -Path \$MsiPath/g)?.length, 2);
  assert.match(runner, /Assert-InstalledScopeMarker/);
  assert.match(runner, /Get-CommandExecutablePath -CommandLine \$service\.PathName/);
  assert.match(runner, /Installed Manager scope '.+' does not match requested scope/);
  assert.match(runner, /Start Menu\\Programs\\UCI Arena Manager\.lnk/);
  assert.match(runner, /"components", "bot-profiles", "bot-candidates"/);
  assert.doesNotMatch(runner, /Remove-Item/);
});

test("Windows installer cycle preserves state by rename and fails closed", () => {
  assert.match(runner, /same-volume sibling/);
  assert.match(runner, /Rename-Item -LiteralPath \$cycle\.paths\.data_root/);
  assert.match(runner, /failed-\$\(\$cycle\.cycle_id\)/);
  assert.match(runner, /original state was restored when possible/);
  assert.match(runner, /Restore moves the active test state and requires -ConfirmRestore/);
  assert.match(runner, /Assert-RetainedManifest/);
  assert.match(runner, /totalBytes \+= \[int64\]\$entry\["size_bytes"\]/);
  assert.doesNotMatch(runner, /Measure-Object size_bytes/);
});

test("Windows installer cycle keeps credential material out of evidence", () => {
  assert.match(runner, /Test-SensitiveRelativePath/);
  assert.match(runner, /if \(\$sensitive\) \{ \$null \}/);
  assert.match(runner, /--kind", "lichess_token_path", "--path", \$LichessTokenFile/);
  assert.match(runner, /path_reference_only/);
  assert.match(runner, /lichess_token_path_imported = \$credentialPathImported/);
  assert.doesNotMatch(runner, /Get-Content[^\r\n]*\$LichessTokenFile/);
  assert.doesNotMatch(runner, /Get-FileHash[^\r\n]*\$LichessTokenFile/);
});

test("release documentation routes local candidates through the cycle runner", () => {
  assert.match(releaseReadme, /windows-installer-cycle\.ps1/);
  assert.match(releaseReadme, /single MSI invocation/i);
  assert.match(releaseReadme, /prepare/);
  assert.match(releaseReadme, /smoke/);
  assert.match(releaseReadme, /restore/);
  assert.match(releaseReadme, /CandidateSourceCommit/);
});
