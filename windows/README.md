<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
# Plex_Patch — Windows x64

Feature-unlock patch for **Plex Media Server** on **Windows x64**. Port of the
Linux `LD_PRELOAD` approach to a DLL injector model.

> ⚠️ **Disclaimer** — Educational / reverse-engineering only, on software you
> legally own and run yourself. No Plex code is shipped. Use at your own risk.

---

## How it works

```
plex_inject.exe ──CreateRemoteThread(LoadLibraryA)──▶ Plex Media Server.exe
                                                           │
                                                     plex_patch.dll (DllMain)
                                                           │
                          ┌────────────────────────────────┘
                          ▼
                   1. Parse PE image (base, .text, .data bounds)
                   2. Version guard: scan for "1.43.2.10687" — refuse if wrong
                   3. Resolve g_feature_bitset via RVA 0x1D9E670 (bounds-checked)
                   4. Immediate force: 14 × InterlockedExchange(0xFFFFFFFF)
                   5. Hook FeatureManager_set_features_from_uuids (Zydis trampoline)
                      → calls original, then re-forces all bits
                   6. Guard thread: polls 2s, re-forces if any dword reverted
```

Two complementary mechanisms keep every feature bit on:

- **Hook** (`trampoline.h`): an inline 14-byte `jmp [rip+0]` redirect on the
  populator function. After the original runs (so Plex's internal state is
  consistent), the hook atomically forces all 14 dwords to `0xFFFFFFFF`. This
  is deterministic — every MyPlex refresh immediately becomes "all-enabled."
- **Guard thread** (`feature_patch.h`): belt-and-suspenders. Polls at 2 s
  and re-forces if any dword reverted — catches code paths that write the
  bitset outside the hooked function, or a failed hook install.

---

## Architecture

```
windows/
├── src/
│   ├── log.h              zero-alloc OutputDebugString logging
│   ├── pe_image.h         PE base, section bounds, version guard, RVA resolution
│   ├── sig_scan.h         byte-pattern scanner + RIP-relative resolver
│   ├── trampoline.h       x64 inline hook engine (14-byte, Zydis prologue decode)
│   ├── feature_patch.h    orchestration: discover → hook → force → guard
│   ├── dllmain.cpp        DLL entry (defers work to a thread: loader-lock safe)
│   └── injector.cpp       attach-to-running or launch-suspended injector
├── build.bat              zig 0.13.0 build (reuses vendored Zydis)
└── README.md
```

**Layering (inward dependencies only):**

| Layer | Module | Depends on |
|-------|--------|------------|
| Primitives | `log.h` | `<windows.h>` only |
| Discovery | `pe_image.h` | `log` |
| Discovery | `sig_scan.h` | nothing (pure) |
| Engine | `trampoline.h` | `log`, Zydis |
| Orchestration | `feature_patch.h` | `pe_image`, `trampoline`, `log` |
| Entry | `dllmain.cpp` | `feature_patch` |
| Launcher | `injector.cpp` | `<windows.h>` only (standalone binary) |

### Design decisions

- **RVA + version guard** over blind signature scan as the primary discovery.
  The version string must be present in the image before any hardcoded RVA is
  used, so a wrong build gets a clean refusal, not memory corruption.
  `sig_scan.h` is included for future update-resilience.
- **Hook + guard thread** (belt and suspenders) over either alone. The hook
  catches refreshes deterministically; the guard catches edge cases and
  compensates if the hook fails. Either alone would work; together they are
  robust.
- **No Zydis for the injector.** Only the DLL links Zydis (for prologue
  decode). The injector is a small standalone binary using only kernel32.
- **`InterlockedExchange`** for all bitset writes, matching the binary's own
  atomic store sequence exactly (not a memset; each dword is written atomically).
- **DllMain defers to a thread.** Heavy work (PE parsing, hook install, guard
  spawn) runs outside the loader lock, avoiding the DllMain deadlock trap.

### Security

- Version guard: the patch refuses to activate on an unknown build.
- Bounds-check: RVAs are validated against the PE section table.
- No shell: the injector passes an argv list, never a command string.
- The DLL logs to `OutputDebugString`, never to disk (no file creation).

---

## Build

Requires `zig` 0.13.0 (same as the Linux build; auto-downloaded to
`toolchain/` by `build.sh`).

```bat
cd windows
build.bat
```

Outputs:
- `build\plex_patch.dll` (723 KB — includes vendored Zydis)
- `build\plex_inject.exe` (153 KB)

## Use

**Attach to a running PMS:**
```bat
build\plex_inject.exe
```

**Launch PMS through the injector (recommended — hook installs before features load):**
```bat
build\plex_inject.exe --launch "C:\Program Files\Plex\Plex Media Server\Plex Media Server.exe"
```

Copy both files to any directory; the injector resolves `plex_patch.dll`
relative to its own path.

**Verify:** open [DebugView](https://learn.microsoft.com/en-us/sysinternals/downloads/debugview)
and look for `[plex_patch INF]` messages:
```
[plex_patch INF] version guard passed (build 1.43.2.10687)
[plex_patch INF] g_feature_bitset at 0x7FF...
[plex_patch INF] hook installed on FeatureManager_set_features_from_uuids
[plex_patch INF] guard thread started (interval 2000 ms)
[plex_patch INF] feature bitset forced after set_features
```

## Known values (build 1.43.2.10687)

| Symbol | RVA | Size | IDB name |
|--------|-----|------|----------|
| `g_feature_bitset` | `0x1D9E670` | 56 B (14 × DWORD) | `g_feature_bitset` |
| `FeatureManager_set_features_from_uuids` | `0x0BC8060` | — | `FeatureManager_set_features_from_uuids` |
| `g_feature_uuid_code_table` | `0x19C9600` | ~2.2 KB (111 × 20 B) | `g_feature_uuid_code_table` |
| `FeatureManager_refresh_from_myplex` | `0x0BC6D10` | — | `FeatureManager_refresh_from_myplex` |

For a new PMS build: update `kExpectedVersion`, `kBitsetRVA`, and `kPopulatorRVA`
in `feature_patch.h`, or add a signature-scan fallback using `sig_scan.h`.

---

## Differences from the Linux version

| Aspect | Linux (`src/hook.cpp`) | Windows (`windows/`) |
|--------|----------------------|---------------------|
| Injection | `LD_PRELOAD` | `CreateRemoteThread` + `LoadLibraryA` |
| Module discovery | `dl_iterate_phdr` | PE header parsing (`GetModuleHandle`) |
| Memory protection | `mmap` / `mprotect` | `VirtualAlloc` / `VirtualProtect` |
| Cache flush | not needed (x86 coherent) | `FlushInstructionCache` (required by API) |
| Bitset storage | 14 × uint64 (libstdc++ `std::bitset`) | 14 × uint32 (MSVC `std::bitset`) |
| Guard thread | not needed (hook-only on Linux) | 2 s poll (belt-and-suspenders) |
| Disassembler | Zydis (vendored, same) | Zydis (vendored, same) |
