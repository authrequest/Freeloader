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
import xml.etree.ElementTree as ET


def _load(path: str) -> tuple[ET.ElementTree, ET.Element]:
    try:
        tree = ET.parse(path)
    except (OSError, ET.ParseError) as exc:
        sys.exit(f"plex_prefs: cannot read {path}: {exc}")
    root = tree.getroot()
    if root.tag != "Preferences":
        sys.exit(f"plex_prefs: unexpected root <{root.tag}>; refusing to edit {path}")
    return tree, root


def _merge_csv(root: ET.Element, attr: str, additions: list[str]) -> None:
    items = [x for x in (s.strip() for s in root.get(attr, "").split(",")) if x]
    for value in additions:
        if value and value not in items:
            items.append(value)
    root.set(attr, ",".join(items))


def cmd_merge(args: argparse.Namespace) -> int:
    tree, root = _load(args.prefs)
    if args.custom_url:
        _merge_csv(root, "customConnections", [args.custom_url])
    if args.lan:
        _merge_csv(root, "LanNetworksBandwidth", [c for c in args.lan.split(",") if c])
    if args.secure in ("0", "1", "2"):
        root.set("secureConnections", args.secure)
    if args.relay in ("0", "1"):
        root.set("RelayEnabled", args.relay)
    tree.write(args.prefs, encoding="utf-8", xml_declaration=True)
    return 0


def cmd_get(args: argparse.Namespace) -> int:
    _, root = _load(args.prefs)
    print(root.get(args.attr, ""))
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="plex_prefs", description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)

    m = sub.add_parser("merge", help="merge tailnet settings into Preferences.xml")
    m.add_argument("prefs")
    m.add_argument("--custom-url", default="")
    m.add_argument("--lan", default="")
    m.add_argument("--secure", default="", help="0=Required 1=Preferred 2=Disabled")
    m.add_argument("--relay", default="", help="0=disable 1=enable Plex Relay")
    m.set_defaults(func=cmd_merge)

    g = sub.add_parser("get", help="print one Preferences.xml attribute")
    g.add_argument("prefs")
    g.add_argument("attr")
    g.set_defaults(func=cmd_get)

    args = parser.parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
