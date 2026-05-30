// SPDX-License-Identifier: AGPL-3.0-or-later
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

static uint64_t hook_fn(uintptr_t user, const char** feature)
{
    return 1;
}

static int get_dottext_info(uintptr_t* start_out, uintptr_t* end_out)
{
    FILE* file = fopen("/proc/self/maps", "r");
    if (!file) return 0;
    
    char line[1024];
    while (fgets(line, sizeof(line), file))
    {
        if (strstr(line, "Plex Media Server") && strstr(line, "r-xp"))
        {
            char* endptr;
            *start_out = strtoull(line, &endptr, 16);
            if (*endptr != '-') continue;
            *end_out = strtoull(endptr + 1, NULL, 16);
            fclose(file);
            return 1;
        }
    }
    fclose(file);
    return 0;
}

static uintptr_t sig_scan(uintptr_t start, uintptr_t end, const uint8_t* pattern, const uint8_t* mask, int pat_len)
{
    for (uintptr_t i = start; i <= end - (uintptr_t)pat_len; i++)
    {
        int match = 1;
        for (int x = 0; x < pat_len; x++)
        {
            if (!mask[x]) continue;
            if (*(uint8_t*)(i + x) != pattern[x]) { match = 0; break; }
        }
        if (match) return i;
    }
    return 0;
}

static void do_hook(uintptr_t target)
{
    if (!target) return;
    
    uint8_t shellcode[] = {
        0x48, 0xB8,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x50,
        0xC3
    };
    *(uint64_t*)(&shellcode[2]) = (uint64_t)&hook_fn;

    long page_size = 4096;
    uintptr_t page_start = target & ~(page_size - 1);
    mprotect((void*)page_start, page_size, PROT_READ | PROT_WRITE | PROT_EXEC);
    memcpy((void*)target, shellcode, sizeof(shellcode));
    mprotect((void*)page_start, page_size, PROT_READ | PROT_EXEC);
}

void __attribute__((constructor)) init_so(void)
{
    uintptr_t start, end;
    if (!get_dottext_info(&start, &end)) return;

    uint8_t pat[] = { 0xE8, 0x00, 0x00, 0x00, 0x00, 0x86, 0x43 };
    uint8_t mask[] = { 1, 0, 0, 0, 0, 1, 1 };
    uintptr_t found = sig_scan(start, end, pat, mask, 7);
    if (found) {
        int32_t rel = *(int32_t*)(found + 1);
        uintptr_t call_target = found + 5 + rel;
        do_hook(call_target);
    }
}
