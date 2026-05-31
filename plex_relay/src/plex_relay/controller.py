# SPDX-License-Identifier: AGPL-3.0-or-later
"""Relay controller -- the orchestration layer.

Depends only on abstractions (:class:`HostKeyTrust`, :class:`TunnelFactory`),
so the network, disk, and process concerns are all injected and substitutable.
:meth:`RelayController.from_config` is the composition root that wires the
default adapters together.

Concurrency: a single ``RLock`` (the binary's ``recursive_mutex``) guards the
connection table and the reaper, which runs on a background ``threading.Timer``
and reschedules itself only while connections remain.

Error contract: :meth:`connect` raises :class:`RelayError` on failure (callers
decide). :meth:`start_relay` -- the plex.tv event entry point -- is resilient:
it logs and returns ``False`` rather than letting an exception escape an event
loop, matching PMS's ``ServerEventManager`` behaviour.
"""
from __future__ import annotations

import logging
import threading

from .cache import HostKeyCache
from .config import RelayConfig
from .errors import RelayError
from .keys import RelayKeyProvider
from .store import HostKeyManager, HostKeyTrust
from .tunnel import SubprocessTunnelFactory, Tunnel, TunnelFactory

log = logging.getLogger("plex_relay.controller")


class RelayController:
    """Manages relay tunnels for one server: connect, reap, stop."""

    def __init__(
        self,
        config: RelayConfig,
        hostkeys: HostKeyTrust,
        tunnels: TunnelFactory,
    ) -> None:
        self._config = config
        self._hostkeys = hostkeys
        self._tunnels = tunnels
        self._lock = threading.RLock()
        self._connections: dict[str, Tunnel] = {}
        self._reaper: threading.Timer | None = None
        self._closed = False

    @classmethod
    def from_config(cls, config: RelayConfig) -> "RelayController":
        """Composition root: wire the default network/disk/process adapters."""
        provider = RelayKeyProvider(
            config.relay_key_url,
            config.key_ttl_seconds,
            fetcher=_default_fetcher(config),
        )
        manager = HostKeyManager(provider, HostKeyCache(config.cache_path))
        return cls(config, manager, SubprocessTunnelFactory(config))

    # -- entry points ------------------------------------------------------

    def start_relay(self, host: str, port: int = 443) -> bool:
        """plex.tv ``startRelay`` handler: gated and resilient."""
        if not self._config.gating_ok():
            log.info(
                "startRelay ignored (signed_in=%s published=%s relay_enabled=%s)",
                self._config.signed_in, self._config.published, self._config.relay_enabled,
            )
            return False
        try:
            return self.connect(host, port)
        except RelayError as exc:
            log.error("relay to %s failed: %s", host, exc)
            return False

    def connect(self, host: str, port: int = 443) -> bool:
        """Establish a tunnel to ``host``. Returns False if already active.

        :raises RelayError: if the key cannot be obtained or ssh cannot launch.
        """
        with self._lock:
            if self._closed:
                raise RelayError("controller is closed")
            existing = self._connections.get(host)
            if existing is not None and existing.is_alive():
                log.info("already have an active relay connection to %s", host)
                return False
            self._hostkeys.ensure_trusted(host, port)
            tunnel = self._tunnels(host, port, self._hostkeys.known_hosts_path)
            tunnel.start()
            self._connections[host] = tunnel
            self._arm_reaper()
            return True

    def stop(self) -> None:
        """Cancel the reaper and tear down every tunnel. Idempotent."""
        with self._lock:
            self._closed = True
            self._cancel_reaper()
            connections, self._connections = self._connections, {}
        for tunnel in connections.values():
            tunnel.stop(self._config.stop_timeout_seconds)

    # -- reaper ------------------------------------------------------------

    def reap_once(self) -> list[str]:
        """Drop finished tunnels; return the hosts removed."""
        with self._lock:
            dead = [h for h, t in self._connections.items() if not t.is_alive()]
            stopped = [self._connections.pop(h) for h in dead]
        for tunnel in stopped:
            log.info("cleaning up inactive relay connection to %s", tunnel.host)
            tunnel.stop(self._config.stop_timeout_seconds)
        return dead

    def _arm_reaper(self) -> None:
        if self._reaper is None and self._connections and not self._closed:
            self._schedule_reaper()

    def _schedule_reaper(self) -> None:
        timer = threading.Timer(self._config.reap_interval_seconds, self._reaper_tick)
        timer.daemon = True
        self._reaper = timer
        timer.start()

    def _reaper_tick(self) -> None:
        self.reap_once()
        with self._lock:
            self._reaper = None
            if self._connections and not self._closed:
                self._schedule_reaper()

    def _cancel_reaper(self) -> None:
        if self._reaper is not None:
            self._reaper.cancel()
            self._reaper = None

    # -- introspection -----------------------------------------------------

    @property
    def active_hosts(self) -> list[str]:
        with self._lock:
            return sorted(h for h, t in self._connections.items() if t.is_alive())

    def __enter__(self) -> "RelayController":
        return self

    def __exit__(self, *exc: object) -> None:
        self.stop()


def _default_fetcher(config: RelayConfig):
    from .keys import HttpsRelayKeyFetcher

    return HttpsRelayKeyFetcher(
        timeout=config.connect_timeout_seconds,
        allow_insecure=config.allow_insecure_key_url,
    )


__all__ = ["RelayController"]
