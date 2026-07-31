# Contributing to LarzOS

Two shapes of thing you can add: a `pkg`-installable command, or a desktop
app (its own window on the real windowed desktop). Both are plain
Larzscript - no C, no kernel rebuild knowledge needed beyond running `make`
(see [`README.md`](README.md#build--run)) to try your own change. A desktop
app does need one small C task-wrapper today (see below) - that's the one
place this still isn't pure Larzscript, and it's the natural next thing to
fix (see `README.md`'s "Next").

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

## A desktop app

Runs as its own real task, in its own window, on the windowed desktop
(`EXTRA=-DLARZ_DESKTOP` - see "A real windowed desktop" in
[`README.md`](README.md)). Uses the `ui` module (`ui.label`/`ui.button`/
`ui.on`/`ui.terminal`/`ui.set_text`, documented alongside the primitives
themselves in [`gfx.h`](gfx.h)) - the identical vocabulary the [browser
build](../native/WEB.md) uses, just backed by `gfx.c`'s window compositor
instead of a DOM. `kernel/rootfs/clock.lz` is the minimal worked example (a
label updated once a second); `kernel/rootfs/larzsh.lz` (running as the
Terminal window) is the fuller one.

Two pieces, matching the existing `task_terminal`/`task_clock` pattern in
`kernel.c` exactly:

```c
/* kernel.c - one small C wrapper: create the window + starting widget(s),
 * positioned inside the window's own client rect, then hand off to your
 * .lz file. This is the one part that isn't yet pure Larzscript - see
 * README.md's "Next" (widgets aren't window-clipped, so today a human
 * picks the coordinates by hand instead of the window system doing it). */
static void task_yourthing(void){
    int idx = gfx_window_create("Your Thing", 400, 40, 300, 200);
    int cx, cy, cw, ch;
    gfx_window_client_rect(idx, &cx, &cy, &cw, &ch);
    gfx_widget_label("yourthing_status", cx+4, cy+4, "starting...");
    char *a[] = { "larzscript", "/yourthing.lz", 0 };
    larz_main(2, a);
    for(;;) __asm__ volatile("hlt");
}
```

```
# kernel/rootfs/yourthing.lz - touches the widget only by id, via ui.*;
# never needs to know its own screen coordinates.
let n = 0
while true {
  n = n + 1
  ui.set_text("yourthing_status", "tick " + str(n))
  sleep(1)
}
```

Then add `task_create(task_yourthing);` alongside the existing
`task_create(task_terminal); task_create(task_clock);` calls in the
`LARZ_DESKTOP` branch of `kernel_main` (`kernel.c`) - within `NTASK`'s
budget (`libk.c`; raise it, and its matching `TSTK` RAM-budget math, if
you're adding enough apps to need more slots). Launching an app **on
demand** (from the taskbar or the shell, rather than every app being wired
in at compile time) isn't built yet - see `README.md`'s "Next" section.

## Verifying a change actually works

This project doesn't take "it compiled" as proof for anything touching
`gfx.c`/hardware behavior - every graphics/widget/mouse change this far was
verified with a real, headless QEMU screendump (`sendkey` for keyboard
interaction, `mouse_move`/`mouse_button` for mouse interaction), not just
reasoning about the code. See "A real windowed desktop" in `README.md` for
the exact commands. For anything else, `make test` / `make run` against the
relevant `EXTRA=-DLARZ_*` boot target is enough.

## Regressions

Whatever you change, confirm the *other* boot targets still build and
behave the same (`make EXTRA=-DLARZ_SHELL` etc. still `make test`/`make
run` cleanly) - several real bugs this project shipped were caught exactly
this way, not by the new feature's own test.
