// SPDX-License-Identifier: AGPL-3.0-or-later
#include "hook.hpp"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include "Zydis.h"
#include "webhook_handler.hpp"

struct FeatureGuidEntry
{
	const char* uuid;
	int feature_code;
	int bitset_slot;
	uint8_t bit_mask;
	const char* alias;
};

// GUID catalog retained for future targeted hooks or diagnostics.
// Current hook path still uses Godmode and enables every feature.
[[maybe_unused]] static constexpr FeatureGuidEntry kFeatureGuidCatalog[] =
{
	{"db965785-ca5c-46fd-bab6-7b3d29c18492", 0, 0, 0x01, nullptr},
	{"fd6683b9-1426-4b00-840f-cd5fb0904a6a", 1, 0, 0x02, nullptr},
	{"7ef84008-9a02-43f9-a22f-86102fd66922", 2, 0, 0x04, nullptr},
	{"075954ad-56ef-4f5e-9519-9cfb0ed05827", 3, 0, 0x08, nullptr},
	{"16abced2-1e64-4f01-b64f-f8ef41b1ea6c", 4, 0, 0x10, nullptr},
	{"a19d495a-1cef-4f7c-ab77-5186e63e17f7", 5, 0, 0x20, "loudness"},
	{"dcecabdf-68cf-4067-8013-73bd9ea3940b", 6, 0, 0x40, nullptr},
	{"52ee04dc-2b82-4142-9e0a-e7ce8087c5b6", 7, 0, 0x80, nullptr},
	{"59127bcf-acc8-4e97-ad88-8ba6380880b9", 8, 1, 0x01, "pro_install"},
	{"ec64b6f6-e804-4ef3-b114-9d5c63e1a941", 9, 1, 0x02, nullptr},
	{"ee352392-2934-4061-ba35-5f3189f19ab4", 10, 1, 0x04, nullptr},
	{"cc987706-05d8-4c1f-9386-2e86f402706d", 11, 1, 0x08, nullptr},
	{"b83c8dc9-5a01-4b7a-a7c9-5870c8a6e21b", 12, 1, 0x10, nullptr},
	{"65685ff8-4375-4e4c-a806-ec1f0b4a8b7f", 13, 1, 0x20, "livetv"},
	{"6380e085-02fe-43b5-8bff-380fa4f2423c", 14, 1, 0x40, "trailers"},
	{"b46d16ae-cbd6-4226-8ee9-ab2b27e5dd42", 15, 1, 0x80, "unsupportedtuners"},
	{"9c982beb-c676-4d6f-a777-ff5d37ec3081", 16, 2, 0x01, nullptr},
	{"dbdc0575-9fc7-4706-9e2d-fca98d10ad71", 17, 2, 0x02, nullptr},
	{"ce30800e-9c3c-4a1f-8bb3-d93d6149ff5f", 18, 2, 0x04, nullptr},
	{"e4532fb1-b2e8-4269-9225-b804657cb3ba", 19, 2, 0x08, "roku-dogfood"},
	{"84a754b0-d1ca-4433-af2d-c949bf4b4936", 20, 2, 0x10, "hwtranscode"},
	{"bf1f3608-e44e-48cb-84d7-11a13f29b090", 21, 2, 0x20, nullptr},
	{"850f3d1e-3f38-44c1-9c0c-e3c9127b8b5a", 22, 2, 0x40, "photosV6-edit"},
	{"0e2acda2-d70d-4df6-96e0-f63cf264d217", 23, 2, 0x80, nullptr},
	{"05690239-443e-43fb-bc1a-95b5d916ca63", 24, 3, 0x01, "session_bandwidth_restrictions"},
	{"ea791163-c28d-4b7c-af88-bcc9553b206d", 25, 3, 0x02, nullptr},
	{"e093a02d-5532-4506-948a-2e994beb032b", 26, 3, 0x04, nullptr},
	{"d4b4e08a-9201-4c99-9a52-8f2de8ff25cd", 27, 3, 0x08, nullptr},
	{"00cc618e-eb08-4e0e-9221-82b4835dd89b", 28, 3, 0x10, nullptr},
	{"d9f42aea-bc9d-47db-9814-cd7a577aff48", 29, 3, 0x20, nullptr},
	{"d85cb60c-0986-4a02-b1e1-36c64c609712", 30, 3, 0x40, nullptr},
	{"c55d5900-b546-416d-a8c5-45b24a13e9bc", 31, 3, 0x80, "download_certificates"},
	{"2797e341-b062-46ed-862f-0acbba5dd522", 32, 4, 0x01, nullptr},
	{"c43d8d0f-7aa3-4fef-b9ad-4902580c90ce", 33, 4, 0x02, "incremental-epg"},
	{"e8230c74-0940-4b91-9e20-6571eb068086", 34, 4, 0x04, "dvr"},
	{"044a1fac-6b55-47d0-9933-25a035709432", 35, 4, 0x08, nullptr},
	{"4ca03b04-54c1-4f9f-aea2-f813ae48f317", 36, 4, 0x10, "session_kick"},
	{"a536a6e1-0ece-498a-bf64-99b53c27de3a", 37, 4, 0x20, nullptr},
	{"76ddd91e-8321-4916-94b6-ded8e3727a64", 38, 4, 0x40, nullptr},
	{"ebbe0bd5-7b9f-4c50-92d2-122eb35b61ad", 39, 4, 0x80, nullptr},
	{"b2403ac6-4885-4971-8b96-59353fd87c72", 40, 5, 0x01, "home"},
	{"56cd352b-0d47-436d-aced-f20db3508de5", 41, 5, 0x02, nullptr},
	{"d49a726d-ef0e-4a04-9ffb-fd018306d3b7", 42, 5, 0x04, nullptr},
	{"0eee866d-782b-4dfd-b42b-3bbe8eb0af16", 43, 5, 0x08, "server-manager"},
	{"1417df52-986e-4e4b-8dcd-3997fbc5c976", 44, 5, 0x10, "collections"},
	{"4264b94c-cb40-4935-83b4-7b5c49d35e7f", 45, 5, 0x20, "remote_watch_pass"}, // g_feature_bits_remote_media&0x20; gates remote playback/download (FeatureManager_matches_client_policy)
	{"88aba3a3-bd62-42a5-91bb-0558a4c1db57", 46, 5, 0x40, nullptr},
	{"c7ae6f8f-05e6-48bb-9024-c05c1dc3c43e", 47, 5, 0x80, "kevin-bacon"},
	{"b58d7f28-7b4a-49bb-97a7-152645505f28", 48, 6, 0x01, "item_clusters"},
	{"62b1e357-5450-41d8-9b60-c7705f750849", 49, 6, 0x02, nullptr},
	{"c225b90f-d4b6-4286-a4dd-2492aa017b63", 50, 6, 0x04, nullptr},
	{"1f952ea5-0837-44cb-8539-a69a14a75d4a", 51, 6, 0x08, nullptr},
	{"d20f9af2-fdb1-4927-99eb-a2eb8fbff799", 52, 6, 0x10, "shared-radio"},
	{"644c4466-05fa-45e0-a478-c594cf81778f", 53, 6, 0x20, nullptr},
	{"6f82ca43-6117-4e55-ae0e-5ea3b3e99a96", 54, 6, 0x40, "webhooks"},
	{"9dc1df45-fb45-4be1-9ab2-eb23eb57f082", 55, 6, 0x80, "sync"},
	{"0de49fa2-30cc-4b54-a22d-ff860c1bf3af", 56, 7, 0x01, nullptr},
	{"a6f3f9b3-c10c-4b94-ad59-755e30ac6c90", 57, 7, 0x02, nullptr},
	{"8536058d-e1dd-4ae7-b30f-e8b059b7cc17", 58, 7, 0x04, nullptr},
	{"e7cea823-02e5-48c4-a501-d37b82bf132f", 59, 7, 0x08, nullptr},
	{"2573654f-0985-4cef-9c53-28e78cc62f26", 60, 7, 0x10, nullptr},
	{"e954ef21-08b4-411e-a1f0-7551f1e57b11", 61, 7, 0x20, nullptr},
	{"84309650-eb7a-41e8-8b6c-a260f084bb9d", 62, 7, 0x40, nullptr},
	{"bcd82ac2-f32e-4a23-bb48-88090248c5db", 63, 7, 0x80, nullptr},
	{"ea442c16-044a-4fa7-8461-62643f313c62", 64, 8, 0x01, nullptr},
	{"07f804e6-28e6-4beb-b5c3-f2aefc88b938", 65, 8, 0x02, nullptr},
	{"32cc8bf5-b425-4582-a52d-71b4f1cf436b", 66, 8, 0x04, "content_filter"},
	{"3f28df36-2648-41b6-b2ef-36c2e1509467", 67, 8, 0x08, nullptr},
	{"67c80530-eae3-4500-a9fa-9b6947d0f6d1", 68, 8, 0x10, nullptr},
	{"222020fb-1504-492d-af33-a0b80a49558a", 69, 8, 0x20, nullptr},
	{"5f4ac7c1-a619-4c17-9f06-7b5564566c94", 70, 8, 0x40, nullptr},
	{"a548af72-b804-4d05-8569-52785952d31d", 71, 8, 0x80, nullptr},
	{"99d17487-7106-4d42-a3b1-c92f68b73165", 72, 9, 0x01, "news"},
	{"abd37b14-706c-461f-8255-fa9563882af3", 73, 9, 0x02, "adaptive_bitrate"},
	{"c9a08c83-fbd1-4f2c-ac21-6b35a0acea0e", 74, 9, 0x04, nullptr},
	{"ba8459cd-81fe-4799-93e2-84358717bfb4", 75, 9, 0x08, nullptr},
	{"002c9f1a-2fc0-4812-b85b-0e6140f21a0f", 76, 9, 0x10, "lyrics"},
	{"fb34e64d-cd89-47b8-8bae-a6d20c542bae", 77, 9, 0x20, "camera_upload"},
	{"c757f4d0-2ce6-42d8-ab73-b4808b97cc81", 78, 9, 0x40, nullptr},
	{"ff204a84-8ff1-4d9e-bf5e-378c97bceb10", 79, 9, 0x80, nullptr},
	{"1df3cd16-faf2-4d37-8349-1fcf3713bf1d", 80, 10, 0x01, nullptr},
	{"53d7b3d9-f1f4-4584-9434-9380295db9fe", 81, 10, 0x02, nullptr},
	{"926bc176-58ca-47da-b8e3-080ed14ea6ba", 82, 10, 0x04, nullptr},
	{"a0220fbb-3a79-4041-8642-add6abf70eb5", 83, 10, 0x08, nullptr},
	{"cbae4949-1643-46f3-a488-71836b025d63", 84, 10, 0x10, nullptr},
	{"93bf35b9-3b62-4a8a-b09b-5c85437fa67b", 85, 10, 0x20, nullptr},
	{"ea02d5cc-d9d1-49a4-ab46-bc54c39f739a", 86, 10, 0x40, nullptr},
	{"bc8d1fca-deb0-4d0a-a6f4-12cfd681002d", 87, 10, 0x80, "hardware_transcoding"},
	{"300231e0-69aa-4dce-97f4-52d8c00e3e8c", 88, 11, 0x01, "radio"},
	{"e6eda780-8db7-4114-8179-2f581207d58f", 89, 11, 0x02, nullptr},
	{"6ab6677b-ad9b-444f-9ca1-b8027d05b3e1", 90, 11, 0x04, nullptr},
	{"3c376154-d47e-4bbf-9428-2ea2592fd20a", 91, 11, 0x08, nullptr},
	{"82999dd3-a2be-482e-9f44-357879b4f603", 92, 11, 0x10, "pass"},
	{"5d819d02-5d04-4116-8eec-f49def4e2d6f", 93, 11, 0x20, "federated-auth"},
	{"4866e9e9-ad14-4c2b-bd92-49576f320fd7", 94, 11, 0x40, "ump-matching-pref"},
	{"1facf910-786e-46bb-894e-ea0b41e3fa3e", 95, 11, 0x80, nullptr},
	{"fef9b829-af7e-430c-a757-d80de818f211", 96, 12, 0x01, nullptr},
	{"4b522f91-ae89-4f62-af9c-76f44d8ef61c", 97, 12, 0x02, "tuner-sharing"},
	{"3a2b0cb6-1519-4431-98e2-823c248c70eb", 98, 12, 0x04, "photosV6-tv-albums"},
	{"d413fb56-de7b-40e4-acd0-f3dbb7c9e104", 99, 12, 0x08, "premium_music_metadata"},
	{"9aea4ca5-2095-4619-9339-88c1e662fde6", 100, 12, 0x10, nullptr},
	{"cd0ef747-af8a-414b-9d3a-dd02b6454db9", 101, 12, 0x20, nullptr},
	{"04d7d794-b76c-49ef-9184-52f8f1f501ee", 102, 12, 0x40, nullptr},
	{"1844737f-1a87-45c3-ab20-01435959e63c", 103, 12, 0x80, "music_videos"},
	{"8d15fdf2-89dc-407e-a2f6-0e7c31daa09a", 104, 13, 0x01, nullptr},
	{"d14556be-ae6d-4407-89d0-b83953f4789a", 105, 13, 0x02, "type-first"},
	{"0a348865-4f87-46dc-8bb2-f37637975724", 106, 13, 0x04, "photos-v5"},
	{"3d572099-e243-49bb-9f94-17de7703f9f9", 107, 13, 0x08, "advanced-playback-settings"},
	{"8fd37970-6e4e-4f00-a64a-e70b52f18e94", 108, 13, 0x10, "music-analysis"},
	{"8b46de05-1f96-4278-87b3-010ba5b1e386", 109, 13, 0x20, nullptr},
	{"4cd4dc0e-6cbe-456c-9988-9f073fadcd73", 110, 13, 0x40, nullptr},
};

