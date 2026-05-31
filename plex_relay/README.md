<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
# plex_relay

A reverse-engineered, runnable reimplementation of the **Plex Media Server**
`RelayController` — the component that makes a server reachable remotely by
opening a **reverse SSH tunnel out to a Plex-operated relay host**.

Reconstructed from the `Plex Media Server` **1.43.2.10687** binary (Linux
x86-64). Ships **no Plex code**, embeds no keys, and authenticates to nothing on
its own. It is a behavioural model for interoperability research on
infrastructure **you operate yourself**.

---

## What Plex Relay does (reversed)

```
plex.tv ──"startRelay"(host,port)──▶ ServerEventManager_handle_pubsub_event (0xF5BE3C)
                                          │  gate: signed_in && published && relay_enabled
                                          ▼
                                    RelayController_connect (0x12307F2)
                                          │ 1 dedup an already-active tunnel for host
                                          │ 2 refresh relay host key (≤24h cache)
                                          │ 3 pin [host]:443 in relayHostKey.txt
                                          │ 4 spawn the ssh reverse tunnel
                                          │ 5 arm the 300s inactive-connection reaper
                                          ▼
   ssh -p <port> -N -R 0:127.0.0.1:<PMS port>
       -o UserKnownHostsFile=<datadir>/relayHostKey.txt
       -o LogLevel=VERBOSE -o PreferredAuthentications=password
       -o PubkeyAuthentication=no -l <myplex-id> -F /dev/null  <relay-host>
   password = MyPlex token, delivered via the PLEXTOKEN env var + SSH_ASKPASS
```

The relay binds an ephemeral remote port (`-R 0:…`) and forwards inbound
remote-client traffic back down the tunnel to the local PMS service. The relay
host's SSH key is **pinned**: PMS downloads it at most once per day from
`https://downloads.plex.tv/relay/relay_v1.pub` and writes a per-endpoint
known_hosts line. `relayHostKey.txt` doubles as PMS's cache and the file handed
to `ssh` (the `#` lines are valid known_hosts comments).

---

## Architecture

High cohesion (one reason to change per module) and low coupling (dependencies
point inward to abstractions, never outward to I/O):

```
cli            composition root / argument parsing
 └─ controller   orchestration; depends ONLY on the two protocols below
     ├─ store        HostKeyTrust  ── composes ↓↓
     │   ├─ keys        RelayKeyProvider  (HTTPS fetch + TTL cache)
     │   └─ cache       HostKeyCache      (relayHostKey.txt, atomic, 0o600)
     └─ tunnel       TunnelFactory ── build_ssh_argv (pure) + SSH_ASKPASS + child process
 models          immutable domain values + parsing  (no I/O, thread-safe)
 config          immutable, validated configuration
 errors          one rooted exception hierarchy
```

**Dependency inversion.** `RelayController` names what it needs as `Protocol`s —
`HostKeyTrust` (store) and `TunnelFactory`/`Tunnel` (tunnel) — and is handed
concrete adapters by `RelayController.from_config`, the single composition root.
Every external concern (HTTP, filesystem, subprocess, clock) sits behind an
injected seam, so the orchestration is unit-tested with fakes and **no network,
disk, or process is touched** in the suite.

| Module | Responsibility | Depends on |
| --- | --- | --- |
| `errors` | exception taxonomy | — |
| `models` | `HostKey`, `RelayKey`, `parse_relay_pub`, endpoint formatting | `errors` |
| `config` | frozen, validated `RelayConfig` (token redacted from `repr`) | `errors` |
| `keys` | fetch `relay_v1.pub` (HTTPS-only, byte-capped, timed) + TTL cache | `errors`, `models` |
| `cache` | load/save `relayHostKey.txt` atomically at `0o600` | `errors`, `models` |
| `store` | `HostKeyManager`: compose key + cache, pin endpoints | `keys`, `cache`, `models` |
| `tunnel` | `build_ssh_argv` (pure) + askpass + `SubprocessTunnel` | `config`, `errors` |
| `controller` | connect / reap / stop lifecycle, thread-safety | the protocols above |
| `cli` | wire adapters, parse args | everything |

### Binary → code map

