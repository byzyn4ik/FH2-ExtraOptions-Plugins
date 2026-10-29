// Azimuth Graphics Options for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Takes over a "call rel32" site without assuming what it calls: the current
// target (the game's function, Extra Options' thunk or another module's hook)
// is kept and called by the new handler. The site is pointed at a small jump
// placed within 2 GB of it ("jmp [rip+0]; dq handler"); only the 4-byte,
// 4-aligned displacement is swapped, atomically. The work is
// common/include/fh2_guard.h's fh2_chain_call, shared with Exit Game, which
// chains the same XAML resource call.
#pragma once
#include <cstdint>

namespace azg {

enum ChainResult { kChained, kNotCall, kTargetNotCode, kUnaligned, kNoNearMemory, kWriteFailed, kRaced };

// `site` points at E8 rel32. On kChained, *previous is what the site called.
ChainResult chain_call(unsigned char *site, void *handler, void **previous);
const char *chain_text(ChainResult result);
// What a call site currently calls.
void *call_target(const unsigned char *site);

} // namespace azg
