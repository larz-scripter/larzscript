# Language & interpreter quality roadmap

**Status:** new 2026-09-28. Companion to [`GROWTH.md`](GROWTH.md) — that
document is about ecosystem reach (SEO, flagship apps, the playground);
this one is about whether the language itself, the interpreter, and the
experience of actually using it hold up to being taken seriously next to
an established language like Python. Four tracks: **diagnostics** (what
the interpreter tells you when something goes wrong), **interpreter
robustness** (the C implementation's own correctness and limits),
**feature breadth** (what you can express), and **testing rigor** (how
confident we can be that all of the above actually holds). A fifth,
**DX/tooling polish**, ties them together.

Every item below is either something already verified true or false by
hand (not assumed) as of this date, or explicitly marked as needing that
verification before work starts — the project's own stated principle in
`CONTRIBUTING.md` ("real tests over reasoning") applies to planning the
work, not just to the code.

## Where things actually stand today (verified, not assumed)

**Real strengths, worth protecting, not just gaps to fix:**
- Error messages are already good where they exist: specific, line-numbered,
  a real taxonomy of 16 distinct error types (`LarzTypeError`,
  `LarzNameError`, `LarzKeyError`, `MoneyError`, `RequireError`,
  `SocketError`, ...) mirroring Python's exception hierarchy naming, not
  one generic `Error`. 366 distinct `runtime_error(...)` call sites — this
  is not a thin afterthought.
- The money-native core (`wallet`/`pay`/`require`/`price`) is genuinely
  robust: tried to break it directly (negative amounts, overspending) and
  every attempt failed cleanly with a catchable `MoneyError`, never silent
  corruption.
- `fmt`, `--check`, a real REPL with line editing, `--emit-c`, and a
  self-updater all exist and work. Editor support exists for VS Code and
  Vim (`editors/`). 183 packages in the registry. A real CI matrix builds
  and tests Linux (x86_64/aarch64), Windows (via Wine) and wasm-node on
  every commit, not just "works on my machine."
- The project already has the right instinct for hard problems: issue #2
  scoped the TCP socket capability gap (design + per-platform reality)
  *before* writing C against the shared interpreter file, rather than
  guessing. That discipline is worth deliberately continuing, not
  reinventing each time.

**Concrete gaps, found by hand, not by assumption:**
- **No "did you mean" suggestions anywhere** (`grep`-confirmed: zero hits
  for any suggestion/edit-distance logic in `native/larzscript.c`). A
  typo'd variable or builtin name just gets `LarzNameError`, no nudge
  toward the fix — table stakes in Python 3.10+, Rust, Elm, Ruby.
- **No warnings channel** — every diagnostic is a hard, fatal error. No
  way to flag something suspicious (an unused variable, a shadowed name,
  a deprecated builtin) without stopping execution.
- **No linter or static-analysis pass**, either for Larzscript *programs*
  (a `larzscript lint file.lz`) or for the interpreter's own C source
  (no `clang-tidy`/cppcheck/scan-build gate found in `.github/workflows/`).
- **No classes.** No `N_CLASS`, no `class` keyword anywhere in
  `native/LANGUAGE.md` or the interpreter. Functions, closures, dicts,
  modules exist; user-defined types with methods do not. This is the
  single biggest feature-parity gap against Python.
- **No generators / lazy `yield`.** `range()` is the only built-in lazy
  sequence; there's no way for user code to define its own.
- **No regex in the language core** — exists as a pure-Larzscript package
  (`larzscript-regex`) instead, which is fine philosophically (small core,
  packages build ergonomics — the same pattern as sockets) but means it's
  easy to miss and isn't cross-linked from `LANGUAGE.md`.
- **Docs are thin**: `docs/index.html` is 154 lines total. `LANGUAGE.md`
  (354 lines) is a solid single-page reference but nothing like
  docs.python.org's tutorial + full per-function stdlib reference +
  language reference split.
- **The test suite is 42 `.lz`/`.expected` pairs for an entire general-
  purpose language implementation.** Compare to any mature language's
  thousands. This isn't a criticism of what's there (every test is a real
  fixture, actually executed — good discipline) — it's a volume and
  coverage-strategy gap.
- **No fuzzing.** Every existing test is a hand-written, roughly-valid
  program. Nothing feeds the lexer/parser malformed or adversarial input
  systematically, so "the interpreter never crashes on bad input" is
  untested, not verified — the same category of untested-but-assumed
  claim that turned out to be false for `MAX_CALL_DEPTH` (see below).
- **The recursion-crash case study.** `MAX_CALL_DEPTH=150`'s comment
  claimed the interpreter "fails safe... before any stack actually runs
  out." Empirically false: it SIGSEGV'd at depth 72 on both x86_64 and
  aarch64 at the real `-O2` release flags — the safety net was dead code.
  Fixed in #34 (real stack-headroom check, verified on two architectures,
  full test suite re-run, a regression test added that's proven to
  actually catch the bug). Logged here specifically as a **template for
  the discipline this whole document asks for**: don't trust a comment's
  claim about a resource limit, an edge case, or "this never happens" —
  measure it for real, on real platforms, before believing it.

