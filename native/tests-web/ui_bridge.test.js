// Regression test for the browser `ui` module + closure callback bridge.
// Runs under Node+jsdom (no real browser needed) against the wasm-web build
// (`make wasm-web` in native/ first). Verifies:
//   - the basic element get/set primitives actually touch a real DOM
//   - ui.on() wires a Larzscript closure to a real addEventListener and the
//     closure's captured state survives repeated firings
//   - a closure registered via ui.on() and never bound to any global name
//     survives garbage collection after its declaring function returns -
//     this is the one genuinely new GC-rooting mechanism this bridge needed
//     (see the g_ui_callbacks comment next to gc_collect() in larzscript.c);
//     without it this exact scenario is a heap-use-after-free.
const path = require("path");
const { JSDOM } = require("jsdom");

const WASM_JS = path.join(__dirname, "..", "wasm", "larzscript-web.js");

async function withPage(html, fn) {
  const dom = new JSDOM(html, { runScripts: "outside-only" });
  global.window = dom.window;
  global.document = dom.window.document;
  delete require.cache[require.resolve(WASM_JS)]; // fresh module state per test
  const Larzscript = require(WASM_JS);
  const mod = await Larzscript();
  function evalSrc(src) {
    const ret = mod.ccall("larz_eval_source", "number", ["string"], [src]);
    if (ret !== 0) throw new Error("larz_eval_source failed - see stderr above");
  }
  await fn({ dom, evalSrc });
}

let failures = 0;
function check(label, got, want) {
  if (got !== want) { console.log(`FAIL ${label}: got ${JSON.stringify(got)} want ${JSON.stringify(want)}`); failures++; }
  else console.log(`ok ${label}`);
}

async function testPrimitives() {
  await withPage(`<!doctype html><body>
    <p id="msg">hello</p><input id="box" value=""><button id="btn"></button><p id="clicks">0</p>
  </body>`, async ({ dom, evalSrc }) => {
    evalSrc(`ui.set_text("#msg", "hi from larzscript")`);
    check("set_text", document.querySelector("#msg").textContent, "hi from larzscript");

    evalSrc(`ui.set_text("#msg", "got:" + ui.get_text("#msg"))`);
    check("get_text round-trip", document.querySelector("#msg").textContent, "got:hi from larzscript");

    evalSrc(`ui.set_value("#box", "42")`);
    check("set_value", document.querySelector("#box").value, "42");

    evalSrc(`ui.set_text("#msg", ui.get_value("#box"))`);
    check("get_value", document.querySelector("#msg").textContent, "42");

    evalSrc(`ui.set_html("#msg", "<b>bold</b>")`);
    check("set_html", document.querySelector("#msg").innerHTML, "<b>bold</b>");

    evalSrc(`ui.add_class("#msg", "highlight")`);
    check("add_class", document.querySelector("#msg").classList.contains("highlight"), true);

    evalSrc(`ui.remove_class("#msg", "highlight")`);
    check("remove_class", document.querySelector("#msg").classList.contains("highlight"), false);

    evalSrc(`
      let n = 0
      fn on_click() { n = n + 1; ui.set_text("#clicks", str(n)) }
      ui.on("#btn", "click", on_click)
    `);
    document.querySelector("#btn").dispatchEvent(new dom.window.Event("click"));
    check("ui.on first click", document.querySelector("#clicks").textContent, "1");
    document.querySelector("#btn").dispatchEvent(new dom.window.Event("click"));
    check("ui.on second click (closure state persisted)", document.querySelector("#clicks").textContent, "2");
  });
}

async function testCallbackSurvivesGC() {
  await withPage(`<!doctype html><body><button id="btn"></button><p id="clicks">0</p></body>`, async ({ dom, evalSrc }) => {
    // on_click is local to setup() and never bound to any global name - once
    // setup() returns, the ONLY remaining reference to it is g_ui_callbacks[].
    // A big allocation loop after it returns forces real GC (default
    // threshold is 200000 allocations) before the click ever fires.
    evalSrc(`
      fn setup() {
        let n = 0
        fn on_click() { n = n + 1; ui.set_text("#clicks", str(n)) }
        ui.on("#btn", "click", on_click)
      }
      setup()
      let junk = []
      let i = 0
      while i < 100000 { junk.push([i, str(i), {"a": i}]); i = i + 1 }
    `);
    document.querySelector("#btn").dispatchEvent(new dom.window.Event("click"));
    check("callback survives GC after its declaring function returns",
      document.querySelector("#clicks").textContent, "1");
  });
}

(async () => {
  await testPrimitives();
  await testCallbackSurvivesGC();
  console.log(failures === 0 ? "\nALL PASSED" : `\n${failures} FAILURE(S)`);
  process.exit(failures === 0 ? 0 : 1);
})().catch(e => { console.error(e); process.exit(1); });
