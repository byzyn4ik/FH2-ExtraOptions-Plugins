// Custom Radio for Forza Horizon 2 Extra Options: guarded, chaining hooks.
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CUSTOM_RADIO_HOOKS_H
#define CUSTOM_RADIO_HOOKS_H
#include "sites.h"
#include "support.h"

// Every hook is checked byte for byte before anything is written:
//  - the hooked function's first bytes (sites.h) must be the game's;
//  - a virtual-table slot must hold that function, or code outside it that
//    another mod put there (then the plugin calls that code instead: chained);
//  - a rel32 call must be E8 between the game's 8 bytes before and after it,
//    calling the function or other code (chained the same way).
// A group of sites is written only when all of them pass, through a
// compare-and-swap of the exact value that was checked; a failed write puts
// back what the group already changed. Calls go through 14-byte jumps in a
// page of our own near the game image (no int3 caves of the game are used).

void hooks_bind(unsigned char *image);
unsigned char *hooks_image(void);
// The function's first bytes are the game's.
int hooks_function_ok(unsigned function);
const char *hooks_function_name(unsigned function);
void *hooks_function(unsigned function);

typedef struct {
    unsigned index;  // SLOT_* or CALL_*
    int call;        // 0 slot, 1 call
    void *current;   // the code there now (game function or another hook): the chain target
    void *replacement;
    int chained, installed;
} Hook;

// Checks the sites and fills `current`; logs the reason and returns 0 on a mismatch.
int hooks_prepare_slot(Hook *hook, unsigned slot);
int hooks_prepare_call(Hook *hook, unsigned call);
// Writes the prepared hooks (all or none).
int hooks_commit(Hook *const *hooks, unsigned count);
const char *hooks_site_name(const Hook *hook);
#endif
