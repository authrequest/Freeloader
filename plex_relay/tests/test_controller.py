# SPDX-License-Identifier: AGPL-3.0-or-later
from pathlib import Path

import pytest

from plex_relay.config import RelayConfig
from plex_relay.controller import RelayController
from plex_relay.errors import RelayError, TunnelError
from plex_relay.models import HostKey, RelayKey, known_hosts_endpoint


class FakeTunnel:
    def __init__(self, host, fail=False):
        self._host = host
        self._fail = fail
        self.alive = False
        self.stopped = False

    @property
    def host(self):
        return self._host

    def start(self):
        if self._fail:
            raise TunnelError("spawn failed")
        self.alive = True

    def is_alive(self):
        return self.alive

    def stop(self, timeout=None):
        self.stopped = True
        self.alive = False


class FakeFactory:
    def __init__(self):
        self.fail = False
        self.created = []

    def __call__(self, host, port, known_hosts_path):
        t = FakeTunnel(host, fail=self.fail)
        self.created.append(t)
        return t


class FakeTrust:
    def __init__(self):
        self.calls = []

    def ensure_trusted(self, host, port=443, *, force=False):
        self.calls.append((host, port))
        return HostKey.for_endpoint(known_hosts_endpoint(host, port), RelayKey("ssh-ed25519", "k"))

    @property
    def known_hosts_path(self):
        return Path("/k")


def build(**cfgkw):
    cfg = RelayConfig(token="t", ssh_user="u", reap_interval_seconds=999.0, **cfgkw)
    trust, factory = FakeTrust(), FakeFactory()
    return RelayController(cfg, trust, factory), trust, factory


def test_connect_tracks_and_trusts():
    ctrl, trust, factory = build()
    try:
        assert ctrl.connect("relay.example", 443) is True
        assert ctrl.active_hosts == ["relay.example"]
        assert trust.calls == [("relay.example", 443)]
        assert len(factory.created) == 1
    finally:
        ctrl.stop()


def test_connect_dedup_when_alive():
    ctrl, _, factory = build()
    try:
        assert ctrl.connect("h") is True
        assert ctrl.connect("h") is False
        assert len(factory.created) == 1
    finally:
        ctrl.stop()


def test_reconnect_after_death():
    ctrl, _, factory = build()
    try:
        ctrl.connect("h")
        factory.created[0].alive = False  # tunnel died
        assert ctrl.connect("h") is True
        assert len(factory.created) == 2
    finally:
        ctrl.stop()


def test_connect_raises_on_tunnel_failure():
    ctrl, _, factory = build()
    factory.fail = True
    with pytest.raises(RelayError):
        ctrl.connect("h")
    assert ctrl.active_hosts == []
    ctrl.stop()


def test_start_relay_blocked_by_gating():
    for kw in ({"relay_enabled": False}, {"published": False}, {"signed_in": False}):
        ctrl, trust, _ = build(**kw)
        assert ctrl.start_relay("h") is False
        assert trust.calls == []
        ctrl.stop()


def test_start_relay_is_resilient_to_failure():
    ctrl, _, factory = build()
    factory.fail = True
    assert ctrl.start_relay("h") is False  # logs + swallows, does not raise
    ctrl.stop()


def test_start_relay_ok():
    ctrl, _, _ = build()
    try:
        assert ctrl.start_relay("h", 443) is True
        assert ctrl.active_hosts == ["h"]
    finally:
        ctrl.stop()


def test_reaper_removes_dead():
    ctrl, _, factory = build()
    try:
        ctrl.connect("a")
        ctrl.connect("b")
        factory.created[0].alive = False
        assert ctrl.reap_once() == ["a"]
        assert ctrl.active_hosts == ["b"]
        assert factory.created[0].stopped is True
    finally:
        ctrl.stop()


def test_stop_terminates_all_and_closes():
    ctrl, _, factory = build()
    ctrl.connect("a")
    ctrl.connect("b")
    ctrl.stop()
    assert ctrl.active_hosts == []
    assert all(t.stopped for t in factory.created)
    with pytest.raises(RelayError):
        ctrl.connect("c")


def test_from_config_builds_controller(tmp_path):
    cfg = RelayConfig(token="t", ssh_user="u", data_dir=tmp_path)
    assert isinstance(RelayController.from_config(cfg), RelayController)
