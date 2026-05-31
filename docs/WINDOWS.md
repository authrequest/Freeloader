# Windows patching guide

Top-level index for the Windows x64 Plex Media Server work in this repo.

## What exists

The Windows implementation lives in [`../windows/`](../windows/) and contains:

- `plex_patch.dll` — injected payload that forces the feature bitset on
- `plex_inject.exe` — injector that attaches to or launches `Plex Media Server.exe`
- a small engine split into:
  - `pe_image.h` — PE parsing, section bounds, RVA resolution, version guard
  - `sig_scan.h` — byte-pattern scanning helpers
  - `trampoline.h` — Zydis-based x64 inline hook engine
  - `feature_patch.h` — bitset forcing, populator hook, guard thread

## Current target

Pinned to:

- **Plex Media Server** `1.43.2.10687-563d026ea`
- **Platform**: Windows x64

The patch uses a **version guard** and **bounds-checked RVA resolution** before
it touches the target process.

## Build

From `windows/`:

```bat
build.bat
```

This produces:

- `build\plex_patch.dll`
- `build\plex_inject.exe`

The build reuses the repo's vendored Zydis and Zig toolchain conventions.

## Run

Attach to a running PMS:

```bat
build\plex_inject.exe
```

Or launch PMS through the injector:

```bat
build\plex_inject.exe --launch "C:\Program Files\Plex\Plex Media Server\Plex Media Server.exe"
```

## Where to read next

- Detailed Windows architecture and usage: [`../windows/README.md`](../windows/README.md)
- Linux build/install flow: [`BUILD.md`](BUILD.md)
- Repo overview: [`../README.md`](../README.md)

## Notes

- `windows/build/` artifacts are intentionally git-ignored.
- The original incompatible `.i64` was left untouched; the working Windows IDB
  was rebuilt in a writable analysis directory during RE.
