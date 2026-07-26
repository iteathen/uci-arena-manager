# UCI Arena Manager

UCI Arena Manager is a standalone, cross-platform control plane for local UCI
engines, chess bots, services, matches, tournaments, game history, logs,
health, statistics, and reports.

The product is intentionally independent of every engine or training
repository. Engines such as LC0, Stockfish, and LatticeKnight are registered as
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
arena engine add --name LatticeKnight --executable C:\engines\latticeknight.exe
arena match plan --name "LC0 vs Stockfish" --engine <lc0-id> --engine <stockfish-id> --games-per-pair 20 --initial-ms 60000 --increment-ms 600
arena tournament create --name "Local round robin" --format round_robin --engine <stockfish-id> --engine <lc0-id> --engine <latticeknight-id>
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

The UCI Arena Evidence Service can be tracked while its independent
implementation is in development. It remains visibly unavailable until an
endpoint satisfies the versioned capability and immutable snapshot contract:

```powershell
arena service discover uci_arena_evidence
arena service track-evidence-service --service-endpoint http://127.0.0.1:<port>
```

## License

AGPL-3.0-or-later.
