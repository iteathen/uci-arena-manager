# UCI Arena Manager

UCI Arena Manager is a standalone, cross-platform control plane for local UCI
engines, chess bots, services, matches, tournaments, game history, logs,
health, statistics, and reports.

The product is intentionally independent of every engine or training
repository. Engines such as UCI Arena Engine, LC0, and Stockfish are registered as
ordinary UCI executables with their own working directory, options, resource
limits, and identity fingerprint.

## Components

- `arena-service`: authoritative C++ service and local HTTP/WebSocket API
- `arena`: native CLI using the same public control contract
- `arena-broker`: minimal allowlisted privileged service-control boundary
- `web`: React and TypeScript operator dashboard
- `specs`: authoritative product, API, persistence, lifecycle, and UX contracts

## Development

Prerequisites:

- CMake 3.28 or newer
- A C++20 compiler
- vcpkg with `VCPKG_ROOT` configured
- Node.js 22 or newer for dashboard development

Configure and build:

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
npm ci --prefix web
npm run build --prefix web
```

Run locally:

```powershell
.\run-local.cmd
```

The launcher resolves every path from the repository root, starts or reuses the
local service, waits for readiness, and opens the paired dashboard. It can be
invoked from any working directory. Use `.\run-local.cmd -NoOpen` to print the
pairing URL without opening a browser, and `.\run-local.cmd -Stop` to stop the
service instance started by the launcher.

The service binds to `127.0.0.1` only. Production packages install it through
Windows SCM or systemd. See `specs/index.yaml` before changing behavior.

## Local match planning

Register each engine, then create a match or tournament plan using engine IDs.
No engine names are hardcoded.

```powershell
arena engine add --name Stockfish --executable C:\engines\stockfish.exe
arena engine add --name LC0 --executable C:\engines\lc0.exe
arena engine add --name "UCI Arena Engine" --executable C:\engines\uci_arena_engine.exe
arena match plan --name "LC0 vs Stockfish" --engine <lc0-id> --engine <stockfish-id> --games-per-pair 20 --initial-ms 60000 --increment-ms 600
arena tournament create --name "Local round robin" --format round_robin --engine <stockfish-id> --engine <lc0-id> --engine <uci-arena-engine-id>
```

## External services

External systems remain independent and cross only declared process, HTTP,
health, and artifact contracts. Book Forge and UCI Arena Evidence Service are
both optional; omitting them never degrades local engine, match, tournament,
CLI, or dashboard operation.

The Services dashboard offers auto-detection from non-secret local installation
hints and always keeps manual path or endpoint fields available. Book Forge can
also be configured through its public CLI without importing its source or
storage:

```powershell
arena service discover book_forge
arena service add-book-forge --node "C:\Program Files\nodejs\node.exe" --cli C:\path\to\book_forge\src\cli.mjs --workspace E:\book-forge
```

The UCI Arena Evidence Service is an implemented optional Node service. Point
the manager at its public CLI and workspace; the manager validates and invokes
that CLI without importing its source or reading its catalog:

```powershell
arena service discover uci_arena_evidence
arena service add-evidence-service --node "C:\Program Files\nodejs\node.exe" --cli C:\path\to\arena_evidence_service\src\cli.mjs --workspace E:\uci-arena-evidence
```

Installed Book Forge and Evidence Service components are reconciled from
`arena-setup` receipts. The receipt and complete provider/private-runtime file
manifests must match before Manager registers either service. A status refresh
validates every
advertised Book Forge snapshot through `book-forge validate` and projects only
the snapshot identity, channel, manifest digest, qualification, and record
count. Evidence integration similarly validates the public dataset-list
contract and projects bounded dataset identities and authority. Manager never
opens either provider's database or rewrites provider output.

## Installer and releases

This repository is the suite composition and release authority. Provider
repositories build immutable component candidates; they cannot publish suite
installers or advance release channels.

Installer sources and official output locations are:

- Windows: `packaging/windows` → `dist/windows`
- Linux: `packaging/linux` → `dist/linux`

`arena-setup` supports `inventory`, `plan`, `apply`, `verify`, `repair`,
`upgrade`, and `uninstall`; every command supports `--json`. It detects
Restaurant but never installs or changes it. To review signed catalog actions
for missing chess knowledge:

```powershell
arena-setup plan --preset manager_only --download-missing-books
arena-setup plan --preset manager_only --download-missing-tablebases
arena-setup plan --preset manager_only --download-missing-knowledge
```

The first two options are independently selectable. The combined option adds
the standard opening-book and bounded Syzygy WDL components. Normal uninstall
preserves these assets; explicit purge removes only receipt-declared files that
still match their installed digest.

## License

AGPL-3.0-or-later.
