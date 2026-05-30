#include "hook.hpp"

#include <cstdlib>

__attribute__((constructor)) void init_so()
{
    // This library is LD_PRELOADed into the (musl) "Plex Media Server" process.
    // Plex later spawns glibc /bin/sh helpers (Plex Tuner Service, Plex Script
    // Host, transcoders) which would inherit LD_PRELOAD and fail to load this
    // musl .so ("/bin/sh: error while loading shared libraries"). The constructor
    // runs before main() and before any child is spawned, so clearing LD_PRELOAD
    // here scopes the preload to this process only.
    unsetenv("LD_PRELOAD");

    hook();
}