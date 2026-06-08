#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Read and edit Plex ``Preferences.xml`` attributes.

Used by ``plex-tailscale-setup.sh``. The XML logic lives here -- not in a bash
heredoc -- so it is cohesive, reviewable, and independently testable. The shell
owns the lifecycle (stop Plex, back up, restore ownership, restart); this owns
the document.

  plex_prefs.py merge PREFS [--custom-url URL] [--lan CIDR[,CIDR...]]
                            [--secure 0|1|2] [--relay 0|1]
  plex_prefs.py get   PREFS ATTR

``merge`` is additive and idempotent: list attributes gain only missing values;
scalar attributes are set only when a value is supplied. Unrelated attributes
(tokens, machine identity, ...) are preserved.
"""
from __future__ import annotations

import argparse
import sys
from collections.abc import Callable
from typing import Protocol, cast

try:
    import defusedxml.ElementTree as ET
except ModuleNotFoundError:
    sys.exit("plex_prefs: missing dependency: install python3-defusedxml")


class _PrefsElement(Protocol):
    tag: str

    def get(self, key: str, default: str = "") -> str: ...
    def set(self, key: str, value: str) -> None: ...


class _PrefsTree(Protocol):
    def getroot(self) -> _PrefsElement: ...
    def write(self, file_or_filename: str, encoding: str, xml_declaration: bool) -> None: ...


def _load(path: str) -> tuple[_PrefsTree, _PrefsElement]:
    try:
        tree = cast(_PrefsTree, cast(object, ET.parse(path)))
    except (OSError, ET.ParseError) as exc:
        sys.exit(f"plex_prefs: cannot read {path}: {exc}")
    root = tree.getroot()
    if root.tag != "Preferences":
        sys.exit(f"plex_prefs: unexpected root <{root.tag}>; refusing to edit {path}")
    return tree, root


def _merge_csv(root: _PrefsElement, attr: str, additions: list[str]) -> None:
    items = [x for x in (s.strip() for s in root.get(attr, "").split(",")) if x]
    for value in additions:
        if value and value not in items:
            items.append(value)
    root.set(attr, ",".join(items))


def cmd_merge(args: argparse.Namespace) -> int:
    prefs = cast(str, args.prefs)
    custom_url = cast(str, args.custom_url)
    lan = cast(str, args.lan)
    secure = cast(str, args.secure)
    relay = cast(str, args.relay)

    tree, root = _load(prefs)
    if custom_url:
        _merge_csv(root, "customConnections", [custom_url])
    if lan:
        _merge_csv(root, "LanNetworksBandwidth", [c for c in lan.split(",") if c])
    if secure in ("0", "1", "2"):
        root.set("secureConnections", secure)
    if relay in ("0", "1"):
        root.set("RelayEnabled", relay)
    tree.write(prefs, encoding="utf-8", xml_declaration=True)
    return 0


def cmd_get(args: argparse.Namespace) -> int:
    prefs = cast(str, args.prefs)
    attr = cast(str, args.attr)

    _, root = _load(prefs)
    print(root.get(attr, ""))
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="plex_prefs", description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)

    m = sub.add_parser("merge", help="merge tailnet settings into Preferences.xml")
    _ = m.add_argument("prefs")
    _ = m.add_argument("--custom-url", default="")
    _ = m.add_argument("--lan", default="")
    _ = m.add_argument("--secure", default="", help="0=Required 1=Preferred 2=Disabled")
    _ = m.add_argument("--relay", default="", help="0=disable 1=enable Plex Relay")
    m.set_defaults(func=cmd_merge)

    g = sub.add_parser("get", help="print one Preferences.xml attribute")
    _ = g.add_argument("prefs")
    _ = g.add_argument("attr")
    g.set_defaults(func=cmd_get)

    args = parser.parse_args(argv)
    func = cast(Callable[[argparse.Namespace], int], args.func)
    return func(args)


if __name__ == "__main__":
    raise SystemExit(main())
