# SPDX-License-Identifier: AGPL-3.0-or-later
"""plex_relay -- a reverse-engineered reimplementation of the Plex Media Server
``RelayController`` (Linux x86-64, build 1.43.2.10687).

Layering (high cohesion, dependency inversion top-to-bottom):

    cli            composition root / argument parsing
    controller     orchestration; depends only on the protocols below
    store          HostKeyTrust: composes the key provider + the disk cache
      keys         relay key acquisition (HTTPS fetch + TTL cache)
      cache        relayHostKey.txt persistence (atomic, 0o600)
      tunnel       ssh argv (pure) + SSH_ASKPASS + child-process tunnel
    models         immutable domain values + parsing (no I/O)
    config         immutable, validated configuration
    errors         one rooted exception hierarchy

Binary provenance of the key symbols is documented in each module and in
``README.md``. Ships no Plex code; authenticates to nothing on its own.
"""
from .config import RelayConfig
from .controller import RelayController
from .errors import (
    ConfigError,
    HostKeyCacheError,
    RelayError,
    RelayKeyError,
    TunnelError,
)
from .models import HostKey, RelayKey, known_hosts_endpoint, parse_relay_pub
from .tunnel import build_ssh_argv

__all__ = [
    "RelayConfig",
    "RelayController",
    "HostKey",
    "RelayKey",
    "known_hosts_endpoint",
    "parse_relay_pub",
    "build_ssh_argv",
    "RelayError",
    "ConfigError",
    "RelayKeyError",
    "HostKeyCacheError",
    "TunnelError",
]
__version__ = "0.2.0"
