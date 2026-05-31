// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// PE image introspection: base address, section bounds, version guard.
//
// Replaces the Linux ``dl_iterate_phdr`` path. All functions operate on the
// in-process image at the address returned by ``GetModuleHandleW(NULL)``,
// so they are valid under ASLR and usable from an injected DLL.
//
// Design:
//   - ``ImageInfo`` is a plain aggregate (no methods, no invariants to break),
//     constructed by ``get_main_image()``.
//   - ``verify_version()`` is a pure scan with no side effects.
//   - ``resolve_rva()`` returns a typed pointer, bounds-checked against the
//     writable data section so a wrong RVA cannot silently corrupt code.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

namespace plex {

// Section-level bounds for the main executable.
struct ImageInfo {
    uintptr_t base       = 0;    // Module base (HMODULE)
    uintptr_t image_size = 0;    // SizeOfImage from the optional header
    uintptr_t text_start = 0;    // First executable byte
    uintptr_t text_end   = 0;    // One past the last executable byte
    uintptr_t data_start = 0;    // First writable, non-executable byte
    uintptr_t data_end   = 0;    // One past the last such byte
};

// Parse the PE headers of the main executable.
inline std::optional<ImageInfo> get_main_image() {
    const auto base = reinterpret_cast<uintptr_t>(::GetModuleHandleW(nullptr));
    if (!base) return std::nullopt;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return std::nullopt;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return std::nullopt;
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return std::nullopt;

    ImageInfo info{};
    info.base       = base;
    info.image_size = nt->OptionalHeader.SizeOfImage;

    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        const uintptr_t start = base + sec->VirtualAddress;
        const uintptr_t end   = start + sec->Misc.VirtualSize;

        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            if (!info.text_start || start < info.text_start) info.text_start = start;
            if (end > info.text_end) info.text_end = end;
        }
        // Writable, non-executable = data/bss (where the bitset lives).
        if ((sec->Characteristics & IMAGE_SCN_MEM_WRITE) &&
            !(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) {
            if (!info.data_start || start < info.data_start) info.data_start = start;
            if (end > info.data_end) info.data_end = end;
        }
    }
    return info;
}

// Scan the image for a build-version string. Returns true iff the exact
// version is found, guarding all hardcoded RVAs against applying to the
// wrong build.
inline bool verify_version(const ImageInfo& img, std::string_view expected) {
    if (expected.empty() || !img.base || !img.image_size) return false;

    const auto* haystack = reinterpret_cast<const char*>(img.base);
    const size_t limit   = img.image_size - expected.size();

    for (size_t i = 0; i <= limit; ++i) {
        if (std::memcmp(haystack + i, expected.data(), expected.size()) == 0)
            return true;
    }
    return false;
}

// Convert an RVA to a typed pointer, bounds-checked against the writable data
// section. Returns nullptr if the address falls outside .data/.bss.
template <typename T>
T* resolve_data_rva(const ImageInfo& img, uint32_t rva, size_t extent = sizeof(T)) {
    const uintptr_t va = img.base + rva;
    if (va < img.data_start || va + extent > img.data_end) return nullptr;
    return reinterpret_cast<T*>(va);
}

// Convert an RVA to a code pointer (bounds-checked against .text).
inline const uint8_t* resolve_text_rva(const ImageInfo& img, uint32_t rva) {
    const uintptr_t va = img.base + rva;
    if (va < img.text_start || va >= img.text_end) return nullptr;
    return reinterpret_cast<const uint8_t*>(va);
}

}  // namespace plex
