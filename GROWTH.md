# Ecosystem growth roadmap — SEO content, at scale

**Status:** #1 (cookbook) wave 1 shipped 2026-08-02 — 29 tasks live at
[larzos.com/learn-larzscript/](https://larzos.com/learn-larzscript/),
generator in [`larzscript-cookbook`](https://github.com/larz-scripter/larzscript-cookbook)
(CI-verified: every example's real output is captured by actually running
it, not hand-written). #2 (flagship app) also shipped 2026-08-02 —
[`larzscript-beatstudio`](https://github.com/larz-scripter/larzscript-beatstudio),
a beat-making/recording/mixing/mastering studio. #3 (playground) is still
just planned. This document exists so the next push has a concrete
starting point instead of re-deriving the plan from scratch.

## Why this, why now

The Larz Stack just crossed 165 real, tested packages (`orbits`/`neows`
being the newest), and the individual `/stack/*` package pages
(larzos.com/stack/) already prove the format works: real code, real
captured output, real JSON-LD/breadcrumb SEO markup, cross-linked. That
format is the reusable unit. The question is what to build *on top of it*
that compounds rather than just adding one more page at a time.

Three directions were considered. All three are worth doing; they're not
mutually exclusive, and each has a different payoff shape.

## 1. Rosetta Larzscript cookbook (highest leverage, do first)

**Wave 1: done (2026-08-02).** 29 tasks live at
[larzos.com/learn-larzscript/](https://larzos.com/learn-larzscript/) —
algorithms (8), data formats (5), CLI & text (5), web (5), data structures
(3), money-native (3). Generator + task manifest persisted in
[`larzscript-cookbook`](https://github.com/larz-scripter/larzscript-cookbook)
(not scratchpad this time — the `/stack/` page generator had to be
rebuilt from scratch once already because nothing was kept). CI
(`run_tasks.py`) actually executes every task's `.lz` source and fails the
build if a task breaks, so the whole cookbook stays honest as packages
change. Cross-linked from the `/stack/` index hero and added to both
`sitemap.xml` files (root + `/stack/`).

**Next wave:** more tasks, same manifest/generator (`tasks.py` +
`build_cookbook.py`/`build_index.py` in that repo) — target another ~40-60
before revisiting scope. Good candidates not yet covered: regex matching,
date arithmetic, template rendering, a minimal HTTP server, retry/backoff,
memoization/caching, a state machine, pagination, semantic version
comparison, and more "money-native extras" (a subscription/paywall, a
loan/amortization calc, currency conversion, a ledger that always
balances) — the differentiator category is currently the thinnest (3 of
29) and is the actual point of the whole exercise.

**The idea:** systematically solve the ~150-300 tasks developers actually
search for — "how to parse JSON in X", "quicksort in X", "read a CSV in
X", "build a CLI tool in X", "debounce a function in X" — as real, tested
Larzscript programs, one SEO landing page each, same rigor as the
`/stack/` pages (real code, real captured output, not fabricated).

**Why it's the highest-leverage option:** "how do I do X in language Y" is
one of the single largest programming-search categories that exists. Most
of the ~150-300 tasks are already a few lines away from done, because the
165 packages already cover JSON, CSV, HTTP, regex, CLIs, algorithms, data
structures, crypto, etc. — this is mostly *composition and packaging* of
existing capability into landing pages, not new engineering. And unlike a
generic "Rosetta Code" entry, every page gets a natural spot to show what
no other language answer can: wherever a task touches money, state, or
metering, the Larzscript version gets to use `wallet`/`pay`/`require` as
language primitives instead of a bolted-on library.

**Rough shape of the work:**
- A task list (~150-300 entries), grouped by category (algorithms, data
  formats, CLI/text, web/HTTP, data structures, security, "money-native
  extras" — tasks with no equivalent in other languages' cookbooks at all,
  e.g. "charge a customer with automatic retry" or "split a bill fairly").
- A page generator following the exact pattern already proven on
  `/stack/larzorbits/` and `/stack/larzneows/`: real `.lz` source, run for
  real to capture real output, HTML-escaped into the same template
  (title/meta/JSON-LD `SoftwareSourceCode` + `BreadcrumbList`/breadcrumbs/
  install snippet/try-it block/related-links), published under something
  like `larzos.com/learn-larzscript/<task-slug>/` or folded into
  `/academy/`.
- Batch this the same way the package-migration and stack-page work was
  batched: don't hand-write 300 pages one at a time in a single session —
  script the generation, spot-check a sample for correctness, publish in
  waves.
- Cross-link every cookbook page back to the `/stack/` package(s) it uses,
  and vice versa (the missing piece from the `orbits`/`neows` push — those
  two still aren't linked *from* `larzmath`/`larzhttp`/etc., since every
  existing page caps at 4 curated related-links and evicting one felt like
  a call for a human to make, not to force through silently).

## 2. One flagship app

**Shipped 2026-08-02:** [`larzscript-beatstudio`](https://github.com/larz-scripter/larzscript-beatstudio)
— a beat-making/recording/mixing/mastering studio. 4 synthesized drum
voices, a 16-step sequencer, a real mixer (per-track gain/pan), and a real
3-band EQ → linked-stereo compressor → limiter → normalize mastering
chain, all pure Larzscript on two new Stack packages built for it
([`dsp`](https://github.com/larz-scripter/larzscript-packages/tree/master/packages/dsp),
[`wav`](https://github.com/larz-scripter/larzscript-packages/tree/master/packages/wav)).
The professional master is money-native (`master --price=DOLLARS`, a real
`pay`/`unless` gate mirroring how real mastering-as-a-service tools price
per master); `preview` is free so the pipeline is demoable unfunded.
Surfaced and worked around a real native-interpreter bug along the way
([larzscript#4](https://github.com/larz-scripter/larzscript/issues/4)).

**Why it matters:** the cookbook wins by volume; this wins by depth — a
"look what this weird money-native language actually shipped" story that
spreads outside search (Hacker News, dev Twitter/X, newsletters) the way a
content page alone can't.

**If picking another flagship app later:** the same bar applies — genuinely
useful (not a tech demo), leaning on `wallet`/`pay`/`require` for something
that would be awkward without them as primitives.

## 3. Interactive playground / tutorial

**The idea:** an in-browser Larzscript runner + guided tutorial site —
try the language with no install, walk through `wallet`/`pay`/`require`
and the Stack interactively.

**Why it matters:** this is a conversion play, not primarily an SEO
play — it's what turns a visitor who found a cookbook page via search into
someone who actually tries the language. Complements #1 and #2 rather than
competing with them.

**Biggest open question:** whether the native interpreter can realistically
compile to WASM for real in-browser execution, or whether this has to be a
server-side sandboxed `larzscript -e` call instead. That's a technical
spike before any content/UX work, and a bigger lift than #1 or #2 — lowest
priority of the three for that reason.

## Suggested sequencing

1. **Cookbook (#1) first** — cheapest, most mechanical, largest total SEO
   surface, and directly reuses tooling already built and proven this
   session.
2. **Flagship app (#2)** once a specific idea is picked — needs a decision
   first, not more research.
3. **Playground (#3)** last, gated on the WASM/sandboxing spike.
