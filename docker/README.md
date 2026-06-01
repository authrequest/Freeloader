# Docker support for plexmediaserver_crack

Patches Plex Media Server running in Docker — both the official
[`plexinc/pms-docker`](https://hub.docker.com/r/plexinc/pms-docker) image
and the community [`lscr.io/linuxserver/plex`](https://hub.docker.com/r/linuxserver/plex)
image — using the same `LD_PRELOAD`-on-the-PMS-exec mechanism as the native
systemd install. See the top-level [README](../README.md) and the full guide
[`docs/DOCKER.md`](../docs/DOCKER.md) for the why and the troubleshooting.

## Build

From the project root:

```bash
# Official image (plexinc/pms-docker)
docker build -f docker/Dockerfile.plexinc -t plex-crack:plexinc .

# Community image (lscr.io/linuxserver/plex)
docker build -f docker/Dockerfile.linuxserver -t plex-crack:lsio .
```

The first build downloads `zig 0.13.0` and the chosen Plex base image.
Subsequent builds reuse cached layers until `src/`, `third_party/`, or
`build.sh` change. Pin the base with `--build-arg PLEX_BASE_IMAGE=...` if
you need reproducibility across PMS updates.

## Run

```bash
# plexinc (no PUID/PGID; the image runs PMS as the upstream 'plex' user)
docker run -d --name plex --network=host \
    -v /srv/plex/config:/config \
    -v /srv/plex/data:/data \
    plex-crack:plexinc

# linuxserver (honor PUID/PGID so /config and /data chown correctly on first start)
docker run -d --name plex --network=host \
    -e PUID=$(id -u) -e PGID=$(id -g) \
    -e TZ=America/Los_Angeles \
    -v /srv/plex/config:/config \
    -v /srv/plex/data:/data \
    plex-crack:lsio
```

Then:

```bash
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:32400/identity   # -> 200
```

## Patch in place (no rebuild)

If you already have a Plex container running and don't want to rebuild
the image or recreate the container, `plex-docker-patch.sh` applies the
same patch to a live container — no `docker build` needed, original
image untouched, original container + its volumes preserved. Revertible
via `uninstall` (a `.orig` copy of the s6 `run` file is kept).

```bash
# Default container name: "plex"
./docker/plex-docker-patch.sh install

# Custom container name
./docker/plex-docker-patch.sh install my-plex

# Revert (restores the s6 run file from its .orig)
./docker/plex-docker-patch.sh uninstall my-plex

# Status
./docker/plex-docker-patch.sh status my-plex
```

The script auto-detects the base image (plexinc vs LSIO) by reading the
s6 `run` file content inside the container, so the same `.so` and
`wrapper.sh` are used in both cases. Zig must be available on the host
(the script invokes `build.sh`); a pre-existing
`build/plexmediaserver_crack.so` is reused.

### When to use which

- **Dockerfile build** (the `docker build` flow above) — best for
  repeat deploys, multi-host, CI/CD, immutable images. You commit a
  patched image and ship it.
- **`plex-docker-patch.sh`** — best for one-off patching of a running
  container you don't want to touch. Modifies the live container's
  filesystem; fully revertible via `uninstall`.

## What's where

| File | Purpose |
|------|---------|
| `Dockerfile.plexinc` | Multi-stage build → patched `plexinc/pms-docker` |
| `Dockerfile.linuxserver` | Multi-stage build → patched `lscr.io/linuxserver/plex` |
| `wrapper.sh` | In-container launcher (env → `LD_PRELOAD` last → `exec` PMS) |
| `plex-docker-patch.sh` | In-place patcher for a running container (`install` / `uninstall` / `status`) |

For docker-compose, signature drift, verification with
`scripts/readbitset.py`, uninstall, and troubleshooting, see
[`../docs/DOCKER.md`](../docs/DOCKER.md).

### Quick troubleshooting (in-place patcher)

| Symptom | Likely cause | First check |
|---|---|---|
| `install` says `ERROR: docker not on PATH` | docker CLI not installed or user not in `docker` group | `docker version` (must run as you) |
| `install` says `could not find s6 svc-plex run file` | upstream image changed its s6 layout | `docker exec <name> ls -la /etc/s6-overlay/s6-rc.d/svc-plex/ /etc/services.d/plex/` — open an issue with output |
| `install` succeeds but `.so is NOT in /proc/$PID/maps` | PMS exited 127 (loader failure) | `docker logs <name> \| tail -50` — usually a glibc `.so` got in (rebuild with `build.sh`) |
| `status` shows `PATCH IS NOT ACTIVE` after `install` | run file wasn't rewritten (e.g., readonly layer) or container wasn't restarted | `docker exec <name> cat /etc/s6-overlay/s6-rc.d/svc-plex/run` — should print `exec .../plex-crack-wrapper.sh` |
| `uninstall` says `no run.orig found` | `.orig` was deleted, or the run file was never backed up (e.g., you ran an older version of the script) | restore manually: `docker cp <upstream-image>:/etc/s6-overlay/s6-rc.d/svc-plex/run <name>:/etc/s6-overlay/s6-rc.d/svc-plex/run` |

For deeper diagnostics (PMS exit 127, glibc vs musl ABI, LSIO `/config`
ownership, signature drift on PMS updates), see
[`../docs/DOCKER.md`](../docs/DOCKER.md#troubleshooting).