[[maybe_unused]] static constexpr size_t kFeatureGuidCatalogCount = sizeof(kFeatureGuidCatalog) / sizeof(kFeatureGuidCatalog[0]);

static uint64_t (*g_feature_flags)[14] = nullptr;

// Trampoline pointers (thunks to the original functions).
static uint64_t (*_bitset_init)(uintptr_t) = nullptr;
static uint64_t (*_is_feature_available)(uintptr_t, const char**) = nullptr;
static uint64_t* (*_map_find)(uintptr_t*, const char**) = nullptr;
static bool (*_is_user_feature_set)(uintptr_t, int, int) = nullptr;
static uint64_t (*_sub_122B2F2)(uintptr_t, char*) = nullptr;
static uint64_t (*_sub_125ACA6)(uintptr_t) = nullptr;

// Returns the runtime [start, end) of the main program's executable code
// by parsing /proc/self/maps for every r-xp segment of "Plex Media Server".
//
// dl_iterate_phdr cannot be used because under musl's dynamic linker the
// LD_PRELOAD library's constructor runs BEFORE the main PIE binary is
// loaded, so dl_iterate_phdr returns 0 callbacks.
TextSpan get_dottext_info()
{
	FILE* maps = fopen("/proc/self/maps", "r");
	if(!maps)
		return {false, 0, 0};

	uintptr_t text_start = UINTPTR_MAX;
	uintptr_t text_end   = 0;
	char line[8192];

	while(fgets(line, sizeof(line), maps))
	{
		// Format: start-end perm offset dev inode [path]
		//
		// We want every r-xp (read-execute, non-writable) segment of the
		// main PIE binary, whose path ends in "Plex Media Server".
		// PMS maps the executable text as several adjacent executable ranges;
		// scanning only the first range misses the hook signatures.
		//
		// Codec .so files live under .../Codecs/... and also contain
		// "Plex Media Server" in their path, so we check for the exact
		// path suffix instead of a substring.

		if(const char* path = strstr(line, "/usr/lib/plexmediaserver/"))
		{
			path += strlen("/usr/lib/plexmediaserver/");
			if(strncmp(path, "Plex Media Server", 17) != 0)
				continue; // codec .so or plugin, skip
		}
		else
		{
			continue; // not our binary
		}

		// Parse permission field (field #2, after address range).
		const char* perm = line;
		while(*perm && *perm != ' ') perm++;
		while(*perm == ' ') perm++;

		if(perm[0] != 'r' || perm[1] != '-' || perm[2] != 'x' || perm[3] != 'p')
			continue; // not an executable mapping

		// Parse address range.
		const char* addr = line;
		char* end = nullptr;
		const uintptr_t seg_start = strtoull(addr, &end, 16);
		if(!end || *end != '-') continue;

		const uintptr_t seg_end = strtoull(end + 1, &end, 16);
		if(!end) continue;

		if(seg_start < text_start) text_start = seg_start;
		if(seg_end > text_end) text_end = seg_end;
	}

	fclose(maps);

	if(text_end <= text_start)
		return {false, 0, 0};

	return {true, text_start, text_end};
}

