// Azimuth Graphics Options for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Azimuth graphics rows and their keys in Azimuth 1.x's data\azimuth.ini,
// as the Extra Options fork (src/ForzaHorizon2_ExtraOptions-0.3.2-openbox,
// features/graphics_launcher.c) had them:
//
//   [title-1a96545b]  frame_rate, vsync, resolution, graphics, shadow_detail
//                     (with shadow_distance and shadow_redraw), texture_filtering,
//                     antialiasing, hdr
//   [launcher]        display
//
// Frame rate offers what Azimuth 1.0.2's own launcher offers (live.h,
// frame_rates): 30 and the steps up to the display's refresh rate. As in that
// launcher, a saved rate between two choices shows as the lower one.
// Azimuth 1.x has no frame_pacing key (it always paces in real time), so that
// row is kept for the table's order but never shown, as in the fork.
// Choosing Shadows or Texture filtering sets graphics=custom.
#pragma once
#include <string>
#include <vector>

namespace azg {

enum Row {
    kFrameRate,
    kVSync,
    kFramePacing,
    kResolution,
    kDisplay,
    kPreset,
    kShadows,
    kTextureFiltering,
    kAntiAliasing,
    kHdr,
    kRowCount
};

enum Language { kEnglish, kPolish };

// What applies while the game runs (see live.h); the rows' descriptions say so.
struct LiveState {
    bool frame_rate, display, shadow_distance, shadow_rate;
};

namespace rows {

void set_file(const std::wstring &azimuth_ini);
const std::wstring &file();
// Empty when data\azimuth.ini has Azimuth 1.x's layout, else what is wrong.
std::string check_layout();

// The Frame rate row's choices (frames per second, ascending, from 30).
void set_frame_rates(const std::vector<int> &rates);
bool frame_rates_set();
const std::vector<int> &frame_rates();
int frame_rate_value(int index);

bool shown(unsigned row);
unsigned count(unsigned row);
int get(unsigned row);
// Writes the row's keys; false if the value is out of range, unchanged or
// cannot be written.
bool set(unsigned row, int value);
// The level the shadow renderer uses: the preset's, or Shadows with Custom
// (0 low, 1 default, 2 high, 3 ultra).
int shadow_level();

const char *property(unsigned row);   // binding name on the Video page, "GfxFrameRate"
const char *collection(unsigned row); // its choices, "GfxFrameRateChoices"
const wchar_t *choice(unsigned row, unsigned index, Language language);
const char *name(unsigned row, Language language);
std::string description(unsigned row, Language language, const LiveState &live);

} // namespace rows
} // namespace azg
