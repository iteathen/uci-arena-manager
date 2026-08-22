# UCI Arena Manager Agent Contract

UCI Arena Manager is the installed local control-plane product: service, CLI, native desktop host, web dashboard, privileged broker boundary, persistent manager state, provider adapters, and operator-facing orchestration. It does not own Engine or provider behavior.

## Required start

1. Read `.agents/core-standard.md`.
2. Before substantive planning, inspection, implementation, or validation run:

```text
node tools/spec-preflight.mjs --task "<description>" --changed-files <likely paths>
```

3. Read every active spec returned by preflight and rerun when touched paths change materially.
4. For timing-policy, timing-profile, transport-profile, bot readiness, service output, or move-time UI/projection work, treat `specs/move_time_projection_boundary_v1.yaml` as mandatory ownership authority.
5. For repository-boundary, provider-registration, broker, packaging, or cross-project contract work, also read `specs/suite_lego_boundaries_v1.yaml` and the canonical suite architecture in `iteathen/uci-arena-installer`.

Do not broadly preload historical provider integrations, extracted implementation paths, old plans, or unrelated specs.

## Authority and reuse

Current Manager specs/contracts own Manager behavior. The suite architecture owns cross-repository responsibility. Historical source layouts and provider implementation details are not authority.

Before creating a new service path, provider adapter, broker action, state store, UI-side authority, process controller, timing projection, or contract, search the current Manager owners/specs first. Reuse or extend the existing owner and keep one control truth.

## Architecture invariants

- Domain code has no platform, SQL, HTTP, UI, or process dependencies.
- The Manager service is the only Manager runtime state authority.
- CLI, dashboard, scripts, desktop host, and automations use the same typed control contract rather than parallel logic.
- Engines and provider products are external adapters; never import another repository's private source.
- Canonical chess state validates every engine move used by Manager-owned match/control workflows.
- Raw engine/provider output cannot block control traffic or become semantic authority.
- The main service remains unprivileged.
- The privileged broker accepts only allowlisted typed actions and never arbitrary commands, shell strings, or unconstrained paths.
- Node.js is a build/test dependency, not a production runtime dependency.
- Secrets are never logged, returned, committed, or placed in argv. Pass only the minimum owned credential reference required by the provider contract.
- Prefer complete vertical slices through the existing ownership model over disconnected scaffolding or duplicate control surfaces.

Use LEGO boundaries: universal typed contracts outside, direct purpose-built implementation inside. Framework/provider/private storage types stay behind adapters.

## Move-time projection hard rule

Manager projects move-time state; it does not calculate or qualify move time.

- Engine/conforming runtime owns current-game chess-time application and move-time applicability.
- Evidence Service owns production/qualification of timing-policy artifacts, including evidence-backed minimum useful blocks and allocation-strategy authority.
- Lichess Bot owns transport/API timing and transport reserve.
- Manager must keep these as distinct typed status/readiness dimensions and must not merge them into one generic timing authority.
- Never compute per-move milliseconds from clocks, increment, useful blocks, policy parameters, or transport reserves.
- Never inspect timing population evidence or strategy coefficients to recreate the owner's qualification verdict.
- Do not expose raw strategy parameters as arbitrary editable controls merely because they are JSON.
- Policy selection, qualification, activation, or rollback actions invoke the owning typed provider/Installer operation when such an operation exists; Manager does not implement the semantic action itself.
- Unknown timing-policy contract versions fail/project through the declared compatibility boundary rather than being guessed from filenames or field similarity.

## Reasoning, debugging, and validation

Use high reasoning for runtime, protocol, concurrency, security, persistence, installation/service control, match execution, broker behavior, and public contract changes. Localized low-blast-radius UI work may use medium reasoning; factual inspection/formatting may use low.

For ambiguous failures, preserve the original failure signature and trace raw expected-vs-actual evidence through the owning boundary before editing. Do not accept UI appearance or a passing projection test as proof that service truth is correct.

Run the smallest complete preflight-selected owner/contract validation locally before broad CI. Security, persistence, broker, public compatibility, move-time authority projection, and recovery-sensitive changes retain their stronger safeguards regardless of diff size.

## Execution and integration

Code changes modify tracked source, include proportionate tests/evidence, are committed and pushed on a branch, and are opened as a draft PR. Do not stop at plans or pseudo-code when real implementation is requested.

Before integration, inspect the complete diff, test the simplest credible failure, freeze the intended head, use exact-head guarded merge where supported, and verify target integration.

## Completion

Complete when the requested Manager outcome is evidenced, Manager remains the sole local control truth, timing semantics remain with their owning Engine/Evidence/provider components, provider behavior stays external, security/privilege boundaries remain intact, and no material invalid partial state remains.
