// Keyboard & Mouse for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Code and pointer patches that are safe to make while the game runs (the
// plugin starts on Extra Options' loader worker after the game started).
//
// - Slots (vtable entries, function pointers): one aligned 8-byte
//   compare-exchange. A slot may hold the game's function or another module's
//   hook (Extra Options', another plugin's): either is chained.
// - Calls (call rel32): the call is pointed at a 14-byte absolute jump placed
//   within 2 GB of it (never in the game's int3 padding, which Extra Options
//   and other mods use). Its target may be the game's function or another
//   module's code: either is chained.
// - Function entries: the first bytes become an absolute jump to the hook; a
//   trampoline runs the moved bytes and jumps back.
// Code bytes are written with every other thread of the process suspended,
// and only when none of them is stopped inside a byte being replaced, so no
// thread ever executes a half-written instruction.
#ifndef KBM_PATCH_H
#define KBM_PATCH_H
#include "common.h"

int patch_is_code(const void *address);
// "module.dll+0x1234" (or the address) for log lines.
void patch_describe(const void *address, char *out, size_t size);

// NULL when done, else why not (nothing changed). `native` is what the slot
// holds in the stock image; anything else must be executable code outside
// [image, image + image_size). *previous gets the old value before the swap.
const char *patch_slot(void **slot, const void *native, const void *image, size_t image_size, void *hook,
                       void **previous);

// The call rel32 at `site` (its target `native`, or another module's code
// outside the image) now goes to `hook`; *previous gets the old target.
const char *patch_call(unsigned char *site, const void *native, const void *image, size_t image_size, void *hook,
                       void **previous);
// The same; `also` is one more accepted target inside the image (a thunk
// another module placed there, checked by the caller).
const char *patch_call_also(unsigned char *site, const void *native, const void *also, const void *image,
                            size_t image_size, void *hook, void **previous);

typedef struct {
    unsigned char *at;                 // function entry
    unsigned size;                     // bytes replaced (>= 14), all position independent
    const unsigned char *expected;     // their stock bytes
    void *hook;
    unsigned char fill;                // after the jump: 0x90 or 0xcc
    int trampoline;                    // 0: no trampoline (*original stays as given)
    void **original;                   // out: the trampoline
} EntryHook;

// All entries at once (one suspension), or none. NULL or why not.
const char *patch_entries(EntryHook *hooks, unsigned count);

#ifdef KBM_TEST
// Test builds: how many times patch_entries / patch_call found a thread in
// the way (always 0 offline).
unsigned patch_busy_retries(void);
#endif
#endif
