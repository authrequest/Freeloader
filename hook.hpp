#pragma once

#include <cstdint>
#include <string>
#include <tuple>
#include <optional>

std::optional<std::tuple<uintptr_t, uintptr_t>> get_dottext_info();
std::optional<uintptr_t> create_hook(uintptr_t from, uintptr_t to);
std::optional<uintptr_t> sig_scan(const uintptr_t start, const uintptr_t end, std::string_view pattern);
uintptr_t follow_call_rel32(const uintptr_t address);
uint64_t hook_is_feature_available(uintptr_t rcx, const char** guid);
uint64_t* hook_map_find(uintptr_t* rcx, const char** str);
uint64_t hook_bitset_init(uintptr_t rcx);
bool hook_is_user_feature_set(uintptr_t rcx, int expected, int feature);
void hook();