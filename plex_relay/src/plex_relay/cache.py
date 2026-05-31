# SPDX-License-Identifier: AGPL-3.0-or-later
"""On-disk known_hosts cache (``relayHostKey.txt``) -- file I/O only.

This module owns the file format and the filesystem; it knows nothing about
HTTP, TTLs, or ssh. The file doubles as PMS's cache and the OpenSSH
``UserKnownHostsFile`` (``#`` lines are valid known_hosts comments).

Writes are atomic (write-temp-then-rename) and the file is mode ``0o600`` --
it pins the keys ssh will trust, so it must not be world-writable.
"""
from __future__ import annotations

import logging
import os
import stat
import tempfile
from pathlib import Path
from typing import Iterable, Mapping

from .errors import HostKeyCacheError
from .models import HostKey

log = logging.getLogger("plex_relay.cache")


def parse_known_hosts(lines: Iterable[str]) -> dict[str, HostKey]:
    """Parse ``# <endpoint>`` + ``<host> <keytype> <keydata>`` line pairs.

    :raises HostKeyCacheError: on any structural violation (mirrors the binary,
        which discards and rebuilds a malformed file).
    """
    items = [ln.rstrip("\n") for ln in lines if ln.strip()]
    entries: dict[str, HostKey] = {}
    i = 0
    while i < len(items):
        comment = items[i]
        if not comment.startswith("#"):
            raise HostKeyCacheError(f"expected '# <endpoint>' marker, got {comment!r}")
        if i + 1 >= len(items):
            raise HostKeyCacheError("comment marker without a following data line")
        tokens = items[i + 1].split()
        if len(tokens) != 3:
            raise HostKeyCacheError(f"data line part count incorrect: {items[i + 1]!r}")
        endpoint = comment[1:].strip() or tokens[0]
        host, keytype, keydata = tokens
        entries[endpoint] = HostKey(host, keytype, keydata)
        i += 2
    return entries


class HostKeyCache:
    """Load/store relay host keys from a single known_hosts-format file."""

    def __init__(self, path: Path) -> None:
        self._path = Path(path)

    @property
    def path(self) -> Path:
        return self._path

    def load(self) -> dict[str, HostKey]:
        """Return cached entries; a malformed file is dropped and rebuilt empty."""
        if not self._path.exists():
            return {}
        try:
            text = self._path.read_text("utf-8", "replace")
        except OSError as exc:
            raise HostKeyCacheError(f"cannot read {self._path}: {exc}") from exc
        try:
            entries = parse_known_hosts(text.splitlines())
        except HostKeyCacheError as exc:
            log.warning("host key file malformed, rebuilding: %s", exc)
            self.save({})
            return {}
        log.info("read %d cached host key entries", len(entries))
        return entries

    def save(self, entries: Mapping[str, HostKey]) -> None:
        """Atomically write ``entries`` to the cache file with mode 0o600."""
        try:
            self._path.parent.mkdir(parents=True, exist_ok=True)
            blocks = "".join(entries[k].cache_block() for k in sorted(entries))
            fd, tmp_name = tempfile.mkstemp(
                dir=self._path.parent, prefix=self._path.name + ".", suffix=".tmp"
            )
            tmp = Path(tmp_name)
            try:
                os.write(fd, blocks.encode("utf-8"))
            finally:
                os.close(fd)
            os.chmod(tmp, stat.S_IRUSR | stat.S_IWUSR)  # 0o600
            os.replace(tmp, self._path)
        except OSError as exc:
            raise HostKeyCacheError(f"cannot write {self._path}: {exc}") from exc


__all__ = ["HostKeyCache", "parse_known_hosts"]
