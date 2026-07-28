# AGENTS.md

## Project

UCI Arena Manager is a standalone, spec-driven C++ control plane with an
embedded web dashboard and native CLI.

## Required reasoning

- Use high reasoning for runtime, protocol, concurrency, security, storage,
  installation, service control, match execution, and public contract changes.
- Use medium reasoning for localized UI and low-blast-radius implementation.
- Use low reasoning only for factual inspection or formatting.

## Mandatory spec preflight

Before substantive planning, inspection, implementation, or validation, run:

```text
node tools/spec-preflight.mjs --task "<description>" --changed-files <paths>
```

Read every returned spec. If behavior changes, update the governing spec first.

## Architecture invariants

- Domain code has no platform, SQL, HTTP, UI, or process dependencies.
- The service is the only runtime state authority.
- CLI, dashboard, scripts, and automations use the same control contract.
- Engines and bots are external adapters; never import another repository.
- Canonical chess state validates every engine move.
- Raw engine output cannot block UCI control traffic.
- The main service remains unprivileged.
- The broker accepts only allowlisted typed actions and never arbitrary
  commands, paths, or shell strings.
- Node.js is a build/test dependency, not a production runtime dependency.
- Secrets are never logged, returned, placed in argv, or committed.
- Prefer small, complete vertical slices over disconnected scaffolding.

## Execution

Code changes must modify tracked source, include proportionate tests, be
committed, pushed, and opened as a draft pull request.