OptAddr create_hook(uintptr_t from, uintptr_t to)
{
	ZydisDecoder decoder;
	ZydisDecodedInstruction instruction;
	ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

	auto trampoline_mem = static_cast<uint8_t*>(mmap(nullptr, getpagesize(), PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0));

	if(trampoline_mem == MAP_FAILED)
	{
		return {false, 0};
	}

	size_t offset = 0;

	while(offset < 14)
	{
		if(ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder, nullptr, reinterpret_cast<void*>(from + offset), ZYDIS_MAX_INSTRUCTION_LENGTH, &instruction)))
		{
			memcpy(trampoline_mem + offset, reinterpret_cast<void*>(from + offset), instruction.length);
			offset += instruction.length;
		}
		else
		{
			munmap(trampoline_mem, getpagesize());
			return {false, 0};
		}
	}

	uint8_t shellcode[] =
	{
		0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, // jmp [rip+0x06]
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 // ?
	};

	// Jump to original code in trampoline and make trampoline executable
	*reinterpret_cast<uintptr_t*>(&shellcode[6]) = from + offset;
	memcpy(trampoline_mem + offset, shellcode, sizeof(shellcode));
	const size_t trampoline_size = offset + sizeof(shellcode);

	if(mprotect(trampoline_mem, trampoline_size, PROT_READ|PROT_EXEC) != 0)
	{
		munmap(trampoline_mem, getpagesize());
		return {false, 0};
	}

	// Jump to target code
	*reinterpret_cast<uintptr_t*>(&shellcode[6]) = to;

	// mprotect requires a page-aligned address.  The target function sits
	// inside .text, which is mapped at page granularity.  Align down to
	// the page boundary so mprotect succeeds.
	const uintptr_t from_page = from & ~(static_cast<uintptr_t>(getpagesize()) - 1);

	if(mprotect(reinterpret_cast<void*>(from_page), getpagesize(), PROT_READ|PROT_WRITE|PROT_EXEC) != 0)
	{
		munmap(trampoline_mem, getpagesize());
		return {false, 0};
	}

	memcpy(reinterpret_cast<void*>(from), shellcode, sizeof(shellcode));

	// Flush instruction cache so the CPU sees the modified code.
	// On x86 this is a no-op at the hardware level (icache is coherent)
	// but acts as a compiler barrier to prevent reordering the memcpy
	// past the subsequent mprotect.
	__builtin___clear_cache(reinterpret_cast<char*>(from),
	                        reinterpret_cast<char*>(from) + sizeof(shellcode));

	if(mprotect(reinterpret_cast<void*>(from_page), getpagesize(), PROT_READ|PROT_EXEC) != 0)
	{
		// Memory is still RWX (suboptimal but not fatal).
	}

	return {true, reinterpret_cast<uintptr_t>(trampoline_mem)};
}

