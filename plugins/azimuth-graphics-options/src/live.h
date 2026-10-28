// Azimuth Graphics Options for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What applies while the game runs, each guarded byte for byte; on a mismatch
// that setting applies after a restart only, and the log says why.
//
// Azimuth 1.0.2 (azimuth.exe, TimeDateStamp 0x6ae05dd1, SizeOfImage 0x9af000;
// the Builder's branded "Forza Horizon 2.exe" is the same build with a larger
// image) - inspected in the release's azimuth.exe:
//   Frame rate   At startup (0x92f3) Azimuth calls 0x4cc50
//                ("mov [0x8e7540],ecx; mov [0x8e7544],edx; ret") with the
//                title's frame_rate and the title's own rate. The pacing code
//                reads the target rate 0x8e7540 again for every frame
//                (0x4dd00, 0x4e710, 0x4f0d8, 0x4fad0: the frame interval is
//                QPC frequency / rate, real-time pacing), so writing it
//                applies at once, for any rate Azimuth accepts (its INI
//                reader takes 30..1000).
//   Display      Azimuth toggles borderless fullscreen on Alt+Enter
//                (WM_SYSKEYDOWN with VK_RETURN: "cmp rdi,0Dh" at 0x6fb27) in
//                0x6f5e0, which keeps the mode in a byte (0x909d61, read by
//                "cmp byte [rip],0" at 0x6f5f8); the row posts that key to
//                Azimuth's window when the mode differs.
// FH2 (the executable Extra Options 0.3.2 supports):
//   Shadow distance  the cascade distances at 0x2207288, constants Azimuth
//                overwrites at startup for High and Ultra and the renderer
//                reads every frame.
//   Shadow update rate  the shadow renderer ([0x36609a0], vtable 0x2e82b88)
//                refreshes [renderer+0x21bc] of its four maps per frame (read
//                at 0x1131c02). Extra Options' Realtime shadows = On replaces
//                that read (all cascades every frame); then this is left to it.
// Not touched: motion blur (0x2e834e4, Extra Options' photo sampling owns it)
// and the game's FrameRate render scenario class (vtable 0x2eb98d0, which the
// smooth-menus Extra Options change raises to 60 in menus): this plugin only
// moves Azimuth's presentation rate.
#pragma once
#include <cstdint>
#include <vector>

#include "rows.h"

namespace azg {
namespace live {

constexpr uint32_t kHostTimestamp = 0x6ae05dd1u;
constexpr uint32_t kHostImageSize = 0x9af000u;

// nullptr when `host` is Azimuth 1.0.2, else what differs.
const char *check_host(const unsigned char *host);
// The frame rates Azimuth 1.0.2's launcher offers for a display refresh rate
// (its 0x10590): 30, then 60, 90, 120, 144, 165 and 240 up to the refresh
// rate, then the refresh rate itself when it is higher (and at most 1000).
// A refresh rate of 0 or 1 counts as 60.
std::vector<int> frame_rates(unsigned refresh);
// The refresh rate of the display Azimuth's window is on (else the primary).
unsigned display_refresh();

// Checks each live site and logs what applies at once.
void prepare(unsigned char *host, unsigned char *game);
const LiveState &state();
// After a row was written; `value` is the row's choice index.
void apply(unsigned row, int value);

} // namespace live
} // namespace azg
