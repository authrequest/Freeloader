# Building & running the Plex patcher in Docker (Linux x86-64)

The same `LD_PRELOAD`-on-the-PMS-exec trick the native systemd install uses
([`BUILD.md`](BUILD.md)) translates to Docker: build a patched Plex image,
run it as you would the upstream image, and the (musl) PMS process is
preloaded with the crack without affecting the (glibc) s6 init, helper
children, or transcoders.

## How this works (read this first)

Three constraints are identical to the native install:

1. **Build target is musl** — Plex bundles and runs against its own musl
   libc. A glibc `.so` won't load. We cross-compile with `zig` in a
   multi-stage build (`debian:bookworm-slim` builder, Plex base image
   runtime).
2. **Inject with `LD_PRELOAD`** — `patchelf` corrupts the PIE under musl's
   loader. The patch is at-rest: drop a `.so` next to PMS in the image and
   have the s6 service exec PMS through a thin wrapper that exports
   `LD_PRELOAD` only at exec time.
3. **Scope `LD_PRELOAD` to the PMS exec** — the wrapper sets `LD_PRELOAD`
   *after* any glibc child work and only for the final `exec`. The `.so`'s
   constructor (see [`../src/main.cpp`](../src/main.cpp)) calls
   `unsetenv("LD_PRELOAD")`, so PMS's glibc helper children (Tuner, Script
   Host, transcoders) are unaffected.

The only difference between the two Dockerfiles is how the s6 service for
Plex is replaced — the `.so` and `wrapper.sh` are identical. In both cases
the upstream run file is backed up as `run.orig` for forensics.

## Prerequisites

- Docker Engine 20.10+ with BuildKit enabled (`DOCKER_BUILDKIT=1` or
  BuildKit as the default builder in recent Docker). Both Dockerfiles use
  `# syntax=docker/dockerfile:1.7`.
- An x86_64 Linux host (or any host with `docker buildx` configured for
  `linux/amd64`).
- A place to keep PMS state on the host. We recommend `/srv/plex/config`
  and `/srv/plex/data`; the Dockerfiles don't bake any of this in.

## Build

From the project root:

```bash
# Official (plexinc/pms-docker)
docker build -f docker/Dockerfile.plexinc -t plex-crack:plexinc .

# Community (lscr.io/linuxserver/plex)
docker build -f docker/Dockerfile.linuxserver -t plex-crack:lsio .
```

The first build downloads `zig 0.13.0` and the chosen Plex base image.
Subsequent builds reuse cached layers until `src/`, `third_party/`, or
`build.sh` change.

### Pinning the base image

Both Dockerfiles accept a build-arg to pin the upstream Plex image:

```bash
docker build -f docker/Dockerfile.plexinc \
    --build-arg PLEX_BASE_IMAGE=plexinc/pms-docker:1.42.1.1007-7e0e6c83c \
    -t plex-crack:plexinc-1.42 .
```

The sanity `RUN` in stage 2 verifies the upstream layout before the patch
layers are added — if plexinc or LSIO moves the PMS binary, the `lib/`
directory, or the s6 service file, the build fails *here* with a clear
message rather than the container failing mysteriously at runtime.

You can also stamp the patched image with a version:

```bash
docker build -f docker/Dockerfile.plexinc \
    --build-arg PATCH_VERSION=v1.2.3 \
    -t plex-crack:plexinc-v1.2.3 .
```

`PATCH_VERSION` ends up in the `plex_patch.version` OCI label and is
visible via `docker inspect`.

## Run

### docker run

```bash
# Official (plexinc/pms-docker). No special env; plex runs as the upstream
# 'plex' user.
docker run -d --name plex --network=host \
    -v /srv/plex/config:/config \
    -v /srv/plex/data:/data \
    plex-crack:plexinc

# Community (linuxserver). Honor the LSIO PUID/PGID convention so /config
# and /data are chowned correctly on first start.
docker run -d --name plex --network=host \
    -e PUID=$(id -u) -e PGID=$(id -g) \
    -e TZ=America/Los_Angeles \
    -v /srv/plex/config:/config \
    -v /srv/plex/data:/data \
    plex-crack:lsio
```

`--network=host` is what Plex expects for direct LAN access; bridge
networking works too if you publish `32400/tcp` (and `3005/tcp`, `8324/tcp`,
`32469/udp` for Bonjour/avahi, etc.) — see the Plex docs.

### docker compose

