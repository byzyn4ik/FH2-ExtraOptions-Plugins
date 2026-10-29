// Keyboard & Mouse for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "patch.h"

#include <stdio.h>
#include <string.h>
#include <tlhelp32.h>

#define REACH 0x7ff00000ull  // a little under 2 GB
#define MAX_THREADS 4096
#define MAX_WRITES 32

int patch_is_code(const void *address) {
    MEMORY_BASIC_INFORMATION region;
    if (!address || VirtualQuery(address, &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT)
        return 0;
    DWORD protect = region.Protect & 0xff;
    return !(region.Protect & PAGE_GUARD) && (protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ ||
                                              protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY);
}

void patch_describe(const void *address, char *out, size_t size) {
    HMODULE module = NULL;
    wchar_t path[MAX_PATH];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)address, &module) &&
        GetModuleFileNameW(module, path, MAX_PATH)) {
        const wchar_t *name = wcsrchr(path, L'\\');
        snprintf(out, size, "%ls+0x%llx", name ? name + 1 : path,
                 (unsigned long long)((const unsigned char *)address - (const unsigned char *)module));
    } else {
        snprintf(out, size, "0x%llx", (unsigned long long)(uintptr_t)address);
    }
}

static int inside(const void *image, size_t size, const void *address) {
    return (const unsigned char *)address >= (const unsigned char *)image &&
           (const unsigned char *)address < (const unsigned char *)image + size;
}

// The game's own value, or another module's executable code.
static int chainable(const void *value, const void *native, const void *image, size_t image_size) {
    return value && (value == native || (!inside(image, image_size, value) && patch_is_code(value)));
}

const char *patch_slot(void **slot, const void *native, const void *image, size_t image_size, void *hook,
                       void **previous) {
    void *value = NULL;
    DWORD protection, ignored;
    if (((uintptr_t)slot & 7) != 0) return "it is not 8-byte aligned";
    if (!memory_read(slot, &value, sizeof(value))) return "it cannot be read";
    if (!chainable(value, native, image, image_size))
        return "it holds neither the game's function nor another module's code";
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &protection)) return "it could not be written";
    *previous = value;  // before the swap: another thread may call through it at once
    void *seen = InterlockedCompareExchangePointer(slot, hook, value);
    VirtualProtect(slot, sizeof(void *), protection, &ignored);
    return seen == value ? NULL : "another module changed it at the same time";
}

// ------------------------------------------------------------ suspension

typedef struct {
    unsigned char *at;
    unsigned length;
    const unsigned char *expected;  // what must still be there
    unsigned char bytes[32];
} CodeWrite;

static HANDLE suspended[MAX_THREADS];
static unsigned busy_retries;

static void resume_all(int count) {
    while (count-- > 0) {
        ResumeThread(suspended[count]);
        CloseHandle(suspended[count]);
    }
}

// Suspends every other thread of the process; -1 (all resumed) when one is
// stopped inside a byte about to change (the first byte of a write is fine:
// the thread then runs the whole new instruction).
static int suspend_others(const CodeWrite *writes, unsigned count) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return -1;
    DWORD self = GetCurrentThreadId(), process = GetCurrentProcessId();
    int found = 0, busy = 0;
    THREADENTRY32 entry;
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more && found < MAX_THREADS;
         more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, entry.th32ThreadID);
        if (!thread) continue;
        if (SuspendThread(thread) == (DWORD)-1) {
            CloseHandle(thread);
            continue;
        }
        suspended[found++] = thread;
        CONTEXT context;
        memset(&context, 0, sizeof(context));
        context.ContextFlags = CONTEXT_CONTROL;
        // GetThreadContext also waits until the thread has really stopped.
        if (GetThreadContext(thread, &context)) {
            for (unsigned i = 0; i < count; ++i)
                if (context.Rip > (DWORD64)(uintptr_t)writes[i].at &&
                    context.Rip < (DWORD64)(uintptr_t)(writes[i].at + writes[i].length))
                    busy = 1;
        } else {
            busy = 1;  // where it is is unknown
        }
    }
    CloseHandle(snapshot);
    if (busy) {
        resume_all(found);
        return -1;
    }
    return found;
}

// Writes all or nothing. Pages are made writable first; while the others are
// suspended only plain stores and system calls run (no heap, no log).
static const char *write_code(CodeWrite *writes, unsigned count) {
    DWORD protections[MAX_WRITES], ignored;
    if (count > MAX_WRITES) return "too many code writes";
    for (unsigned i = 0; i < count; ++i) {
        if (!VirtualProtect(writes[i].at, writes[i].length, PAGE_EXECUTE_READWRITE, &protections[i])) {
            while (i--) VirtualProtect(writes[i].at, writes[i].length, protections[i], &ignored);
            return "the code could not be made writable";
        }
    }
    int threads = -1;
    for (int attempt = 0; attempt < 200 && threads < 0; ++attempt) {
        threads = suspend_others(writes, count);
        if (threads < 0) {
            ++busy_retries;
            Sleep(attempt < 20 ? 0 : 1);
        }
    }
    const char *problem = threads < 0 ? "a game thread kept running inside the code" : NULL;
    for (unsigned i = 0; !problem && i < count; ++i)
        if (writes[i].expected && memcmp(writes[i].at, writes[i].expected, writes[i].length))
            problem = "another module changed the code at the same time";
    if (!problem) {
        for (unsigned i = 0; i < count; ++i) {
            memcpy(writes[i].at, writes[i].bytes, writes[i].length);
            FlushInstructionCache(GetCurrentProcess(), writes[i].at, writes[i].length);
        }
    }
    if (threads >= 0) resume_all(threads);
    for (unsigned i = 0; i < count; ++i) VirtualProtect(writes[i].at, writes[i].length, protections[i], &ignored);
    return problem;
}

