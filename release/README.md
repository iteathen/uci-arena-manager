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

## Local Windows installer-candidate validation

A developer-workstation MSI is an unsigned test candidate, never an official
release. Record its source commit and SHA-256 before testing. Do not use an MSI
from a dirty worktree or infer provenance from its filename.

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
