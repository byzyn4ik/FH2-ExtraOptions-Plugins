// Keyboard & Mouse for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The KEYBOARD & MOUSE tile's XAML and its page (kbm_menu.c holds the rows):
//
// - XAML. The game reads every XAML resource through one call on its
//   UIRenderer LoadComponent path, "call 0xaabec0" at game+0x16ebb37. Extra
//   Options points that call at its thunk (game+0x13fe354) and other plugins
//   (Azimuth Graphics Options, Exit Game) chain it too. This plugin swaps the
//   call's 4-byte displacement (compare-exchange) for a jump to its reader,
//   which calls whatever the call went to first, then edits two pages:
//   the Settings hub (A_PauseOptions: Controls becomes 1x1, the KEYBOARD &
//   MOUSE tile takes the cell to its right) and, right after that tile was
//   chosen, the Video page (A_VideoOptions) as the game, Extra Options and
//   other plugins left it, rebuilt as the KEYBOARD & MOUSE page: its own
//   title, only this plugin's rows, the brightness preview hidden, marked
//   <!-- EO:StandaloneOptionsPage --> so other readers leave it alone.
// - Properties. Each row binds to an int property (and a choice list) the
//   plugin registers on the VideoOptions type (registry game+0x35470d0, after
//   the game's own flag-guarded set-up 0xeca710), made from the page's own
//   Subtitles and OnOffOptions property descriptors, as Extra Options does.
//   They are registered on the UI thread when the page is first built.
// - Reset. The Video page's reset (vtable slot game+0x2e60900; Extra Options
//   and Weather Control chain it) is chained: on the KEYBOARD & MOUSE page it
//   resets the options rows, on any other page it calls the previous reset.
#ifndef KBM_SETTINGS_PAGE_H
#define KBM_SETTINGS_PAGE_H
#include "common.h"

// NULL when the game image is as inspected and the sites can be chained,
// else what differs. *resource_target / *reset_target describe what the
// resource call and the reset slot hold now.
const char *settings_page_check(const unsigned char *game, const char **resource_target, const char **reset_target);
// Chains the resource call and the reset slot; 0 (logged) when it cannot.
int settings_page_install(unsigned char *game);

// The KEYBOARD & MOUSE page from the Video page's XAML, or 0 (layout not
// as expected, not the Video page, already the keyboard page, too small).
size_t settings_page_keyboard_xaml(unsigned char *game, const char *text, size_t length, char *output,
                                   size_t capacity);
#endif
