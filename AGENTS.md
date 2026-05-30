# AGENTS.md - Plex_Patch

## Project Overview

Binary patching/hooking library for Plex Media Server (`libsoci_core.so`) on Linux x86-64. Enables premium/Plex Pass features without subscription by intercepting feature availability checks.

## Architecture

- **Platform**: Linux x86-64 only (uses `/proc/self/maps`, `mmap`, `mprotect`)
- **Language**: C++17
- **Dependencies**: Zydis disassembler library (included via `Zydis.h`)
- **Target**: `libsoci_core.so` - Plex Media Server's core shared library

## How It Works

1. `hook()` entry point scans the loaded binary via `/proc/self/maps`
2. `sig_scan()` locates target functions using byte patterns with wildcards
3. `create_hook()` installs trampolines that redirect execution to hook functions
4. Hook functions intercept feature checks and return `true` for premium features

## Hooked Functions

| Original Function | Hook | Purpose |
|---|---|---|
| `is_feature_available` | `hook_is_feature_available` | Main feature gate |
| `map_find` | `hook_map_find` | Alternative feature lookup |
| `bitset_init` | `hook_bitset_init` | Atomic feature bitset (post-2024/08/13) |
| `is_user_feature_set` | `hook_is_user_feature_set` | User feature flags |

## Key Files

- `hook.cpp` - All implementation (452 lines)
- `hook.hpp` - Public API declarations
- `libsoci_core.so` - Target binary for reverse engineering
- `libsoci_core.so.i64` - IDA Pro database for analysis

## Development Notes

- No build system present - compile with g++ targeting x86-64 Linux
- Signature patterns are version-specific - may break with PMS updates
- The `bitset_init` hook (post-August 2024) supersedes older hooks - returns early if found
- Feature GUIDs in `g_features` map correspond to internal Plex feature flags

## Signature Patterns

Patterns use hex bytes with `?` wildcards:
- `?` = single byte wildcard
- `??` = two byte wildcard
- Spaces are ignored

Example: `"55 48 89 E5 48 8B 07 48 85 C0 74 6A"` matches `is_user_feature_set`
