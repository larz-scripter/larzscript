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
[`README.md`](README.md)). Uses the `ui` module (`ui.window`/`ui.label`/
`ui.button`/`ui.on`/`ui.terminal`/`ui.set_text`, documented alongside the
primitives themselves in [`gfx.h`](gfx.h)) - the identical vocabulary the
[browser build](../native/WEB.md) uses (minus `ui.window`, which only
exists here), just backed by `gfx.c`'s window compositor instead of a DOM.

**The primary path: pure Larzscript, no C at all.** `ui.window(title, w, h)`
creates your app's own window (auto-cascaded position - you don't pick x,y)
and makes it "current", so every `ui.label`/`ui.button`/`ui.terminal` call
after that lands inside it automatically, as an offset from its client
origin, not an absolute screen coordinate:

```
# kernel/rootfs/yourthing.lz - the whole app. No C wrapper, no boot-target
# wiring, nothing else to write.
ui.window("Your Thing", 300, 200)
ui.label("yourthing_status", 4, 4, "starting...")
let n = 0
while true {
  n = n + 1
  ui.set_text("yourthing_status", "tick " + str(n))
  sleep(1)
}
```

Then add it to the desktop-icon launcher's manifest (`kernel.c`,
`g_app_manifest_labels`/`g_app_manifest_scripts` next to `about.lz`/
`clock.lz`/`files.lz`) so it shows up as a clickable icon - `desktop_icon_
activate()` calls the existing generic `launch_app("/yourthing.lz")`,
which spawns it as a new task via `task_create(task_generic_app)` within
`NTASK`'s budget (`libk.c`) the same way every other on-demand app does.
No boot-target wiring needed at all; `larzsh.lz`/`about.lz`/`clock.lz`/
`files.lz` are all worked examples of this exact pattern (the Terminal
itself used to be the one bespoke-C-wrapper holdout - `task_terminal` -
until it too became self-contained via `ui.window()`/`ui.window_size()`,
which is what let it join the icon manifest and become relaunchable after
being closed), and `apptest.lz` is the minimal one. A window's close-X
(`gfx_window_close_hit_test`) frees its task slot (`task_exit`)
automatically the moment the user closes it, so a manifest entry doesn't
need to worry about NTASK running out - closing one app always makes room
for launching a different one.

**The fallback path: a bespoke C wrapper**, only worth it for an app that
needs to create its widgets with specific pre-computed values a plain
`ui.window()` call can't express (multiple windows from one task, for
example). Nothing in the shipped kernel needs this anymore, but the pattern
still works if you do:

```c
/* kernel.c - create the window + starting widget(s) yourself, then hand
 * off to your .lz file, which touches them only by id via ui.*. */
static void task_yourthing(void){
    int idx = gfx_window_create("Your Thing", 400, 40, 300, 200);
    int cx, cy, cw, ch;
    gfx_window_client_rect(idx, &cx, &cy, &cw, &ch);
    gfx_widget_label("yourthing_status", 4, 4, "starting...");   /* window-relative now (stage 1) */
    char *a[] = { "larzscript", "/yourthing.lz", 0 };
    larz_main(2, a);
    for(;;) __asm__ volatile("hlt");
}
```

then `task_create(task_yourthing);` in the `LARZ_DESKTOP` branch of
`kernel_main`, auto-started at boot rather than launched on demand.

Windows can be **dragged** by their title bar, **resized** from the
bottom-right grip, and **closed** via the X in the top-right of the title
bar - all built-in, nothing an app needs to opt into. Resizing only resizes
the frame; widgets don't reflow (a resized terminal keeps its original
wrap width) - see `README.md`'s "Known limitations".

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
