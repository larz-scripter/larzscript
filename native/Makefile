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

.PHONY: install clean windows
