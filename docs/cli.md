# UCI Arena command line

Recommended per-user Windows setup adds the installed `bin` directory to the
user `PATH`; all-users setup adds it to the machine `PATH`. Open a new terminal
after installation and run:

```text
arena status
arena --output json health
arena doctor
arena ui
```

Normal desktop startup does not require this command: open **UCI Arena
Manager** from the Windows Start menu or Linux application menu. `arena ui`
opens the same paired view in an external browser for monitoring and
troubleshooting; native setup and account-control authority remain in the
desktop host.

With per-user setup, the desktop host starts Manager under your account and
stops it when the native window closes. CLI commands therefore use that active
desktop session. All-users setup and Linux native packages keep Manager
available through SCM or systemd. Windows entry points read the MSI-owned
`share/uci-arena/install-scope.txt` marker; a missing or invalid marker fails
safely to machine scope instead of silently assuming a non-elevated install.

Global flags must precede the command:

- `--endpoint URL` selects the loopback Manager endpoint.
- `--data-dir PATH` selects the Manager data root containing `client.key`.
- `--output human|json|jsonl` selects output formatting.
- `--help` shows context-sensitive help; for example, `arena engine --help`.

Common engine and service commands:

```text
arena engine list
arena engine probe C:\engines\engine.exe
arena engine add --executable C:\engines\engine.exe
arena service profiles
arena service discover book_forge
arena daemon status
```

On Linux, the same reference is installed as `arena(1)`:

```text
man arena
```
