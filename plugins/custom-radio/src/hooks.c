// Custom Radio for Forza Horizon 2 Extra Options: guarded, chaining hooks.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "hooks.h"

typedef struct {
    const char *name;
    uint32_t rva;
    const char *bytes;
    uint32_t size;
} FunctionSite;

typedef struct {
    const char *name;
    uint32_t rva;
    unsigned function;
} SlotSite;

typedef struct {
    const char *name;
    uint32_t rva;
    unsigned function;
    const char *before, *after;
} CallSite;

#define FUNCTION_ROW(name, rva, bytes) {#name, rva, bytes, sizeof(bytes) - 1},
#define SLOT_ROW(name, rva, function) {#name, rva, function},
#define CALL_ROW(name, rva, function, before, after) {#name, rva, function, before, after},
static const FunctionSite functions[FN_COUNT] = {RADIO_FUNCTIONS(FUNCTION_ROW)};
static const SlotSite slots[SLOT_COUNT] = {RADIO_SLOTS(SLOT_ROW)};
static const CallSite calls[CALL_COUNT] = {RADIO_CALLS(CALL_ROW)};

static unsigned char *image;
static unsigned char *jumps;  // our page of jumps near the image
static unsigned jumps_used;
enum { JUMP_SIZE = 16, JUMP_PAGE = 4096 };

void hooks_bind(unsigned char *base) {
    image = base;
}

unsigned char *hooks_image(void) {
    return image;
}

int hooks_function_ok(unsigned function) {
    unsigned char bytes[32];
    const FunctionSite *site = &functions[function];
    return image && function < FN_COUNT && site->size <= sizeof(bytes) &&
           memory_read(image + site->rva, bytes, site->size) &&
           memory_equal(bytes, site->bytes, site->size);
}

const char *hooks_function_name(unsigned function) {
    return function < FN_COUNT ? functions[function].name + 3 : "?";
}

void *hooks_function(unsigned function) {
    return image + functions[function].rva;
}

const char *hooks_site_name(const Hook *hook) {
    return hook->call ? calls[hook->index].name + 5 : slots[hook->index].name + 5;
}

static void refuse(const char *site, const char *reason) {
    log_append_parts("CUSTOM RADIO: ", site, ": ", reason);
}

int hooks_prepare_slot(Hook *hook, unsigned slot) {
    const SlotSite *site = &slots[slot];
    void *value = NULL;
    memory_zero(hook, sizeof(*hook));
    hook->index = slot;
    if (!hooks_function_ok(site->function)) {
        refuse(site->name, "the game function's first bytes are not as expected; not hooked.");
        return 0;
    }
    if (!memory_read(image + site->rva, &value, sizeof(value))) {
        refuse(site->name, "the slot is not readable; not hooked.");
        return 0;
    }
    if (value != hooks_function(site->function)) {
        // Another mod's hook: chain to it, if it is code.
        if (!memory_is_code(value)) {
            refuse(site->name, "the slot holds neither the game function nor code; not hooked.");
            return 0;
        }
        hook->chained = 1;
    }
    hook->current = value;
    return 1;
}

int hooks_prepare_call(Hook *hook, unsigned call) {
    const CallSite *site = &calls[call];
    unsigned char bytes[21];
    int32_t displacement;
    memory_zero(hook, sizeof(*hook));
    hook->index = call;
    hook->call = 1;
    if (!hooks_function_ok(site->function)) {
        refuse(site->name, "the called game function's first bytes are not as expected; not hooked.");
        return 0;
    }
    if (!memory_read(image + site->rva - 8, bytes, sizeof(bytes)) ||
        !memory_equal(bytes, site->before, 8) || bytes[8] != 0xe8 ||
        !memory_equal(bytes + 13, site->after, 8)) {
        refuse(site->name, "the code around the call is not as expected; not hooked.");
        return 0;
    }
    memory_copy(&displacement, bytes + 9, 4);
    void *target = image + site->rva + 5 + displacement;
    if (target != hooks_function(site->function)) {
        if (!memory_is_code(target)) {
            refuse(site->name, "the call targets neither the game function nor code; not hooked.");
            return 0;
        }
        hook->chained = 1;
    }
    hook->current = target;
    return 1;
}

