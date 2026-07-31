# Contributing to LarzOS

Two shapes of thing you can add: a `pkg`-installable command, or a GUI app.
Both are plain Larzscript - no C, no kernel rebuild knowledge needed beyond
running `make` (see [`README.md`](README.md#build--run)) to try your own
change.

**The honest difference from [Larzscript packages](../packages/PUBLISHING.md)**:
`kernel/net.c` has no TLS, so LarzOS can't dynamically fetch anything from
GitHub the way `larzpkg install`/`publish` now lets native Larzscript do -
there's no gateway service for it (yet - see `packages/PUBLISHING.md`'s note
on this). Contributing to LarzOS today means a real git PR against this
repo, reviewed and merged like any other open-source change - the same bar
as contributing to the interpreter or kernel source itself, just without
the "one command, no review" dynamism native packages just got. Still a
completely open, standard path - no special access beyond a GitHub account.

## A `pkg`-installable command

Like the existing `banner`/`figlet`/`todo`/`strutil` (`kernel/rootfs/repo/`).
These are baked into the kernel image at build time (`mkramfs.py` bundles
`kernel/rootfs/` into the boot RAM filesystem), so there's no install step
at runtime beyond `pkg install <name>` copying it from `/repo` into `/home`
(see `kernel/rootfs/pkg.lz`).

```
# kernel/rootfs/repo/yourcommand/main.lz
fn run(args) {
  print("hello from yourcommand, args: " + join(args, " "))
}
```

```
# kernel/rootfs/repo/yourcommand/meta  (one line, shown by `pkg list`)
does a useful thing
```

Test it by booting the shell target and running `pkg list` / `pkg install
yourcommand` / `yourcommand some args` (`make EXTRA=-DLARZ_SHELL iso`, then
`make run` or `make test`). Open a PR adding the directory.

## A GUI app

Uses the `ui` module (`ui.label`/`ui.button`/`ui.on`/`ui.terminal`/`ui.run`/
`ui.quit`, documented alongside the primitives themselves in
[`gfx.h`](gfx.h)) - real VGA graphics, keyboard-driven (Tab focus, Enter
click), the identical vocabulary the [browser build](../native/WEB.md)
uses, just backed by `gfx.c` instead of a DOM. `kernel/rootfs/gui.lz` is the
worked example - a wallet, a `pay`-driven button, a `capability`-gated
action, and a `ui.terminal()` log pane that plain `print()` calls
automatically show up in (see the "A GUI on bare metal" section of
[`README.md`](README.md)).

Wiring one up today needs a new boot target - a `#elif defined(LARZ_YOURTHING)`
branch in `kernel.c` dispatching to your `.lz` file, matching the
existing `LARZ_GUI`/`LARZ_SHELL`/`LARZ_INIT` pattern exactly. (Launching a
GUI app on demand from `larzsh`, without a dedicated boot target, isn't
built yet - see `README.md`'s "Next" section.)

## Verifying a change actually works

This project doesn't take "it compiled" as proof for anything touching
`gfx.c`/hardware behavior - every graphics/widget change this far was
verified with a real, headless QEMU screendump (and `sendkey` for
interaction), not just reasoning about the code. See "A GUI on bare metal"
in `README.md` for the exact commands. For anything else, `make test` /
`make run` against the relevant `EXTRA=-DLARZ_*` boot target is enough.

## Regressions

Whatever you change, confirm the *other* boot targets still build and
behave the same (`make EXTRA=-DLARZ_SHELL` etc. still `make test`/`make
run` cleanly) - several real bugs this project shipped were caught exactly
this way, not by the new feature's own test.
