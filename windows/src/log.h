// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// Minimal, zero-allocation logging to OutputDebugString.
//
// Every log line is prefixed with "plex_patch" so it stands out in DbgView /
// WinDbg. The format matches the Linux side's printf style.  No heap allocs;
// buffer is on the stack so this is safe inside DllMain / loader-lock context
// for short messages (truncated at 511 chars rather than crashing).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace plex {

enum class LogLevel : uint8_t { kDebug, kInfo, kWarn, kError };

inline void log(LogLevel level, const char* fmt, ...) {
    static constexpr const char* kPrefix[] = {"DBG", "INF", "WRN", "ERR"};
    char buf[512];
    const int hdr = std::snprintf(
        buf, sizeof(buf), "[plex_patch %s] ",
        kPrefix[static_cast<uint8_t>(level)]);

    std::va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf + hdr, sizeof(buf) - hdr, fmt, args);
    va_end(args);

    ::OutputDebugStringA(buf);
}

}  // namespace plex
