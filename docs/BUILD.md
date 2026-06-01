# Building & installing plexmediaserver_crack (Linux)

## How this works (read this first)

Plex Media Server ships and runs against its **own bundled musl libc + libgcompat**
(`/usr/lib/plexmediaserver/lib/{libc.so,ld-musl-x86_64.so.1,libgcompat.so.0}`),
**not** the host's glibc. Two consequences drive everything below:

- **Build target must be musl.** A glibc-built `.so` fails to load into Plex: the
  loader can't relocate glibc-only symbols (`__isoc23_strtol`, `arc4random`,
  `*_chk`, `_dl_find_object`) and PMS exits 127. We cross-compile with **zig**
  (`-target x86_64-linux-musl`), which bundles musl + libc++ and statically links
  the C++ runtime, leaving only musl libc references that Plex's `libc.so` satisfies.
- **Inject with `LD_PRELOAD`, never `patchelf`.** `patchelf --add-needed` rewrites
  the 22 MB BIND_NOW/PIE binary's program headers in a way musl's loader cannot
  tolerate, which **corrupts the executable** (instant SIGSEGV on start). `LD_PRELOAD`
  touches nothing on disk and is trivially reversible.

The crack hooks `FeatureManager_apply_feature_list_xml` and forces all 14 entries
of `g_feature_bitset_slots` on (the "Godmode" approach), enabling every feature
including Plex Pass (feature code 92, slot 11).

## Prerequisites

A Linux environment (native or WSL). `build.sh` will download a pinned `zig`
toolchain on first run, so you only need `curl` + `tar`/`xz` available (or `zig`
already on `PATH`, or pass `ZIG=/path/to/zig`). No cmake/g++/glibc toolchain needed.

## Build

```bash
bash build.sh
```

Output: `build/plexmediaserver_crack.so` (a musl shared object). The script runs an ABI
sanity check and refuses to emit a `.so` that references any glibc-only symbol.
It prints the install steps below on success.

## Install (native systemd installs)

The launcher `plex-crack-wrapper.sh` sets `LD_PRELOAD` **after** `/bin/sh` is
already running and just before it `exec`s Plex, so only the (musl) Plex process
is preloaded. The crack's constructor also calls `unsetenv("LD_PRELOAD")`, so the
glibc `/bin/sh` helper children Plex spawns (Tuner Service, Script Host) are
unaffected.

```bash
# 1. Place the artifacts on the Plex host:
install -o plex -g plex -m644 build/plexmediaserver_crack.so /usr/lib/plexmediaserver/lib/
install -m755 scripts/plex-crack-wrapper.sh /usr/local/bin/

# 2. Drop-in that swaps ExecStart for the wrapper:
mkdir -p /etc/systemd/system/plexmediaserver.service.d
printf '[Service]\nExecStart=\nExecStart=/usr/local/bin/plex-crack-wrapper.sh\n' \
  > /etc/systemd/system/plexmediaserver.service.d/override.conf

# 3. Apply:
systemctl daemon-reload
systemctl restart plexmediaserver
```

### Verify

```bash
systemctl is-active plexmediaserver                 # -> active
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:32400/identity   # -> 200
# Confirm the .so is mapped into the process:
PID=$(systemctl show -p MainPID --value plexmediaserver)
grep -F plexmediaserver_crack.so /proc/$PID/maps
```

`scripts/readbitset.py <pid>` (root) dumps the live feature bitset; all 14 slots should
read `0xffffffffffffffff` when the hook is active.

## Uninstall

```bash
rm -f /etc/systemd/system/plexmediaserver.service.d/override.conf
rm -f /usr/lib/plexmediaserver/lib/plexmediaserver_crack.so /usr/local/bin/plex-crack-wrapper.sh
systemctl daemon-reload && systemctl restart plexmediaserver
```

### Recovering a binary already corrupted by patchelf

If a prior attempt ran `patchelf` against the Plex binary and PMS now SIGSEGVs on
start, restore the pristine executable:

```bash
apt-get install --reinstall -y plexmediaserver
# If the repo isn't configured, fetch the exact installed version and dpkg -i it:
V=$(dpkg-query -W -f='${Version}' plexmediaserver)
curl -fL -o /tmp/pms.deb "https://downloads.plex.tv/plex-media-server-new/$V/debian/plexmediaserver_${V}_amd64.deb"
dpkg -i /tmp/pms.deb
systemctl reset-failed plexmediaserver && systemctl restart plexmediaserver
```

## Notes

- x86-64 Linux only.
- "Godmode": enables ALL features regardless of GUID. The full feature UUID
  catalog is in `src/hook.cpp` (`kFeatureGuidCatalog`) for reference.
- Signature patterns in `hook()` are version-specific; you may need to re-verify
  them after a PMS update.
- For Docker (`plexinc/pms-docker`, `lscr.io/linuxserver/plex`), see
  [`DOCKER.md`](DOCKER.md) — same `LD_PRELOAD`-on-the-PMS-exec principle,
  but the s6 service plumbing is different per image.
- For intro/credit detection: Settings -> Library -> Marker source -> "local detection only".