| Binary symbol | Address | Code |
| --- | --- | --- |
| `RelayController` ctor (loads cache) | `0x122FDDA` | `HostKeyCache.load` + `HostKeyManager.__init__` |
| `RelayController_connect` | `0x12307F2` | `RelayController.connect` + `store` + `tunnel` |
| stopRelay | `0x123068C` | `RelayController.stop` |
| inactive-connection reaper (300s) | `0x12320EE` | `RelayController.reap_once` / `_reaper_tick` |
| `relayHostKey.txt` path | `0x1231FEE` | `RelayConfig.cache_path` |
| `startRelay` PubSub dispatch | `0xF5BE3C` | `RelayController.start_relay` |

---

## Error model

All failures derive from `RelayError`, so callers catch the subsystem broadly or
a specific mode. Adapter exceptions (`urllib`, `OSError`, `subprocess`) are
caught at the boundary and re-raised as domain errors — they never leak.

- `ConfigError` — invalid configuration (raised eagerly in `RelayConfig`).
- `RelayKeyError` — relay key fetch/parse (non-HTTPS, oversize, transport, bad format).
- `HostKeyCacheError` — unreadable/malformed cache (load auto-rebuilds, as PMS does).
- `TunnelError` — `ssh` could not be launched.

**Contract:** `connect()` *raises* on failure (library callers decide).
`start_relay()` — the plex.tv event entry — is *resilient*: it logs and returns
`False`, mirroring PMS so a bad event can't kill an event loop.

---

## Security

- **Credential never on the command line.** The token is passed to `ssh` only
  via the `PLEXTOKEN` env var, read by a generated `SSH_ASKPASS` helper. The
  helper is mode `0o700` and removed on stop *and* via a `weakref.finalize`, so a
  crash can't leak it. `RelayConfig` excludes the token from `repr`.
- **Fetch hardening.** The relay-key URL is HTTPS-only by default (configurable
  URLs are an SSRF surface), the response is byte-capped, and the request is
  time-limited.
- **Trust-store integrity.** `relayHostKey.txt` is written atomically at `0o600`;
  it pins the host key `ssh` verifies, so it must not be world-writable.
- **No shell.** Processes are spawned from an argv list, never a shell string.

---

## Performance & scale

- **At most one key fetch per TTL**, behind a lock, on a **monotonic** clock
  (immune to wall-clock jumps).
- **Disk writes only on change** — re-pinning an unchanged endpoint is a no-op.
- **O(1) liveness** via `Popen.poll()`; the reaper is a single background
  `threading.Timer` that sweeps O(n) connections every 300s and reschedules
  itself only while connections remain (idle controllers spawn no timers).
- **Immutable domain + value objects** are freely shareable across threads;
  mutable state lives behind one `RLock`.

---

## Install & test

```bash
cd plex_relay
python -m pip install -e ".[test]"
python -m pytest -q          # 51 tests + 1 POSIX-only; no network, no ssh
```

## Use

```bash
export PLEX_RELAY_TOKEN=...                       # keep the secret off the cmdline
plex-relay show    --host RELAY_HOST --user MY_ID # dry run: resolve key + print argv
plex-relay connect --host RELAY_HOST --user MY_ID --local-port 32400
```

```python
from plex_relay import RelayConfig, RelayController

with RelayController.from_config(RelayConfig(token="…", ssh_user="my-id")) as ctrl:
    ctrl.start_relay("relay.example.net", 443)   # gated + resilient, like PMS
```

For tests or custom transports, bypass the composition root and inject your own
collaborators: `RelayController(config, hostkeys=…, tunnels=…)`.

---

## Fidelity & limitations

- **Faithful:** ssh argv (order + flags), the `relayHostKey.txt` format, the 24h
  key cache, per-endpoint pinning, the dedup / 300s reaper lifecycle, and the
  `startRelay` gating.
- **Adapted, with intent:** the known_hosts endpoint is keyed by the actual ssh
  port (the binary hardcodes `:443`); the local forward target is configurable
  (the binary reads the PMS port from its own config); TTLs use a monotonic clock.
- **POSIX only:** password delivery uses `SSH_ASKPASS`, which Windows OpenSSH
  does not honour.
- **Not a turnkey relay swap:** this is only the *server→relay* leg. Plex brokers
  both ends, so pointing it at your own relay also needs a relay `sshd` you
  control plus client-discovery redirection (see the parent project's notes).
