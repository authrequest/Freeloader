# Plex_Patch

Reverse-engineering notes and a runtime patch for **Plex Media Server** on
**Linux x86-64**. The patch hooks Plex's `FeatureManager` and forces every entry
of its feature bitset on, unlocking all gated features.

> ⚠️ **Disclaimer** — This is for educational and reverse-engineering purposes,
> on software you legally run yourself. It does not bypass any account/server
> authentication and ships **no** Plex code. If you rely on Plex, buy a Plex
> Pass — it funds the developers. Use at your own risk; no warranty.

## How it works

- Plex's feature gates read a single in-memory table, `g_feature_bitset_slots`
  (14 × `uint64`), populated from the MyPlex feature list. A feature with
  internal code `C` is "available" iff `slots[C >> 3] & (1 << (C & 7))`.
- The patch (`linux/`) is a small shared library whose constructor finds
  `FeatureManager_apply_feature_list_xml`, installs a trampoline, and forces all
  14 slots to `0xFF…FF` after Plex applies its feature list — so every feature
  (including Plex Pass, code 92) reads as enabled.

Two non-obvious requirements make or break this on a real install:

1. **Build against musl, not glibc.** Plex bundles its own musl libc + libgcompat
   (`/usr/lib/plexmediaserver/lib/`). A glibc-built `.so` fails to relocate
   glibc-only symbols and Plex exits 127. The build uses `zig` to target
   `x86_64-linux-musl`.
2. **Inject with `LD_PRELOAD`, never `patchelf`.** `patchelf --add-needed`
   corrupts the PIE under musl's loader (instant SIGSEGV). A tiny launcher sets
   `LD_PRELOAD` only for the Plex `exec`, and the library `unsetenv`s it so
   Plex's glibc helper children are unaffected.

Full build + install + uninstall instructions: **[`linux/README.md`](linux/README.md)**.

## Quick start

```bash
cd linux
bash build.sh          # produces plexmediaserver_crack.so (musl), prints install steps
```

## Repository layout

| Path | What |
|------|------|
| `linux/hook.cpp` / `hook.hpp` | hooking engine: `/proc`-free module discovery (`dl_iterate_phdr`), signature scan, trampoline, feature logic, full feature-UUID catalog |
| `linux/main.cpp` | library constructor (`unsetenv` + `hook()`) |
| `linux/plex-crack-wrapper.sh` | systemd `ExecStart` launcher that scopes `LD_PRELOAD` to the Plex process |
| `linux/readbitset.py` | verifier: dumps the live feature bitset from a running PMS |
| `linux/build.sh` | musl build via `zig` (auto-downloaded) with an ABI sanity gate |
| `linux/Zydis.{c,h}` | vendored [Zydis](https://github.com/zyantific/zydis) disassembler (MIT) |
| `AGENTS.md` | architecture / RE notes |

## Not in this repo (by design)

The copyrighted Plex binaries (`Plex Media Server`, `libsoci_core.so`) and the
IDA Pro databases (`*.i64`, `*.id0`, …) are intentionally **git-ignored** — they
are large and not ours to distribute. Point your own analysis tools at your own
Plex install.