// One page for the jumps, within rel32 reach of the whole image.
static unsigned char *jump_for(void *destination) {
    if (!jumps) {
        SYSTEM_INFO system;
        GetSystemInfo(&system);
        uintptr_t granularity = system.dwAllocationGranularity ? system.dwAllocationGranularity : 0x10000;
        uintptr_t base = (uintptr_t)image, end = base + GAME_IMAGE_SIZE;
        uintptr_t up = (end + granularity - 1) & ~(granularity - 1);
        uintptr_t down = (base & ~(granularity - 1)) - granularity;
        for (uintptr_t step = 0; !jumps && step < 0x60000000u; step += granularity) {
            if (up + step > up) {
                jumps = VirtualAlloc((void *)(up + step), JUMP_PAGE, MEM_RESERVE | MEM_COMMIT,
                                     PAGE_READWRITE);
            }
            if (!jumps && down > step) {
                jumps = VirtualAlloc((void *)(down - step), JUMP_PAGE, MEM_RESERVE | MEM_COMMIT,
                                     PAGE_READWRITE);
            }
        }
        if (!jumps) {
            log_append("CUSTOM RADIO: no memory near the game for call hooks.\r\n");
            return NULL;
        }
    }
    if ((jumps_used + 1) * JUMP_SIZE > JUMP_PAGE) {
        return NULL;
    }
    unsigned char *jump = jumps + jumps_used * JUMP_SIZE;
    // jmp qword [rip+0]; dq destination
    unsigned char code[JUMP_SIZE] = {0xff, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xcc, 0xcc};
    uintptr_t address = (uintptr_t)destination;
    memory_copy(code + 6, &address, 8);
    DWORD previous;
    if (!VirtualProtect(jumps, JUMP_PAGE, PAGE_READWRITE, &previous)) {
        return NULL;
    }
    memory_copy(jump, code, sizeof(code));
    VirtualProtect(jumps, JUMP_PAGE, PAGE_EXECUTE_READ, &previous);
    FlushInstructionCache(GetCurrentProcess(), jump, JUMP_SIZE);
    ++jumps_used;
    return jump;
}

// Replaces `size` bytes at `at` that must still be `expected`, with one
// interlocked compare-and-swap (the bytes may be running on another thread).
static int swap_bytes(unsigned char *at, const void *expected, const void *value, unsigned size) {
    uintptr_t address = (uintptr_t)at;
    int done = 0;
    DWORD previous, ignored;
    unsigned char *page_start = (unsigned char *)(address & ~(uintptr_t)15);
    if (!VirtualProtect(page_start, 32, PAGE_EXECUTE_READWRITE, &previous)) {
        return 0;
    }
    if ((address & 7) + size <= 8) {
        volatile LONG64 *block = (volatile LONG64 *)(address & ~(uintptr_t)7);
        LONG64 old_value = *block, new_value;
        unsigned offset = (unsigned)(address & 7);
        if (memory_equal((unsigned char *)&old_value + offset, expected, size)) {
            new_value = old_value;
            memory_copy((unsigned char *)&new_value + offset, value, size);
            done = InterlockedCompareExchange64(block, new_value, old_value) == old_value;
        }
    } else if ((address & 15) + size <= 16) {
        volatile unsigned __int128 *block = (volatile unsigned __int128 *)(address & ~(uintptr_t)15);
        unsigned __int128 old_value, new_value;
        unsigned offset = (unsigned)(address & 15);
        memory_copy(&old_value, (const void *)block, sizeof(old_value));
        if (memory_equal((unsigned char *)&old_value + offset, expected, size)) {
            new_value = old_value;
            memory_copy((unsigned char *)&new_value + offset, value, size);
            done = __sync_bool_compare_and_swap(block, old_value, new_value);  // cmpxchg16b
        }
    }
    VirtualProtect(page_start, 32, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), at, size);
    return done;
}

static int write_hook(Hook *hook, int install) {
    if (!hook->call) {
        unsigned char *slot = image + slots[hook->index].rva;
        void *from = install ? hook->current : hook->replacement;
        void *to = install ? hook->replacement : hook->current;
        return swap_bytes(slot, &from, &to, sizeof(void *));
    }
    unsigned char *site = image + calls[hook->index].rva;
    int32_t old_displacement = (int32_t)((unsigned char *)hook->current - (site + 5));
    int32_t new_displacement;
    if (install) {
        unsigned char *jump = jump_for(hook->replacement);
        intptr_t distance = jump ? jump - (site + 5) : 0;
        if (!jump || distance != (int32_t)distance) {
            return 0;
        }
        new_displacement = (int32_t)distance;
        hook->replacement = jump;  // undo compares against the jump
        return swap_bytes(site + 1, &old_displacement, &new_displacement, 4);
    }
    new_displacement = (int32_t)((unsigned char *)hook->replacement - (site + 5));
    return swap_bytes(site + 1, &new_displacement, &old_displacement, 4);
}

int hooks_commit(Hook *const *hooks, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        if (!write_hook(hooks[i], 1)) {
            log_append_text("CUSTOM RADIO: ", hooks_site_name(hooks[i]),
                            ": the hook could not be written; this group is left as it was.\r\n");
            while (i--) {
                if (!write_hook(hooks[i], 0)) {
                    log_append_text("CUSTOM RADIO: ", hooks_site_name(hooks[i]),
                                    ": could not be put back.\r\n");
                }
                hooks[i]->installed = 0;
            }
            return 0;
        }
        hooks[i]->installed = 1;
        if (hooks[i]->chained) {
            log_append_text("CUSTOM RADIO: ", hooks_site_name(hooks[i]),
                            " was already hooked by another module; chained to it.\r\n");
        }
    }
    return 1;
}
