#include "hook.hpp"

#include <vector>
#include <iostream>
#include <fstream>
#include <cstring>
#include <sstream>
// #include <unordered_map> // No longer needed - Godmode approach
#include <bitset>
#include <unistd.h>
#include <sys/mman.h>
#include "Zydis.h"

// Feature GUIDs removed - now using Godmode approach (return true for all features)
// This is more robust against PMS updates as it doesn't depend on knowing specific GUIDs

std::bitset<704>* g_feature_flags;
auto _is_feature_available = reinterpret_cast<decltype(&hook_is_feature_available)>(0);
auto _map_find = reinterpret_cast<decltype(&hook_map_find)>(0);
auto _bitset_init = reinterpret_cast<decltype(&hook_bitset_init)>(0);
auto _is_user_feature_set = reinterpret_cast<decltype(&hook_is_user_feature_set)>(0);

std::optional<std::tuple<uintptr_t, uintptr_t>> get_dottext_info()
{
	std::ifstream file("/proc/self/maps");
	std::string line;
	
	while(std::getline(file, line))
	{
		// I don't understand Linux :|
		// Only .text should have `r-xp`. This works I guess..
		if(line.find("Plex Media Server") != std::string::npos && line.find("r-xp") != std::string::npos)
		{
			const uintptr_t start = std::stoull(line.substr(0, line.find('-')), nullptr, 16);
			const uintptr_t end = std::stoull(line.substr(line.find('-') + 1), nullptr, 16);

			return std::make_tuple(start, end);
		}
	}

	return std::nullopt;
}

std::optional<uintptr_t> create_hook(uintptr_t from, uintptr_t to)
{
	ZydisDecoder decoder;
	ZydisDecodedInstruction instruction;
	ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

	auto trampoline_mem = reinterpret_cast<uint8_t*>(mmap(NULL, getpagesize(), PROT_READ|PROT_WRITE|PROT_EXEC, MAP_ANONYMOUS|MAP_SHARED, -1, 0));
	size_t offset = 0;

	while(offset < 14)
	{
		if(ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder, nullptr, reinterpret_cast<void*>(from + offset), ZYDIS_MAX_INSTRUCTION_LENGTH, &instruction)))
		{
			std::memcpy(trampoline_mem + offset, reinterpret_cast<uintptr_t*>(from + offset), instruction.length);
			offset += instruction.length;
		}
		
		else
		{
			delete[] trampoline_mem;

			return std::nullopt;
		}
	}

	uint8_t shellcode[] =
	{
		0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, // jmp [rip+0x06]
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 // ?
	};

	// Jump to original code in trampoline and make trampoline executable
	*reinterpret_cast<uintptr_t*>(&shellcode[6]) = from + offset;
	std::memcpy(trampoline_mem + offset, shellcode, sizeof(shellcode));
	mprotect(reinterpret_cast<void*>(trampoline_mem), 64, PROT_READ|PROT_EXEC);

	// Jump to target code
	*reinterpret_cast<uintptr_t*>(&shellcode[6]) = to;
	mprotect(reinterpret_cast<void*>(from), sizeof(shellcode), PROT_READ|PROT_WRITE|PROT_EXEC);
	memcpy(reinterpret_cast<void*>(from), shellcode, sizeof(shellcode));
	mprotect(reinterpret_cast<void*>(from), sizeof(shellcode), PROT_READ|PROT_EXEC);

	return reinterpret_cast<uintptr_t>(trampoline_mem);
}

std::optional<uintptr_t> sig_scan(const uintptr_t start, const uintptr_t end, std::string_view pattern)
{
	constexpr const uint16_t WILDCARD = 0xFFFF;
	std::vector<uint16_t> pattern_vec;
	
	for(uintptr_t i = 0; i < pattern.length(); i++)
	{
		if(pattern[i] == ' ')
		{
			continue;
		}

		if(pattern[i] == '?')
		{
			if(pattern[i + 1] == '?')
			{
				i++;
			}

			pattern_vec.push_back(WILDCARD);

			continue;
		}

		pattern_vec.push_back(static_cast<uint16_t>(std::strtol(&pattern[i], nullptr, 16)));
		i++;
	}

	const auto vec_length = pattern_vec.size();

	for(uintptr_t i = start; i <= end - vec_length; i++)
	{
		bool mismatch = false;

		for(uintptr_t x = 0; x < vec_length; x++)
		{
			const auto mem = *reinterpret_cast<uint8_t*>(i + x);

			if(pattern_vec[x] != WILDCARD && mem != pattern_vec[x])
			{
				mismatch = true;

				break;
			}
		}

		if(!mismatch)
		{
			return i;
		}
	}

	return std::nullopt;
}

