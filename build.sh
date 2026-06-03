#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Build plexmediaserver_crack.so for Plex Media Server on Linux.
#
# IMPORTANT: Plex ships and runs against its OWN bundled musl libc + libgcompat
# (see /usr/lib/plexmediaserver/lib/{libc.so,ld-musl-x86_64.so.1,libgcompat.so.0}).
# A glibc-built .so will NOT load into Plex -- the dynamic loader fails to
# relocate glibc-only symbols (__isoc23_strtol, arc4random, *_chk, _dl_find_object)
# and Plex exits 127. We therefore cross-compile against musl with zig, which
# bundles musl for clean cross-compilation.
#
# IMPORTANT: The .so must NOT statically link libc++ or libc++abi. Plex's
# runtime uses GCC's libstdc++ for C++ exception handling. If our .so defines
# __cxa_throw/__cxa_begin_catch/__gxx_personality_v0 (from libc++abi), they
# override libstdc++'s versions via LD_PRELOAD, breaking boost::filesystem
# exception handling and crashing Plex. We use -nostdlib++ and strip all C++
# runtime usage from the source to avoid this entirely.
#
# Injection is done with LD_PRELOAD (NOT patchelf): patchelf rewrites the 22MB
# BIND_NOW/PIE binary's program headers in a way musl's loader cannot tolerate,
# which corrupts the executable (instant SIGSEGV on start). See install notes
# printed at the end.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

ZIG_VERSION="0.13.0"
TARGET="x86_64-linux-musl"
OUT="build/plexmediaserver_crack.so"

echo "=== Plex Media Server Crack - Linux (musl) Build ==="

# Resolve a zig toolchain: $ZIG override, then PATH, then a local download.
if [ -n "${ZIG:-}" ] && [ -x "${ZIG}" ]; then
    :
elif command -v zig &> /dev/null; then
    ZIG="$(command -v zig)"
else
    ZIG_DIR="toolchain/zig-linux-x86_64-${ZIG_VERSION}"
    if [ ! -x "${ZIG_DIR}/zig" ]; then
        echo "zig not found; downloading ${ZIG_VERSION}..."
        mkdir -p toolchain
        curl -fL --connect-timeout 20 \
            -o "toolchain/zig.tar.xz" \
            "https://ziglang.org/download/${ZIG_VERSION}/zig-linux-x86_64-${ZIG_VERSION}.tar.xz"
        tar -C toolchain -xf "toolchain/zig.tar.xz"
    fi
    ZIG="${ZIG_DIR}/zig"
fi
echo "Using zig: ${ZIG} ($("${ZIG}" version))"

# Required sources.
for f in src/hook.cpp src/hook.hpp src/main.cpp third_party/zydis/Zydis.c third_party/zydis/Zydis.h; do
    [ -f "$f" ] || { echo "ERROR: missing $f"; exit 1; }
done

CFLAGS=(-target "${TARGET}" -O2 -fPIC -I src -I third_party/zydis)
CXXFLAGS=(-target "${TARGET}" -std=c++20 -O2 -fPIC -fno-exceptions -fno-rtti -nostdlib++ -I src -I third_party/zydis)

mkdir -p build
echo "=== compiling Zydis.c (C) ==="
"${ZIG}" cc  "${CFLAGS[@]}"   -c third_party/zydis/Zydis.c -o build/Zydis.o
echo "=== compiling hook.cpp (C++) ==="
"${ZIG}" c++ "${CXXFLAGS[@]}" -c src/hook.cpp -o build/hook.o
echo "=== compiling main.cpp (C++) ==="
"${ZIG}" c++ "${CXXFLAGS[@]}" -c src/main.cpp -o build/main.o
echo "=== linking ${OUT} ==="
"${ZIG}" c++ -target "${TARGET}" -nostdlib++ -shared -o "${OUT}" build/main.o build/hook.o build/Zydis.o
rm -f build/Zydis.o build/hook.o build/main.o

echo ""
echo "=== ABI sanity check (external symbols must all be musl libc) ==="
# Any of these glibc-only names appearing as UND means the .so will fail to load.
if readelf --dyn-syms "${OUT}" | grep -E "UND .*(__isoc23_|_chk$|arc4random|_dl_find_object)" ; then
    echo "ERROR: glibc-only symbols present -- this will not load into Plex."
    exit 1
fi
echo "OK: only musl libc symbols are referenced."
echo "NEEDED: $(readelf -d "${OUT}" | awk '/NEEDED/{print $5}' | tr -d '[]' | tr '\n' ' ')"
echo ""
echo "=== BUILD SUCCESSFUL: $(pwd)/${OUT} ($(stat -c %s "${OUT}") bytes) ==="

cat <<'EOF'

Install (proper method -- LD_PRELOAD, no patchelf):

1. Copy the artifacts to the Plex host:
     build/plexmediaserver_crack.so  -> /usr/lib/plexmediaserver/lib/plexmediaserver_crack.so
     scripts/plex-crack-wrapper.sh   -> /usr/local/bin/   (chmod 755)

2. Add a systemd drop-in that swaps ExecStart for the wrapper (the wrapper sets
   LD_PRELOAD *after* /bin/sh starts, so only the musl Plex process is preloaded
   and glibc helper children are unaffected):

     mkdir -p /etc/systemd/system/plexmediaserver.service.d
     printf '[Service]\nExecStart=\nExecStart=/usr/local/bin/plex-crack-wrapper.sh\n' \
       > /etc/systemd/system/plexmediaserver.service.d/override.conf

3. Apply:
     systemctl daemon-reload
     systemctl restart plexmediaserver

To uninstall: remove the drop-in (and rm the .so), then daemon-reload + restart.
Do NOT use `patchelf --add-needed` on the Plex binary -- it corrupts it under
musl's loader. If a previous attempt did, restore with:
     apt-get install --reinstall plexmediaserver   # or dpkg -i the matching .deb
EOF