// ----------------------------------------------------------------- calls

static unsigned char *allocate_near(const unsigned char *around) {
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    uintptr_t granularity = system.dwAllocationGranularity;
    uintptr_t center = (uintptr_t)around & ~(granularity - 1);
    uintptr_t high = (uintptr_t)system.lpMaximumApplicationAddress;
    for (uintptr_t step = granularity; step < REACH; step += granularity) {
        for (int up = 0; up < 2; ++up) {
            if (!up && center < step + granularity) continue;
            uintptr_t at = up ? center + step : center - step;
            if (up && at >= high) continue;
            MEMORY_BASIC_INFORMATION region;
            if (VirtualQuery((void *)at, &region, sizeof(region)) != sizeof(region) || region.State != MEM_FREE)
                continue;
            unsigned char *page = VirtualAlloc((void *)at, system.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!page) continue;
            intptr_t distance = (intptr_t)page - (intptr_t)around;
            if (distance > -(intptr_t)REACH && distance < (intptr_t)REACH) return page;
            VirtualFree(page, 0, MEM_RELEASE);
        }
    }
    return NULL;
}

static void absolute_jump(unsigned char *at, const void *target) {
    static const unsigned char jump[6] = {0xff, 0x25, 0, 0, 0, 0};  // jmp qword [rip+0]; dq target
    memcpy(at, jump, sizeof(jump));
    memcpy(at + 6, &target, sizeof(target));
}

static void *call_target(const unsigned char *site) {
    unsigned char code[5];
    int32_t displacement;
    if (!memory_read(site, code, sizeof(code)) || code[0] != 0xe8) return NULL;
    memcpy(&displacement, code + 1, 4);
    return (void *)(site + 5 + displacement);
}

const char *patch_call(unsigned char *site, const void *native, const void *image, size_t image_size, void *hook,
                       void **previous) {
    return patch_call_also(site, native, NULL, image, image_size, hook, previous);
}

const char *patch_call_also(unsigned char *site, const void *native, const void *also, const void *image,
                            size_t image_size, void *hook, void **previous) {
    void *target = call_target(site);
    if (!target) return "it is not a call";
    if (!(also && target == also) && !chainable(target, native, image, image_size))
        return "it calls neither the game's function nor another module's code";
    unsigned char *jump = allocate_near(site);
    if (!jump) return "no free memory within 2 GB of it";
    absolute_jump(jump, hook);
    DWORD protection;
    if (!VirtualProtect(jump, 14, PAGE_EXECUTE_READ, &protection)) {
        VirtualFree(jump, 0, MEM_RELEASE);
        return "the jump could not be made executable";
    }
    FlushInstructionCache(GetCurrentProcess(), jump, 14);
    *previous = target;  // before the call is changed
    CodeWrite write = {site + 1, 4, site + 1, {0}};
    int32_t displacement = (int32_t)(jump - (site + 5));
    memcpy(write.bytes, &displacement, 4);
    const char *problem = write_code(&write, 1);
    if (problem) VirtualFree(jump, 0, MEM_RELEASE);
    return problem;
}

// --------------------------------------------------------- entry hooks

const char *patch_entries(EntryHook *hooks, unsigned count) {
    if (!count || count > MAX_WRITES) return "bad hook list";
    for (unsigned i = 0; i < count; ++i)
        if (hooks[i].size < 14 || hooks[i].size > 32 || !memory_equal(hooks[i].at, hooks[i].expected, hooks[i].size))
            return "a function entry is not as expected";
    unsigned char *trampolines = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!trampolines) return "no memory for the trampolines";
    unsigned char *cursor = trampolines;
    CodeWrite writes[MAX_WRITES];
    for (unsigned i = 0; i < count; ++i) {
        // The moved bytes are register moves, pushes and stack adjustments
        // only (no RIP-relative operand): they run unchanged in the trampoline.
        if (hooks[i].trampoline) {
            memcpy(cursor, hooks[i].at, hooks[i].size);
            absolute_jump(cursor + hooks[i].size, hooks[i].at + hooks[i].size);
            *hooks[i].original = cursor;
            cursor += (hooks[i].size + 14 + 15) & ~15u;
        }
        writes[i].at = hooks[i].at;
        writes[i].length = hooks[i].size;
        writes[i].expected = hooks[i].expected;
        memset(writes[i].bytes, hooks[i].fill, sizeof(writes[i].bytes));
        absolute_jump(writes[i].bytes, hooks[i].hook);
    }
    DWORD ignored;
    if (!VirtualProtect(trampolines, 4096, PAGE_EXECUTE_READ, &ignored)) {
        VirtualFree(trampolines, 0, MEM_RELEASE);
        return "the trampolines could not be made executable";
    }
    FlushInstructionCache(GetCurrentProcess(), trampolines, 4096);
    const char *problem = write_code(writes, count);
    if (problem) {
        for (unsigned i = 0; i < count; ++i)
            if (hooks[i].trampoline) *hooks[i].original = NULL;
        VirtualFree(trampolines, 0, MEM_RELEASE);
    }
    return problem;
}

#ifdef KBM_TEST
unsigned patch_busy_retries(void) { return busy_retries; }
#endif
