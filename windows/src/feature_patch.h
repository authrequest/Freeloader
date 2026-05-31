// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// Feature-unlock patch for Plex Media Server (Windows x64).
//
// Two complementary mechanisms ensure every feature bit stays on:
//
//   1. **Hook** ``FeatureManager_set_features_from_uuids`` (the function that
//      populates the bitset after fetching /api/v2/features). The hook calls
//      the original, then atomically forces all 14 dwords to 0xFFFFFFFF. This
//      is deterministic: every refresh immediately becomes "all-enabled."
//
//   2. **Guard thread** polls at 2 s and re-forces if any dword reverted
//      (belt-and-suspenders for code paths that write the bitset outside the
//      hooked function, or if the hook fails to install).
//
// Discovery uses **RVA + version guard**: the expected build string must be
// present in the image before any RVA is applied. If the guard fails (wrong
// build), the patch refuses to activate rather than corrupting memory.
//
// All writes use ``InterlockedExchange``, matching the binary's own atomic
// store sequence exactly (14 × ``_InterlockedExchange``).
//
// Known values — Plex Media Server 1.43.2.10687-563d026ea (Windows x64):
//   g_feature_bitset                        RVA 0x1D9E670  (14 dwords, 56 bytes)
//   FeatureManager_set_features_from_uuids  RVA 0x0BC8060

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>

#include "log.h"
#include "pe_image.h"
#include "trampoline.h"

namespace plex {

// ---- constants (build-specific) -------------------------------------------

inline constexpr const char  kExpectedVersion[]    = "1.43.2.10687";
inline constexpr uint32_t    kBitsetRVA            = 0x1D9E670;
inline constexpr uint32_t    kPopulatorRVA         = 0x0BC8060;
inline constexpr uint32_t    kBitsetDwords         = 14;
inline constexpr uint32_t    kBitsetBytes          = kBitsetDwords * sizeof(LONG);
inline constexpr DWORD       kGuardIntervalMs      = 2000;
inline constexpr DWORD       kGuardInitialDelayMs  = 5000;  // let Plex finish startup

// ---- bitset operations (pure, testable) -----------------------------------

inline void force_bitset(volatile LONG* bitset) {
    for (uint32_t i = 0; i < kBitsetDwords; ++i)
        ::InterlockedExchange(&bitset[i], static_cast<LONG>(0xFFFFFFFF));
}

inline bool bitset_is_full(volatile LONG* bitset) {
    for (uint32_t i = 0; i < kBitsetDwords; ++i)
        if (::InterlockedCompareExchange(&bitset[i], 0, 0) != static_cast<LONG>(0xFFFFFFFF))
            return false;
    return true;
}

// ---- hook callback --------------------------------------------------------

// Microsoft x64 ABI: __fastcall (rcx, rdx, r8, r9).
// Signature from IDB: void __fastcall sub_140BC8060(char *a1, __int64 a2)
using SetFeaturesFn = void(__fastcall*)(void* this_ptr, void* uuid_vec);

inline volatile LONG*  g_bitset_ptr    = nullptr;
inline SetFeaturesFn   g_original_fn   = nullptr;

void __fastcall hooked_set_features(void* this_ptr, void* uuid_vec) {
    // Call the real populator so Plex's internal state is consistent.
    if (g_original_fn) g_original_fn(this_ptr, uuid_vec);
    // Now force every feature bit on.
    if (g_bitset_ptr) {
        force_bitset(g_bitset_ptr);
        log(LogLevel::kInfo, "feature bitset forced after set_features");
    }
}

// ---- guard thread ---------------------------------------------------------

inline std::atomic<bool> g_guard_active{false};
inline HANDLE            g_guard_thread = nullptr;

DWORD WINAPI guard_thread_fn(LPVOID) {
    log(LogLevel::kInfo, "guard thread: waiting %lu ms for Plex startup", kGuardInitialDelayMs);
    for (DWORD elapsed = 0; elapsed < kGuardInitialDelayMs && g_guard_active.load(); elapsed += 500)
        ::Sleep(500);

    while (g_guard_active.load()) {
        if (g_bitset_ptr && !bitset_is_full(g_bitset_ptr)) {
            force_bitset(g_bitset_ptr);
            log(LogLevel::kInfo, "guard thread: re-forced feature bitset");
        }
        ::Sleep(kGuardIntervalMs);
    }
    log(LogLevel::kInfo, "guard thread: stopped");
    return 0;
}

// ---- orchestration --------------------------------------------------------

struct PatchResult {
    bool version_ok  = false;
    bool bitset_ok   = false;
    bool hook_ok     = false;
    bool guard_ok    = false;
};

inline PatchResult apply_patch() {
    PatchResult r;

    auto img = get_main_image();
    if (!img) {
        log(LogLevel::kError, "failed to parse PE image");
        return r;
    }

    // Version guard: refuse to patch an unknown build.
    r.version_ok = verify_version(*img, kExpectedVersion);
    if (!r.version_ok) {
        log(LogLevel::kError, "version guard FAILED: expected '%s' not found in image",
            kExpectedVersion);
        return r;
    }
    log(LogLevel::kInfo, "version guard passed (build %s)", kExpectedVersion);

    // Resolve the feature bitset.
    g_bitset_ptr = resolve_data_rva<volatile LONG>(*img, kBitsetRVA, kBitsetBytes);
    r.bitset_ok = (g_bitset_ptr != nullptr);
    if (!r.bitset_ok) {
        log(LogLevel::kError, "bitset RVA 0x%X resolves outside writable data", kBitsetRVA);
        return r;
    }
    log(LogLevel::kInfo, "g_feature_bitset at %p (base %p + 0x%X)",
        const_cast<const void*>(reinterpret_cast<const volatile void*>(g_bitset_ptr)),
        reinterpret_cast<void*>(img->base), kBitsetRVA);

    // Immediate force (features may already be loaded).
    force_bitset(g_bitset_ptr);

    // Hook the populator so future refreshes are caught deterministically.
    const auto* populator = resolve_text_rva(*img, kPopulatorRVA);
    if (populator) {
        auto tramp = create_hook(
            reinterpret_cast<uintptr_t>(populator),
            reinterpret_cast<uintptr_t>(&hooked_set_features));
        if (tramp) {
            g_original_fn = reinterpret_cast<SetFeaturesFn>(*tramp);
            r.hook_ok = true;
            log(LogLevel::kInfo, "hook installed on FeatureManager_set_features_from_uuids");
        } else {
            log(LogLevel::kWarn, "hook install failed; guard thread will compensate");
        }
    } else {
        log(LogLevel::kWarn, "populator RVA 0x%X outside .text; skipping hook", kPopulatorRVA);
    }

    // Guard thread: belt-and-suspenders re-force on a 2 s poll.
    g_guard_active.store(true);
    g_guard_thread = ::CreateThread(nullptr, 0, guard_thread_fn, nullptr, 0, nullptr);
    r.guard_ok = (g_guard_thread != nullptr);
    if (r.guard_ok)
        log(LogLevel::kInfo, "guard thread started (interval %lu ms)", kGuardIntervalMs);

    return r;
}

inline void remove_patch() {
    g_guard_active.store(false);
    if (g_guard_thread) {
        ::WaitForSingleObject(g_guard_thread, 5000);
        ::CloseHandle(g_guard_thread);
        g_guard_thread = nullptr;
    }
    // Note: the trampoline and hook-site patch are NOT reversed on unload.
    // Reversing an inline hook while threads may be executing the trampoline is
    // unsafe. The DLL stays loaded for the process lifetime anyway.
}

}  // namespace plex
