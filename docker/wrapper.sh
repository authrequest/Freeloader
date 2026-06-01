#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# In-container launcher for Plex Media Server. Used by the patched
# plexinc/pms-docker and lscr.io/linuxserver/plex images; both Dockerfiles
# replace the upstream s6 service run file so it execs this script.
#
# Mirrors scripts/plex-crack-wrapper.sh from the native systemd install, with
# one key invariant:
#
#   LD_PRELOAD is exported *last*, immediately before `exec`, so the glibc
#   shell helpers spawned for the PLEX_MEDIA_SERVER_INFO_* assignments
#   (grep/awk/uname/tr) are NOT preloaded. The .so's constructor
#   (src/main.cpp) calls unsetenv("LD_PRELOAD") when it loads into the
#   (musl) PMS process, so PMS's glibc helper children (Tuner Service,
#   Script Host, transcoders) are unaffected as well.
#
# `exec` is mandatory so s6-supervise sees PMS as the supervised process
# (no fork). "$@" preserves any args the upstream invocation might add.

set -eu

# 1. Platform strings PMS echoes in /identity. Mirrors upstream start.sh.
export PLEX_MEDIA_SERVER_INFO_VENDOR="$(grep ^NAME= /etc/os-release | awk -F= '{print $2}' | tr -d '"')"
export PLEX_MEDIA_SERVER_INFO_MODEL="$(uname -m)"
export PLEX_MEDIA_SERVER_INFO_PLATFORM_VERSION="$(grep ^VERSION= /etc/os-release | awk -F= '{print $2}' | tr -d '"')"

# 2. Set LD_PRELOAD only now. The .so path matches where the Dockerfiles
#    copy it. Do NOT change this without updating both Dockerfiles.
export LD_PRELOAD="/usr/lib/plexmediaserver/lib/plexmediaserver_crack.so"

# 3. Hand off to PMS.
exec "/usr/lib/plexmediaserver/Plex Media Server" "$@"
