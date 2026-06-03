# Security Policy

TaterTOS64v3 is an experimental operating system under active development. It is
not yet intended for production use, but security reports are welcome and taken
seriously.

## Reporting a vulnerability

Please report security issues **privately** rather than opening a public issue.

- Email: **legacyindiesubmissions@gmail.com** with a subject beginning
  `[TaterTOS security]`.
- Include a description, affected component(s), and reproduction steps if
  possible.

You can expect an initial acknowledgement within a reasonable time. Coordinated
disclosure is appreciated: please give us an opportunity to address the issue
before any public disclosure.

## Scope

Reports about the TaterTOS kernel, drivers, networking/TLS stack, filesystems,
the Linux compatibility layer, or the bundled userspace are in scope. Issues in
third-party vendored libraries (under `src/user/ports/` and
`src/user/apps/*/vendor/`) should generally be reported upstream as well.
