# Suite release authority

`iteathen/uci-arena-manager` is the sole composition, installer, signing-request,
channel, and suite-publication authority. `iteathen/UCI_Arena` may publish only
immutable component candidates and their provider results.

Windows suite bundles expose Engine, Book Forge, Evidence Service, and Lichess
adapter as persisted default-on feature variables. The custom WiX options page
is used during initial setup and through Apps and Features Modify. Burn passes
only typed `component-id=0|1` selections to `arena-setup`; setup resolves
dependencies, stops Manager during the bounded transition, invokes provider
startup uninstall/install contracts, restarts Manager, and fails closed on a
transition error. Major upgrades reuse the persisted selection rather than
silently restoring a component the operator removed.

The checked-in `catalog.lock.json` is deliberately non-releasable until a
reviewed pull request pins every required platform candidate and managed
dependency by digest. Setting `release_ready` to `true` without complete
attested rows fails release-lock validation.

Manager-owned Node.js and Stockfish dependencies use private immutable
`managed/*` prereleases. Each lock row pins the archive, managed result, and
Sigstore bundle; composition verifies the protected workflow identity and the
exact signed DSSE predicate. Mutable public artifact URLs and unsigned
checksum-only provenance are not accepted.

All provider repositories remain private. Provider candidates use Cosign `v3.0.6`
keyless signing with the GitHub Actions OIDC identity; the resulting
`<artifact>.sigstore.json` asset contains the signing certificate,
transparency-log inclusion proof, and an in-toto statement whose sole subject
is the exact artifact digest and whose predicate is the exact provider result.
Each provider lock row pins that bundle's asset name and SHA-256 plus the exact
certificate identity, issuer, workflow name, `refs/heads/main` ref, and
`workflow_dispatch` trigger. A release steward copies those exact bytes into
the lock-declared private Manager transport prerelease with:

```powershell
node tools/release/mirror-provider-assets.mjs release/catalog.lock.json
```

The tool downloads each asset from its original private provider release,
verifies the reviewed digest before upload, and neither alters nor resigns it.
Suite CI downloads the mirror with its built-in repository token, verifies all
three digests, then runs Cosign with the original provider identity and source
commit before accepting the candidate. The mirror grants no release authority.
The signed provider result and bundle are retained under the candidate's
`provenance` directory.

The protected release workflow uses the same mechanism for Manager outputs. It
creates one signed multi-subject statement for each platform Manager build and
one for the final suite artifact set. No release workflow uses an
ignore-transparency-log, ignore-certificate, ignore-claim, ignore-SCT, or
unsigned fallback flag. A missing Sigstore service, bundle, claim, proof,
predicate, or digest fails closed; making either repository public is not a
fallback.

Repository administrators must configure these controls before the first
production run:

- Protect `main`, `suite-v*` tags, `.github/workflows/release.yml`,
  `packaging/**`, `release/**`, and `specs/schemas/**` through branch rules and
  CODEOWNERS review.
- Create the `production-release` environment with `@iteathen` as required
  reviewer and prevent self-review.
- After GitHub Advanced Security and the dependency graph are enabled for this
  private repository, set `DEPENDENCY_REVIEW_ENABLED=true`. Until then, CI
  keeps the lockfile-backed `npm audit --omit=dev` gate and skips the
  unavailable GitHub dependency-review action instead of failing every PR.
- Configure Azure Trusted Signing OIDC variables and the isolated Linux/TUF
  signing endpoint variables used by the workflow.
- Set `TUF_ROOT_SHA256` to the lowercase SHA-256 of the offline-root metadata.
  Official `arena-setup` binaries pin this value at compile time; an empty or
  different root is rejected.

Build and test jobs retain read-only repository contents permissions. The
Manager build receives only `id-token: write` in addition so it can issue
keyless Sigstore provenance; production signing jobs receive OIDC behind their
protected environments, and only the final publisher receives temporary
`contents: write`. Stable promotion publishes already-built and already-tested
bytes; it never rebuilds a candidate.

Native package outputs are staged under:

- `dist/windows`
- `dist/linux`

Production output names are governed by
`specs/installation_contract_v1.yaml`. Official artifacts may not be built,
signed, or promoted from a developer workstation.

## Canonical clean builds

The same explicitly scoped entrypoint builds and verifies native Manager-Core
installers locally and in CI:

```powershell
node tools/release/clean-build.mjs --manager-only --platform windows-x86_64
node tools/release/clean-build.mjs --manager-only --platform linux-x86_64
```

Omitting `--manager-only` fails closed: a native Manager package must never be
mislabelled as Complete Arena. The canonical complete clean build is the
protected `Suite release` workflow. It requires a release-ready lock, composes
reviewed provider candidates, runs both platform acceptance matrices, and emits
the Windows setup executables and Linux portable installers together with the
native Manager packages.

For a private unsigned Windows acceptance candidate from the same release-ready
lock, use:

```powershell
node tools/release/clean-build.mjs --complete-development --platform windows-x86_64
```

This path builds Manager Core cleanly, downloads the exact private provider
mirror assets, verifies every original-provider Sigstore identity and digest,
assembles retained online/offline media, and emits the two Suite Setup
executables plus the Manager MSI under `dist/windows`. Install
`UCI-Arena-Setup-<version>-windows-x64-offline.exe` for the most complete
single-cycle test. The bare MSI is retained for Manager-Core diagnostics and is
not the normal Complete Arena handoff.

Before a provider candidate is promoted into that lock, exact-package
acceptance must invoke its documented Manager API from outside the extracted
component directory. Output inspection uses `api outputs inspect --id
OUTPUT_ID`; a typed missing-output rejection must still identify the operation
as `outputs_inspect`.

