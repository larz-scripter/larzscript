# Build the native, standalone larzscript interpreter (no Python needed).
CC     ?= cc
CFLAGS ?= -O2 -std=c11 -Wall
PREFIX ?= /usr/local

larzscript: larzscript.c
	$(CC) $(CFLAGS) -o larzscript larzscript.c

install: larzscript
	install -d $(PREFIX)/bin
	install -m 755 larzscript $(PREFIX)/bin/larzscript

clean:
	rm -f larzscript larzscript.exe

# Windows: cross-compiled from Linux/macOS via mingw-w64 (apt install
# gcc-mingw-w64-x86-64). -static avoids a runtime dependency on
# libwinpthread-1.dll (needed by clock_gettime) so the .exe stays
# drop-anywhere with no installer/DLLs - only kernel32/msvcrt, present on
# every Windows install.
windows: larzscript.c
	x86_64-w64-mingw32-gcc -O2 -std=c11 -Wall -static -o larzscript.exe larzscript.c -lwinpthread

# WebAssembly via Emscripten (emsdk: https://emscripten.org/docs/getting_started/downloads.html).
# Two separate outputs, not one build, because they serve different jobs:
#  - wasm-node: a plain CLI build (argv flows straight from process.argv, same
#    as the native binary) used ONLY to run native/run_tests.sh's existing
#    suite under Node, proving the wasm build isn't silently behind native.
#    NODERAWFS=1 gives it real filesystem access so test fixtures resolve the
#    same way they do natively.
#  - wasm-web: the browser bundle (MODULARIZE, no CLI argv, no filesystem) -
#    added alongside the dom/callback-bridge work.
# STACK_SIZE is bumped well past Emscripten's 64KB default: this is a
# recursive tree-walking interpreter (exec/eval call each other, and eval's
# call path keeps a `Value args[64]` on the stack per frame) - the default
# stack overflows silently under wasm on nothing more exotic than a function
# that does a dynamic `import <expr> as x` twice, corrupting the heap and
# surfacing as a bogus "out of memory" rather than a clean stack-overflow
# trap. 8MB matches a typical native thread stack and leaves headroom.
wasm-node: larzscript.c
	emcc $(CFLAGS) -o wasm/larzscript-node.js larzscript.c \
		-s ENVIRONMENT=node -s ALLOW_MEMORY_GROWTH=1 -s NODERAWFS=1 -s STACK_SIZE=8388608

# The browser bundle: MODULARIZE (no auto-run, no CLI argv - see
# web/larzscript-web.js for the page-side bootstrap), only larz_eval_source
# and larz_invoke_callback exported (the two entry points JS ever calls into).
# ENVIRONMENT=web,node (not just web) so the SAME bundle also loads under
# plain Node for the test suite (native/tests-web/, via jsdom) - no real
# browser needed to verify the dom/callback-bridge mechanics, only to eyeball
# the actual demo page.
wasm-web: larzscript.c
	emcc $(CFLAGS) -o wasm/larzscript-web.js larzscript.c \
		-s MODULARIZE=1 -s EXPORT_NAME=Larzscript \
		-s ALLOW_MEMORY_GROWTH=1 -s STACK_SIZE=8388608 \
		-s ENVIRONMENT=web,node \
		-s EXPORTED_RUNTIME_METHODS=ccall,cwrap \
		-s EXPORTED_FUNCTIONS=_larz_eval_source,_larz_invoke_callback,_malloc,_free \
		-s INVOKE_RUN=0

.PHONY: install clean windows wasm-node wasm-web