OptAddr sig_scan(uintptr_t start, uintptr_t end, const char* pattern)
{
	static constexpr uint16_t WILDCARD = 0xFFFF;
	static constexpr size_t MAX_PAT = 128;

	uint16_t pat[MAX_PAT];
	size_t   pat_count = 0;
	size_t   i = 0;
	size_t   len = strlen(pattern);

	while(i < len && pat_count < MAX_PAT)
	{
		if(pattern[i] == ' ')
		{
			i++;
			continue;
		}

		if(pattern[i] == '?')
		{
			pat[pat_count++] = WILDCARD;
			if(i + 1 < len && pattern[i + 1] == '?') i++;
			i++;
			continue;
		}

		char hex[3] = {pattern[i], i + 1 < len ? pattern[i + 1] : '\0', '\0'};
		pat[pat_count++] = static_cast<uint16_t>(strtol(hex, nullptr, 16));
		i += 2;
	}

	if(pat_count == 0 || pat_count > end - start)
	{
		return {false, 0};
	}

	for(uintptr_t addr = start; addr <= end - pat_count; addr++)
	{
		bool mismatch = false;

		for(size_t x = 0; x < pat_count; x++)
		{
			if(pat[x] != WILDCARD && *reinterpret_cast<const uint8_t*>(addr + x) != static_cast<uint8_t>(pat[x]))
			{
				mismatch = true;
				break;
			}
		}

		if(!mismatch)
		{
			return {true, addr};
		}
	}

	return {false, 0};
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

	if(g_feature_flags)
	{
		for(int i = 0; i < 14; i++)
		{
			(*g_feature_flags)[i] = UINT64_MAX;
		}
	}

	return ret;
}

