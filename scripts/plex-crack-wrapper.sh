#!/bin/sh
# Launcher for plexmediaserver that LD_PRELOADs the (musl-built) crack into the
# Plex Media Server process only. LD_PRELOAD must NOT be set via the unit's
# Environment= because ExecStart is run by /bin/sh (glibc) first, and a musl .so
# cannot load into a glibc process. Setting it here, after this shell is already
# running, means only the final `exec` of the (musl) Plex binary is preloaded.
#
# The three PLEX_MEDIA_SERVER_INFO_* exports mirror the stock unit's ExecStart.
export PLEX_MEDIA_SERVER_INFO_VENDOR="$(grep ^NAME= /etc/os-release | awk -F= '{print $2}' | tr -d '"')"
export PLEX_MEDIA_SERVER_INFO_MODEL="$(uname -m)"
export PLEX_MEDIA_SERVER_INFO_PLATFORM_VERSION="$(grep ^VERSION= /etc/os-release | awk -F= '{print $2}' | tr -d '"')"
export LD_PRELOAD="/usr/lib/plexmediaserver/lib/plexmediaserver_crack.so"
exec "/usr/lib/plexmediaserver/Plex Media Server"
