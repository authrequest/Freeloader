# SPDX-License-Identifier: AGPL-3.0-or-later
from pathlib import Path

import pytest

from plex_relay.config import RelayConfig
from plex_relay.errors import TunnelError
from plex_relay.tunnel import SubprocessTunnel, SubprocessTunnelFactory, build_ssh_argv


def cfg(**kw):
    base = dict(token="secret", ssh_user="machineid", local_host="127.0.0.1", local_port=32400)
    base.update(kw)
    return RelayConfig(**base)


def test_argv_matches_binary_layout():
    argv = build_ssh_argv(cfg(), "relay.example", 443, Path("/data/relayHostKey.txt"))
    assert argv == [
        "ssh", "-p", "443", "-N", "-R", "0:127.0.0.1:32400",
        "-o", "UserKnownHostsFile=/data/relayHostKey.txt",
        "-o", "LogLevel=VERBOSE",
        "-o", "PreferredAuthentications=password",
        "-o", "PubkeyAuthentication=no",
        "-l", "machineid", "-F", "/dev/null", "relay.example",
    ]


def test_argv_honours_port_and_target():
    argv = build_ssh_argv(cfg(local_host="10.0.0.5", local_port=32500), "h", 2222, Path("/k"))
    assert "0:10.0.0.5:32500" in argv
    assert argv[argv.index("-p") + 1] == "2222"


class FakeProc:
    def __init__(self):
        self.alive = True
        self.terminated = False

    def poll(self):
        return None if self.alive else 0

    def terminate(self):
        self.terminated = True
        self.alive = False

    def kill(self):
        self.alive = False

    def wait(self, timeout=None):
        self.alive = False
        return 0


def test_tunnel_start_sets_secret_env_and_cleans_askpass():
    captured = {}

    def spawner(argv, env):
        captured["argv"] = argv
        captured["env"] = dict(env)
        return FakeProc()

    t = SubprocessTunnel(cfg(), "relay.example", 443, Path("/k"), spawner)
    t.start()
    assert t.is_alive()
    assert captured["env"]["PLEXTOKEN"] == "secret"
    assert "SSH_ASKPASS" in captured["env"]
    askpass = Path(captured["env"]["SSH_ASKPASS"])
    assert askpass.exists()
    assert "secret" not in captured["argv"]  # never on the command line
    t.stop()
    assert not t.is_alive()
    assert not askpass.exists()  # helper removed on stop


def test_tunnel_start_wraps_spawn_failure():
    def boom(argv, env):
        raise OSError("ssh not found")
    t = SubprocessTunnel(cfg(), "h", 443, Path("/k"), boom)
    with pytest.raises(TunnelError):
        t.start()
    assert not t.is_alive()


def test_factory_builds_tunnel():
    factory = SubprocessTunnelFactory(cfg(), spawner=lambda a, e: FakeProc())
    t = factory("h", 443, Path("/k"))
    assert t.host == "h"
    t.start()
    assert t.is_alive()
    t.stop()
