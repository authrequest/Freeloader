# SPDX-License-Identifier: AGPL-3.0-or-later
import os
import stat

import pytest

from plex_relay.cache import HostKeyCache, parse_known_hosts
from plex_relay.errors import HostKeyCacheError
from plex_relay.models import HostKey


def _entries():
    return {
        "[1.2.3.4]:443": HostKey("[1.2.3.4]:443", "ssh-ed25519", "AAAAfirst"),
        "[5.6.7.8]:443": HostKey("[5.6.7.8]:443", "ssh-ed25519", "AAAAsecond"),
    }


def test_save_load_roundtrip(tmp_path):
    cache = HostKeyCache(tmp_path / "relayHostKey.txt")
    cache.save(_entries())
    loaded = HostKeyCache(tmp_path / "relayHostKey.txt").load()
    assert loaded == _entries()


def test_save_is_sorted_and_blocked(tmp_path):
    p = tmp_path / "relayHostKey.txt"
    HostKeyCache(p).save(_entries())
    text = p.read_text()
    assert text.index("[1.2.3.4]") < text.index("[5.6.7.8]")
    assert "# [1.2.3.4]:443\n[1.2.3.4]:443 ssh-ed25519 AAAAfirst\n" in text


@pytest.mark.skipif(os.name != "posix", reason="POSIX file modes only")
def test_save_is_0600(tmp_path):
    p = tmp_path / "relayHostKey.txt"
    HostKeyCache(p).save(_entries())
    assert stat.S_IMODE(os.stat(p).st_mode) == 0o600


def test_missing_file_loads_empty(tmp_path):
    assert HostKeyCache(tmp_path / "nope.txt").load() == {}


def test_malformed_file_is_rebuilt_empty(tmp_path):
    p = tmp_path / "relayHostKey.txt"
    p.write_text("not a comment\ngarbage line\n")
    assert HostKeyCache(p).load() == {}
    assert p.read_text() == ""


@pytest.mark.parametrize("lines", [
    ["data without marker"],
    ["# marker only"],
    ["# marker", "too many tokens here now"],
])
def test_parse_rejects_malformed(lines):
    with pytest.raises(HostKeyCacheError):
        parse_known_hosts(lines)
