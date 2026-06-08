// SPDX-License-Identifier: AGPL-3.0-or-later
#include "hook.hpp"
#include "webhook_handler.hpp"

#include <stdlib.h>

// ---------------------------------------------------------------------------
// The old constructor called hook() synchronously, but under musl the
// LD_PRELOAD library's constructor runs *before* the main binary is loaded
// → dl_iterate_phdr returned 0 callbacks, and /proc/self/maps doesn't yet
// contain the PMS text segment → hook() = no-op.
//
// We now combine two strategies:
//
//   1. Try hook() immediately (best-effort – will fail if the binary
//      isn't mapped yet, which is both harmless and informative).
//
//   2. Override a function that PMS calls during runtime startup but well
//      after all libraries are loaded.  When that override first fires, it
//      calls hook() and then passes through to the real function.
//
// Strategy 2 uses a trigger that's guaranteed to fire during normal PMS
// operation but AFTER boost::uuids, epoll, and the HTTP server are all
// initialised.  We override `epoll_create1` (called by the event loop)
// rather than `bind` (which the musl dynamic linker may reference during
// loading, causing boost UUID crashes).
//
// Both strategies are idempotent: hook() is guarded by the TextSpan check
// on the inside (returns immediately if text range not found / hooks
// already set), and the deferred trigger is a one-shot flag.
// ---------------------------------------------------------------------------

static int g_hooks_attempted = 0;

// ---- deferred trigger: epoll_create1 override ----
//
// PMS uses epoll for its event loop.  The first epoll_create1 call happens
// during server initialisation, long after the main binary is fully mapped.
// This is a safer trigger than bind (which libc's NSS/resolver may call
// during dlopen, triggering boost::uuids crashes).

#include <sys/epoll.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/syscall.h>

typedef int (*RealEpollFn)(int);

static int g_epoll_triggered = 0;

int epoll_create1(int flags)
{
    if(!g_epoll_triggered)
    {
        g_epoll_triggered = 1;
        hook();

        RealEpollFn real = (RealEpollFn)dlsym(RTLD_NEXT, "epoll_create1");
        if(real)
            return real(flags);
        return syscall(SYS_epoll_create1, flags);
    }

    RealEpollFn real = (RealEpollFn)dlsym(RTLD_NEXT, "epoll_create1");
    if(real)
        return real(flags);
    return syscall(SYS_epoll_create1, flags);
}

__attribute__((constructor)) void init_so()
{
    // This library is LD_PRELOADed into the (musl) "Plex Media Server" process.
    // Plex later spawns glibc /bin/sh helpers (Plex Tuner Service, Plex Script
    // Host, transcoders) which would inherit LD_PRELOAD and fail to load this
    // musl .so ("/bin/sh: error while loading shared libraries"). The constructor
    // runs before main() and before any child is spawned, so clearing LD_PRELOAD
    // here scopes the preload to this process only.
    unsetenv("LD_PRELOAD");

    // Socket hooks in webhook_handler.cpp are active as soon as the library is
    // preloaded. Resolve their real libc targets before PMS startup code reads
    // from /dev/urandom; otherwise our read() interposer would fail early reads.
    webhook_handler_init();

    // Strategy 1: skipped — hook() will be triggered by the first
    // epoll_create1 call, which happens during PMS's event loop init
    // after everything is loaded.
}
