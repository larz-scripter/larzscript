# Larzscript in the browser

`native/larzscript.c` also compiles to WebAssembly and runs client-side, in a
real browser tab - the same interpreter that ships the standalone binary and
the LarzOS kernel, not a separate JavaScript port. Live example:
[larzos.com/larzscript/gui/](https://larzos.com/larzscript/gui/).

## Why

Every other Larzscript target is server/systems-side: the native interpreter,
the `larzc` compiler, the LarzOS kernel. All of them can move money, but none
of them can draw a button. The browser build is the UI layer for the same
language - `wallet`/`pay`/`capability` keep working exactly as they do
natively (pure in-memory value operations, no OS dependency), and a small `ui`
module adds the one thing a browser needs that a terminal doesn't: a way to
read and write a page.

`ui.*` is deliberately small and money-native-flavored, not a DOM clone. A
"pay button" or a live wallet-balance badge isn't a built-in widget - it's
ordinary Larzscript composing `ui.set_text`/`ui.on` with `wallet`/`pay`/
`require`, the same way any other Larzscript program is composed. See the
[live demo's source](https://larzos.com/larzscript/gui/) for a real one.

## Build

```bash
# Install Emscripten first: https://emscripten.org/docs/getting_started/downloads.html
source /path/to/emsdk/emsdk_env.sh
make -C native wasm-web    # -> native/wasm/larzscript-web.js + .wasm
```

This produces the same two files (`.js` glue + `.wasm`) Emscripten always
produces for a `MODULARIZE=1` build. Serve them alongside
[`web/larzscript-web-bootstrap.js`](web/larzscript-web-bootstrap.js) - the
page-side bootstrap that boots the module and feeds it every
`<script type="text/larzscript">` tag on the page, in document order:

```html
<script src="larzscript-web.js"></script>          <!-- Emscripten glue -->
<script src="larzscript-web-bootstrap.js"></script> <!-- the bootstrap -->
<script type="text/larzscript">
  ui.set_text("#msg", "hello from Larzscript")
</script>
```

All `<script type="text/larzscript">` tags on a page share **one persistent
interpreter** - the same global scope, exactly like ordinary `<script>` tags
sharing `window`. A wallet declared in one tag is visible to tags that run
after it, and anything registered via `ui.on()` keeps firing for the lifetime
of the page, independent of which tag registered it.

There's a second build, `make -C native wasm-node`, used only to run the
existing native test suite (`run_tests.sh`) under Node as a regression check
that the wasm build isn't silently behind native - it's a plain CLI build
with real filesystem access (`NODERAWFS=1`), not what a page loads.

## The `ui` module

| Function | Does |
|---|---|
| `ui.set_text(sel, text)` | Sets `textContent` on every element matching `sel`. Auto-escaped - never `innerHTML` - so this has no XSS surface. |
| `ui.get_text(sel)` | Returns the first matching element's `textContent`. |
| `ui.set_value(sel, v)` / `ui.get_value(sel)` | Form input value get/set. |
| `ui.set_html(sel, html)` | Raw `innerHTML`. The one deliberately unsafe primitive - named differently from `set_text` so any XSS risk is opt-in and visible at the call site. |
| `ui.add_class(sel, cls)` / `ui.remove_class(sel, cls)` | `classList` add/remove. |
| `ui.on(sel, event, fn)` | Registers a Larzscript closure as a real `addEventListener` on every matching element. `fn` is called with **no arguments** - read whatever you need via `ui.get_value`/`ui.get_text` inside the handler. |
| `ui.fetch(url, fn)` | Fires a real `fetch(url)`; `fn(status, body)` is called once it settles. `status` is `0` on a network-level failure (offline, DNS, CORS) - an actual HTTP error status (404, 500, ...) still resolves normally, since `fetch()` itself doesn't reject for those. `fn` always fires exactly once. Callbacks passed to `ui.fetch` must accept exactly `(status, body)` - Larzscript errors if you're handed more arguments than a function declares. |

`sel` is any CSS selector; the setters apply to every match, the getters read
the first. All of it is built the same way `bank.fmt(...)` works after
`import "bank" as bank` - a module, just constructed in C from real DOM calls
instead of parsed from a `.lz` file, so `ui.foo(...)` is ordinary method-call
syntax, no new grammar.

## The one new mechanism: the callback bridge

Everything above is a thin wrapper over a DOM call, except `ui.on`/`ui.fetch`,
which have to solve a real problem: a Larzscript closure has to be invoked
later, from JS, after the call frame that registered it is long gone. That
closure is stored in a small C-side table and handed to JS by index; when the
event fires (or the fetch settles), JS calls back into an exported C function
that looks the closure up and invokes it through the interpreter's ordinary
call path - the exact same one every normal function call already uses. No
new call mechanism, just a new entry point into the existing one.

The one thing this needed that didn't already exist: a callback sitting only
in that table - never bound to any global name, e.g. one declared inside a
`setup()` function after which its call frame is popped - is unreachable from
the interpreter's normal GC roots (the Env-chain/rootstack). Without an
explicit fix, the mark-sweep collector would free it out from under a still-
registered DOM listener the next time it runs, and firing the event later
would call into freed memory. Fixed by rooting the callback table in
`gc_collect()` exactly like the module cache (`ip->modcache`) already is - see
the `g_ui_callbacks` comment in `native/larzscript.c`. Verified with an
AddressSanitizer build both ways: reverting the fix reproduces a clean
heap-use-after-free (`native/tests-web/ui_bridge.test.js` has the exact
scenario), and the real code passes clean.

Deliberately no Asyncify: only `ui.on`/`ui.fetch` use this callback pattern.
`wallet`/`pay`/`require` and the rest of the language stay exactly as
synchronous as they are natively.

## Wallets in the browser aren't real settlement

`wallet`/`pay`/`paywall`/`capability`/`split` all work unmodified in the
browser build - they're pure in-memory value operations. But say this
plainly: a browser-resident wallet is **local, per-tab, in-memory state**. It
resets on reload and isn't connected to any real settlement rail. Real
Larzscript payments move the way the
[two-machine money demo](../kernel/README.md) already proves they do: a
server-side `.lz` paywall enforces payment over a real HTTP round trip.
Client-side Larzscript's job is the UI layer - show the balance, handle the
button, fire the request - not a second place real money lives.

## Tests

```bash
make -C native wasm-node wasm-web
cd native
BINARY="$PWD/wasm/larzscript-node.js" RUN_PREFIX=node ./run_tests.sh   # same suite as every platform
cd tests-web && npm install
node ui_bridge.test.js    # ui.on + the callback-bridge GC test, via Node+jsdom
node ui_fetch.test.js     # ui.fetch, against a real local HTTP server
```

`tests-web/` is a Node+jsdom harness (no real browser needed) that boots the
actual wasm-web build against a fake DOM and checks it end to end - element
writes actually landing, a dispatched click actually invoking a stored
closure, that closure surviving forced GC. CI (`.github/workflows/native.yml`,
job `web`) runs all of this on every push, plus packages `larzscript-web.js` +
`.wasm` + the bootstrap into a release artifact.
