# Repository context: uci-arena-manager

Universal engineering and design guidance comes from the account-global `AGENTS.md`.

## Mission and ownership

UCI Arena Manager is the installed local control-plane product: service, CLI, native desktop host, web dashboard, privileged broker boundary, persistent Manager state, provider adapters, and operator-facing orchestration. It does not own Engine or provider behavior.

## Decision reasoning

Before a substantive architecture, design, implementation-selection, or cross-component
decision, read `reasoning/component_graph_v1.yaml` plus the canonical suite methodology,
concern census, and suite graph at
`iteathen/uci-arena-installer/reasoning/README.md`,
`iteathen/uci-arena-installer/reasoning/concern_census_v1.yaml`, and
`iteathen/uci-arena-installer/reasoning/suite_graph_v1.yaml`.

The local graph owns Manager reasoning only; cross-component assumptions belong in the suite
graph. Accepted Manager specs/contracts remain behavior authority. A graph/spec contradiction
requires review instead of silently changing either one.

## Local routing

Before substantive work run:

```text
node tools/spec-preflight.mjs --task "<description>" --changed-files <likely paths>
```

Read the active specs returned by preflight and rerun it when touched paths change materially. Repository-boundary/provider/broker/packaging work also follows `specs/suite_lego_boundaries_v1.yaml` and the canonical suite architecture in `iteathen/uci-arena-installer`.

Timing-policy/profile projections follow `specs/move_time_projection_boundary_v1.yaml`.
Manager may forward actual consumer clocks through declared UCI/profile bindings;
timing-policy calculations and qualification remain with their existing owners.
Current probe/competition admission remains in `specs/engine_onboarding_v1.yaml`.

## Local constraints

Current Manager specs/contracts own Manager behavior. Keep one control truth for service paths, provider registration, broker actions, persistent state, UI authority, and process control; cross-repository exchange uses published contracts rather than private source.
