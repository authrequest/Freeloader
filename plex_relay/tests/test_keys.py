# SPDX-License-Identifier: AGPL-3.0-or-later
import pytest

from plex_relay.errors import RelayKeyError
from plex_relay.keys import HttpsRelayKeyFetcher, RelayKeyProvider
from plex_relay.models import RelayKey

PUB = "ssh-ed25519 AAAAkeydata comment"
KEY = RelayKey("ssh-ed25519", "AAAAkeydata")


def test_provider_respects_ttl_and_force():
    calls = []
    now = [1000.0]
    provider = RelayKeyProvider(
        "https://x/relay_v1.pub", 86_400.0,
        fetcher=lambda url: (calls.append(url), PUB)[1],
        clock=lambda: now[0],
    )
    assert provider.get() == KEY
    now[0] += 3600          # within TTL -> reuse
    provider.get()
    assert len(calls) == 1
    now[0] += 86_400        # past TTL -> refetch
    provider.get()
    assert len(calls) == 2
    provider.get(force=True)  # force -> refetch
    assert len(calls) == 3


# --- HttpsRelayKeyFetcher ---------------------------------------------------

class _FakeResp:
    def __init__(self, data: bytes):
        self._data = data

    def read(self, n: int = -1) -> bytes:
        return self._data[:n] if n >= 0 else self._data

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def test_fetcher_rejects_non_https():
    f = HttpsRelayKeyFetcher(opener=lambda *a, **k: _FakeResp(b""))
    with pytest.raises(RelayKeyError):
        f("http://insecure/relay_v1.pub")


def test_fetcher_allows_insecure_when_opted_in():
    f = HttpsRelayKeyFetcher(allow_insecure=True, opener=lambda *a, **k: _FakeResp(PUB.encode()))
    assert f("file:///tmp/relay_v1.pub") == PUB


def test_fetcher_caps_response_size():
    big = b"x" * 100
    f = HttpsRelayKeyFetcher(max_bytes=10, opener=lambda *a, **k: _FakeResp(big))
    with pytest.raises(RelayKeyError, match="exceeds"):
        f("https://x/relay_v1.pub")


def test_fetcher_wraps_transport_errors():
    def boom(*a, **k):
        raise OSError("connection refused")
    f = HttpsRelayKeyFetcher(opener=boom)
    with pytest.raises(RelayKeyError, match="failed to fetch"):
        f("https://x/relay_v1.pub")
