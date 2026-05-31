# SPDX-License-Identifier: AGPL-3.0-or-later
"""Relay host-key acquisition: fetch ``relay_v1.pub`` and cache it with a TTL.

Separated from on-disk caching (:mod:`plex_relay.cache`) so the network policy
(HTTPS-only, byte-capped, time-limited) and the freshness policy (TTL) live in
one cohesive place and can be swapped wholesale in tests via the injected
``fetcher``/``clock`` seams.
"""
from __future__ import annotations

import logging
import threading
import time
import urllib.parse
import urllib.request
from typing import Callable, Protocol

from .errors import RelayKeyError
from .models import RelayKey, parse_relay_pub

log = logging.getLogger("plex_relay.keys")

#: Fetches the raw body of a relay-key URL. The seam that tests stub.
Fetcher = Callable[[str], str]
#: Monotonic time source (seconds). Monotonic so TTLs survive wall-clock jumps.
Clock = Callable[[], float]

_MAX_KEY_BYTES = 64 * 1024  # a host key is a few hundred bytes; cap to bound I/O


class _Opener(Protocol):
    def __call__(self, url: str, timeout: float): ...  # pragma: no cover


class HttpsRelayKeyFetcher:
    """Default fetcher: HTTPS-only, time-limited, response-size-capped.

    Hardened against the obvious abuse of a configurable URL: non-HTTPS schemes
    are refused unless explicitly allowed, the read is bounded, and every
    transport failure is normalised to :class:`RelayKeyError`.
    """

    def __init__(
        self,
        *,
        timeout: float = 15.0,
        max_bytes: int = _MAX_KEY_BYTES,
        allow_insecure: bool = False,
        opener: _Opener = urllib.request.urlopen,
    ) -> None:
        self._timeout = timeout
        self._max_bytes = max_bytes
        self._allow_insecure = allow_insecure
        self._opener = opener

    def __call__(self, url: str) -> str:
        scheme = urllib.parse.urlsplit(url).scheme.lower()
        if scheme != "https" and not (self._allow_insecure and scheme in ("http", "file")):
            raise RelayKeyError(f"refusing non-HTTPS relay key URL: {url!r}")
        try:
            with self._opener(url, timeout=self._timeout) as resp:
                data = resp.read(self._max_bytes + 1)
        except (OSError, ValueError) as exc:
            raise RelayKeyError(f"failed to fetch relay key from {url!r}: {exc}") from exc
        if len(data) > self._max_bytes:
            raise RelayKeyError(f"relay key response exceeds {self._max_bytes} bytes")
        return data.decode("utf-8", "replace")


class RelayKeyProvider:
    """Provides the relay :class:`RelayKey`, refreshing past a TTL.

    Thread-safe: concurrent ``get()`` calls serialise on a lock so the key is
    fetched at most once per TTL window even under contention.
    """

    def __init__(
        self,
        url: str,
        ttl_seconds: float,
        fetcher: Fetcher | None = None,
        clock: Clock = time.monotonic,
    ) -> None:
        self._url = url
        self._ttl = ttl_seconds
        self._fetch = fetcher or HttpsRelayKeyFetcher()
        self._clock = clock
        self._lock = threading.Lock()
        self._key: RelayKey | None = None
        self._fetched_at = 0.0

    def get(self, *, force: bool = False) -> RelayKey:
        """Return the relay key, fetching only when stale or ``force``."""
        with self._lock:
            now = self._clock()
            if not force and self._key is not None and (now - self._fetched_at) < self._ttl:
                log.debug("relay key reused (age %.0fs)", now - self._fetched_at)
                return self._key
            key = parse_relay_pub(self._fetch(self._url))
            self._key, self._fetched_at = key, now
            log.info("relay key refreshed from %s", self._url)
            return key


__all__ = ["Fetcher", "Clock", "HttpsRelayKeyFetcher", "RelayKeyProvider"]
