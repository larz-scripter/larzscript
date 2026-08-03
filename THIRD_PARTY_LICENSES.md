# Third-party licenses

`larzscript` itself is MIT-licensed (see `LICENSE`). The native binaries for
some platforms (currently: linux-x86_64 - see `.github/workflows/native.yml`
for which platforms, this expands over time) statically link two additional
libraries to provide real SSH support (the `ssh_*` builtins / `ssh` package):

## libssh (LGPL-2.1-or-later)

https://www.libssh.org/ - the real SSH protocol implementation these
builtins call into, rather than a from-scratch reimplementation (Larzscript's
own numeric type can't honestly do the big-integer/elliptic-curve math real
SSH key exchange needs).

**LGPL's static-linking relink provision, satisfied by:** `larzscript` is
already fully open source under a permissive license with a public, scripted
build - `native/larzscript.c` plus `.github/workflows/native.yml`, which
pins the exact libssh version built and how. Anyone can already reproduce
this exact binary, or rebuild it against a modified/different version of
libssh, using that public workflow as-is. No separate object-file drop is
provided beyond that, since the scripted source build already serves the
same purpose LGPL requires.

## mbedTLS (Apache License 2.0)

https://www.trustedfirmware.org/projects/mbed-tls/ - libssh's crypto
backend here, chosen over OpenSSL or libgcrypt specifically to avoid
stacking a second copyleft dependency (libgcrypt is GPL/LGPL) on top of
libssh's own LGPL obligation. Apache 2.0 is permissive and adds no
additional linking obligations beyond attribution (this file).
