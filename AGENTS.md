# AGENTS.md - Plex_Patch

## Project overview

Reverse-engineering notes and a runtime patch for **Plex Media Server** on
**Linux x86-64** (and **ARM64/aarch64**). It hooks Plex's
`FeatureManager` and forces every feature bit on, unlocking gated features.
Educational / RE use on software you run yourself; it ships no Plex code and
bypasses no account/server authentication.

## Target & runtime reality (important)

- **Target:** the main `Plex Media Server` executable (PIE). The feature machinery
  moved here from `libsoci_core.so` on post-2024/08/13 builds; `libsoci_core.so`
  is no longer the target.
- **Plex runs against its OWN bundled musl libc + libgcompat**
  (`/usr/lib/plexmediaserver/lib/`), NOT the host glibc. This drives the build and
  injection choices below.
- **ARM64:** confirmed working on aarch64-linux-musl. The ARM64 binary has the
  same musl constraint; zig cross-compilation targets `aarch64-linux-musl`.

## Architecture

### x86-64

- **Language:** C++20, plus the vendored Zydis C amalgamation for instruction
  decoding.
- **Module discovery:** `dl_iterate_phdr` used in early versions; current code
  uses `/proc/self/maps` (the constructor runs before the main PIE is loaded by
  musl, so `dl_iterate_phdr` returns 0 callbacks).
- **Hook:** `sig_scan()` finds target functions by byte-pattern signature;
  `create_hook()` installs a 14-byte `jmp [rip+0x06]` trampoline (Zydis decodes
  the prologue so relocated bytes stay valid).
- **Feature unlock effect:** after Plex applies its MyPlex feature list, the
  hook forces all 14 `g_feature_bitset_slots` qwords on (`std::bitset<896>`),
  so every feature (including Plex Pass, code 92, slot 11) reads as enabled.
- **Webhook unlock:** an additional hook targets `sub_122B2F2` (the generic
  preference getter at `0x122B2F2`). When the key is `"WebHooksEnabled"`, it
  returns `true` regardless of the actual persisted value, enabling Plex's
  built-in webhook dispatch (play/pause/stop/rate events). This is a separate
  mechanism from the feature bitset — `WebHooksEnabled` is a plain boolean
  preference, not a feature bit. Both dispatch functions (`sub_125A6D4` @
  `0x125A6D4` and `sub_125B766` @ `0x125B766`) check only this preference
  with no secondary feature-bit gate.

### ARM64 / aarch64

- **Language:** C++20. Zydis is NOT used on ARM64 (fixed 4-byte instructions,
  no length decoding needed). `Zydis.h` is conditionally excluded via
  `#ifndef __aarch64__`.
- **Module discovery:** same `/proc/self/maps` parser (`get_dottext_info()`)
  works identically on ARM64.
- **Hook:** `create_hook_arm64()` installs a 16-byte
  `LDR X17, [PC, #8]; BR X17; <8-byte target>` trampoline. No Zydis needed.
  The trampoline copies 4 original instructions (16 bytes) and appends the
  same LDR+BR jump-back sequence.
- **Feature unlock effect:** same bitset-force approach — hook the
  FeatureManager constructor via ARM64 signature, then force all 14 uint64_t
  slots to `UINT64_MAX`.
- **Webhook unlock:** currently targets preference init functions (SSO-check
  prologue patterns like `ldrb w?, [x?, #0x17]`). The ARM64 callbacks are
  generic feature-return-true for now; WebHooksEnabled-specific key matching
  (like the x86-64 sub_122B2F2 hook) is pending identification of the exact
  ARM64 preference getter function.
- **Analysis tooling:** `get_arm64_sigs.py` uses Capstone to extract ARM64
  function signatures with ADRP page counting; `check_webhooks.py` dumps
  ADRP+ADD string loads inside candidate functions; `disasm_final.py` /
  `disasm_key_areas.py` are earlier (broken) Capstone analysis scripts.

### ARM64 function signatures (from get_arm64_sigs.py)

