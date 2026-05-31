// SPDX-License-Identifier: AGPL-3.0-or-later
//
// plex_inject.exe — Inject ``plex_patch.dll`` into Plex Media Server.
//
// Two modes:
//   plex_inject.exe                 — find the running PMS, inject into it
//   plex_inject.exe --launch PATH   — spawn PMS suspended, inject, resume
//
// The DLL path is resolved relative to the injector's own location so the two
// files can live side by side anywhere on disk (no need to copy to Program Files).
//
// Elevation: the injector must run as the same user as PMS or as Administrator
// (OpenProcess needs PROCESS_ALL_ACCESS).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstring>

// ---- helpers ---------------------------------------------------------------

static void err(const char* msg) {
    std::fprintf(stderr, "[x] %s (GetLastError=%lu)\n", msg, ::GetLastError());
}

static bool get_own_dir(char* buf, size_t buflen) {
    DWORD n = ::GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(buflen));
    if (n == 0 || n >= buflen) return false;
    // Strip the exe name, keep trailing backslash.
    char* last = std::strrchr(buf, '\\');
    if (!last) last = std::strrchr(buf, '/');
    if (last) *(last + 1) = '\0'; else buf[0] = '\0';
    return true;
}

static DWORD find_process(const char* name) {
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (::Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
        } while (::Process32Next(snap, &pe));
    }
    ::CloseHandle(snap);
    return pid;
}

static bool inject_dll(HANDLE proc, const char* dll_path) {
    const size_t path_len = std::strlen(dll_path) + 1;

    // Allocate memory in the target for the DLL path string.
    void* remote_buf = ::VirtualAllocEx(
        proc, nullptr, path_len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_buf) { err("VirtualAllocEx failed"); return false; }

    if (!::WriteProcessMemory(proc, remote_buf, dll_path, path_len, nullptr)) {
        err("WriteProcessMemory failed");
        ::VirtualFreeEx(proc, remote_buf, 0, MEM_RELEASE);
        return false;
    }

    // LoadLibraryA is at the same address in every process (kernel32 is always
    // mapped at its preferred base on Windows x64).
    auto load_lib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        ::GetProcAddress(::GetModuleHandleA("kernel32.dll"), "LoadLibraryA"));
    if (!load_lib) { err("GetProcAddress(LoadLibraryA) failed"); return false; }

    HANDLE thread = ::CreateRemoteThread(
        proc, nullptr, 0, load_lib, remote_buf, 0, nullptr);
    if (!thread) { err("CreateRemoteThread failed"); return false; }

    ::WaitForSingleObject(thread, 10000);
    DWORD exit_code = 0;
    ::GetExitCodeThread(thread, &exit_code);
    ::CloseHandle(thread);
    ::VirtualFreeEx(proc, remote_buf, 0, MEM_RELEASE);

    if (exit_code == 0) {
        err("LoadLibraryA returned NULL in the target (DLL load failed)");
        return false;
    }
    return true;
}

// ---- main ------------------------------------------------------------------

int main(int argc, char** argv) {
    std::printf("[*] plex_inject — Plex Media Server feature patch injector\n");

    // Resolve the DLL path relative to the injector binary.
    char dir[MAX_PATH]{};
    if (!get_own_dir(dir, sizeof(dir))) { err("cannot determine own directory"); return 1; }
    char dll_path[MAX_PATH]{};
    std::snprintf(dll_path, sizeof(dll_path), "%splex_patch.dll", dir);

    // Check the DLL exists before attempting injection.
    if (::GetFileAttributesA(dll_path) == INVALID_FILE_ATTRIBUTES) {
        std::fprintf(stderr, "[x] DLL not found: %s\n", dll_path);
        return 1;
    }
    std::printf("[+] DLL: %s\n", dll_path);

    bool launched = false;
    HANDLE proc = nullptr;
    HANDLE main_thread = nullptr;
    DWORD pid = 0;

    if (argc >= 3 && std::strcmp(argv[1], "--launch") == 0) {
        // Spawn PMS suspended, inject before it runs.
        STARTUPINFOA si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!::CreateProcessA(argv[2], nullptr, nullptr, nullptr, FALSE,
                              CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
            err("CreateProcess failed");
            return 1;
        }
        proc = pi.hProcess;
        main_thread = pi.hThread;
        pid = pi.dwProcessId;
        launched = true;
        std::printf("[+] launched PMS (pid %lu) suspended\n", pid);
    } else {
        // Attach to an already-running PMS.
        pid = find_process("Plex Media Server.exe");
        if (!pid) { err("Plex Media Server.exe not found (is it running?)"); return 1; }
        proc = ::OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
        if (!proc) { err("OpenProcess failed (run as admin?)"); return 1; }
        std::printf("[+] attached to PMS (pid %lu)\n", pid);
    }

    bool ok = inject_dll(proc, dll_path);
    if (ok) {
        std::printf("[+] plex_patch.dll injected into pid %lu\n", pid);
    } else {
        std::fprintf(stderr, "[x] injection failed\n");
    }

    if (launched) {
        if (ok) {
            ::ResumeThread(main_thread);
            std::printf("[+] PMS main thread resumed\n");
        } else {
            ::TerminateProcess(proc, 1);
            std::printf("[!] PMS terminated (injection failed)\n");
        }
        ::CloseHandle(main_thread);
    }
    ::CloseHandle(proc);
    return ok ? 0 : 1;
}
