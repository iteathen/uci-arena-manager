# Manager component release

This repository publishes only the immutable `manager.core` component
candidate described by `release/components/manager-core.json`.

The explicitly dispatched `Manager component candidate` workflow builds and
tests the Manager service, CLI, desktop host, broker, and packaged web view,
stages the exact Manager-only install tree, and emits
one deterministic component artifact, provider result, and Sigstore bundle for
the requested target. The candidate is not a suite installer and has no suite
release authority.

`iteathen/uci-arena-installer` exclusively owns product composition, platform
release locks, managed Node/Stockfish candidates, provider transport mirrors,
suite MSI/Burn/Linux packaging, upgrade/repair/removal orchestration, signing,
and suite publication. Manager does not carry a fallback suite workflow or
release lock. This repository must never make another repository public or
weaken provenance to publish `manager.core`.

Local Manager package diagnostics remain available through:

```powershell
node tools/release/clean-build.mjs --manager-only --platform windows-x86_64
node tools/release/clean-build.mjs --manager-only --platform linux-x86_64
```

Those commands produce Manager-Core diagnostic packages only. They do not
assemble Complete Arena, acquire provider artifacts, publish a channel, or
claim that an Installer-owned suite lifecycle has passed.

For a local diagnostic MSI, use
`tools/release/windows-installer-cycle.ps1` to prepare one exact candidate,
perform a single MSI invocation, and run repeatable non-installing smoke checks against that
same installation. The runner preserves pre-existing data by same-volume
rename; `-CandidateSourceCommit` binds an adopted MSI to its real source, and
the explicit `restore -ConfirmRestore` action returns quarantined state without
deleting either tree. The runner never records credential contents. It remains a Manager-package
diagnostic until the equivalent Installer-owned full-suite cycle has passed;
it is not suite release authority.
