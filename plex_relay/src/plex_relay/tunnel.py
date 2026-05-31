# SPDX-License-Identifier: AGPL-3.0-or-later
"""The relay tunnel adapter: build the ssh argv and run it as a child process.

``build_ssh_argv`` is a pure function (the byte-for-byte reproduction of the
argv ``RelayController_connect`` assembles), kept separate from process control
so it is testable without spawning anything.

Security: the relay credential is delivered to ssh via the ``PLEXTOKEN``
environment variable read by a generated ``SSH_ASKPASS`` helper -- it never
appears on a command line or in the argv list. The helper is mode ``0o700`` and
is removed on stop *and* via a finaliser, so a crash cannot leak it.
"""
from __future__ import annotations

import logging
import os
import stat
import subprocess
import tempfile
import weakref
from pathlib import Path
from typing import Callable, Mapping, Protocol

from .config import RelayConfig
from .errors import TunnelError

log = logging.getLogger("plex_relay.tunnel")

_ASKPASS_SCRIPT = "#!/bin/sh\nprintf '%s' \"$PLEXTOKEN\"\n"


class ProcessHandle(Protocol):
    """The slice of ``subprocess.Popen`` the tunnel relies on."""

    def poll(self) -> int | None: ...
    def terminate(self) -> None: ...
    def kill(self) -> None: ...
    def wait(self, timeout: float | None = ...) -> int: ...


#: Launches a child process from an argv + environment. The seam tests stub.
Spawner = Callable[[list[str], Mapping[str, str]], ProcessHandle]


def build_ssh_argv(
    config: RelayConfig, host: str, port: int, known_hosts_path: Path
) -> list[str]:
    """Assemble the ssh reverse-tunnel argv, exactly as the binary does.

    The known_hosts path uses POSIX separators (identical on Linux; OpenSSH
    accepts forward slashes everywhere).
    """
    return [
        config.ssh_binary,
        "-p", str(port),
        "-N",
        "-R", f"0:{config.local_host}:{config.local_port}",
        "-o", f"UserKnownHostsFile={Path(known_hosts_path).as_posix()}",
        "-o", "LogLevel=VERBOSE",
        "-o", "PreferredAuthentications=password",
        "-o", "PubkeyAuthentication=no",
        "-l", config.ssh_user,
        "-F", "/dev/null",
        host,
    ]


class _Askpass:
    """A short-lived, self-cleaning SSH_ASKPASS helper script."""

    def __init__(self, token: str) -> None:
        fd, name = tempfile.mkstemp(prefix="plex_relay_askpass_", suffix=".sh")
        try:
            os.write(fd, _ASKPASS_SCRIPT.encode("ascii"))
        finally:
            os.close(fd)
        self.path = Path(name)
        self.path.chmod(stat.S_IRWXU)  # 0o700
        self._token = token
        self._finalizer = weakref.finalize(self, _unlink, self.path)

    def env(self, base: Mapping[str, str]) -> dict[str, str]:
        env = dict(base)
        env.update(
            PLEXTOKEN=self._token,
            SSH_ASKPASS=str(self.path),
            SSH_ASKPASS_REQUIRE="force",
            DISPLAY=base.get("DISPLAY", ":0"),
        )
        return env

    def cleanup(self) -> None:
        self._finalizer()


def _unlink(path: Path) -> None:
    try:
        path.unlink()
    except OSError:
        pass


def _default_spawner(argv: list[str], env: Mapping[str, str]) -> ProcessHandle:
    # Detach from the controlling tty so ssh uses SSH_ASKPASS for the password.
    return subprocess.Popen(  # noqa: S603 - argv is fully built; no shell
        argv,
        env=dict(env),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )


class Tunnel(Protocol):
    """Lifecycle of a single relay tunnel."""

    @property
    def host(self) -> str: ...
    def start(self) -> None: ...
    def is_alive(self) -> bool: ...
    def stop(self, timeout: float | None = ...) -> None: ...


class TunnelFactory(Protocol):
    def __call__(self, host: str, port: int, known_hosts_path: Path) -> Tunnel: ...


class SubprocessTunnel:
    """A relay tunnel backed by a child ``ssh`` process."""

    def __init__(
        self,
        config: RelayConfig,
        host: str,
        port: int,
        known_hosts_path: Path,
        spawner: Spawner,
    ) -> None:
        self._config = config
        self._host = host
        self._port = port
        self._known_hosts = Path(known_hosts_path)
        self._spawn = spawner
        self._proc: ProcessHandle | None = None
        self._askpass: _Askpass | None = None

    @property
    def host(self) -> str:
        return self._host

    @property
    def argv(self) -> list[str]:
        return build_ssh_argv(self._config, self._host, self._port, self._known_hosts)

    def start(self) -> None:
        if self.is_alive():
            return
        askpass = _Askpass(self._config.token)
        try:
            log.info("starting relay tunnel to %s:%d", self._host, self._port)
            self._proc = self._spawn(self.argv, askpass.env(os.environ))
        except OSError as exc:
            askpass.cleanup()
            raise TunnelError(f"failed to launch ssh for {self._host}: {exc}") from exc
        self._askpass = askpass

    def is_alive(self) -> bool:
        return self._proc is not None and self._proc.poll() is None

    def stop(self, timeout: float | None = 5.0) -> None:
        proc, self._proc = self._proc, None
        if proc is not None:
            log.info("stopping relay tunnel to %s", self._host)
            try:
                proc.terminate()
                proc.wait(timeout=timeout)
            except Exception:  # noqa: BLE001 - escalate to kill on any wait failure
                try:
                    proc.kill()
                except OSError:
                    pass
        if self._askpass is not None:
            self._askpass.cleanup()
            self._askpass = None


class SubprocessTunnelFactory:
    """Default :class:`TunnelFactory` producing :class:`SubprocessTunnel`."""

    def __init__(self, config: RelayConfig, spawner: Spawner | None = None) -> None:
        self._config = config
        self._spawner = spawner or _default_spawner

    def __call__(self, host: str, port: int, known_hosts_path: Path) -> SubprocessTunnel:
        return SubprocessTunnel(self._config, host, port, known_hosts_path, self._spawner)


__all__ = [
    "ProcessHandle",
    "Spawner",
    "Tunnel",
    "TunnelFactory",
    "SubprocessTunnel",
    "SubprocessTunnelFactory",
    "build_ssh_argv",
]
