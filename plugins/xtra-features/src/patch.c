// Xtra Features for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Patches that are safe to make while the game runs (the plugin starts on
// Extra Options' loader worker after the game started):
//   - chain_call: a "call rel32" whose displacement is not 4-byte aligned
//     (fh2_chain_call refuses those). The displacement is swapped inside the
//     smallest aligned 8- or 16-byte block that holds it, with one
//     compare-exchange of the whole block, so no thread ever sees half of it.
//     The call then goes to a jump placed within 2 GB (fh2_allocate_near),
//     never into the game's int3 padding, which Extra Options uses.
//   - chain_slot: fh2_chain_slot, accepting the game's function or another
//     module's hook.
//   - write_code: bytes that change instructions (not one aligned block) are
//     written with every other thread suspended and none of them executing
//     inside them.
#include <stdio.h>

#include "xtra.h"
// After windows.h (xtra.h).
#include <tlhelp32.h>

#define MAX_THREADS 4096
#define WRITE_TRIES 100

static const char *chain_problem(Fh2ChainResult result) {
    switch (result) {
    case FH2_CHAINED: return NULL;
    case FH2_NOT_CALL: return "it is not a call";
    case FH2_TARGET_NOT_CODE: return "its target is not executable code";
    case FH2_UNALIGNED: return "it is not aligned";
    case FH2_NO_NEAR_MEMORY: return "there is no free memory within 2 GB of it";
    case FH2_JUMP_FAILED: return "the jump could not be made executable";
    case FH2_WRITE_FAILED: return "it could not be written";
    case FH2_RACED: return "another module changed it at the same time";
    }
    return "?";
}

// The game's own target, or another module's executable code.
static const char *chainable(const void *value, const void *native, const unsigned char *game) {
    if (!value) return "it cannot be read";
    if (value == native) return NULL;
    if (in_game(game, value)) return "it leads somewhere unexpected in the game";
    if (!fh2_is_code(value)) return "it leads to something that is not executable code";
    return NULL;
}

const char *chain_slot(void **slot, const void *native, const unsigned char *game, void *handler, void **previous) {
    void *value = read_pointer(slot);
    const char *problem = chainable(value, native, game);
    if (problem) return problem;
    *previous = value;  // before the swap: another thread may call through it at once
    return chain_problem(fh2_chain_slot(slot, value, handler));
}

typedef unsigned __int128 Block16;

const char *chain_call(unsigned char *site, const void *native, const unsigned char *game, void *handler,
                       void **previous) {
    void *target = fh2_call_target(site);
    if (!target) return "it is not a call";
    const char *problem = chainable(target, native, game);
    if (problem) return problem;
    uintptr_t displacement_at = (uintptr_t)(site + 1);
    // The block: 8 bytes when the displacement fits one aligned qword, else 16.
    unsigned size = (displacement_at & ~(uintptr_t)7) == ((displacement_at + 3) & ~(uintptr_t)7) ? 8 : 16;
    unsigned char *block = (unsigned char *)(displacement_at & ~(uintptr_t)(size - 1));
    if ((uintptr_t)block + size < displacement_at + 4) return "its displacement crosses a 16-byte boundary";
    unsigned offset = (unsigned)(displacement_at - (uintptr_t)block);

    unsigned char *jump = fh2_allocate_near(site);
    if (!jump) return "there is no free memory within 2 GB of it";
    unsigned char code[14] = {0xff, 0x25, 0, 0, 0, 0};  // jmp qword [rip+0]; dq handler
    uintptr_t address = (uintptr_t)handler;
    fh2_copy_bytes(code + 6, &address, 8);
    fh2_copy_bytes(jump, code, sizeof(code));
    DWORD protection, ignored;
    if (!VirtualProtect(jump, sizeof(code), PAGE_EXECUTE_READ, &protection)) {
        VirtualFree(jump, 0, MEM_RELEASE);
        return "the jump could not be made executable";
    }
    FlushInstructionCache(GetCurrentProcess(), jump, sizeof(code));

    unsigned char before[16], after[16];
    if (!fh2_safe_read(block, before, size)) {
        VirtualFree(jump, 0, MEM_RELEASE);
        return "it cannot be read";
    }
    int32_t old_displacement = (int32_t)((unsigned char *)target - (site + 5));
    int32_t new_displacement = (int32_t)(jump - (site + 5));
    fh2_copy_bytes(before + offset, &old_displacement, 4);  // what the check above saw
    fh2_copy_bytes(after, before, size);
    fh2_copy_bytes(after + offset, &new_displacement, 4);
    *previous = target;
    if (!VirtualProtect(block, size, PAGE_EXECUTE_READWRITE, &protection)) {
        VirtualFree(jump, 0, MEM_RELEASE);
        return "it could not be written";
    }
    int swapped;
    if (size == 8) {
        LONG64 old_value, new_value;
        fh2_copy_bytes(&old_value, before, 8);
        fh2_copy_bytes(&new_value, after, 8);
        swapped = InterlockedCompareExchange64((volatile LONG64 *)block, new_value, old_value) == old_value;
    } else {
        Block16 old_value, new_value;
        fh2_copy_bytes(&old_value, before, 16);
        fh2_copy_bytes(&new_value, after, 16);
        swapped = __sync_bool_compare_and_swap((volatile Block16 *)block, old_value, new_value);
    }
    VirtualProtect(block, size, protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), block, size);
    if (!swapped) {
        VirtualFree(jump, 0, MEM_RELEASE);
        return "another module changed it at the same time";
    }
    return NULL;
}

