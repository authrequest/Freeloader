# SPDX-License-Identifier: AGPL-3.0-or-later
from pathlib import Path

from plex_relay.keys import RelayKeyProvider
from plex_relay.store import HostKeyManager


class FakeCache:
    def __init__(self):
        self.entries = {}
        self.saves = 0

    @property
    def path(self) -> Path:
        return Path("/tmp/relayHostKey.txt")

    def load(self):
        return dict(self.entries)

    def save(self, entries):
        self.saves += 1
        self.entries = dict(entries)


def _provider(text_box):
    return RelayKeyProvider("https://x/relay_v1.pub", 86_400.0,
                            fetcher=lambda u: text_box[0], clock=lambda: 0.0)


def test_ensure_trusted_pins_and_persists():
    cache = FakeCache()
    mgr = HostKeyManager(_provider(["ssh-ed25519 AAAAfirst c"]), cache)
    entry = mgr.ensure_trusted("1.2.3.4", 443)
    assert entry.known_hosts_line() == "[1.2.3.4]:443 ssh-ed25519 AAAAfirst"
    assert cache.saves == 1
    assert "[1.2.3.4]:443" in cache.entries


def test_ensure_trusted_is_idempotent():
    cache = FakeCache()
    mgr = HostKeyManager(_provider(["ssh-ed25519 AAAAfirst c"]), cache)
    mgr.ensure_trusted("1.2.3.4", 443)
    mgr.ensure_trusted("1.2.3.4", 443)  # unchanged -> no extra write
    assert cache.saves == 1


def test_ensure_trusted_rewrites_on_key_change():
    cache = FakeCache()
    box = ["ssh-ed25519 AAAAfirst c"]
    mgr = HostKeyManager(_provider(box), cache)
    mgr.ensure_trusted("h", 443)
    box[0] = "ssh-ed25519 AAAAsecond c"
    entry = mgr.ensure_trusted("h", 443, force=True)
    assert entry.keydata == "AAAAsecond"
    assert cache.saves == 2


def test_known_hosts_path_is_cache_path():
    cache = FakeCache()
    mgr = HostKeyManager(_provider(["ssh-ed25519 k c"]), cache)
    assert mgr.known_hosts_path == cache.path
