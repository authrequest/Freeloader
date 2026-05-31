# SPDX-License-Identifier: AGPL-3.0-or-later
"""Immutable, validated runtime configuration.

Frozen so it can be shared freely across threads and never mutated behind a
collaborator's back. The relay credential is excluded from ``repr`` so it cannot
leak into logs or tracebacks.

Defaults mirror constants observed in ``RelayController_connect`` (``0x12307F2``):
relay key URL, the 24h key TTL (``86400000000`` us) and the 300s reaper
(``300000000`` us). The reverse-forward target (``0:127.0.0.1:%d``) is the PMS
service port, which the binary reads from its own config -- hence configurable.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

from .errors import ConfigError

DEFAULT_RELAY_KEY_URL = "https://downloads.plex.tv/relay/relay_v1.pub"
DEFAULT_KEY_TTL_SECONDS = 86_400.0
DEFAULT_REAP_INTERVAL_SECONDS = 300.0
DEFAULT_LOCAL_HOST = "127.0.0.1"
DEFAULT_LOCAL_PORT = 32400
CACHE_FILENAME = "relayHostKey.txt"


def _default_data_dir() -> Path:
    return Path.home() / ".plex_relay"


@dataclass(frozen=True, slots=True)
class RelayConfig:
    """Everything :class:`~plex_relay.controller.RelayController` needs to run."""

    token: str = field(repr=False)  # SSH password (PLEXTOKEN); never logged
    ssh_user: str

    local_host: str = DEFAULT_LOCAL_HOST
    local_port: int = DEFAULT_LOCAL_PORT

    relay_key_url: str = DEFAULT_RELAY_KEY_URL
    key_ttl_seconds: float = DEFAULT_KEY_TTL_SECONDS
    reap_interval_seconds: float = DEFAULT_REAP_INTERVAL_SECONDS

    data_dir: Path = field(default_factory=_default_data_dir)
    ssh_binary: str = "ssh"
    ssh_interface: str = "tailscale0"

    connect_timeout_seconds: float = 15.0
    stop_timeout_seconds: float = 5.0
    allow_insecure_key_url: bool = False

    # startRelay gating, mirroring ServerEventManager_handle_pubsub_event:
    #   signin_state == 4 && PublishServerOnPlexOnlineKey && RelayEnabled
    signed_in: bool = True
    published: bool = True
    relay_enabled: bool = True

    def __post_init__(self) -> None:
        # Frozen dataclass: normalise/validate via object.__setattr__.
        object.__setattr__(self, "data_dir", Path(self.data_dir))
        if not self.token:
            raise ConfigError("token is required (relay SSH password)")
        if not self.ssh_user:
            raise ConfigError("ssh_user is required (relay SSH login)")
        if not 0 < self.local_port < 65536:
            raise ConfigError(f"local_port out of range: {self.local_port}")
        for name in ("key_ttl_seconds", "reap_interval_seconds",
                     "connect_timeout_seconds", "stop_timeout_seconds"):
            if getattr(self, name) <= 0:
                raise ConfigError(f"{name} must be positive")

    @property
    def cache_path(self) -> Path:
        """``<data_dir>/relayHostKey.txt`` (the binary's ``sub_1231FEE``)."""
        return self.data_dir / CACHE_FILENAME

    def gating_ok(self) -> bool:
        """True iff all three startRelay preconditions hold."""
        return self.signed_in and self.published and self.relay_enabled


__all__ = ["RelayConfig", "DEFAULT_RELAY_KEY_URL", "DEFAULT_LOCAL_HOST",
           "DEFAULT_LOCAL_PORT", "CACHE_FILENAME"]