// ------------------------------------------------------------ suspension

static HANDLE suspended[MAX_THREADS];

static void resume_all(unsigned count) {
    while (count--) {
        ResumeThread(suspended[count]);
        CloseHandle(suspended[count]);
    }
}

// Suspends every other thread of the process. Returns how many, or -1 (all
// resumed) when one of them is executing inside [at, at + length) or cannot
// be stopped.
static int suspend_others(const unsigned char *at, size_t length) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return -1;
    THREADENTRY32 entry = {.dwSize = sizeof(entry)};
    DWORD process = GetCurrentProcessId(), self = GetCurrentThreadId();
    unsigned count = 0;
    int busy = 0;
    for (BOOL more = Thread32First(snapshot, &entry); more && !busy; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, entry.th32ThreadID);
        if (!thread) continue;  // already gone
        if (count >= MAX_THREADS || SuspendThread(thread) == (DWORD)-1) {
            CloseHandle(thread);
            busy = count >= MAX_THREADS;
            continue;
        }
        suspended[count++] = thread;
        CONTEXT context = {.ContextFlags = CONTEXT_CONTROL};
        if (!GetThreadContext(thread, &context) ||
            (context.Rip >= (DWORD64)(uintptr_t)at && context.Rip < (DWORD64)(uintptr_t)(at + length)))
            busy = 1;
    }
    CloseHandle(snapshot);
    if (busy) {
        resume_all(count);
        return -1;
    }
    return (int)count;
}

const char *write_code(unsigned char *at, const unsigned char *expected, const unsigned char *bytes, size_t length) {
    if (length > 64) return "too many bytes";
    for (unsigned attempt = 0; attempt < WRITE_TRIES; ++attempt) {
        int count = suspend_others(at, length);
        if (count < 0) {
            Sleep(1);
            continue;
        }
        const char *problem = NULL;
        DWORD protection, ignored;
        if (!fh2_bytes_equal(at, expected, length))
            problem = "another module changed it";
        else if (!VirtualProtect(at, length, PAGE_EXECUTE_READWRITE, &protection))
            problem = "it could not be written";
        else {
            fh2_copy_bytes(at, bytes, length);
            VirtualProtect(at, length, protection, &ignored);
            FlushInstructionCache(GetCurrentProcess(), at, length);
        }
        resume_all((unsigned)count);
        return problem;
    }
    return "a thread kept running inside it";
}
