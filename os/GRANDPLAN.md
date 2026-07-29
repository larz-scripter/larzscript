# LarzOS — the Grand Plan

Where we are: a 64-bit kernel with interrupts + a preemptive scheduler, running
the Larzscript interpreter on bare metal, with a writable persistent filesystem,
a self-hosting shell, a package manager + App Store, networking (TCP, an HTTP
client and a money-native web service), multi-user login with file permissions,
init/services, and a real clock. Everything money-native, 100% Larzscript
userland, all persistent.

This plan takes it the rest of the way, in dependency order. Each phase ships a
verifiable milestone.

## Phase 1 — Per-process gas (money-native CPU metering)   ★ the moat
The deepest money-native feature and a runaway-program killer, using the timer +
scheduler we just built.
- Kernel: meter CPU as timer ticks per command; a budget the kernel enforces.
  A `/dev/gas` write starts metering with a budget; on the timer, a command over
  budget sets a kill flag.
- Interpreter: a weak `larz_gas_kill` global checked at statement boundaries →
  raises `GasError` (caught by the shell). No effect on the host build.
- Shell: set a budget before each command, charge the wallet by ticks used
  (real CPU-time billing), and report kills.
- Verify: a normal command is cheap and completes; an infinite `spin` is killed
  after its budget and billed the maximum.

## Phase 2 — Useful concurrency (web server + shell at once)
Make multitasking *useful* by running two Larzscript programs concurrently.
- The interpreter's file-scope state (GC head, error jmp, module cache) becomes
  per-instance (an `Interp`-owned context), so two interpreter tasks don't
  corrupt each other.
- Run the web service as a background task while the shell stays interactive.
- Verify: `curl` serves pages while you type commands in the shell.

## Phase 3 — Networking depth
- **DNS** resolver (UDP) so `pkg`/HTTP client use hostnames, not just IPs.
- **Multi-connection** web server (a small accept queue; serve back-to-back
  without dropping concurrent SYNs).
- Sturdier TCP (basic retransmit/timeout).
- Verify: `pkg` installs from a named host; two quick clients both get served.

## Phase 4 — Filesystem depth
- **Full path resolution** in the permission check (close the relative-traversal
  gap): resolve to an absolute path, then authorize.
- Per-file **ownership + mode bits** (not just path-based home privacy); `ls -l`,
  `chmod`, `chown`.
- A real **on-disk layout** for LarzFS (inodes/extents) so large files and deep
  trees persist efficiently.
- Verify: ownership survives reboot; `chmod` denies/permits; a big file round-trips.

## Phase 5 — Graphics (framebuffer)
- Switch to a **linear framebuffer** (VBE/GOP) instead of VGA text.
- A font blitter, then a minimal windowing/status UI; the money-native dashboard
  (wallet, services, tasks) rendered graphically.
- Verify: boots to a graphical screen in QEMU/VirtualBox.

## Phase 6 — The language endgame (`larzc`)
- A **Larzscript → native code** compiler (emit C first, then direct codegen),
  so kernel-level code can be written in Larzscript and the C seed shrinks toward
  zero — the TempleOS-style "all one language" endgame.
- Verify: a Larzscript module compiled to native and linked into the kernel.

## Cross-cutting
- Boot on **real hardware** (a laptop / a cloud VM), UEFI path.
- More packages in the App Store; a LarzOS docs site.
- Harden: full register (incl. xmm) save on context switch; a TSS for faults.

Built in order, each phase is a shippable milestone. Phase 1 starts now.