uintptr_t follow_call_rel32(const uintptr_t address)
{
	return address + 5 + *reinterpret_cast<uint32_t*>(address + 1);
}

bool process_feature([[maybe_unused]] const char* guid)
{
	// Godmode: Enable ALL features regardless of GUID
	// This is more robust against PMS updates as it doesn't depend on knowing specific GUIDs
	return true;
}

uint64_t hook_is_feature_available(uintptr_t user, const char** feature)
{
	if(process_feature(*feature))
	{
		return true;
	}

	return _is_feature_available(user, feature);
}

uint64_t* hook_map_find(uintptr_t* rcx, const char** str)
{
	if(str != nullptr && process_feature(*str))
	{
		static uint64_t FAKE_PTR = 0;

		return &FAKE_PTR;
	}

	return _map_find(rcx, str);
}

uint64_t hook_bitset_init(uintptr_t rcx)
{
	auto ret = _bitset_init(rcx);
	g_feature_flags->set();

	return ret;
}

bool hook_is_user_feature_set([[maybe_unused]] uintptr_t rcx, [[maybe_unused]] int expected, [[maybe_unused]] int feature)
{
	return static_cast<bool>(expected);
}

void hook()
{
	auto info = get_dottext_info();
	
	if(!info)
	{
		return;
	}

	const auto start = std::get<0>(info.value());
	const auto end = std::get<1>(info.value());

	if(const auto is_user_feature_set = sig_scan(start, end, "55 48 89 E5 48 8B 07 48 85 C0 74 6A"); is_user_feature_set)
	{
		if(auto trampoline = create_hook(is_user_feature_set.value(), reinterpret_cast<uintptr_t>(hook_is_user_feature_set)); trampoline)
		{
			_is_user_feature_set = reinterpret_cast<decltype(_is_user_feature_set)>(trampoline.value());
		}
	}

	// Features are now enabled in boost::atomic<std::bitset> as of 2024/08/13 PMS BETA
	if(const auto bitset = sig_scan(start, end, "48 8D 0D ? ? ? ? 48 8B 94 05 90 FE FF FF"); bitset)
	{
		const uintptr_t addr = bitset.value() + 7 + *reinterpret_cast<uint32_t*>(bitset.value() + 3);
		g_feature_flags = reinterpret_cast<std::bitset<704>*>(addr);

		if(const auto bitset_init = sig_scan(start, end, "55 48 89 E5 41 57 41 56 41 55 41 54 53 48 81 EC ? ? 00 00 49 89 FE 48  8D 9D ? ? ? ? 48 89 DF E8 ? ? ? ? 48 8B 1B 48 85 DB"); bitset_init)
		{
			if(auto trampoline = create_hook(bitset_init.value(), reinterpret_cast<uintptr_t>(hook_bitset_init)); trampoline)
			{
				_bitset_init = reinterpret_cast<decltype(_bitset_init)>(trampoline.value());

				// No reason to hook the rest
				return;
			}
		}
	}

	if(const auto is_feature_available_ref = sig_scan(start, end, "E8 ? ? ? ? 86 43"); is_feature_available_ref)
	{
		const auto is_feature_available = follow_call_rel32(is_feature_available_ref.value());

		if(auto trampoline = create_hook(is_feature_available, reinterpret_cast<uintptr_t>(hook_is_feature_available)); trampoline)
		{
			_is_feature_available = reinterpret_cast<decltype(_is_feature_available)>(trampoline.value());
		}
	}

	if(const auto map_find = sig_scan(start, end, "55 48 89 E5 41 57 41 56 53 48 83 EC ? 49 89 F7 4C 8D 77"); map_find)
	{
		if(auto trampoline = create_hook(map_find.value(), reinterpret_cast<uintptr_t>(hook_map_find)); trampoline)
		{
			_map_find = reinterpret_cast<decltype(_map_find)>(trampoline.value());
		}
	}
}
