# Plex_Patch

Reverse-engineering notes and tooling for **Plex Media Server** on **Linux
x86-64** — covering both *feature unlocking* and *remote access*.

> ⚠️ **Disclaimer** — For educational and reverse-engineering purposes, on
> software you legally run yourself. Nothing here bypasses account or server
> authentication, and **no Plex code** is included or redistributed. If you rely
> on Plex, buy a Plex Pass — it funds the developers. Use at your own risk; no
> warranty.

## What's here

| # | Component | Path | Summary |
|---|-----------|------|---------|
| 1 | **Feature-unlock patch** | `src/`, `build.sh` | `LD_PRELOAD` shared library that forces every `FeatureManager` bit on |
| 2 | **Relay RE + model** | `plex_relay/` | Reverse-engineered, runnable reimplementation of Plex's `RelayController` |
| 3 | **Remote access (no patch)** | `scripts/plex-tailnet/` | Reach your server over Tailscale/Headscale instead of Plex Relay |

Each subsystem has its own README; this page is the map.

---

## 1 · Feature-unlock patch

Plex's feature gates read a single in-memory table, `g_feature_bitset_slots`
(14 × `uint64`), populated from the MyPlex feature list. A feature with internal
code `C` is "available" iff `slots[C >> 3] & (1 << (C & 7))`. The patch (`src/`)
is a small shared library whose constructor finds
`FeatureManager_apply_feature_list_xml`, installs a trampoline, and forces all 14
slots to `0xFF…FF` after Plex applies its feature list — so every feature
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

```bash
bash build.sh          # -> build/plexmediaserver_crack.so (musl); prints install steps
```

Full build / install / uninstall guide: **[`docs/BUILD.md`](docs/BUILD.md)**.

## 2 · Plex Relay — `plex_relay/`

A study of how Plex makes a server reachable when no direct connection exists: it
opens a **reverse SSH tunnel to a Plex-operated relay host**. `plex_relay/` is a
clean-room, dependency-free Python reimplementation of the `RelayController`
translation unit (key fetch + 24h cache, `relayHostKey.txt` pinning, the ssh
tunnel, the 300s reaper), with a typed error model, injected I/O seams, and a
full test suite. See **[`plex_relay/README.md`](plex_relay/README.md)**.

## 3 · Remote access without patching — `scripts/plex-tailnet/`

The pragmatic alternative to both Plex Relay and patching: put the server and its
viewers on a **Tailscale/Headscale mesh VPN** and let Plex publish the tailnet
address. Includes an idempotent setup script (security questionnaire, firewall
lockdown, health check), an optional self-hosted Headscale installer, and a
shared shell library. See **[`scripts/plex-tailnet/README.md`](scripts/plex-tailnet/README.md)**.

---

## Repository layout

| Path | What |
|------|------|
| `src/hook.cpp` · `hook.hpp` | hooking engine: `dl_iterate_phdr` discovery, signature scan, trampoline, feature logic, feature-UUID catalog |
| `src/main.cpp` | library constructor (`unsetenv` + `hook()`) |
| `build.sh` | musl build via `zig` (auto-downloaded) with an ABI sanity gate |
| `scripts/plex-crack-wrapper.sh` | systemd `ExecStart` launcher scoping `LD_PRELOAD` to the Plex process |
| `scripts/readbitset.py` | verifier: dumps the live feature bitset from a running PMS |
| `scripts/plex-tailnet/` | Tailscale/Headscale remote-access setup (see its README) |
| `plex_relay/` | Python reimplementation of Plex's `RelayController` (see its README) |
| `third_party/zydis/` | vendored [Zydis](https://github.com/zyantific/zydis) disassembler (MIT) |
| `docs/BUILD.md` | full build / install / uninstall guide |
| `experimental/debug_hook.c` | standalone alternate hook (legacy signature) |
| `AGENTS.md` | architecture / RE notes |

## Not in this repo (by design)

The copyrighted Plex binaries (`Plex Media Server`, `libsoci_core.so`), the IDA
Pro databases (`*.i64`, `*.id0`, …), the auto-downloaded `toolchain/`, and any
local machine config (`.mcp.json`, keys, `.env`) are intentionally
**git-ignored** — they are large, sensitive, or not ours to distribute. Point
your own analysis tools at your own Plex install.

## License

[GNU AGPL-3.0-or-later](LICENSE) © the Plex_Patch authors. Each source file
carries an `SPDX-License-Identifier: AGPL-3.0-or-later` tag.

The vendored Zydis disassembler in `third_party/zydis/` is **MIT**-licensed (see
`third_party/zydis/README.md`); its terms are preserved and unaffected.
