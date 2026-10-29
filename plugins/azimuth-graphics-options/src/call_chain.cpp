// Azimuth Graphics Options for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "call_chain.h"

#include "fh2_guard.h"

namespace azg {

void *call_target(const unsigned char *site) { return fh2_call_target(site); }

ChainResult chain_call(unsigned char *site, void *handler, void **previous) {
    switch (fh2_chain_call(site, handler, previous)) {
    case FH2_CHAINED: return kChained;
    case FH2_NOT_CALL: return kNotCall;
    case FH2_TARGET_NOT_CODE: return kTargetNotCode;
    case FH2_UNALIGNED: return kUnaligned;
    case FH2_NO_NEAR_MEMORY: return kNoNearMemory;
    case FH2_JUMP_FAILED:
    case FH2_WRITE_FAILED: return kWriteFailed;
    case FH2_RACED: return kRaced;
    }
    return kWriteFailed;
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

} // namespace azg
