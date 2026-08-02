# Contributing to Larzscript

There are a few different things in this repo — pick the one that matches
what you want to change.

## The language itself (`native/`)

The native interpreter is one file, `native/larzscript.c`, no build system
beyond a C compiler:

```bash
cc -O2 -o larzscript native/larzscript.c
sh native/run_tests.sh          # fixture-based: a .lz file + a .expected file, diffed
```

Add a test by dropping a new `native/tests/yourfeature.lz` +
`native/tests/yourfeature.expected` pair — `run_tests.sh` picks it up
automatically, no registration needed. For a language-level change (new
syntax, a new builtin), update [`native/LANGUAGE.md`](native/LANGUAGE.md) in
the same PR — an undocumented feature isn't done yet.

Every commit to `main` runs the same test suite in CI
(`.github/workflows/native.yml`), and a `native-v*` tag triggers a full
release build across Linux (x86_64/aarch64), Windows and web — so a change
that passes locally gets re-verified for real before it ships.

## A package (`packages/`)

You don't need write access to this repo to publish a package — host it
yourself, keep your own version control, and publish a one-line registry
entry. See [`packages/PUBLISHING.md`](packages/PUBLISHING.md) for the exact
steps. If you *do* want a package to live in this repo's own monorepo
(`packages/<name>/main.lz`), open a PR the normal way — include a real,
tested "Try it" example in the package's own docstring, not illustrative
pseudocode (every package here follows that rule; verify your example
actually runs before submitting).

## LarzOS (`os/`, `kernel/`)

The from-scratch operating system has its own contributing guide, since it
has its own verification discipline (real QEMU screendumps for anything
touching graphics/hardware behavior, not just "it compiled"):
[`kernel/CONTRIBUTING.md`](kernel/CONTRIBUTING.md).

## Reporting a bug

Open an issue with: the exact `.lz` snippet that reproduces it, what you
expected, what actually happened, and `larzscript --version` (or the exact
commit if built from source). A minimal reproduction saves everyone time —
if you can shrink it, shrink it.

## General principles

- **Real tests over reasoning.** This project's whole discipline is "run it
  for real, don't assume" — every example in the README and every package's
  docstring was actually executed before being committed. Hold new
  contributions to the same bar.
- **Zero dependencies stays zero dependencies.** The native binary has no
  runtime deps by design; packages that need an external tool shell out to
  something genuinely standard (`curl`, `sha256sum`, `flock`) rather than
  requiring a new install step.
- **Small, focused PRs.** One feature or one package per PR is easier to
  review and easier to revert if something's wrong.