```yaml
services:
  plex:
    image: plex-crack:lsio
    container_name: plex
    network_mode: host
    environment:
      - PUID=1000
      - PGID=1000
      - TZ=America/Los_Angeles
    volumes:
      - /srv/plex/config:/config
      - /srv/plex/data:/data
    restart: unless-stopped
```

For `plex-crack:plexinc`, drop the `PUID`/`PGID` lines.

## Verify

```bash
# 1. PMS is up.
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:32400/identity   # -> 200

# 2. The .so is mapped into the PMS process. s6 runs PMS as a child, so
#    find it first.
docker exec plex ps -ef | grep 'Plex Media Server' | grep -v grep
# Take the PID (first column) -- call it $PMS_PID.
docker exec plex grep -F plexmediaserver_crack.so /proc/$PMS_PID/maps
# -> should list the .so with r-xp perms.

# 3. Dump the live feature bitset. python3 must be available inside the
#    container (the upstream images include it; if not, `docker exec -u root
#    plex apk add python3` on alpine or `apt-get install -y python3` on
#    debian/ubuntu).
docker cp scripts/readbitset.py plex:/tmp/readbitset.py
docker exec -u root plex python3 /tmp/readbitset.py $PMS_PID
# -> all 14 slots should read 0xffffffffffffffff.
```

Step 3 is the strongest check: if any slot is `0x0…0`, the hook didn't
fire and the signature patterns in `src/hook.cpp` need to be re-verified
against the new PMS binary.

## Alternative: patch a running container in place

The `docker build` flow above builds a new patched image and starts a
new container. If you already have a Plex container running — and you
want to apply the same patch **without** rebuilding, recreating, or
pulling a new image — `docker/plex-docker-patch.sh` does it in place.
The container's filesystem is modified directly; the upstream image is
left untouched. A `.orig` copy of the s6 `run` file is kept for
`uninstall`.

```bash
# Apply (default container name: "plex").
./docker/plex-docker-patch.sh install

# Or with a custom container name.
./docker/plex-docker-patch.sh install media-plex

# Verify.
./docker/plex-docker-patch.sh status media-plex

# Revert (restores s6 run file from .orig).
./docker/plex-docker-patch.sh uninstall media-plex
```

Requirements on the host: `docker` on `PATH`, `bash 4+`, and either
`zig` available (so `build.sh` can cross-compile) or an existing
`build/plexmediaserver_crack.so` (the script reuses it if present).

### What it does, step by step

1. Verifies the container exists and is running.
2. Detects the s6 service `run` file path
   (`/etc/s6-overlay/s6-rc.d/svc-plex/run` for v3,
   `/etc/services.d/plex/run` for v2) and reads it to auto-detect the
   base image (plexinc vs linuxserver — the latter uses
   `s6-setuidgid abc`).
3. Invokes `build.sh` (skipped if `build/plexmediaserver_crack.so` is
   already present) to produce the musl `.so`.
4. `docker cp`s the `.so` and `wrapper.sh` into the container.
5. Backs up the s6 `run` file to `.orig` (preserved across re-installs).
6. Writes a new `run` file that execs the wrapper (with or without
   `s6-setuidgid abc`, depending on the base image).
7. `docker restart`s the container.
8. Waits for `http://127.0.0.1:32400/identity` to return 200.
9. Confirms `plexmediaserver_crack.so` is mapped into the PMS process
   (`/proc/$PID/maps`).
10. Prints the `readbitset.py` command for full feature-bitset
    verification.

### When to use which

- **Dockerfile build** — best for repeat deploys, multi-host, CI/CD,
  immutable images. You commit a patched image and ship it; running
  instances are disposable.
- **In-place patch** — best for one-off patching of an existing
  container you don't want to touch (complex `docker run` invocation,
  custom network, volume layout, or a single-node homelab). Modifies
  the live container's filesystem; fully revertible via `uninstall`.

Both use the same `docker/wrapper.sh` and the same
`LD_PRELOAD`-on-the-PMS-exec mechanism — only the injection plumbing
differs.

### Limitations

- x86_64 Linux hosts only (the `.so` is `x86_64-linux-musl`).
- The host must be able to `docker exec -u root` into the container.
  On rootless Docker setups this should still work since the
  container's `root` is mapped to the host's user.
- If the upstream image's s6 layout changes (plexinc or LSIO move the
  service), the script's auto-detection will fail with a clear error.
  The Dockerfile flow would fail its sanity `RUN` at build time with
  the same kind of error.
- The patch is **per-container**, not per-image. Re-creating the
  container (e.g., `docker rm` + `docker run` of the upstream image)
  reverts the patch; you have to re-run `plex-docker-patch.sh install`.
  Use the Dockerfile flow for image-baked persistence.