The initial elevated development transaction records the exact retained
catalog digest beside the retained media. Native component install, installer
Modify, repair, and upgrade may reuse only that administrator-protected exact
catalog; changed or external unsigned catalogs still fail closed.

The command requires a clean tracked worktree, runs the spec and neutral naming
gates, builds and tests the dashboard and Manager Core, cleans the native build
before recompiling, and invokes the existing CPack generators. Windows builds
require WiX `6.0.2`; Linux builds require the platform package toolchain. The
Windows clean-build entrypoint uses `ARENA_CMAKE_EXECUTABLE` when supplied,
otherwise selects the newest supported portable CMake under
`build/toolchain/cmake-<version>/cmake-<version>-windows-x86_64/bin`, then falls
back to `PATH`. CMake, CPack, and CTest always come from that same toolchain.
Complete development composition likewise uses `COSIGN_EXECUTABLE` when set,
then the pinned `build/tooling/cosign-3.0.6/cosign-windows-amd64.exe`, then
`PATH`; it never skips or weakens provenance verification.

A successful Manager-Core Windows build emits exactly the versioned MSI and ZIP under
`dist/windows`. A successful Linux build emits exactly the versioned DEB, RPM,
and tarball under `dist/linux`. Each directory also contains
`clean-build-result.json`, which binds the exact artifact names, sizes, and
SHA-256 digests to the source commit and build preset. Missing, empty,
ambiguously named, or stale output fails the build.

Automatic pull-request CI is intentionally absent. Local pre-merge validation
runs these gates, and the manual `Suite release` workflow runs the platform
matrix only when explicitly dispatched. An incomplete release lock still
blocks suite composition, signing, and publication.

These unsigned outputs are development installers. The protected release
workflow remains the only authority that can compose full suite setup media,
sign accepted bytes, or promote a channel. Provider builds from
`iteathen/UCI_Arena` remain component handoffs and cannot satisfy this installer
contract.

## Local Windows installer-candidate validation

A developer-workstation MSI is an unsigned test candidate, never an official
release. Record its source commit and SHA-256 before testing. Do not use an MSI
from a dirty worktree or infer provenance from its filename.

Windows MSI and bundle builds use WiX `6.0.2`. Per-user MSI generation also
uses pinned CMake `4.3.4`, whose CPack generator creates the required HKCU
component key paths for profile-owned files. ICE38 is never suppressed. The MSI deliberately rejects a
different build with the same three-field product version as an upgrade; bump
the suite version for an upgrade or use the explicit repair lifecycle for the
same installed version.

Use `tools/release/windows-installer-cycle.ps1` as the local entrypoint. It
stores a commit-bound, digest-bound state machine under
`build/installer-validation/<scope>/<commit>/cycle.json`. The state is written as
`installing` before `msiexec` starts, so an interrupted run cannot silently
invoke the installer a second time. `smoke` is repeatable and never invokes
MSI.

Prepare and validate a candidate from a clean tracked worktree:

```powershell
pwsh tools/release/windows-installer-cycle.ps1 -Action prepare `
  -Scope per-user `
  -WixExe C:\path\to\wix.exe
```

Run the single per-user MSI invocation from an ordinary PowerShell session,
using the state path printed by `prepare`. It modifies only user-owned package
state and does not invoke UAC. This remains a single MSI invocation:

```powershell
pwsh tools/release/windows-installer-cycle.ps1 -Action install `
  -Scope per-user `
  -StatePath C:\path\to\cycle.json
```

Pass `-Scope per-machine` to every action when validating the all-users MSI;
only that scope requires an elevated session for install, service restart, and
restore.

Run as many non-installing smokes as needed against that instance. Optional UCI
executables are probed through the installed Manager, and the temporary Lichess
credential workflow passes only the token-file path to `arena-setup`:

```powershell
pwsh tools/release/windows-installer-cycle.ps1 -Action smoke `
  -Scope per-user `
  -StatePath C:\path\to\cycle.json `
  -ProbeEngine C:\path\to\engine.exe `
  -LichessTokenFile C:\path\to\lichess-token.txt
```

Use `-RestartService` to include post-restart database and authentication
recovery. It restarts the same-user hidden Manager process without elevation
for per-user cycles and requires elevation for the per-machine Windows service. Use
`-RequirePlanningEvidence` after registering two test engines and creating one
match plan and one tournament; this makes the repeatable smoke fail if those
durable records disappear. Multiple probe engines may be passed as a native
PowerShell array or as one comma-separated `-ProbeEngine` value when invoking
the script through `pwsh -File`.

If a cycle was started manually before the runner existed, `adopt
-ConfirmAdopt` records its exact MSI digest and quarantine without reinstalling
it. Pass the full original candidate commit with `-CandidateSourceCommit`; the
cycle records that separately from the newer runner commit and refuses to
misattribute the MSI. Adoption also verifies the fixed-scope MSI name, the
installed scope marker, and the exact running executable beneath the requested
install root; a reachable endpoint or same-named service is not sufficient.
`status` prints only redacted cycle metadata. `restore
-ConfirmRestore` requires elevation only for per-machine cycles, moves the test-created data root to a
separate evidence sibling, restores the quarantined root, verifies its safe
manifest and ACL, and does not delete either state tree.

This runner is a developer validation tool. It does not accept legal
agreements, sign artifacts, set the release lock, replace the protected VM
matrix, or produce an official release.

The Windows default data roots are:

```text
per-user:    %LOCALAPPDATA%\UCI Arena Manager
per-machine: C:\ProgramData\UCI Arena Manager
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
