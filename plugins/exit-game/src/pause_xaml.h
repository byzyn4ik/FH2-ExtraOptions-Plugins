// Exit Game for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The pause menu's Home tab (Scenes/Anthem/A_PauseMenu.xaml in media\UI.zip)
// with an Exit Game tile:
//
//   - Online Road Trip and Horizon Solo (the wide 2x1 tile under the map; the
//     game shows one of them) become 1x1 tiles drawn by
//     EO_ControlTemplate_SquareImageTile, the game's tile look with the wide
//     picture cropped to its middle;
//   - PART_Button_EOExitGame, a 1x1 icon tile with the game's Quit icon and the
//     caption in the game's language, takes the square beside them (grid 1,1);
//   - the template and the tile's TileMenuDataItem go before the closing
//     </ResourceDictionary> of <Page.Resources>.
//
// Everything else stays byte for byte, so the edit works on the game's page and
// on the page as Extra Options 0.4.4 (CarsNextToHome) or another plugin left it.
#pragma once
#include <stddef.h>
#include <wchar.h>

#define EXIT_GAME_TILE_NAME "PART_Button_EOExitGame"

// 1 when `text` is the pause menu page (any state).
int pause_xaml_is_page(const char *text, size_t length);

// Writes the page with the tile to output (NUL-terminated) and returns its
// length; 0 (and *why) when the page is not as expected, already has the
// tile, or the result does not fit `capacity` (including the NUL).
size_t pause_xaml_extend(const char *text, size_t length, char *output, size_t capacity,
                         const wchar_t *caption, const char **why);
