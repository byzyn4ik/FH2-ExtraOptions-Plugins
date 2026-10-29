// Performance for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "call_chain.h"

#include <cstring>

#include "common.h"

namespace perf {

namespace {

constexpr uintptr_t kReach = 0x7ff00000u; // a little under 2 GB

bool reachable(const unsigned char *from, const void *to) {
    intptr_t distance = (intptr_t)to - (intptr_t)from;
    return distance > -(intptr_t)kReach && distance < (intptr_t)kReach;
}

// A page of free memory within reach of `around`, searched outwards.
unsigned char *allocate_near(const unsigned char *around) {
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    uintptr_t granularity = system.dwAllocationGranularity;
    uintptr_t center = (uintptr_t)around & ~(granularity - 1);
    uintptr_t low = center > kReach ? center - kReach : granularity;
    uintptr_t high = center + kReach;
    if ((uintptr_t)system.lpMaximumApplicationAddress < high) high = (uintptr_t)system.lpMaximumApplicationAddress;
    for (uintptr_t step = granularity; step < kReach; step += granularity) {
        for (int direction = 0; direction < 2; ++direction) {
            uintptr_t at = direction ? center + step : center - step;
            if (direction ? at >= high : (center < step || at < low)) continue;
            MEMORY_BASIC_INFORMATION region;
            if (VirtualQuery((void *)at, &region, sizeof(region)) != sizeof(region) || region.State != MEM_FREE)
                continue;
            void *page = VirtualAlloc((void *)at, system.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (page && reachable(around, page)) return (unsigned char *)page;
            if (page) VirtualFree(page, 0, MEM_RELEASE);
        }
    }
    return nullptr;
}

} // namespace

void *call_target(const unsigned char *site) {
    unsigned char code[5];
    if (!safe_read(site, code, sizeof(code)) || code[0] != 0xe8) return nullptr;
    int32_t displacement;
    memcpy(&displacement, code + 1, 4);
    return (void *)(site + 5 + displacement);
}

ChainResult chain_call(unsigned char *site, void *handler, void **previous) {
    void *target = call_target(site);
    if (!target) return kNotCall;
    if (!is_code(target)) return kTargetNotCode;
    if (((uintptr_t)(site + 1) & 3) != 0) return kUnaligned;
    unsigned char *jump = allocate_near(site);
    if (!jump) return kNoNearMemory;
    // jmp qword [rip+0]; dq handler
    unsigned char code[14] = {0xff, 0x25, 0, 0, 0, 0};
    uintptr_t address = (uintptr_t)handler;
    memcpy(code + 6, &address, 8);
    memcpy(jump, code, sizeof(code));
    DWORD previous_protection, ignored;
    if (!VirtualProtect(jump, sizeof(code), PAGE_EXECUTE_READ, &previous_protection)) {
        VirtualFree(jump, 0, MEM_RELEASE);
        return kWriteFailed;
    }
    FlushInstructionCache(GetCurrentProcess(), jump, sizeof(code));
    *previous = target;
    int32_t old_displacement = (int32_t)((unsigned char *)target - (site + 5));
    int32_t new_displacement = (int32_t)(jump - (site + 5));
    if (!VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &previous_protection)) {
        VirtualFree(jump, 0, MEM_RELEASE);
        return kWriteFailed;
    }
    LONG seen = InterlockedCompareExchange((volatile LONG *)(site + 1), (LONG)new_displacement, (LONG)old_displacement);
    VirtualProtect(site + 1, 4, previous_protection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    if (seen != (LONG)old_displacement) {
        VirtualFree(jump, 0, MEM_RELEASE);
        return kRaced;
    }
    return kChained;
}

const char *chain_text(ChainResult result) {
    switch (result) {
    case kChained: return "chained";
    case kNotCall: return "the site is not a call";
    case kTargetNotCode: return "the site's current target is not executable code";
    case kUnaligned: return "the call's displacement is not 4-byte aligned";
    case kNoNearMemory: return "no free memory within 2 GB of the site";
    case kWriteFailed: return "the site could not be written";
    case kRaced: return "another module changed the site at the same time";
    }
    return "?";
}

} // namespace perf
