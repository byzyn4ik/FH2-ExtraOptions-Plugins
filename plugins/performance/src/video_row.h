// Performance for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The "FPS counter" row (Off / On / Detailed) on the game's Video settings
// page (media\UI.zip, Scenes/Anthem/A_VideoOptions.xaml).
//
// The game reads every XAML resource through one call on its UIRenderer
// LoadComponent path: "call 0xaabec0" at 0x16ebb37. Extra Options 0.4.4
// points that call at its own thunk (0x13fe354) to extend the Video page and
// others; other plugins (Azimuth Graphics Options, Exit Game, Weather
// Control) chain it too. This plugin chains it the same way (call_chain.h):
// its handler first calls whatever the site called, then edits the Video page
// that came back:
//   - one TileMenuDataItem (PERF_FpsCounter) before the first
//     </ResourceDictionary>;
//   - one SpinnerControl as the last row of the options list;
//   - a list with the fixed style (A_Style_ListBox_List_Default) gets the
//     scrollable style, and its height of 110 (the page alone) becomes 275 (as
//     after Extra Options); any other height is kept.
// The edit keeps everything Azimuth Graphics Options looks for (its list
// height of 110 or 275 and its place before Extra Options' rows), so the two
// plugins make a valid page in either order.
// Pages marked "<!-- EO:StandaloneOptionsPage -->" and pages built from the
// Video page class (the Extra Options fork's KEYBOARD & MOUSE and World) are
// left alone.
//
// The row binds to "PerfFpsCounter" (an int) and "PerfFpsCounterChoices" (a
// collection) registered in the Video page type's registry (0x35470d0) with
// the game's own property descriptors (copied from Subtitles and
// OnOffOptions), as Extra Options registers its own rows.
#pragma once
#include <cstddef>
#include <cstdint>

namespace perf {
namespace video_row {

constexpr uint32_t kImageSize = 0x3a18000u;
constexpr uint32_t kResourceCall = 0x16ebb37u;

// The row's captions in one of the game's languages.
struct Captions {
    const char *code;
    const wchar_t *name;
    const wchar_t *choices[3];
    const wchar_t *description;
};
// For a game language code ("EN", "PL", "CHT", ...); English when unknown.
const Captions &captions_for(const char *code);
unsigned caption_count();
const Captions &caption(unsigned index);
// The game's current text language code, "" when unreadable.
const char *game_language(const unsigned char *game);

// nullptr when the game image is as inspected (every native function the row
// uses, the Video page type and the resource call site), else what differs.
// *target (when given) describes what the call site calls now.
const char *check_game(const unsigned char *game, uint32_t size, const char **target);

// The Video page XAML with the row added; 0 (and *why) when it is not that
// page as expected or already has the row.
size_t extend_video(const char *text, size_t length, char *output, size_t capacity, const Captions &captions,
                    const char **why);

using Changed = void (*)(unsigned mode);
using Current = unsigned (*)();
// Checks the game and chains the resource call; nullptr when done, else why not.
const char *install(unsigned char *game, uint32_t size, Changed changed, Current current);

} // namespace video_row
} // namespace perf
