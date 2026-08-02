# Ecosystem growth roadmap — SEO content, at scale

**Status: planned, not started.** This is a queued set of "big project"
initiatives to grow Larzscript's footprint in search and in the coding
world generally, decided 2026-08-02. Nothing below is built yet — this
document exists so the next push has a concrete starting point instead of
re-deriving the plan from scratch.

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

**The idea:** a single large, genuinely useful real-world tool — bigger in
scope than `larzscript-missionbudget` or `larzscript-impactwatch` — built
entirely in Larzscript, real enough that it's worth writing about and
earning backlinks to on its own merits, not just as a content-grid entry.

**Why it matters:** the cookbook wins by volume; this wins by depth — a
"look what this weird money-native language actually shipped" story that
spreads outside search (Hacker News, dev Twitter/X, newsletters) the way a
content page alone can't.

**Not yet scoped.** Needs a real decision on what the app *is* before any
work starts — candidates should be genuinely useful (not a tech demo) and
should lean on `wallet`/`pay`/`require` for something that would be
awkward in a language without those as primitives (subscription billing,
marketplace escrow, metered API access, split-cost tooling — anything
where "money that fails closed" is the actual hard part). Pick this only
once a specific app idea has been agreed on.

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
