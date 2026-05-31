// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// x86-64 inline hook via a 14-byte absolute indirect jump.
//
// Ported from the Linux ``create_hook()`` in ``src/hook.cpp``.
// Differences from the POSIX version:
//   - VirtualAlloc / VirtualProtect instead of mmap / mprotect.
//   - FlushInstructionCache after patching (required on Windows).
//   - The hook site and trampoline share the same 14-byte shellcode layout:
//         FF 25 00 00 00 00  <8-byte absolute target>   // jmp [rip+0]
//
// Zydis (vendored, MIT) decodes the prologue to ensure we relocate only
// complete instructions. The trampoline contains:
//     [relocated prologue bytes] [14-byte jmp to original+offset]
// and the patched call site is:
//     [14-byte jmp to hook function]
//
// Returns the trampoline address (= pointer to the "original" function that
// the hook body calls through to execute the un-hooked path).
//
// Thread safety: installing a hook while other threads may be executing the
// target function is inherently racy on x64 (no single atomic 14-byte write).
// Install hooks early — from DLL_PROCESS_ATTACH on a suspended process — to
// avoid this.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <optional>

#include "Zydis.h"
#include "log.h"

namespace plex {

// Absolute indirect jump: ``jmp [rip+0]`` followed by an 8-byte address.
inline constexpr size_t kJmpSize = 14;

inline void write_abs_jmp(uint8_t* site, uintptr_t target) {
    // FF 25 00 00 00 00 = jmp qword ptr [rip+0]
    site[0] = 0xFF;
    site[1] = 0x25;
    site[2] = site[3] = site[4] = site[5] = 0x00;
    std::memcpy(site + 6, &target, 8);
}

// Install a 14-byte inline hook at ``from``, redirecting to ``to``.
// Returns the trampoline (original function entry) on success.
inline std::optional<uintptr_t> create_hook(uintptr_t from, uintptr_t to) {
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    ZydisDecodedInstruction inst;

    // --- 1. Determine prologue length (>= 14 bytes of complete instructions) ---
    size_t stolen = 0;
    while (stolen < kJmpSize) {
        const auto* ip = reinterpret_cast<const void*>(from + stolen);
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(
                &decoder, nullptr, ip, 15 /*max x64 len*/, &inst))) {
            log(LogLevel::kError, "trampoline: failed to decode at %p+%zu",
                reinterpret_cast<void*>(from), stolen);
            return std::nullopt;
        }
        stolen += inst.length;
    }

    // --- 2. Allocate the trampoline (RW, flipped to RX after write) ----------
    const size_t tramp_size = stolen + kJmpSize;
    auto* tramp = static_cast<uint8_t*>(
        ::VirtualAlloc(nullptr, tramp_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!tramp) {
        log(LogLevel::kError, "trampoline: VirtualAlloc failed (%lu)", ::GetLastError());
        return std::nullopt;
    }

    // Copy the stolen prologue bytes, then append a jump back to from+stolen.
    std::memcpy(tramp, reinterpret_cast<const void*>(from), stolen);
    write_abs_jmp(tramp + stolen, from + stolen);

    DWORD old_prot = 0;
    ::VirtualProtect(tramp, tramp_size, PAGE_EXECUTE_READ, &old_prot);
    ::FlushInstructionCache(::GetCurrentProcess(), tramp, tramp_size);

    // --- 3. Patch the original site to jump to our hook ----------------------
    DWORD site_prot = 0;
    if (!::VirtualProtect(reinterpret_cast<void*>(from), kJmpSize,
                          PAGE_EXECUTE_READWRITE, &site_prot)) {
        log(LogLevel::kError, "trampoline: VirtualProtect(hook site) failed (%lu)",
            ::GetLastError());
        ::VirtualFree(tramp, 0, MEM_RELEASE);
        return std::nullopt;
    }

    write_abs_jmp(reinterpret_cast<uint8_t*>(from), to);

    ::VirtualProtect(reinterpret_cast<void*>(from), kJmpSize, site_prot, &site_prot);
    ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(from), kJmpSize);

    log(LogLevel::kInfo, "hook installed: %p -> %p  (trampoline at %p, %zu stolen bytes)",
        reinterpret_cast<void*>(from), reinterpret_cast<void*>(to),
        static_cast<void*>(tramp), stolen);
    return reinterpret_cast<uintptr_t>(tramp);
}

}  // namespace plex
