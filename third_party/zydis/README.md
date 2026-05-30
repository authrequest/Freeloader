# Zydis (vendored)

Single-file amalgamation of the [Zydis](https://github.com/zyantific/zydis)
x86/x86-64 disassembler. The hook engine uses it to decode and copy
instruction-aligned bytes when installing a trampoline (so a relocated prologue
stays valid).

- **Upstream:** https://github.com/zyantific/zydis
- **License:** MIT (see the header in `Zydis.h` and upstream `LICENSE`)

`Zydis.c` / `Zydis.h` are generated amalgamations — regenerate from upstream
instead of hand-editing them here.
