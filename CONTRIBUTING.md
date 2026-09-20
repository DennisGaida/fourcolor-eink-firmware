# Contributing

## Commit Messages

This repo follows [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<optional scope>): <short summary>

<optional body>

<optional footer(s)>
```

Common types: `feat`, `fix`, `perf`, `docs`, `chore`, `refactor`, `test`,
`ci`, `build`, `style`. Mark a breaking change with a `!` after the
type/scope (`feat!: ...` or `feat(api)!: ...`) or a `BREAKING CHANGE:`
footer.

This isn't just a style preference - the presence/calendar bridge's
Docker image version is computed automatically from commits touching
`server/` (see `.github/workflows/build-bridge.yml`): `feat` bumps
minor, `fix`/`perf` bump patch, and a breaking-change marker bumps
major. Non-conforming commit messages under `server/` are silently
treated as no-ops for versioning, so use the right type prefix when
touching bridge code.

## Setup

There's no repo-level package manager step - see the root
[README.md](README.md) for firmware build instructions (ESP-IDF) and for
running the presence/calendar bridge scripts under `server/`.

## Secrets

See [SECURITY.md](SECURITY.md) for what must never be committed and what
network surfaces the device/bridge expose.
