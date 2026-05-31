# SPDX-License-Identifier: AGPL-3.0-or-later
import pytest

from plex_relay.errors import RelayKeyError
from plex_relay.models import HostKey, RelayKey, known_hosts_endpoint, parse_relay_pub

PUB = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIabc relay@plex"  # keytype keydata comment
KH = "* ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIabc"            # host keytype keydata
EXPECT = RelayKey("ssh-ed25519", "AAAAC3NzaC1lZDI1NTE5AAAAIabc")


def test_parse_pubkey_form():
    assert parse_relay_pub(PUB) == EXPECT


def test_parse_known_hosts_form():
    assert parse_relay_pub(KH) == EXPECT


def test_parse_skips_comments_and_blanks():
    assert parse_relay_pub(f"# header\n\n{PUB}\n") == EXPECT


@pytest.mark.parametrize("bad", ["ssh-ed25519 onlytwo", "aaa bbb ccc", "", "# only comment\n"])
def test_parse_rejects_bad_payloads(bad):
    with pytest.raises(RelayKeyError):
        parse_relay_pub(bad)


def test_endpoint_bracket_notation():
    assert known_hosts_endpoint("1.2.3.4", 443) == "[1.2.3.4]:443"
    assert known_hosts_endpoint("relay.example", 2222) == "[relay.example]:2222"
    assert known_hosts_endpoint("relay.example", 22) == "relay.example"


def test_endpoint_rejects_empty_host():
    with pytest.raises(RelayKeyError):
        known_hosts_endpoint("   ", 443)


def test_hostkey_serialization():
    hk = HostKey.for_endpoint("[1.2.3.4]:443", EXPECT)
    assert hk.known_hosts_line() == "[1.2.3.4]:443 ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIabc"
    assert hk.cache_block() == f"# [1.2.3.4]:443\n{hk.known_hosts_line()}\n"
    assert hk.key == EXPECT
