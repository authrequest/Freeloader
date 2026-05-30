#!/bin/bash
# Build plexmediaserver_crack.so for Plex Media Server on Linux.
#
# IMPORTANT: Plex ships and runs against its OWN bundled musl libc + libgcompat
# (see /usr/lib/plexmediaserver/lib/{libc.so,ld-musl-x86_64.so.1,libgcompat.so.0}).
# A glibc-built .so will NOT load into Plex -- the dynamic loader fails to
# relocate glibc-only symbols (__isoc23_strtol, arc4random, *_chk, _dl_find_object)
# and Plex exits 127. We therefore cross-compile against musl with zig, which
# bundles musl + libc++ and statically links the C++ runtime, leaving only musl
# libc references that Plex's bundled libc.so satisfies.
#
# Injection is done with LD_PRELOAD (NOT patchelf): patchelf rewrites the 22MB
# BIND_NOW/PIE binary's program headers in a way musl's loader cannot tolerate,
# which corrupts the executable (instant SIGSEGV on start). See install notes
# printed at the end.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

ZIG_VERSION="0.13.0"
TARGET="x86_64-linux-musl"
OUT="plexmediaserver_crack.so"

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
for f in hook.cpp hook.hpp main.cpp Zydis.c Zydis.h; do
    [ -f "$f" ] || { echo "ERROR: missing $f"; exit 1; }
done

CFLAGS=(-target "${TARGET}" -O2 -fPIC -I.)
CXXFLAGS=(-target "${TARGET}" -std=c++20 -O2 -fPIC -I.)

echo "=== compiling Zydis.c (C) ==="
"${ZIG}" cc  "${CFLAGS[@]}"   -c Zydis.c -o build_Zydis.o
echo "=== compiling hook.cpp (C++) ==="
"${ZIG}" c++ "${CXXFLAGS[@]}" -c hook.cpp -o build_hook.o
echo "=== compiling main.cpp (C++) ==="
"${ZIG}" c++ "${CXXFLAGS[@]}" -c main.cpp -o build_main.o
echo "=== linking ${OUT} ==="
"${ZIG}" c++ -target "${TARGET}" -shared -o "${OUT}" build_main.o build_hook.o build_Zydis.o
rm -f build_Zydis.o build_hook.o build_main.o

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
     plexmediaserver_crack.so  -> /usr/lib/plexmediaserver/lib/
     plex-crack-wrapper.sh     -> /usr/local/bin/   (chmod 755)

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
