// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <stdint.h>
#include <stddef.h>

// Return type for functions that may fail (no C++ exceptions/rtti).
// check `ok` before using `addr` / `start` / `end`.
struct OptAddr  { bool ok; uintptr_t addr; };
struct TextSpan { bool ok; uintptr_t start; uintptr_t end; };

TextSpan get_dottext_info();
OptAddr create_hook(uintptr_t from, uintptr_t to);
OptAddr sig_scan(uintptr_t start, uintptr_t end, const char* pattern);
uintptr_t follow_call_rel32(uintptr_t address);

// Hook callbacks (thunk style).
uint64_t hook_is_feature_available(uintptr_t rcx, const char** guid);
uint64_t* hook_map_find(uintptr_t* rcx, const char** str);
uint64_t hook_bitset_init(uintptr_t rcx);
bool     hook_is_user_feature_set(uintptr_t rcx, int expected, int feature);
uint64_t hook_sub_122B2F2(uintptr_t prefs, char* key);
uint64_t hook_sub_125ACA6(uintptr_t manager);

void hook();
