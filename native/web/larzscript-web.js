// larzscript-web.js - page-side bootstrap for running Larzscript client-side.
//
// Drop this next to larzscript-web.js (the Emscripten glue, `make wasm-web`)
// and larzscript-web.wasm, then include both on a page:
//
//   <script src="larzscript-web.js"></script>          <!-- Emscripten glue -->
//   <script src="larzscript-web-bootstrap.js"></script> <!-- this file -->
//   <script type="text/larzscript">
//     ui.set_text("#msg", "hello from Larzscript")
//   </script>
//
// All <script type="text/larzscript"> tags on the page share ONE persistent
// interpreter (same global scope, same as ordinary <script> tags sharing
// `window`) - a wallet or capability declared in one tag is visible to
// tags that run after it. Tags run in document order as soon as the wasm
// module finishes loading; anything registered via ui.on() keeps firing for
// the lifetime of the page, independent of which tag registered it.
(function () {
  function boot(Module) {
    var tags = document.querySelectorAll('script[type="text/larzscript"]');
    tags.forEach(function (tag) {
      var src = tag.textContent;
      var ret = Module.ccall("larz_eval_source", "number", ["string"], [src]);
      if (ret !== 0) {
        console.error("Larzscript: error running", tag, "- see the message above");
      }
    });
  }

  function start() {
    if (typeof Larzscript !== "function") {
      console.error("larzscript-web.js: Larzscript() factory not found - " +
        "make sure the Emscripten glue script is included first");
      return;
    }
    Larzscript().then(boot);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", start);
  } else {
    start();
  }
})();