| VAddr | Signature | Notes |
|-------|-----------|-------|
| `0x10dd904` | `FD 7B BA A9 FC 6F 01 A9 FA 67 02 A9 F8 5F 03 A9 F6 57 04 A9 F4 4F 05 A9 FD 03 00 91 FF 43 09 D1` | Preference init function — 6 stp pairs (save 12 regs), sub sp,#0x250. The function at this address loads "WebHooksEnabled" string via ADRP+ADD at 0x10e0eac. |
| `0x658120` | `FD 7B BD A9 F5 0B 00 F9 F4 4F 02 A9 FD 03 00 91 F3 03 01 AA F4 03 00 AA 61 00 80 52 E0 03 13 AA` | FeatureManager class function — saves 2 regs, calls with x0/x1, loads FeatureManager strings from page 0x1B7000. |
| `0x658070` | `FD 7B 03 A9 F4 4F 04 A9 FD C3 00 91` | FeatureManager constructor-like — stp x29,x30,[sp,#-0x30]! ; stp x20,x19,[sp,#0x10] ; mov x29,sp. Used as catch-all FeatureManager hook target. |
| `0xeba0b4` | `FD 7B BE A9 F3 0B 00 F9 FD 03 00 91 08 5C 40 39 09 04 40 F9 F3 03 00 AA 0A 1D 00 13 5F 01 00 71` | Feature-check function — stp x29,x30,[sp,#-0x10]! ; str x19,[sp,#8] ; mov x29,sp ; ldrb w8,[x0,#0x17] (SSO check). 3 ADRP refs to page 0x1BD000 (hasPlexPass strings). |
| `0x5e4188` | `FD 7B 01 A9 FD 43 00 91 A8 65 00 B0 08 85 42 F9 49 66 00 F0 ...` | hasPlexPass checking function — 3 ADRP refs to page 0x1BD000. |
| `0x5e42c0` | `FD 7B BE A9 F4 4F 01 A9 FD 03 00 91 74 66 00 90 88 82 46 39 ...` | hasPlexPass checking function — 3 ADRP refs to page 0x1BD000. |
| `0x5e4368` | `FD 7B BE A9 F3 0B 00 F9 FD 03 00 91 73 66 00 90 68 A2 47 39 ...` | hasPlexPass checking function — 3 ADRP refs to page 0x1BD000. |

### ARM64 binary layout (from `_Plex Media Server`, text section at file offset 0x5d35bc)

| Page | Contains | Notable Strings |
|------|----------|-----------------|
| `0x1B7000` | FeatureManager strings, preference init strings | `"FeatureManager"`, `"FeatureManager: Using cached data"` (@ 0x1B72AB), `"hasPlexPass"` (@ 0x1B749B) |
| `0x1BD000` | Feature checking strings | `"playing"`, `"paused"`, `"buffering"`, `"media_css_min_assets_cache_ms"` |
| `0x363000` | Webhook/web-related strings | `"WebHooksEnabled"` (@ 0x363871) |

### ARM64 text section

- Text section: vaddr=0x5e35bc, file_offset=0x5d35bc, size=0xc05964 (~12MB)
- Built for aarch64 Linux (little-endian), PIE position-independent
- All functions use ARM64 standard prologue: `stp x29, x30, [sp, #-N]!`
- String references via ADRP+ADD pairs (PC-relative page + 12-bit offset)
- No fixed function addresses when PIE-loaded; all discovery via sig_scan

## Build & inject (details in README.md / docs/BUILD.md)

- **Build with musl** via `zig` (`-target x86_64-linux-musl`): `bash build.sh`.
  For ARM64: `bash build.sh --arm64`.
  A glibc build cannot relocate glibc-only symbols (`__isoc23_strtol`,
  `arc4random`, `*_chk`, `_dl_find_object`) in Plex's musl runtime → exit 127.
- **Inject with `LD_PRELOAD`** via `scripts/plex-crack-wrapper.sh` + a systemd

## Key files

- `src/hook.cpp` / `src/hook.hpp` — hook engine, feature logic, feature-UUID catalog
- `src/main.cpp` — library constructor (`unsetenv("LD_PRELOAD")` then `hook()`)
- `build.sh` — musl build via zig (auto-downloaded) with an ABI sanity gate
- `scripts/plex-crack-wrapper.sh` — `LD_PRELOAD` launcher scoped to the PMS process
- `scripts/readbitset.py` — live feature-bitset verifier
- `third_party/zydis/` — vendored Zydis disassembler (MIT)
- `get_arm64_sigs.py` — Capstone-based ARM64 function signature extractor with ADRP page counting
- `check_webhooks.py` — dump ADRP+ADD string loads within candidate ARM64 functions
- `experimental/debug_hook.c` — standalone alternate hook (legacy `is_feature_available` signature)

RE artifacts (the `Plex Media Server` binary, `libsoci_core.so`, and `*.i64` IDA
databases) are git-ignored and not redistributed.

## Signature patterns

Hex bytes with `?` wildcards; spaces ignored (`?` = one-byte wildcard). Patterns
are version-specific — re-verify after PMS updates.

### x86-64 signatures

| Target | Address | Signature | Notes |
|--------|---------|-----------|-------|
| `bitset_init` (modern path constructor) | dynamic | `55 48 89 E5 41 57 41 56 41 55 41 54 53 48 81 EC ? ? 00 00 49 89 FE 48 8D 9D ? ? ? ? 48 89 DF E8 ? ? ? ? 48 8B 1B 48 85 DB` | `FeatureManager_apply_feature_list_xml` — post-2024/08/13 |
| `sub_122B2F2` (preference getter) | `0x122B2F2` | `48 89 F3 4C 89 F7 0F B6 46 17 48 89 F1 84 C0` | `mov rbx, rsi; mov r14, rdi; movzx eax,[rsi+0x17]` — std::string SSO check prologue |
| `is_user_feature_set` (legacy) | dynamic | `55 48 89 E5 48 8B 07 48 85 C0 74 09` | Pre-2024/08/13 fallback |
| `is_feature_available` (legacy) | dynamic | `E8 ? ? ? ? 86 43` (call rel32 + `test al, byte ptr [rbx+3]`) | Rel32 followed, pre-2024/08/13 fallback |
| `map_find` (legacy) | dynamic | `55 48 89 E5 41 57 41 56 53 48 83 EC ? 49 89 F7 4C 8D 77` | Pre-2024/08/13 fallback |

### ARM64 signatures (built with `bash build.sh --arm64`)

| Target | Signature | Notes |
|--------|-----------|-------|
| `FeatureManager_init` (bitset constructor) | `FD 7B 03 A9 F4 4F 04 A9 FD C3 00 91` | stp x29,x30,[sp,#-0x30]! ; stp x20,x19,[sp,#0x10] ; mov x29,sp. Catches FeatureManager init to force bits on. |
| `FeatureManager_class_method` (alternative) | `FD 7B BD A9 F5 0B 00 F9 F4 4F 02 A9 FD 03 00 91 F3 03 01 AA F4 03 00 AA 61 00 80 52 E0 03 13 AA` | Saves 1 reg pair + str ; handles x0/x1 args. |
| `feature_check_sso` (feature checker) | `FD 7B BE A9 F3 0B 00 F9 FD 03 00 91 08 5C 40 39` | stp x29,x30,[sp,#-0x10]! ; str x19,[sp,#8] ; mov x29,sp ; ldrb w8,[x0,#0x17]. Hooked to always return true. |
| `pref_init` (preference init) | `FD 7B BA A9 FC 6F 01 A9 FA 67 02 A9 F8 5F 03 A9 F6 57 04 A9 F4 4F 05 A9 FD 03 00 91 FF 43 09 D1` | 6 stp pairs, sub sp,#0x250. Loads "WebHooksEnabled" string. WebHooksEnabled-specific hook pending. |
| `pref_getter_sso` (generic getter, placeholder) | `FD 7B ?? A9 ?? ?? ?? A9 FD 03 00 91 ?? 5C 40 39 ??` | Broad pattern: prologue + SSO check on any x-reg. May match the preference getter. |
