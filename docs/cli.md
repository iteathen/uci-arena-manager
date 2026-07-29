# UCI Arena command line

Windows setup adds the installed `bin` directory to the machine `PATH`. Open a
new terminal after installation and run:

```text
arena status
arena --output json health
arena doctor
arena ui
```

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
