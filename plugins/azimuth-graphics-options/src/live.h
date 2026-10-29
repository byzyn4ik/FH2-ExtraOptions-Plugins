// Azimuth Graphics Options for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Azimuth host and what applies while the game runs, each guarded byte for
// byte; on a mismatch that setting applies after a restart only, and the log
// says why.
//
// Azimuth builds, inspected in the release ZIPs' azimuth.exe (the Builder's
// branded "Forza Horizon 2.exe" of a build has the same code in a larger
// image):
//
//                       1.0.4 (the goal)        1.0.2
//   TimeDateStamp       0x6ae393eb              0x6ae05dd1
//   SizeOfImage         >= 0x8a2000             >= 0x9af000
//   launcher FPS list   0x104c0 (table 0x10528) 0x10590 (table 0x105f8)
//   rate setter         0x4ba30, from 0x945f    0x4cc50, from 0x92f3
//   target rate         0x7d8554                0x8e7540
//   title's own rate    0x7d8550                0x8e7544
//   pacing read         0x4e8e7                 0x4fae7
//   fullscreen byte     0x7fcbc1 (read 0x6e868) 0x909d61 (read 0x6f5f8)
//   Alt+Enter           0x6eda7                 0x6fb27
//   volume setter       0xc37f0, from 0x938b    0xbaea0, from 0x921f
//   volume              0x7da2e0                0x8e7be8
//
//   Frame rate   At startup Azimuth calls the rate setter ("mov [target],ecx;
//                mov [title],edx; ret") with the title's frame_rate and the
//                title's own rate (30 for FH2). Its pacing reads the target
//                again for every frame (frame interval = QPC frequency /
//                rate), so writing it applies at once, for any rate Azimuth
//                accepts (its INI reader takes 30..1000).
//   Display      Azimuth toggles borderless fullscreen on Alt+Enter
//                (WM_SYSKEYDOWN with VK_RETURN: "cmp rdi,0Dh; jne; bt rsi,1Dh")
//                and keeps the mode in a byte its toggle reads first
//                ("cmp byte [rip],0"); the row posts that key to Azimuth's
//                window when the mode differs.
//   Volume       Azimuth's own live volume setter, which its in-game menu
//                calls too: clamps to 0..100 under its audio lock, stores the
//                value and sets every output's level. The row calls it.
//   Audio output Azimuth latches the channel layout when it opens the audio
//                output, so this one applies after a restart.
// FH2 (the executable Extra Options supports):
//   Shadow distance  the cascade distances at 0x2207288, constants Azimuth
//                overwrites at startup for High and Ultra and the renderer
//                reads every frame.
//   Shadow update rate  the shadow renderer ([0x36609a0], vtable 0x2e82b88)
//                refreshes [renderer+0x21bc] of its four maps per frame (read
//                at 0x1131c02). Extra Options' Realtime shadows = On replaces
//                that read (all cascades every frame); then this is left to it.
// Not touched: motion blur (0x2e834e4, Extra Options' photo sampling owns it)
// and the game's FrameRate render scenario class (vtable 0x2eb98d0): this
// plugin only moves Azimuth's presentation rate.
#pragma once
#include <cstdint>
#include <vector>

#include "rows.h"

namespace azg {
namespace live {

struct Host;
// The inspected Azimuth build `host` is, or nullptr (*why says what differs).
const Host *identify(const unsigned char *host, const char **why);
const char *host_name(const Host *build); // "1.0.4"

// The frame rates Azimuth's launcher offers for a display refresh rate (the
// same code in 1.0.4 and 1.0.2): 30, then 60, 90, 120, 144, 165 and 240 up to
// the refresh rate, then the refresh rate itself when it is higher (and at
// most 1000). A refresh rate of 0 or 1 counts as 60.
std::vector<int> frame_rates(unsigned refresh);
// The refresh rate of the display Azimuth's window is on (else the primary).
unsigned display_refresh();

// Checks each live site and logs what applies at once.
void prepare(const Host *build, unsigned char *host, unsigned char *game);
const LiveState &state();
// After a row was written; `value` is the row's value (a choice index, or the
// volume 0..100).
void apply(unsigned row, int value);

#ifdef AZG_TEST_EXPORTS
// Test build: a site of an identified build (0 target rate, 1 volume setter,
// 2 fullscreen byte, 3 launcher table, 4 volume).
uint32_t test_site(const Host *build, unsigned which);
#endif

} // namespace live
} // namespace azg
