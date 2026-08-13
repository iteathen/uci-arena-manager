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
- `arena-broker`: minimal allowlisted privileged service-control boundary
- `web`: React and TypeScript operator dashboard
- `specs`: authoritative product, API, persistence, lifecycle, and UX contracts

The adjacent `arena-setup` executable is produced by the independent
[`uci-arena-installer`](https://github.com/iteathen/uci-arena-installer)
repository. Manager calls it only through the authenticated typed desktop
helper and does not compile or publish a second installer engine.

## Start the installed application

On Windows, open **UCI Arena Manager** from the Start menu. The GUI-subsystem
launcher starts the authenticated interactive session and embeds the packaged
dashboard in its native window without a terminal or default-browser launch.
On Linux, open UCI Arena Manager from the application menu. `arena ui` remains
the optional external-browser monitoring and troubleshooting entrypoint.

The installed CLI is `arena`. Recommended per-user Windows setup adds it to
your user `PATH`; the explicitly elevated all-users package adds it to the
machine `PATH`. Open a new terminal after setup. Linux packages install both
the command and the `arena(1)` man page.

```text
arena status
arena --output json health
arena doctor
arena ui
arena engine list
arena engine probe PATH-TO-ENGINE
arena engine set-model ENGINE-ID --model-root PATH-TO-MODEL-PACKAGE
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

The service binds to `127.0.0.1` only. The recommended Windows per-user package
runs it beneath the native launcher under the interactive account; closing the
desktop host stops that Manager process. The all-users Windows package uses SCM,
and Linux native packages use systemd. See `specs/index.yaml` before changing behavior.

## Local match planning

Register each engine, then create a match or tournament plan using engine IDs.
No engine names are hardcoded.

```powershell
arena engine add --executable C:\engines\stockfish.exe
arena engine add --executable C:\engines\lc0.exe
arena engine add --executable C:\engines\uci_arena_engine.exe
arena engine set-model <uci-arena-engine-id> --model-root C:\models\selected-package
arena match plan --name "LC0 vs Stockfish" --engine <lc0-id> --engine <stockfish-id> --games-per-pair 20 --initial-ms 60000 --increment-ms 600
arena tournament create --name "Local round robin" --format round_robin --engine <stockfish-id> --engine <lc0-id> --engine <uci-arena-engine-id>
```

`engine probe` and `engine add` also accept `--model-root PATH`. Manager sends
the value only through an advertised `ModelRoot` UCI option and repeats the
full probe before a selection is registered. `engine set-model` preserves every
non-model Engine option and retains the selected timing profile as a reusable
operational baseline. Only the marginal relationship between additional search
time and move quality becomes non-exact for the selected model/search pairing.
A materially different model architecture, inference provider, or input/output
contract may also warrant refreshed timing evidence.

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
the snapshot identity, channel, manifest digest, qualification, record count,
and validated artifact root. Evidence integration similarly validates the
public dataset-list contract and projects bounded dataset identities and
authority. Manager never opens either provider's database or rewrites provider
output.

The Services page presents **Use in Engine** beside validated Book Forge
snapshots and qualified Evidence timing profiles. The native setup helper first
previews and commits the Installer-owned locator transaction. Manager then
rediscovers the receipt-managed Engine and repeats its complete UCI and typed
runtime-identity probe before updating the same engine resource by revision. A
failed probe or active competition leaves the prior registration authoritative
and visibly marks the candidate as needing refresh. Provider publication alone
never changes a registered Engine or a running game.

When Evidence is waiting for its first exact target, the Services page offers
**Use UCI Arena Engine** if the receipt-verified recommended engine is already
registered. The browser sends only that engine resource id. Manager repeats
the same universal UCI readiness probe used for every engine, obtains the
runtime model/provider identity from the optional typed UCI info signal, and
combines it with receipt-derived default-model metadata. Evidence validates the
component-relative model root, hashes the engine itself, and creates or reuses
the exact target. Paths, fingerprints, providers, UCI JSON, commands, and argv
are never accepted from the browser. If the preferred engine is not registered,
the prerequisite control links directly to Engines instead of leaving the
operator at a dead end.

Receipt engine discovery resolves a verified `default_model.root` beneath the
immutable component directory and projects it through the engine's advertised
`ModelRoot` UCI option. Registration applies that option before `readyok` and
the legal-bestmove check, then persists it for matches and Evidence probes. A
model label or product name never selects an engine path or parser.

The Bots dashboard installs and detects the packaged Lichess adapter, presents
a masked direct API-key field by default, keeps existing token-file selection
behind **Use an existing key file instead**, and verifies the BOT account
without a terminal. Direct entry is sent only to the authenticated native
helper as a bounded octet-stream body. The helper atomically materializes an
access-restricted per-user file outside Manager data and installed software;
only that file path enters the same reviewed Installer import transaction used
by Configuration. The preceding file is restored if locator commit fails, and
replacement requires provider startup to be disabled. The Manager service only
reads and projects redacted locator availability. The credential probe
runs through the authenticated, ephemeral helper hosted by the desktop
launcher under the interactive user; only the receipt-verified adapter may
open the token file, and only redacted account identity returns to the
dashboard. Readiness is projected separately for the adapter runtime, verified
UCI Arena Engine and default model, opening book, Syzygy, timing, single-game,
capacity and transport evidence, token reference, account probe, and provider
startup; a missing dimension is never collapsed into a generic active state.
Every installation supplies its own operator-owned token file outside the
repository, Manager data, and installed provider component. Installer stores
only the normalized external path, rejects a file placed under Manager-owned
data, and regenerates affected consumer configuration before commit.
The same desktop-helper session provides **Refresh startup status**, **Enable
startup**, and **Disable startup** controls. Enablement repeats the BOT-only
account probe and uses that verified identity; the browser cannot provide an
account name. Status and disable do not open the token file. Closing the
desktop helper removes control authority but does not stop provider-owned
startup that is already enabled.

When an Engine upgrade makes exact single-game or capacity evidence stale,
disable BOT startup and choose **Requalify installed engine** on the Bots page.
The desktop helper invokes the provider's fixed receipt-verified qualification
operation; the dashboard cannot supply an executable, path, command, option, or
credential. Compatible timing evidence remains the timing authority while fresh
complete-game, single-game, and capacity evidence bind the installed Engine.
Successful outputs are imported through adjacent Suite Setup and the dashboard
returns only model, provider, search-core, concurrency, and completion status.

Provider-owned behavior controls on the same page cover qualified connection
capacity, human and bot slots, accepted inbound speeds and casual/rated modes,
base and increment limits, public-human and bot challenge policy, outbound
clock and color, rated play, wait-for-incoming versus actively-seek-games mode,
scheduler timing, opponent limits and cooldowns, owners, reservations, and a
live scheduler pause. Saves cross only the typed Lichess options contract; the
Manager dashboard does not edit provider configuration directly.

## Installer and releases

This repository publishes only the immutable `manager.core` candidate. The
independent `uci-arena-installer` repository owns suite composition, release
locks, platform installers, install/upgrade/repair/removal transactions, and
release-channel advancement.

A fix merged here is not present in the suite until Manager has a new immutable
component version for each supported platform and the Installer repository's
reviewed release lock selects those exact signed artifacts. Installer never
silently substitutes newer source for locked component bytes.

That normal component release changes the Manager artifact, its component
manifest, and the suite composition lock; it does not require a change to the
Installer engine. Installer mechanism changes are reserved for new lifecycle,
transaction, platform, or component-contract behavior.

Complete Arena setup presents UCI Arena Engine, Book Forge, UCI Arena Evidence
Service, and the Lichess Bot adapter as named, selected-by-default components.
The desktop Engines, Services, Bots, and Configuration views can review and
apply a missing signed component from retained release media. The React view
supplies only a fixed component id or asset kind; the native helper invokes the
adjacent verified `arena-setup` and shows dependencies and disk impact. The
recommended per-user package commits in user-owned storage without UAC; the
all-users package asks for administrator confirmation for machine changes. External-browser sessions
copy commands instead and never claim that copying performed installation.
Configuration also offers one bounded **Auto-detect large assets** operation.
The adjacent Installer-owned setup tool queries Windows Search or the Linux
locate database for existing opening books, Syzygy tables, and timing evidence;
it never recursively walks a drive or registers a result. Partial tablebase
coverage and legacy or mismatched timing evidence remain visible with their
observed classification. Exact identity is required only when an operator
reviews activation for a runtime or relies on a protected-live safety claim.
On Windows, reopen **UCI Arena Suite Setup (per-user)** or **UCI Arena Suite
Setup (all users)** from Apps and Features and choose
**Modify**, then **Choose Components**, to add or remove them. Feature choices
are persisted across upgrades. The Lichess adapter is installed disabled until
its token-file reference and evidence gates are explicitly completed.

Component changes are executed by `arena-setup`, not by direct file deletion.
Before repair or upgrade replaces an active component root, the installer uses
that receipt's verified public startup adapter to stop and unregister the
affected provider, then waits a bounded time for operating-system process
teardown. It stops Manager for the bounded receipt/configuration commit, applies
or removes verified payloads, then starts Manager and verifies the resulting
state. A failed transaction restores the prior payload, receipt, and generated
configuration before reinstalling its previous startup registration; an
incomplete startup restoration is reported as `rollback_failed`. Linux DEB/RPM
lifecycle scripts perform the equivalent systemd stop, daemon-reload, enable,
and start; portable `.run` installers use the same `arena-setup` operations and
catalog.

Evidence startup activation independently verifies registration presence,
agreement with the current package/workspace/configuration, and a live daemon.
Its install path first drains any previous registration and exact lock-owning
daemon, then starts and verifies the replacement. This provider-owned check is
what closes the short Task Scheduler/Job Object teardown race during an upgrade;
Manager does not infer activity from a retained status file.

When a saved Lichess token-file reference is available, Book Forge activation
uses only its public `acquisition retry-auth` response. A transient `blocked`
state is retried at most three times with a short bounded delay; malformed or
credential-missing responses fail immediately, and the transaction restores
the prior payload and startup state. Failure text identifies only the rejected
public field and never includes the token path or token contents.
For an all-users Windows or native Linux installation, `arena-setup` grants
only the Manager service account inherited access to each generated Book Forge,
Evidence Service, or Lichess workspace: `LOCAL SERVICE` on Windows or
`uci-arena` through the packaged `acl` dependency on Linux. A per-user Windows
installation needs no identity grant because Manager and providers run as that
same user. Provider ownership is unchanged, and token files or other
operator-profile paths are never included in a permission grant.

Windows has two fixed-scope installer sets because the project stays on the
free WiX 6 toolchain. Both packages use one tested native binary build; the MSI
installs a scope marker and the owning bundle passes that same scope explicitly
to setup, preventing a user update from drifting into machine-owned resources:

- `...-windows-x64-user-*.exe` is recommended. Install, Modify, repair, update,
  and uninstall use `%LOCALAPPDATA%`, user PATH, and the interactive account, so
  they do not request UAC.
- `...-windows-x64-machine-*.exe` installs for all users in Program Files,
  writes machine PATH, and owns a LocalService Windows Service. Those operations
  legitimately require UAC.

Signing can improve publisher and SmartScreen trust, but it cannot suppress a
legitimate all-users UAC prompt. Setup never uses an elevation bypass. Updating
preserves the installed scope; moving between scopes requires an explicit
uninstall and install so two Manager authorities cannot compete for the same
loopback port.

The installed Installer-owned `arena-setup` supports `inventory`, `plan`, `apply`, `verify`, `repair`,
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
`%LOCALAPPDATA%\UCI Arena Manager` for per-user setup or
`C:\ProgramData\UCI Arena Manager` for all-users setup. This intentionally retains the database,
local client key, knowledge assets, saved locators, verified repair media,
rollback-protection state, content cache, and transaction evidence. Managed
component roots, startup registrations, receipt documents, the Windows
service, and MSI-owned program files are removed. The exact retention contract
and reversible clean-install test procedure are owned by
`uci-arena-installer`; Manager's read-only consumption and component
publication boundary is documented in
`specs/manager_component_release_v1.yaml` and `release/README.md`.

## License

AGPL-3.0-or-later.
