// Regression test for ui.fetch() - the async half of the callback bridge
// (native/tests-web/ui_bridge.test.js covers ui.on()). Runs against a real
// local HTTP server (not an external URL) so the test is hermetic: fast,
// deterministic, and doesn't depend on network access in CI.
const path = require("path");
const http = require("http");
const { JSDOM } = require("jsdom");

const WASM_JS = path.join(__dirname, "..", "wasm", "larzscript-web.js");

let failures = 0;
function check(label, got, want) {
  if (got !== want) { console.log(`FAIL ${label}: got ${JSON.stringify(got)} want ${JSON.stringify(want)}`); failures++; }
  else console.log(`ok ${label}`);
}

function startServer() {
  return new Promise((resolve) => {
    const server = http.createServer((req, res) => {
      if (req.url === "/ok") { res.writeHead(200); res.end("hello from the server"); }
      else if (req.url === "/missing") { res.writeHead(404); res.end("not found"); }
      else { res.writeHead(500); res.end("?"); }
    });
    server.listen(0, "127.0.0.1", () => resolve(server));
  });
}

async function main() {
  const server = await startServer();
  const port = server.address().port;

  const dom = new JSDOM(`<!doctype html><body><p id="out">-</p></body>`, { runScripts: "outside-only" });
  global.window = dom.window;
  global.document = dom.window.document;
  const Larzscript = require(WASM_JS);
  const mod = await Larzscript();
  function evalSrc(src) {
    const ret = mod.ccall("larz_eval_source", "number", ["string"], [src]);
    if (ret !== 0) throw new Error("larz_eval_source failed - see stderr above");
  }

  // successful fetch
  evalSrc(`
    fn on_ok(status, body) {
      ui.set_text("#out", str(status) + ":" + body)
    }
    ui.fetch("http://127.0.0.1:${port}/ok", on_ok)
  `);
  await new Promise(r => setTimeout(r, 300));
  check("ui.fetch 200 delivers status+body", document.querySelector("#out").textContent, "200:hello from the server");

  // a real HTTP error status still resolves fetch() normally (not a JS-level failure)
  evalSrc(`
    fn on_404(status, body) {
      ui.set_text("#out", str(status) + ":" + body)
    }
    ui.fetch("http://127.0.0.1:${port}/missing", on_404)
  `);
  await new Promise(r => setTimeout(r, 300));
  check("ui.fetch 404 still delivers status+body", document.querySelector("#out").textContent, "404:not found");

  // unreachable host -> network-level failure -> status 0, callback still fires exactly once
  evalSrc(`
    fn on_fail(status, body) {
      ui.set_text("#out", "failed:" + str(status))
    }
    ui.fetch("http://127.0.0.1:1/nope", on_fail)
  `);
  await new Promise(r => setTimeout(r, 500));
  check("ui.fetch network failure calls back with status 0", document.querySelector("#out").textContent, "failed:0");

  server.close();
  console.log(failures === 0 ? "\nALL PASSED" : `\n${failures} FAILURE(S)`);
  process.exit(failures === 0 ? 0 : 1);
}

main().catch(e => { console.error(e); process.exit(1); });
