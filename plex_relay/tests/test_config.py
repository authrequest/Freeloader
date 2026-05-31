# SPDX-License-Identifier: AGPL-3.0-or-later
import dataclasses

import pytest

from plex_relay.config import RelayConfig
from plex_relay.errors import ConfigError


def test_valid_config_and_cache_path(tmp_path):
    cfg = RelayConfig(token="t", ssh_user="u", data_dir=tmp_path)
    assert cfg.cache_path == tmp_path / "relayHostKey.txt"
    assert cfg.gating_ok() is True


@pytest.mark.parametrize("kwargs", [
    {"token": "", "ssh_user": "u"},
    {"token": "t", "ssh_user": ""},
    {"token": "t", "ssh_user": "u", "local_port": 0},
    {"token": "t", "ssh_user": "u", "local_port": 70000},
    {"token": "t", "ssh_user": "u", "key_ttl_seconds": 0},
    {"token": "t", "ssh_user": "u", "reap_interval_seconds": -1},
])
def test_invalid_config_raises(kwargs):
    with pytest.raises(ConfigError):
        RelayConfig(**kwargs)


def test_gating_requires_all_three():
    base = dict(token="t", ssh_user="u")
    assert RelayConfig(**base, relay_enabled=False).gating_ok() is False
    assert RelayConfig(**base, published=False).gating_ok() is False
    assert RelayConfig(**base, signed_in=False).gating_ok() is False


def test_token_is_not_in_repr():
    cfg = RelayConfig(token="SUPERSECRET", ssh_user="u")
    assert "SUPERSECRET" not in repr(cfg)


def test_config_is_frozen():
    cfg = RelayConfig(token="t", ssh_user="u")
    with pytest.raises(dataclasses.FrozenInstanceError):
        cfg.local_port = 1  # type: ignore[misc]
