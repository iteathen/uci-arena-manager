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

Local validation stops at the same scope-neutral Manager component tree and
component-artifact contract. MSI, DEB, RPM, portable launcher, scope selection,
install-cycle, and suite acceptance tooling live only in the Installer
repository and consume the immutable candidate rather than this source tree.
