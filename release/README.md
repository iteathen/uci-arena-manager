# Suite release authority

`iteathen/uci-arena-manager` is the sole composition, installer, signing-request,
channel, and suite-publication authority. `iteathen/UCI_Arena` may publish only
immutable component candidates and their provider results.

The checked-in `catalog.lock.json` is deliberately non-releasable until a
reviewed pull request pins every required platform candidate and managed
dependency by digest. Setting `release_ready` to `true` without complete
attested rows fails release-lock validation.

Repository administrators must configure these controls before the first
production run:

- Protect `main`, `suite-v*` tags, `.github/workflows/release.yml`,
  `packaging/**`, `release/**`, and `specs/schemas/**` through branch rules and
  CODEOWNERS review.
- Create the `production-release` environment with `@iteathen` as required
  reviewer and prevent self-review.
- Configure the read-only component GitHub App as
  `COMPONENT_READER_APP_ID` / `COMPONENT_READER_PRIVATE_KEY`.
- Configure Azure Trusted Signing OIDC variables and the isolated Linux/TUF
  signing endpoint variables used by the workflow.
- Set `TUF_ROOT_SHA256` to the lowercase SHA-256 of the offline-root metadata.
  Official `arena-setup` binaries pin this value at compile time; an empty or
  different root is rejected.

Build and test jobs retain read-only repository permissions. Only the
environment-approved signing jobs receive OIDC, and only the final publisher
receives temporary `contents: write`. Stable promotion publishes already-built
and already-tested bytes; it never rebuilds a candidate.

Native package outputs are staged under:

- `dist/windows`
- `dist/linux`

Production output names are governed by
`specs/installation_contract_v1.yaml`. Official artifacts may not be built,
signed, or promoted from a developer workstation.

## Canonical clean builds

The same entrypoint builds and verifies native Manager installers locally and
in CI:

```powershell
node tools/release/clean-build.mjs --platform windows-x86_64
node tools/release/clean-build.mjs --platform linux-x86_64
```

The command requires a clean tracked worktree, runs the spec and neutral naming
gates, builds and tests the dashboard and Manager Core, cleans the native build
before recompiling, and invokes the existing CPack generators. Windows builds
require WiX `6.0.2`; Linux builds require the platform package toolchain.

A successful Windows build emits exactly the versioned MSI and ZIP under
`dist/windows`. A successful Linux build emits exactly the versioned DEB, RPM,
and tarball under `dist/linux`. Each directory also contains
`clean-build-result.json`, which binds the exact artifact names, sizes, and
SHA-256 digests to the source commit and build preset. Missing, empty,
ambiguously named, or stale output fails the build.

These unsigned outputs are development installers. The protected release
workflow remains the only authority that can compose full suite setup media,
sign accepted bytes, or promote a channel. Provider builds from
`iteathen/UCI_Arena` remain component handoffs and cannot satisfy this installer
contract.

## Local Windows installer-candidate validation

A developer-workstation MSI is an unsigned test candidate, never an official
release. Record its source commit and SHA-256 before testing. Do not use an MSI
from a dirty worktree or infer provenance from its filename.

Windows MSI and bundle builds use WiX `6.0.2`. The MSI deliberately rejects a
different build with the same three-field product version as an upgrade; bump
the suite version for an upgrade or use the explicit repair lifecycle for the
same installed version.

Use `tools/release/windows-installer-cycle.ps1` as the local entrypoint. It
stores a commit-bound, digest-bound state machine under
`build/installer-validation/<commit>/cycle.json`. The state is written as
`installing` before `msiexec` starts, so an interrupted run cannot silently
invoke the installer a second time. `smoke` is repeatable and never invokes
MSI.

Prepare and validate a candidate from a clean tracked worktree:

```powershell
pwsh tools/release/windows-installer-cycle.ps1 -Action prepare `
  -WixExe C:\path\to\wix.exe
```

Run the single MSI invocation from an elevated PowerShell session, using the
state path printed by `prepare`:

```powershell
pwsh tools/release/windows-installer-cycle.ps1 -Action install `
  -StatePath C:\path\to\cycle.json
```

Run as many non-installing smokes as needed against that instance. Optional UCI
executables are probed through the installed Manager, and the temporary Lichess
credential workflow passes only the token-file path to `arena-setup`:

```powershell
pwsh tools/release/windows-installer-cycle.ps1 -Action smoke `
  -StatePath C:\path\to\cycle.json `
  -ProbeEngine C:\path\to\engine.exe `
  -LichessTokenFile C:\path\to\lichess-token.txt
```

Run `smoke` from an elevated session with `-RestartService` to include
post-restart database and authentication recovery. Use
`-RequirePlanningEvidence` after registering two test engines and creating one
match plan and one tournament; this makes the repeatable smoke fail if those
durable records disappear. Multiple probe engines may be passed as a native
PowerShell array or as one comma-separated `-ProbeEngine` value when invoking
the script through `pwsh -File`.

If a cycle was started manually before the runner existed, `adopt
-ConfirmAdopt` records its exact MSI digest and quarantine without reinstalling
it. Pass the full original candidate commit with `-CandidateSourceCommit`; the
cycle records that separately from the newer runner commit and refuses to
misattribute the MSI. `status` prints only redacted cycle metadata. `restore
-ConfirmRestore` requires elevation, moves the test-created data root to a
separate evidence sibling, restores the quarantined root, verifies its safe
manifest and ACL, and does not delete either state tree.

This runner is a developer validation tool. It does not accept legal
agreements, sign artifacts, set the release lock, replace the protected VM
matrix, or produce an official release.

The Windows default data root is:

```text
C:\ProgramData\UCI Arena Manager
```

Normal uninstall intentionally preserves this directory. The authoritative
retention table is in `specs/installation_contract_v1.yaml`; in summary it
retains `arena.db`, `client.key`, `knowledge`, `locators`, verified release
media, trust state, content cache, transaction evidence, and provider
workspaces. It removes installed component roots, startup registrations,
receipt JSON documents, the Manager service, and MSI-owned program files.

Use a same-volume quarantine to test first-run behavior without destroying
retained state:

1. Confirm that `UCIArenaManager`, `arena-service`, `arena`, `arena-broker`,
   and `arena-setup` are stopped and no UCI Arena scheduled task is active.
2. Record the source path, owner, ACL, timestamps, file count, byte count, and
   content hashes. Never print `client.key` or any provider credential.
3. Rename the canonical data root to a unique sibling such as
   `UCI Arena Manager.pre-clean-install-<UTC timestamp>`. Do not copy it and do
   not move it to another volume.
4. Confirm the canonical path is absent, then install the exact recorded
   candidate.
5. Verify service registration and startup, packaged dashboard loading,
   CLI/browser pairing, an application smoke test, repair, and uninstall.
6. Move the test-created canonical data root to a separate evidence location.
   Never merge it into the preserved root.
7. Rename the preserved sibling back to the exact canonical path.
8. Recheck its owner, ACL, inventory, hashes, and timestamps before starting a
   restored or newly installed service.

This proves a clean application-data lifecycle on the workstation. It does not
prove a pristine-machine lifecycle because Windows Installer caches, registry
state, service ACLs, firewall state, and shared runtimes may remain elsewhere.
Official readiness still requires the protected disposable-VM matrix.
