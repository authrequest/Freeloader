# SPDX-License-Identifier: AGPL-3.0-or-later
"""Exception hierarchy for :mod:`plex_relay`.

A single rooted hierarchy lets callers catch the whole subsystem
(``except RelayError``) or a specific failure mode, and keeps adapter-specific
exceptions (``urllib``, ``OSError``, ``subprocess``) from leaking across module
boundaries.
"""
from __future__ import annotations


class RelayError(Exception):
    """Base class for every error raised by this package."""


class ConfigError(RelayError):
    """Invalid configuration (bad port, empty credential, ...)."""


class RelayKeyError(RelayError):
    """The relay host key could not be fetched or parsed."""


class HostKeyCacheError(RelayError):
    """The on-disk known_hosts cache is unreadable or malformed."""


class TunnelError(RelayError):
    """The ssh relay tunnel could not be launched."""


__all__ = [
    "RelayError",
    "ConfigError",
    "RelayKeyError",
    "HostKeyCacheError",
    "TunnelError",
]
