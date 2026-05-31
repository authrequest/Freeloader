<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
# Plex over Tailscale / Headscale (no code patching)

Make a local Plex server reachable by remote users through a **mesh VPN** instead
of Plex Relay, router port-forwarding, or a binary patch. Every device that joins
your tailnet reaches the Plex host's private tailnet IP directly; WireGuard
encrypts the transport end-to-end.

```
  Remote client (Tailscale) ─────WireGuard────▶ Plex host (Tailscale)  100.x.y.z:32400
        │                                              ▲
        └── learns the server from plex.tv, which now publishes
            the custom URL  https://100.x.y.z:32400
```

---

## Layout & design

```
plex-tailnet/
├── plex-tailscale-setup.sh     # run on the Plex host: VPN + Plex config + healthcheck
├── headscale-server-setup.sh   # run on a VPS (optional): self-hosted control plane
├── lib/
│   ├── common.sh               # shared shell helpers (sourced, never executed)
│   └── plex_prefs.py           # Preferences.xml read/merge (XML lives here, not in bash)
└── README.md
```

Principles applied:

- **Low coupling / high cohesion.** Generic concerns (colour logging, dry-run
  execution, prompts, root/command guards, secret redaction) live once in
  `lib/common.sh`; each script keeps only its own orchestration. All XML editing
  is isolated in `lib/plex_prefs.py` — cohesive, reviewable, and testable on its
  own (`plex_prefs.py merge|get`), so the shell never hand-parses XML.
- **Fail fast, located.** `set -euo pipefail` plus an `ERR` trap that reports the
  failing line; tolerated failures are explicitly guarded (`|| true` / `|| warn`).
- **Idempotent & reversible.** Re-runs merge (never duplicate) settings; every
  `Preferences.xml` change is preceded by a timestamped backup and ownership is
  restored afterwards. `--dry-run` previews every action and changes nothing.
- **Secret hygiene.** Auth keys are redacted in logs **and** never routed through
  the dry-run echo. The `headscale` pre-auth key is printed once, labelled as a
  secret.

| Component | Runs on | Responsibility |
| --- | --- | --- |
| `plex-tailscale-setup.sh` | Plex host (Linux/systemd) | install/join Tailscale, security questionnaire, edit `Preferences.xml`, firewall, healthcheck |
| `headscale-server-setup.sh` | public VPS *(optional)* | install + configure Headscale, create user, mint pre-auth key |
| `lib/common.sh` | sourced | logging, `run` (dry-run), prompts, guards, redaction |
| `lib/plex_prefs.py` | invoked | merge/read `Preferences.xml` attributes |

---

## Quick start

### A) Tailscale's control plane (simplest; free for up to 3 users)

```bash
sudo ./plex-tailscale-setup.sh                 # interactive login (prints a URL)
sudo ./plex-tailscale-setup.sh --authkey tskey-auth-xxxxx   # unattended
```

