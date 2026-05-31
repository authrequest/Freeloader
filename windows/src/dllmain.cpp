// SPDX-License-Identifier: AGPL-3.0-or-later
//
// DLL entry point for the Plex feature-unlock patch (Windows x64).
//
// Loaded into the Plex Media Server process via the injector. Work is deferred
// to a background thread: DllMain runs under the loader lock, where calling
// non-trivial APIs (thread sync, LoadLibrary, ...) is forbidden. The
// background thread waits for Plex to finish init, then applies the patch.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "feature_patch.h"
#include "log.h"

static DWORD WINAPI patch_entry(LPVOID) {
    plex::log(plex::LogLevel::kInfo,
              "plex_patch DLL loaded (pid %lu)", ::GetCurrentProcessId());

    const auto result = plex::apply_patch();

    if (!result.version_ok) {
        plex::log(plex::LogLevel::kError,
                  "patch ABORTED: build mismatch (expected %s)", plex::kExpectedVersion);
        return 1;
    }
    plex::log(plex::LogLevel::kInfo,
              "patch applied: bitset=%s hook=%s guard=%s",
              result.bitset_ok ? "OK" : "FAIL",
              result.hook_ok   ? "OK" : "SKIP",
              result.guard_ok  ? "OK" : "FAIL");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        ::DisableThreadLibraryCalls(hModule);
        HANDLE t = ::CreateThread(nullptr, 0, patch_entry, nullptr, 0, nullptr);
        if (t) ::CloseHandle(t);  // detach; thread runs independently
    } else if (reason == DLL_PROCESS_DETACH) {
        plex::remove_patch();
    }
    return TRUE;
}