bool hook_is_user_feature_set([[maybe_unused]] uintptr_t rcx, [[maybe_unused]] int expected, [[maybe_unused]] int feature)
{
	return static_cast<bool>(expected);
}

uint64_t hook_sub_122B2F2(uintptr_t this_ptr, char* key)
{
	if(_sub_122B2F2) {
		if(strcmp(key, "WebHooksEnabled") == 0) {
			const char true_str[] = "1";
			// "1" is 2 bytes + null within 22-byte SSO threshold.
			// Construct SSO std::string: bytes 0=N, byte 23=22-N.
			// For length 1: byte 23 = 22-1 = 21 = 0x15.
			memcpy(reinterpret_cast<void*>(this_ptr + 16), true_str, 2);
			reinterpret_cast<uint8_t*>(this_ptr)[23] = 0x15;
			return 1;
		}
		return _sub_122B2F2(this_ptr, key);
	}
	return 0;
}

uint64_t hook_sub_125ACA6(uintptr_t manager)
{
	if(_sub_125ACA6)
	{
		const auto ret = _sub_125ACA6(manager);
		webhook_set_manager(reinterpret_cast<void*>(manager));
		webhook_inject_into_manager(reinterpret_cast<void*>(manager));
		return ret;
	}
	return 0;
}

void hook()
{
	auto info = get_dottext_info();

	if(!info.ok)
	{
		return;
	}

	const uintptr_t start = info.start;
	const uintptr_t end = info.end;

	// STEP 1: sub_122B2F2 (the generic preference getter): force WebHooksEnabled.
	if(const auto wh = sig_scan(start, end,
		"55 48 89 E5 41 57 41 56 53 48 83 EC 18 48 89 F3 49 89 FE 0F B6 46 17 48 89 F1 84 C0"); wh.ok)
	{
		if(auto trampoline = create_hook(wh.addr, reinterpret_cast<uintptr_t>(hook_sub_122B2F2)); trampoline.ok)
		{
			_sub_122B2F2 = reinterpret_cast<decltype(_sub_122B2F2)>(trampoline.addr);
		}
	}

	// STEP 2: sub_125E524+0x6DD inline patch (forces feature bit return to true)
	// Temporarily disabled — suspected to corrupt adjacent instructions.
	// The bitset_init hook (step 3) achieves the same goal cleanly.
#if 0
	if(const auto sa = sig_scan(start, end,
		"55 48 89 E5 41 57 41 56 41 55 41 54 53 48 81 EC ? ? ? ? 49 89 F7 49 89 FC 41 BE ? ? ? ? 4C 03 77 28"); sa.ok)
	{
		const uintptr_t target = sa.addr + 0x6DD;
		const uintptr_t page = target & ~(getpagesize() - 1);

		if(mprotect(reinterpret_cast<void*>(page), getpagesize(), PROT_READ|PROT_WRITE|PROT_EXEC) == 0)
		{
			const uint8_t patch[] = {0xB0, 0x01, 0x90, 0x90, 0x90};
			memcpy(reinterpret_cast<void*>(target), patch, sizeof(patch));
			mprotect(reinterpret_cast<void*>(page), getpagesize(), PROT_READ|PROT_EXEC);
		}
	}
#endif

	// [WEBHOOK DISABLED] sub_125ACA6 hook — causes SIGSEGV at startup with
	// the mprotect fix. The `create_hook` trampoline now works (page-aligned)
	// and this hook fires during PMS init, but the manager layout offsets
	// (+0x78 for map, +0x40 for vector pointers) are version-specific and
	// crash on this build (1.43.2.10687).  The socket-level interposer
	// (webhook_handler.cpp) works independently via LD_PRELOAD and handles
	// webhook CRUD without this hook.
	//
	// Re-enable *only* after verifying the struct layout matches.
	//

	// STEP 3: Modern path (PMS BETA 2024/08/13+): force the entire feature bitset.
	if(const auto bitset = sig_scan(start, end, "48 8D 0D ? ? ? ? 48 8B 94 05 90 FE FF FF"); bitset.ok)
	{
		const uintptr_t addr = bitset.addr + 7 + *reinterpret_cast<uint32_t*>(bitset.addr + 3);
		g_feature_flags = reinterpret_cast<uint64_t(*)[14]>(addr);

		if(const auto bs_init = sig_scan(start, end,
			"55 48 89 E5 41 57 41 56 41 55 41 54 53 48 81 EC ? ? 00 00 49 89 FE 48  8D 9D ? ? ? ? 48 89 DF E8 ? ? ? ? 48 8B 1B 48 85 DB"); bs_init.ok)
		{
			if(auto trampoline = create_hook(bs_init.addr, reinterpret_cast<uintptr_t>(hook_bitset_init)); trampoline.ok)
			{
				_bitset_init = reinterpret_cast<decltype(_bitset_init)>(trampoline.addr);
				return;
			}
		}
	}

	// STEP 4: Legacy fallback — pre-2024/08/13 per-feature hooks.
	if(const auto usf = sig_scan(start, end, "55 48 89 E5 48 8B 07 48 85 C0 74 09"); usf.ok)
	{
		if(auto trampoline = create_hook(usf.addr, reinterpret_cast<uintptr_t>(hook_is_user_feature_set)); trampoline.ok)
		{
			_is_user_feature_set = reinterpret_cast<decltype(_is_user_feature_set)>(trampoline.addr);
		}
	}

	if(const auto ifa_ref = sig_scan(start, end, "E8 ? ? ? ? 86 43"); ifa_ref.ok)
	{
		const auto ifa = follow_call_rel32(ifa_ref.addr);

		if(auto trampoline = create_hook(ifa, reinterpret_cast<uintptr_t>(hook_is_feature_available)); trampoline.ok)
		{
			_is_feature_available = reinterpret_cast<decltype(_is_feature_available)>(trampoline.addr);
		}
	}

	if(const auto mf = sig_scan(start, end, "55 48 89 E5 41 57 41 56 53 48 83 EC ? 49 89 F7 4C 8D 77"); mf.ok)
	{
		if(auto trampoline = create_hook(mf.addr, reinterpret_cast<uintptr_t>(hook_map_find)); trampoline.ok)
		{
			_map_find = reinterpret_cast<decltype(_map_find)>(trampoline.addr);
		}
	}
}
