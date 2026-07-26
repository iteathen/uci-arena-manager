# Security policy

Report vulnerabilities privately through GitHub Security Advisories.

The manager binds to loopback in v1. The web-facing service runs without
administrator privileges. Operating-system service mutations are delegated to
the local broker, which accepts only administrator-owned allowlisted resource
identities and typed actions.

Never submit credentials, Lichess tokens, private keys, database copies, or
diagnostic bundles containing secrets in an issue. Secret values are write-only
through the API and are redacted from logs, events, reports, and exports.
