# Publishing a Larzscript package

The registry (`registry.txt` in this directory) is an **index**, not a
hosting requirement. You never need write access to any repo you don't
already own to publish a package - you host it, you version it, you keep
the issues.

## Writing one

A package is just a `main.lz` file: plain top-level `let`/`fn` declarations,
no wrapper boilerplate. Whatever you declare at the top level is what
`import` exposes:

```
# main.lz
let PI = 3.14159265358979
fn mean(xs) { return sum(xs) / len(xs) }
fn fib(n) {
  if n < 2 { return n }
  let a = 0
  let b = 1
  for i in range(n - 1) { let t = a + b; a = b; b = t }
  return b
}
```

```
import "mathx" as m
print(m.mean([1, 2, 3, 4]), m.fib(10))
```

Test it locally before publishing anything, straight from your working
directory - no install step needed to try it out:

```
import "./my-package/main.lz" as m
print(m.whatever())
```

## Publishing

**Option A - your own repo (recommended for everyone but the core team).**
Push a repo with `main.lz` at its root (a `README.md` is nice too - see
[`lz-example-external`](https://github.com/larz-scripter/lz-example-external)
for a minimal real one). Then:

```bash
larzscript tools/larzpkg.lz publish https://github.com/you/lz-yourpackage
```

This checks the repo is reachable and shaped like a package, then prints
the exact line to add to `registry.txt` and a link that lets you fork, edit,
and open the PR right in GitHub's browser UI - no `gh` CLI, no extra
tooling. That one-line PR is the entire review surface; your code stays in
your repo, under your control, versioned however you like.

Two things worth setting on your own repo once you have real users:
- **Pin a version** by tagging a release and listing it with `@tag` in the
  registry line: `yourpackage  https://github.com/you/lz-yourpackage@v1.0.0`
  - otherwise `larzpkg install`/`update` always tracks your default branch.
- If `main.lz` isn't at the repo root, add `#subpath` to the URL:
  `yourpackage  https://github.com/you/lz-yourpackage#path/to/pkg`.

**Option B - the official monorepo**
([`larz-scripter/larzscript-packages`](https://github.com/larz-scripter/larzscript-packages)).
For packages the core project wants to maintain directly. Open a PR there
adding `packages/<name>/main.lz`, then a second PR here listing
`<name>  packages/<name>`. Higher bar (core-team review, shared
maintenance), so reserve it for packages that belong in the standard
library, not every community package.

## What about LarzOS?

This registry is for the native/hosted interpreter (Linux/Windows/macOS,
and the browser build) - anywhere with real internet access. LarzOS, the
bare-metal kernel, can't use it: `kernel/net.c` has no TLS at all, so it
can't reach GitHub the way `larzpkg install`/`publish` do here. Contributing
to LarzOS itself (a `pkg`-installable command, or a GUI app using the `ui`
module) is a real, open, git-native path too - just a PR against the kernel
source directly rather than a one-line registry entry. See
[`kernel/CONTRIBUTING.md`](../kernel/CONTRIBUTING.md).

## Removing or renaming

Nothing to coordinate beyond the registry line - it's just a pointer, not a
copy. Rename your repo, retag a version, or delete the line here; existing
installs on other people's machines are unaffected until they run `update`.
