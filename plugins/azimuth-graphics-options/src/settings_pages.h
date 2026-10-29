// Azimuth Graphics Options for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Azimuth rows on two of the game's own settings pages (media\UI.zip):
//   Video (Scenes/Anthem/A_VideoOptions.xaml): frame rate ... HDR;
//   Audio (Scenes/Anthem/A_AudioOptions.xaml): audio output and volume.
//
// The game reads every XAML resource through one call on its UIRenderer
// LoadComponent path: "call 0xaabec0" at 0x16ebb37. Extra Options (0.3.2 and
// 0.4.4 alike) points that call at its own thunk (0x13fe354, in padding) to
// extend the Video, HUD, Audio and other pages. This plugin chains the call
// (see call_chain.h): its handler first calls whatever the site called
// (Extra Options' hook, or the game's reader when Extra Options left it
// alone), then edits the XAML that came back, for these two pages only:
//
//   Video  one TileMenuDataItem per row before the first </ResourceDictionary>;
//          one SpinnerControl per row after the page's own rows (Brightness,
//          Subtitles) and before Extra Options' (Realtime shadows, ...); the
//          list gets the scrollable list style and a 440 px height (it was
//          110, or 275 after Extra Options).
//   Audio  the resources likewise, plus a 0..1 slider style in 5 % steps;
//          after the last row (the page's own, then Extra Options' dialogue
//          and checkpoint rows) a SpinnerControl (audio output) and a Slider
//          (volume); the list (550 px) gets the scrollable list style.
//
// Pages built from these classes but marked "<!-- EO:StandaloneOptionsPage -->"
// (plugins' own pages, such as Weather Control's) and pages Extra Options'
// fork builds from the Video page (KEYBOARD & MOUSE, World) are left alone.
//
// The rows bind to properties ("GfxFrameRate", "AzVolume", ...) registered in
// the page type's registry (Video 0x35470d0, Audio 0x35107b0) with the game's
// own int, float and collection property descriptors, the way Extra Options
// registers its own rows.
#pragma once
#include <cstddef>
#include <cstdint>

#include "rows.h"

namespace azg {
namespace pages {

constexpr uint32_t kImageSize = 0x3a18000u;
constexpr uint32_t kResourceCall = 0x16ebb37u;

// nullptr when the game image is as inspected (every native function the
// rows use, both page types, and the resource call site), else what differs.
// *target (when given) describes what the call site calls now.
const char *check_game(const unsigned char *image, uint32_t size, const char **target);

// The page XAML with the rows added; 0 (and *why) when it is not that page as
// expected or already has them.
size_t extend_video(const char *text, size_t length, char *output, size_t capacity, Language language,
                    const LiveState &live, const char **why);
size_t extend_audio(const char *text, size_t length, char *output, size_t capacity, Language language,
                    const LiveState &live, const char **why);

// Chains the resource call; false (logged) when it cannot.
bool install(unsigned char *image, const LiveState &live, bool apply_live);

} // namespace pages
} // namespace azg
