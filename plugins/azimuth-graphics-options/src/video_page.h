// Azimuth Graphics Options for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Azimuth rows on the game's own Video settings page
// (Scenes/Anthem/A_VideoOptions.xaml in media\UI.zip), as the fork put them.
//
// The game reads every XAML resource through one call on its UIRenderer
// LoadComponent path: "call 0xaabec0" at 0x16ebb37. Extra Options 0.3.2
// already points that call at its own thunk (0x13fe354, in padding) to extend
// the Video, HUD, Audio and other pages. This plugin chains the call (see
// call_chain.h): its handler first calls whatever the site called (Extra
// Options' hook, or the game's reader when Extra Options left it alone), then,
// for the Video page only, adds its rows to the XAML that came back:
//
//   - one TileMenuDataItem per row before the first </ResourceDictionary>;
//   - one SpinnerControl per row after the page's own rows (Brightness,
//     Subtitles) and before Extra Options' (Realtime shadows, ...);
//   - the list gets the scrollable list style and a 440 px height (it was 110,
//     or 275 after Extra Options).
//
// The rows bind to properties ("GfxFrameRate", "GfxFrameRateChoices", ...)
// registered in the Video page type's registry (0x35470d0), the way Extra
// Options registers its own, with the game's native int and collection
// property descriptors.
#pragma once
#include <cstddef>
#include <cstdint>

#include "rows.h"

namespace azg {
namespace video {

constexpr uint32_t kImageSize = 0x3a18000u;
constexpr uint32_t kResourceCall = 0x16ebb37u;

// nullptr when the game image is as inspected (every native function the
// rows use, and the resource call site), else what differs. *target (when
// given) describes what the call site calls now.
const char *check_game(const unsigned char *image, uint32_t size, const char **target);

// The Video page XAML with the rows added; 0 (and *why) when it is not the
// Video page as expected or already has them.
size_t extend_xaml(const char *text, size_t length, char *output, size_t capacity, Language language,
                   const LiveState &live, const char **why);

// Chains the resource call; false (logged) when it cannot.
bool install(unsigned char *image, const LiveState &live, bool apply_live);

Language game_language(const unsigned char *image);

} // namespace video
} // namespace azg
