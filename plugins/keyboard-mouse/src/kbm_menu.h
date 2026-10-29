// Keyboard & Mouse for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef FH2_KBM_MENU_H
#define FH2_KBM_MENU_H
#include "kbm_rebind.h"

/* The KEYBOARD & MOUSE tile on the pause menu's Settings hub and its page:
   the main [KeyboardMouse] options, then one row per key binding and Reset
   (kbm_rebind.c). The page is the native Video page rebuilt with these rows
   (settings_page.c); every change is written to KeyboardMouse.ini
   [KeyboardMouse] and applied at once. Installed only with [KeyboardMouse]
   Enabled=1 on a supported Azimuth host; otherwise the hub, the Video page
   and their properties are left as the game has them.
   Rows: 0..KBM_MENU_OPTION_ROWS-1 the options, then the kbm_rebind.c items. */
enum { KBM_MENU_OPTION_ROWS = KBM_TEXT_ROWS,
       KBM_MENU_ROWS = KBM_MENU_OPTION_ROWS + KBM_REBIND_ITEMS,
       /* The hidden list-selection tracker; the page lists every row before it. */
       KBM_MENU_LIST_ROW = KBM_MENU_OPTION_ROWS + KBM_REBIND_LIST_ITEM };

/* Chains the Settings hub's init (its vtable slot); 0 (logged) when the
   hub is not as expected. */
int kbm_menu_install(unsigned char *game_image);
int kbm_menu_installed(void);
/* The Settings hub (A_PauseOptions) and its copy with the new tile. */
int kbm_menu_is_hub_xaml(const char *text, size_t length);
size_t kbm_menu_hub_xaml(unsigned char *game_image, const char *text, size_t length,
                         char *output, size_t capacity);
/* 1 once when the tile asked for the page in the last few seconds. */
int kbm_menu_take_request(void);
/* A new KEYBOARD & MOUSE page was built. */
void kbm_menu_page_built(void);
const char *kbm_menu_property(unsigned row);
/* NULL for the list-selection tracker. */
const char *kbm_menu_collection(unsigned row);
int kbm_menu_get(unsigned row);
/* 1 when the value changed and the page should read it again. */
int kbm_menu_set(unsigned row, int value);
/* The option rows back to the mod's defaults (the page's Reset to default). */
void kbm_menu_reset(void);
unsigned kbm_menu_choice_count(unsigned row);
const wchar_t *kbm_menu_choice(unsigned char *game_image, unsigned row, unsigned index);
const char *kbm_menu_name(unsigned char *game_image, unsigned row);
const char *kbm_menu_description(unsigned char *game_image, unsigned row);
/* Key binding rows: the choice collection must be built again / the value
   changed outside its setter (once). */
int kbm_menu_labels_stale(unsigned row);
int kbm_menu_take_dirty(unsigned row);
/* UI thread, once per UserInterface::Update (kbm_input.c). */
void kbm_menu_ui_tick(void);
/* Provided by settings_page.c: the KEYBOARD & MOUSE page object while the
   game still holds it (collect: the UI tick drops pages the game let go),
   and the value notifications for the key rows that changed. */
void *settings_page_rebind_page(int collect);
void settings_page_rebind_tick(void);
#endif
