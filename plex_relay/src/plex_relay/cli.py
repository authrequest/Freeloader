# SPDX-License-Identifier: AGPL-3.0-or-later
"""Command-line composition root.

  plex-relay show    --host H [--port 443] ...   # resolve key + print ssh argv (no spawn)
  plex-relay connect --host H [--port 443] ...   # establish and hold the tunnel

The token is read from ``$PLEX_RELAY_TOKEN`` by default so it never appears in
the process list; ``--token`` overrides. ``show`` is a safe dry run.
"""
from __future__ import annotations

import argparse
import logging
import os
import signal
import sys
import threading
from pathlib import Path

from .config import DEFAULT_LOCAL_HOST, DEFAULT_LOCAL_PORT, DEFAULT_RELAY_KEY_URL, RelayConfig
from .errors import RelayError
from .keys import HttpsRelayKeyFetcher, RelayKeyProvider
from .models import known_hosts_endpoint
from .tunnel import build_ssh_argv

log = logging.getLogger("plex_relay.cli")


def _add_common(p: argparse.ArgumentParser) -> None:
    p.add_argument("--host", required=True, help="relay host to dial")
    p.add_argument("--port", type=int, default=443, help="relay SSH port (default 443)")
    p.add_argument("--user", required=True, help="relay SSH login (PMS: MyPlex identity)")
    p.add_argument("--token", default=os.environ.get("PLEX_RELAY_TOKEN", ""),
                   help="relay password (default: $PLEX_RELAY_TOKEN)")
    p.add_argument("--local-host", default=DEFAULT_LOCAL_HOST)
    p.add_argument("--local-port", type=int, default=DEFAULT_LOCAL_PORT)
    p.add_argument("--data-dir", type=Path, default=Path.home() / ".plex_relay")
    p.add_argument("--key-url", default=DEFAULT_RELAY_KEY_URL)
    p.add_argument("--insecure-key-url", action="store_true",
                   help="permit a non-HTTPS relay key URL (e.g. file:// for testing)")
    p.add_argument("--ssh", default="ssh", help="ssh binary")


def _config(args: argparse.Namespace) -> RelayConfig:
    return RelayConfig(
        token=args.token or "dry-run",
        ssh_user=args.user,
        local_host=args.local_host,
        local_port=args.local_port,
        relay_key_url=args.key_url,
        data_dir=args.data_dir,
        ssh_binary=args.ssh,
        allow_insecure_key_url=args.insecure_key_url,
    )


def _cmd_show(args: argparse.Namespace) -> int:
    cfg = _config(args)
    provider = RelayKeyProvider(
        cfg.relay_key_url,
        cfg.key_ttl_seconds,
        fetcher=HttpsRelayKeyFetcher(
            timeout=cfg.connect_timeout_seconds, allow_insecure=cfg.allow_insecure_key_url
        ),
    )
    endpoint = known_hosts_endpoint(args.host, args.port)
    try:
        key = provider.get()
        print(f"relay key   : {key.keytype} {key.keydata[:24]}... (from {cfg.relay_key_url})")
        print(f"known_hosts : {endpoint} {key.keytype} {key.keydata[:24]}...")
    except RelayError as exc:
        print(f"relay key   : <unavailable: {exc}>")
    argv = build_ssh_argv(cfg, args.host, args.port, cfg.cache_path)
    print("ssh argv    :\n  " + " ".join(argv))
    print("env         : PLEXTOKEN=*** SSH_ASKPASS=<helper> SSH_ASKPASS_REQUIRE=force")
    return 0


def _cmd_connect(args: argparse.Namespace) -> int:
    if not args.token:
        print("error: no token (set $PLEX_RELAY_TOKEN or --token)", file=sys.stderr)
        return 2
    from .controller import RelayController

    cfg = _config(args)
    stop = threading.Event()
    with RelayController.from_config(cfg) as ctrl:
        if not ctrl.start_relay(args.host, args.port):
            print("error: relay did not start (gating, already active, or failure)", file=sys.stderr)
            return 1
        print(f"relay up: {args.host}:{args.port} -> {cfg.local_host}:{cfg.local_port} (Ctrl-C to stop)")
        signal.signal(signal.SIGINT, lambda *_: stop.set())
        signal.signal(signal.SIGTERM, lambda *_: stop.set())
        while not stop.is_set():
            stop.wait(2.0)
            if not ctrl.active_hosts:
                print("relay connection ended", file=sys.stderr)
                return 1
    print("relay stopped")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="plex-relay", description=__doc__)
    parser.add_argument("-v", "--verbose", action="store_true")
    sub = parser.add_subparsers(dest="cmd", required=True)
    for name, func, help_ in (("show", _cmd_show, "resolve key + print ssh argv (no spawn)"),
                              ("connect", _cmd_connect, "establish and hold the relay tunnel")):
        sp = sub.add_parser(name, help=help_)
        _add_common(sp)
        sp.set_defaults(func=func)

    args = parser.parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(levelname)s %(name)s: %(message)s",
    )
    try:
        return int(args.func(args))
    except RelayError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