## Track 1 — Diagnostics: what the interpreter tells you

1. **Did-you-mean suggestions** (high leverage, well-scoped, do first).
   `LarzNameError`/`LarzKeyError` already know the failing name; add a
   Levenshtein/edit-distance check against names actually in scope (for
   name errors) or dict keys (for key errors) and append
   `- did you mean 'x'?` when a close match exists. Small, self-contained,
   directly improves the single most common first-run experience (a typo).
2. **A warnings channel, separate from errors.** Non-fatal, printed to
   stderr, doesn't stop execution. Candidates: an unused `let` binding, a
   variable shadowing an outer scope, calling a builtin in a way that's
   deprecated-but-still-works. Add a `--strict` flag that promotes
   warnings to errors (mirrors Python's `-W error`, Rust's `#[deny]`) so
   CI/serious projects can opt into zero tolerance.
3. **Colorized terminal error output**, TTY-detected (`isatty(stderr)`),
   respecting `NO_COLOR` (an actual, real, documented convention -
   no-color.org) and a `--no-color` flag. Error type in a color, message
   in default, line reference dim.
4. **A real traceback for uncaught errors**, not just the innermost line.
   `ip->curline` is already tracked; `call_value()` is the one chokepoint
   every nested call goes through (same chokepoint the stack-headroom
   check now lives in) - a lightweight call-site stack pushed/popped there
   specifically for traceback purposes (freed as soon as an error is
   caught, so it costs nothing on the success path) would let an uncaught
   error print the full call chain, the way Python does, instead of one
   line divorced from how you got there.
5. **Audit exit codes.** Does every error category map to a distinct,
   documented process exit code (`sysexits.h`-style), or is it all just
   `exit(1)`? Worth confirming and formalizing either way - scripts that
   shell out to `larzscript` deserve to distinguish failure types without
   parsing stderr text.

## Track 2 — Interpreter robustness

1. **Systematic audit for the same class of bug #32 was.** The recursion
   fix found one *measured, wrong* assumption about a resource limit.
   Worth deliberately searching for siblings: fixed-size buffers
   (`char buf[4096]` appears repeatedly in `native/larzscript.c` - are any
   of them reachable with attacker/user-controlled length that could
   truncate silently rather than erroring?), other hardcoded constants
   whose comments assert a guarantee that was never empirically checked
   the way #32's finally was.
2. **Fuzzing.** A libFuzzer or AFL harness around `lex()`/`parse_program()`
   feeding random and mutated `.lz` source, checked into CI as its own
   job (time-boxed, not blocking every commit, but running regularly) -
   the direct answer to "does the interpreter ever crash on malformed
   input" being currently untested rather than actually known.
3. **Static analysis of the C source itself.** `clang-tidy`/`cppcheck`/
   `scan-build` as a CI gate on `native/larzscript.c` - check if this
   already exists in `.github/workflows/` before assuming it doesn't;
   add it if not.
