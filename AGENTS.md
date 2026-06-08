# AGENTS.md - Plex_Patch

## Project overview

Reverse-engineering notes and a runtime patch for **Plex Media Server** on
**Linux x86-64**. It hooks Plex's `FeatureManager` and forces every feature bit
on, unlocking gated features. Educational / RE use on software you run yourself;
it ships no Plex code and bypasses no account/server authentication.

## Target & runtime reality (important)

- **Target:** the main `Plex Media Server` executable (PIE). The feature machinery
  moved here from `libsoci_core.so` on post-2024/08/13 builds; `libsoci_core.so`
  is no longer the target.
- **Plex runs against its OWN bundled musl libc + libgcompat**
  (`/usr/lib/plexmediaserver/lib/`), NOT the host glibc. This drives the build and
  injection choices below.

## Architecture

- **Language:** C++20, plus the vendored Zydis C amalgamation.
- **Module discovery:** `dl_iterate_phdr` locates the main program's executable
  segment (the legacy `experimental/debug_hook.c` still uses `/proc/self/maps`).
- **Hook:** `sig_scan()` finds target functions by byte-pattern signature;
  `create_hook()` installs a 14-byte trampoline (Zydis decodes the prologue so
  relocated bytes stay valid).
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

## Build & inject (details in README.md / docs/BUILD.md)

- **Build with musl** via `zig` (`-target x86_64-linux-musl`): `bash build.sh`.
  A glibc build cannot relocate glibc-only symbols (`__isoc23_strtol`,
  `arc4random`, `*_chk`, `_dl_find_object`) in Plex's musl runtime → exit 127.
- **Inject with `LD_PRELOAD`** via `scripts/plex-crack-wrapper.sh` + a systemd
  drop-in. NEVER `patchelf --add-needed` the Plex binary — it corrupts the PIE
  under musl's loader (instant SIGSEGV); recover with a package reinstall.

## Key files

- `src/hook.cpp` / `src/hook.hpp` — hook engine, feature logic, feature-UUID catalog
- `src/main.cpp` — library constructor (`unsetenv("LD_PRELOAD")` then `hook()`)
- `build.sh` — musl build via zig (auto-downloaded) with an ABI sanity gate
- `scripts/plex-crack-wrapper.sh` — `LD_PRELOAD` launcher scoped to the PMS process
- `scripts/readbitset.py` — live feature-bitset verifier
- `third_party/zydis/` — vendored Zydis disassembler (MIT)
- `experimental/debug_hook.c` — standalone alternate hook (legacy `is_feature_available` signature)

RE artifacts (the `Plex Media Server` binary, `libsoci_core.so`, and `*.i64` IDA
databases) are git-ignored and not redistributed.

## Signature patterns

Hex bytes with `?` wildcards; spaces ignored (`?` = one-byte wildcard). Patterns
are version-specific — re-verify after PMS updates.

| Target | Address | Signature | Notes |
|--------|---------|-----------|-------|
| `bitset_init` (modern path constructor) | dynamic | `55 48 89 E5 41 57 41 56 41 55 41 54 53 48 81 EC ? ? 00 00 49 89 FE 48 8D 9D ? ? ? ? 48 89 DF E8 ? ? ? ? 48 8B 1B 48 85 DB` | `FeatureManager_apply_feature_list_xml` — post-2024/08/13 |
| `sub_122B2F2` (preference getter) | `0x122B2F2` | `48 89 F3 4C 89 F7 0F B6 46 17 48 89 F1 84 C0` | `mov rbx, rsi; mov r14, rdi; movzx eax,[rsi+0x17]` — std::string SSO check prologue |
| `is_user_feature_set` (legacy) | dynamic | `55 48 89 E5 48 8B 07 48 85 C0 74 09` | Pre-2024/08/13 fallback |
| `is_feature_available` (legacy) | dynamic | `E8 ? ? ? ? 86 43` (call rel32 + `test al, byte ptr [rbx+3]`) | Rel32 followed, pre-2024/08/13 fallback |
| `map_find` (legacy) | dynamic | `55 48 89 E5 41 57 41 56 53 48 83 EC ? 49 89 F7 4C 8D 77` | Pre-2024/08/13 fallback |
