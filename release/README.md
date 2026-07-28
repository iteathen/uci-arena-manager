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