## Uninstall

Two ways to remove the patch, depending on which flow you used.

### Dockerfile-built image

The patch lives entirely in the image — there is no host state to undo
beyond the host's PMS data volumes, which the patch never touches.

```bash
# Stop and remove the container.
docker rm -f plex

# Remove the image.
docker rmi plex-crack:plexinc    # or plex-crack:lsio
```

To revert to the unmodified upstream image:

```bash
docker pull plexinc/pms-docker:latest
docker run -d --name plex --network=host \
    -v /srv/plex/config:/config \
    -v /srv/plex/data:/data \
    plexinc/pms-docker:latest
```

The original s6 service `run` file is preserved in the patched image as
`/etc/s6-overlay/s6-rc.d/svc-plex/run.orig` — it can be recovered by
rebuilding the patched image without the patch layer (just use the
upstream image directly).

### In-place patch

```bash
# Restore the s6 run file from its .orig and restart the container.
./docker/plex-docker-patch.sh uninstall plex

# Optional cleanup of the .so, wrapper, and .orig backup:
docker exec -u root plex rm -f \
    /usr/lib/plexmediaserver/lib/plexmediaserver_crack.so \
    /usr/lib/plexmediaserver/plex-crack-wrapper.sh \
    /etc/s6-overlay/s6-rc.d/svc-plex/run.orig
```

To verify the patch is gone:

```bash
./docker/plex-docker-patch.sh status plex
# -> "PATCH IS NOT ACTIVE (s6 run file does not point to the wrapper)."
```

## Troubleshooting

### `error while loading shared libraries: ...musl...` on PMS start

A glibc `.so` accidentally got into the image. The build's ABI sanity gate
(`grep -E "UND .*(__isoc23_|_chk$|arc4random|_dl_find_object)"` in
`build.sh`) should have caught this — if you bypassed `build.sh` and copied
in a pre-built artifact, rebuild via the Dockerfile so the gate runs.

### `patchelf: ...` warnings during build

We don't use `patchelf`. If you see this, you're running a non-canonical
build flow — see [`BUILD.md`](BUILD.md) for why `patchelf` corrupts the
PIE under musl's loader.

### `Plex Media Server` exits 127 immediately

The s6 service exec'd PMS but PMS can't find a library. Usually this is
either the `.so` ABI mismatch (rebuild) or the wrapper is in the wrong
location. Check:

```bash
docker exec plex ls -l /usr/lib/plexmediaserver/lib/plexmediaserver_crack.so
docker exec plex ls -l /usr/lib/plexmediaserver/plex-crack-wrapper.sh
docker exec plex cat /etc/s6-overlay/s6-rc.d/svc-plex/run
```

The last command should print the wrapper path (with or without
`s6-setuidgid abc` depending on which image you built).

### Feature bitset is not all 1s after `readbitset.py`

The hook didn't fire. Likely cause: PMS has a version the signature
patterns in `src/hook.cpp` don't match. Re-verify the signature patterns
against the new PMS binary; the project's RE notes are in
[`../AGENTS.md`](../AGENTS.md). After fixing the patterns, rebuild with
`--no-cache` so the builder stage re-runs:

```bash
docker build --no-cache -f docker/Dockerfile.plexinc -t plex-crack:plexinc .
```

### Container restarts in a loop, s6 logs `script /etc/s6-overlay/s6-rc.d/svc-plex/run exited 1`

The wrapper is missing or not executable, or the `.so` failed its ABI
gate. Check `docker logs plex` and the in-container paths listed above.

### LSIO: /config or /data ends up owned by root

PUID/PGID weren't set, or were set to 0. LSIO's perms init runs once and
chowns to the configured UID/GID; if PMS is later started as root (which
it would be if the s6-setuidgid was dropped from the run file), the
subsequent writes will be root-owned. The Dockerfile preserves
`s6-setuidgid abc` exactly so this shouldn't happen with a clean build —
rebuild without modifying the `cat > "${RUN_SCRIPT}"` block.

## Notes

- x86_64 Linux only — the build emits `x86_64-linux-musl` and the base
  images are amd64. There is no arm64/v3 PMS Docker image today.
- Both Dockerfiles emit OCI labels: `plex_patch.base` (the upstream image
  reference) and `plex_patch.version` (a build-time stamp, default `dev`).
- For intro/credit detection: Settings → Library → Marker source → "local
  detection only".
- The patch has no effect on PMS's network behavior, media transcoding,
  or library scanning — it only forces the in-memory feature bitset on
  after PMS applies its MyPlex feature list.
