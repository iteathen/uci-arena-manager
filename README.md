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
- `uci-arena-manager`: thin native desktop host for the packaged React view
- `arena-setup`: typed, provenance-verifying component installer
- `arena-broker`: minimal allowlisted privileged service-control boundary
- `web`: React and TypeScript operator dashboard
- `specs`: authoritative product, API, persistence, lifecycle, and UX contracts

## Start the installed application

On Windows, open **UCI Arena Manager** from the Start menu. The GUI-subsystem
launcher starts the authenticated interactive session and embeds the packaged
dashboard in its native window without a terminal or default-browser launch.
On Linux, open UCI Arena Manager from the application menu. `arena ui` remains
the optional external-browser monitoring and troubleshooting entrypoint.

The installed CLI is `arena`. Windows setup adds it to the machine `PATH`;
open a new terminal after setup. Linux packages install both the command and
the `arena(1)` man page.

```text
arena status
arena --output json health
arena doctor
arena ui
arena engine list
arena engine probe PATH-TO-ENGINE
arena service profiles
```

Global flags precede the command:

- `--endpoint URL` selects the loopback service endpoint.
- `--data-dir PATH` selects the Manager data root.
- `--output human|json|jsonl` controls output formatting.
- `--help` is context-sensitive; for example, `arena engine --help`.

See `docs/cli.md` or run `man arena` on Linux for the complete quick reference.

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
arena engine add --executable C:\engines\stockfish.exe
arena engine add --executable C:\engines\lc0.exe
arena engine add --executable C:\engines\uci_arena_engine.exe
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

The Bots dashboard installs and detects the packaged Lichess adapter, lets the
operator select an existing token file, and verifies the BOT account without a
terminal. Manager saves only normalized path metadata. The credential probe
runs through the authenticated, ephemeral helper hosted by the desktop
launcher under the interactive user; only the receipt-verified adapter may
open the token file, and only redacted account identity returns to the
dashboard. Readiness is projected separately for the adapter runtime, verified
UCI Arena Engine and default model, opening book, Syzygy, timing, single-game,
capacity and transport evidence, token reference, account probe, and provider
startup; a missing dimension is never collapsed into a generic active state.
Every installation supplies its own operator-owned token file outside the
repository, Manager data, and installed provider component. Manager stores only
the normalized external path and rejects a file placed under Manager-owned data.
The same desktop-helper session provides **Refresh startup status**, **Enable
startup**, and **Disable startup** controls. Enablement repeats the BOT-only
account probe and uses that verified identity; the browser cannot provide an
account name. Status and disable do not open the token file. Closing the
desktop helper removes control authority but does not stop provider-owned
startup that is already enabled.

## Installer and releases

This repository is the suite composition and release authority. Provider
repositories build immutable component candidates; they cannot publish suite
installers or advance release channels.

Complete Arena setup presents UCI Arena Engine, Book Forge, UCI Arena Evidence
Service, and the Lichess Bot adapter as named, selected-by-default components.
The desktop Engines, Services, Bots, and Configuration views can review and
apply a missing signed component from retained release media. The React view
supplies only a fixed component id or asset kind; the native helper invokes the
adjacent verified `arena-setup`, shows dependencies and disk impact, and asks
for administrator confirmation only for the commit. External-browser sessions
copy commands instead and never claim that copying performed installation.
On Windows, reopen **UCI Arena Suite Setup** from Apps and Features and choose
**Modify**, then **Choose Components**, to add or remove them. Feature choices
are persisted across upgrades. The Lichess adapter is installed disabled until
its token-file reference and evidence gates are explicitly completed.

Component changes are executed by `arena-setup`, not by direct file deletion.
The installer stops affected provider startup registrations, stops Manager for
the bounded receipt/configuration commit, applies or removes verified payloads,
then starts Manager and verifies the resulting state. Linux DEB/RPM lifecycle
scripts perform the equivalent systemd stop, daemon-reload, enable, and start;
portable `.run` installers use the same `arena-setup` operations and catalog.

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

On Windows, normal uninstall also preserves the Manager data root at
`C:\ProgramData\UCI Arena Manager`. This intentionally retains the database,
local client key, knowledge assets, saved locators, verified repair media,
rollback-protection state, content cache, and transaction evidence. Managed
component roots, startup registrations, receipt documents, the Windows
service, and MSI-owned program files are removed. The exact retention contract
and the reversible clean-install test procedure are documented in
`specs/installation_contract_v1.yaml` and `release/README.md`.

## License

AGPL-3.0-or-later.
