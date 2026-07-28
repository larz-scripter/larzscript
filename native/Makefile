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
	rm -f larzscript

.PHONY: install clean
