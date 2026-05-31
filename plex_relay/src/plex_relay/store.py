# SPDX-License-Identifier: AGPL-3.0-or-later
"""Host-key trust management -- composes the key provider and the disk cache.

This is the seam the controller depends on (the :class:`HostKeyTrust` protocol):
"make sure ssh will trust this relay endpoint, and tell me which file to hand
it." It owns no I/O of its own; it wires together :class:`RelayKeyProvider`
(network + TTL) and :class:`HostKeyCache` (disk), keeping each collaborator
single-purpose and independently testable.
"""
from __future__ import annotations

import logging
import threading
from pathlib import Path
from typing import Protocol

from .cache import HostKeyCache
from .keys import RelayKeyProvider
from .models import HostKey, known_hosts_endpoint

log = logging.getLogger("plex_relay.store")


class HostKeyTrust(Protocol):
    """What the controller requires to make a relay endpoint trusted by ssh."""

    def ensure_trusted(self, host: str, port: int = 443, *, force: bool = False) -> HostKey: ...

    @property
    def known_hosts_path(self) -> Path: ...


class HostKeyManager:
    """Default :class:`HostKeyTrust`: refresh key, pin endpoint, persist on change."""

    def __init__(self, provider: RelayKeyProvider, cache: HostKeyCache) -> None:
        self._provider = provider
        self._cache = cache
        self._lock = threading.Lock()
        self._entries: dict[str, HostKey] = cache.load()

    @property
    def known_hosts_path(self) -> Path:
        return self._cache.path

    @property
    def entries(self) -> dict[str, HostKey]:
        with self._lock:
            return dict(self._entries)

    def ensure_trusted(self, host: str, port: int = 443, *, force: bool = False) -> HostKey:
        """Ensure ``relayHostKey.txt`` trusts ``host:port``; persist iff changed."""
        key = self._provider.get(force=force)
        endpoint = known_hosts_endpoint(host, port)
        entry = HostKey.for_endpoint(endpoint, key)
        with self._lock:
            if self._entries.get(endpoint) == entry:
                return entry
            self._entries[endpoint] = entry
            self._cache.save(self._entries)
            log.info("pinned relay host key for %s", endpoint)
            return entry


__all__ = ["HostKeyTrust", "HostKeyManager"]