Each remote user installs Tailscale (https://tailscale.com/download), joins the
same tailnet, opens Plex — done.

### B) Your own Headscale (no user limits, full control)

On a public VPS with a DNS name and ports 80/443 open:

```bash
sudo ./headscale-server-setup.sh --domain hs.example.com --user plex   # prints a key
```

On the Plex host and every client:

```bash
sudo tailscale up --login-server https://hs.example.com --authkey <preauth-key>
# Plex host can do VPN + Plex config in one go:
sudo ./plex-tailscale-setup.sh --login-server https://hs.example.com --authkey <preauth-key>
```

---

## What the Plex script changes

`Preferences.xml` is edited **while Plex is stopped** (Plex overwrites it on
shutdown), via `lib/plex_prefs.py`, after a backup, with ownership restored:

| Attribute | Change | Why |
| --- | --- | --- |
| `customConnections` | append `https://<tailscale-ip>:32400` | plex.tv publishes the tailnet address for discovery |
| `LanNetworksBandwidth` | append `100.64.0.0/10`, `fd7a:115c:a1e0::/48` | treat the tailnet as **LAN**: full quality, no remote throttle |
| `secureConnections` | your choice (default Preferred) | clean connect over the already-encrypted tunnel |
| `RelayEnabled` | `0` (if you disable Relay) | stop bouncing through Plex's relay once on the tailnet |

### Security questionnaire (interactive)

On a terminal the script asks three questions (each has a safe default; pass the
flag — or `--yes` — to skip the prompt):

| Prompt | Flag(s) | Default | Effect |
| --- | --- | --- | --- |
| Secure connections mode | `--secure required\|preferred\|disabled\|keep` | preferred | `secureConnections` |
| Disable Plex Relay? | `--disable-relay` / `--keep-relay` | disable | `RelayEnabled=0` |
| Firewall lockdown of `32400/tcp` | `--firewall none\|tailnet\|lan` | none | see below |

**Firewall lockdown** restricts Plex's port to the VPN (`tailnet`) or VPN + RFC1918
LAN (`lan`). It only ever touches `32400/tcp` (SSH stays open), acts only on an
**already-active** ufw/firewalld (never enables a firewall — that risks an SSH
lockout), and otherwise prints an equivalent `nftables` snippet.

### Health check

Runs after install, and standalone with `sudo ./plex-tailscale-setup.sh
--healthcheck` (**no changes, no root**). PASS/WARN/FAIL for: Tailscale backend +
tailnet IP, the Plex service, Plex's local API, Plex reachable at its tailnet IP,
the `customConnections` / LAN-networks / Relay values Plex actually persisted, and
the firewall posture.

Other flags: `--prefs PATH` (quote it), `--service`, `--port`, `--url-scheme`,
`--ts-iface`, `--skip-tailscale`, `--skip-plex`, `--dry-run`.

**Prerequisites:** Plex installed, **claimed**, owner signed in; Linux + systemd;
`python3` + `curl`; run as root (except `--healthcheck`).

---

## Letting other people in

1. They install Tailscale and join your tailnet/Headscale (a per-user reusable
   pre-auth key from `headscale preauthkeys create` is the easy path).
2. In Plex, **Settings → Users & Sharing**, share the libraries with their Plex
   account.
3. They sign into Plex; the server shows up over the tailnet.

### Lock guests to the Plex port with ACLs (recommended)

Headscale (`/etc/headscale/acl.hujson`, referenced by `policy.path`):

```hujson
{
  "groups": { "group:plexusers": ["alice@", "bob@"] },
  "hosts":  { "plexserver": "100.64.0.5/32" },
  "acls": [
    { "action": "accept", "src": ["group:plexusers"], "dst": ["plexserver:32400"] }
  ]
}
```

Tailscale's admin console (Access Controls) uses the equivalent `acls`/`tagOwners`.

---

## Caveats

- **2026 Plex Pass enforcement.** Reports indicate Plex now requires Plex Pass /
  Remote Watch Pass on the **server account** for *remote* streaming even over
  Tailscale. `LanNetworksBandwidth` makes Plex treat the tailnet as local (which
  historically sidestepped the cap and the entitlement gate); if your build still
  gates, the lever is on the server account, not the client. VPN connectivity
  works regardless.
- **TLS / certificates.** Capable clients reach `https://<ip>:32400` via Plex's
  auto-generated `plex.direct` hostname (valid cert). For a strict client, use
  `--secure preferred` (default) or `--url-scheme http`; WireGuard already
  encrypts the wire.
- **Headscale TLS.** `--no-tls` listens on `127.0.0.1:8080` for a reverse proxy;
  otherwise built-in Let's Encrypt needs ports 80 + 443 reachable.
- **POSIX/systemd only.** Targets Debian/Ubuntu-family Plex hosts.

Interoperability/remote-access tooling for infrastructure you operate yourself.
It ships no Plex code and bypasses no account authentication.
