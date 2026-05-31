// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// Byte-pattern signature scanner.
//
// Ported from the Linux ``hook.cpp`` ``sig_scan()`` with one addition:
// ``resolve_rip_rel32()`` decodes a RIP-relative displacement at a matched
// site, which is how we recover absolute addresses from x64 instructions.
//
// Pattern format (same as IDA/Linux side):
//   "48 8D 0D ?? ?? ?? ??"   hex bytes; ?? = one-byte wildcard
//
// This is a pure scan over [start, end) — no allocations, no side effects.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

namespace plex {

// A compiled pattern ready for scanning.  Opaque; use ``compile_pattern()``.
struct Pattern {
    struct Atom { uint8_t byte; bool wild; };
    std::vector<Atom> atoms;
};

// Compile a hex+wildcard string into a scannable pattern.
inline std::optional<Pattern> compile_pattern(std::string_view text) {
    Pattern pat;
    for (size_t i = 0; i < text.size(); ) {
        const char c = text[i];
        if (c == ' ') { ++i; continue; }
        if (c == '?') {
            // Consume '?' or '??'.
            if (i + 1 < text.size() && text[i + 1] == '?') ++i;
            pat.atoms.push_back({0, true});
            ++i;
            continue;
        }
        // Two hex characters.
        if (i + 1 >= text.size()) return std::nullopt;
        char pair[3] = {text[i], text[i + 1], '\0'};
        char* end = nullptr;
        const unsigned long v = std::strtoul(pair, &end, 16);
        if (end != pair + 2 || v > 0xFF) return std::nullopt;
        pat.atoms.push_back({static_cast<uint8_t>(v), false});
        i += 2;
    }
    if (pat.atoms.empty()) return std::nullopt;
    return pat;
}

// Scan [start, end) for the first occurrence of ``pat``.
inline std::optional<uintptr_t> sig_scan(
    uintptr_t start, uintptr_t end, const Pattern& pat) {
    const size_t len = pat.atoms.size();
    if (len == 0 || end <= start || end - start < len) return std::nullopt;

    const auto* mem = reinterpret_cast<const uint8_t*>(start);
    const size_t limit = (end - start) - len;

    for (size_t i = 0; i <= limit; ++i) {
        bool match = true;
        for (size_t j = 0; j < len; ++j) {
            if (!pat.atoms[j].wild && mem[i + j] != pat.atoms[j].byte) {
                match = false;
                break;
            }
        }
        if (match) return start + i;
    }
    return std::nullopt;
}

// Convenience: compile + scan in one call.
inline std::optional<uintptr_t> sig_scan(
    uintptr_t start, uintptr_t end, std::string_view pattern_text) {
    auto pat = compile_pattern(pattern_text);
    if (!pat) return std::nullopt;
    return sig_scan(start, end, *pat);
}

// Resolve a RIP-relative ``disp32`` at ``inst_addr + disp_offset`` within an
// instruction of ``inst_len`` bytes.  Returns the absolute target address.
//
// Use case: a ``lea rcx, [rip + disp32]`` at a matched site lets us recover
// the address of a global (e.g. the feature bitset) without hardcoding its RVA.
inline uintptr_t resolve_rip_rel32(
    uintptr_t inst_addr, size_t disp_offset, size_t inst_len) {
    const auto disp = *reinterpret_cast<const int32_t*>(inst_addr + disp_offset);
    return inst_addr + inst_len + disp;
}

}  // namespace plex