4. **`Value` struct size reduction.** Found while investigating #32: the
   `Value` struct stores every possible variant as its own named field
   (`str`, `wal`, `fn`, `bi`, `list`, `pw`, `dict`, `mod`, `modname`,
   `rng` - nine mutually-exclusive pointer fields, never live
   simultaneously per the existing `.t`-tag discipline already used
   everywhere) instead of a real C union. Rough estimate: ~104+ bytes per
   `Value` today vs. ~24-32 bytes as a tagged union - a 3-4x reduction
   that would shrink every list, dict, GC root/temp stack, and call-frame
   array (`Value args[64]`, confirmed during #32's investigation to be a
   real, if partial, contributor to stack cost) at once. Bigger and
   riskier than the recursion fix (touches nearly every `Value`-producing
   site in the file), so it's deliberately a later, carefully-scoped item
   - not a "just do it" rewrite. Needs its own design pass (which fields
   the union groups, whether any code reads a field without checking the
   tag first - grep for that specifically before starting) before a PR,
   the same way sockets (#2) and the recursion fix (#32) were scoped
   before code was written.
5. **A formal grammar.** `LANGUAGE.md` is good prose but not a specification
   - a real EBNF grammar file would let third-party tooling (a real LSP,
   alternate parsers, a Tree-sitter grammar for even better editor
   support than the current TextMate-style syntax files) build against
   something authoritative instead of reverse-engineering the parser.

## Track 3 — Feature breadth ("expanding it bigger to do more")

Ranked by gap size against "looks professional like Python," not by ease:

1. **Classes / user-defined types with methods.** The single largest
   feature-parity gap. Needs a real design pass before any code: how does
   a class interact with the existing closure/`Env` model? Does a method
   share the money-native primitives' capability-checking story
   (`grant`/`revoke`/`requires`)? Scope this as its own tracked issue
   first, exactly like #2 did for sockets - this is the highest-risk,
   highest-value item on the whole list and deserves that same care.
2. **Generators / lazy `yield`.** Second-largest gap; likely easier than
   classes since `range()` already proves the interpreter can represent a
   lazy sequence type (`V_RANGE`) - the design question is whether
   user-defined generators reuse that same lazy-sequence machinery or need
   their own coroutine-like suspend/resume mechanism (much bigger lift).
3. **Pattern matching / destructuring** (a `match` expression, or at
   minimum `let [a, b] = list` / `let {x, y} = dict` destructuring
   assignment) - increasingly expected in modern languages and would pair
   well with the existing comprehension syntax.
4. **Gradual, optional type annotations** - not full static type checking,
   but even documentation-only annotations (`fn add(a: number, b: number)`)
   would improve both tooling (real autocomplete/hover types for an LSP)
   and diagnostics (a type mismatch caught at the call site with a clear
   message, reusing the existing `LarzTypeError` machinery, instead of
   surfacing three frames deeper).
5. **Promote/cross-link the regex package.** Lower effort than the above -
   either bless `larzscript-regex` as a documented "the" regex answer
   linked directly from `LANGUAGE.md`'s stdlib section, or fold a small
   regex engine into the core if the package route proves to have real
   ergonomic gaps. Needs a quick comparison pass, not a big design doc.

## Track 4 — Testing rigor ("test properly")

1. **Grow the suite deliberately, not organically.** 42 tests today; the
   goal isn't a specific number but systematic coverage - one file per
   operator, per builtin, per error type, per documented `LANGUAGE.md`
   section, so a gap is visible (a missing test file) rather than merely
   possible.
2. **A permanent regression test for every closed bug, as a standing
   CONTRIBUTING.md rule, not a one-off.** #32's fix added
   `recursion_guard.lz` and *proved* it actually catches the bug (confirmed
   it SIGSEGVs on the unpatched build first, passes clean on the patched
   one) - that verification step, not just "add a test," should be the
   documented bar for every future bug fix.
3. **Fuzzing** (listed under Track 2 too - the same harness serves both
   "does the interpreter crash" and "is input-handling actually tested").
4. **Cross-platform parity audit.** Confirm the *same* test suite runs
   (not silently skipped) on every CI target - Linux x86_64/aarch64,
   Windows under Wine, wasm-node - rather than assuming the matrix in
   `.github/workflows/native.yml` covers what it claims to.
5. **Automate `BENCHMARKS.md`.** Confirm whether it's re-run and checked
   in CI on every change or was written once and can silently go stale -
   a performance regression should be as visible as a test failure.

## Track 5 — Beauty in design / DX polish

1. **Audit editor syntax files for freshness** - do `editors/vscode/` and
   `editors/vim/` cover every current keyword and error type (including
   ones added recently, like the ones surfaced fixing #21-32)? A drifted
   syntax file is a paper cut every single session for anyone using it.
2. **REPL tab-completion** of variable/builtin names in scope - line
   editing already landed (#15); completion is the natural next step and
   likely reuses the same input-handling code path.
3. **A real documentation site**, modeled on Python's tutorial → language
   reference → full stdlib reference structure, ideally generating the
   stdlib reference from source doc-comments so it can't silently drift
   out of sync with `native/larzscript.c` the way a hand-maintained page
   eventually always does.
4. **Consistent `--help` output and a man page** - a small, cheap,
   high-visibility polish item.
5. **A real Language Server (LSP)** for autocomplete, hover-docs, and
   inline diagnostics in any editor, not just VS Code/Vim's syntax
   highlighting - correctly sequenced last on this track since it wants
   the formal grammar (Track 2) and ideally type annotations (Track 3)
   to exist first, or it's building on sand.

## How to use this document

- **Sequence quick, low-risk, high-leverage wins first**: did-you-mean,
  the warnings channel, more tests, docs expansion, editor-file freshness.
  Save the big structural bets - classes, generators, the `Value` union
  refactor - for once there's a real design pass, the same discipline
  already proven on sockets (#2) and the recursion fix (#32).
- **Every substantial item gets its own tracked issue before
  implementation.** This document is the index; issues are where the
  actual design and review happens - don't let a big change land as a
  surprise PR against the shared interpreter file.
- **Update this file when something lands** (inline, with a date and a
  link to the issue/PR - mirroring how `GROWTH.md` records ship dates)
  so it stays a trustworthy status board instead of a wish list that
  quietly goes stale.
