# SPDX-License-Identifier: AGPL-3.0-or-later
"""Pure domain model: value objects and parsing, no I/O.

Everything here is deterministic and side-effect free, so it is trivially
testable and safe to share across threads (all types are immutable).

Provenance: in ``Plex Media Server`` 1.43.2.10687, ``RelayController_connect``
(``0x12307F2``) downloads ``relay_v1.pub``, splits it into exactly three
whitespace tokens (rejecting otherwise -- the "part count incorrect" log), and
writes a per-endpoint OpenSSH known_hosts line ``[host]:443 <keytype> <keydata>``
into ``relayHostKey.txt``.
"""
from __future__ import annotations

from dataclasses import dataclass

from .errors import RelayKeyError

# Key types OpenSSH recognises. Used to disambiguate a public-key line
# ("keytype keydata comment") from a known_hosts line ("host keytype keydata").
_KEY_TYPES: frozenset[str] = frozenset(
    {
        "ssh-rsa",
        "ssh-dss",
        "ssh-ed25519",
        "ecdsa-sha2-nistp256",
        "ecdsa-sha2-nistp384",
        "ecdsa-sha2-nistp521",
        "sk-ssh-ed25519@openssh.com",
        "sk-ecdsa-sha2-nistp256@openssh.com",
        "ssh-rsa-cert-v01@openssh.com",
        "ssh-ed25519-cert-v01@openssh.com",
    }
)


def known_hosts_endpoint(host: str, port: int = 443) -> str:
    """Return the OpenSSH known_hosts host field for ``host:port``.

    OpenSSH uses bracket notation for any non-default port. The binary hardcodes
    ``[host]:443``; keying by the actual port keeps the entry valid for relays on
    other ports while remaining identical at 443.
    """
    host = host.strip()
    if not host:
        raise RelayKeyError("empty relay host")
    return host if port == 22 else f"[{host}]:{port}"


@dataclass(frozen=True, slots=True)
class RelayKey:
    """An SSH host key: an algorithm and its base64-encoded blob."""

    keytype: str
    keydata: str


@dataclass(frozen=True, slots=True)
class HostKey:
    """A single OpenSSH known_hosts entry for a relay endpoint."""

    endpoint: str
    keytype: str
    keydata: str

    @classmethod
    def for_endpoint(cls, endpoint: str, key: RelayKey) -> "HostKey":
        return cls(endpoint=endpoint, keytype=key.keytype, keydata=key.keydata)

    @property
    def key(self) -> RelayKey:
        return RelayKey(self.keytype, self.keydata)

    def known_hosts_line(self) -> str:
        """The line OpenSSH consumes: ``<endpoint> <keytype> <keydata>``."""
        return f"{self.endpoint} {self.keytype} {self.keydata}"

    def cache_block(self) -> str:
        """PMS ``relayHostKey.txt`` representation: ``# <endpoint>`` + data line."""
        return f"# {self.endpoint}\n{self.known_hosts_line()}\n"


def parse_relay_pub(body: str) -> RelayKey:
    """Parse a ``relay_v1.pub`` payload into a :class:`RelayKey`.

    Accepts both the OpenSSH public-key form (``keytype keydata comment``) and
    the known_hosts form (``host keytype keydata``), disambiguated by which token
    is a recognised key type.

    :raises RelayKeyError: if no usable key line is present (mirrors the binary's
        "part count incorrect" rejection).
    """
    for raw in body.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        tokens = line.split()
        if len(tokens) < 3:
            raise RelayKeyError(
                f"relay key: part count incorrect ({len(tokens)} tokens)"
            )
        if tokens[0] in _KEY_TYPES:  # keytype keydata comment
            return RelayKey(tokens[0], tokens[1])
        if tokens[1] in _KEY_TYPES:  # host keytype keydata
            return RelayKey(tokens[1], tokens[2])
        raise RelayKeyError("relay key: no recognised key type in payload")
    raise RelayKeyError("relay key: payload contained no key line")


__all__ = ["RelayKey", "HostKey", "known_hosts_endpoint", "parse_relay_pub"]
