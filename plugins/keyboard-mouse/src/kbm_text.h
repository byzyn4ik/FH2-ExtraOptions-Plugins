// Keyboard & Mouse for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The tile, page and row texts in the game's 15 languages
// (src/translations.json, compiled into translations.h by build.py).
#ifndef KBM_TEXT_H
#define KBM_TEXT_H
#include "common.h"

/* Keyboard & mouse page rows (kbm_menu.c): input mode, mouse look, mouse look
   up/down, invert mouse Y, mouse sensitivity, keyboard steering speed, menu
   pointer, cursor size, map cursor speed. Key row labels (kbm_rebind.c). */
enum { KBM_TEXT_ROWS = 9, KBM_TEXT_ACTIONS = 33 };
enum { KBM_LABEL_PRESS_KEY, KBM_LABEL_NONE, KBM_LABEL_UNAVAILABLE, KBM_LABEL_OFF,
       KBM_LABEL_SAVE_FAILED, KBM_LABEL_IN_USE, KBM_LABEL_RESET, KBM_LABEL_DONE,
       KBM_LABEL_COUNT };

typedef struct {
    const char *code;           /* the game's language code: EN, GB, BR, DE, ... */
    const char *keyboard_mouse; /* tile caption and page title, in capitals (UTF-8) */
    const char *kbm_names[KBM_TEXT_ROWS], *kbm_descriptions[KBM_TEXT_ROWS];
    const wchar_t *kbm_modes[3]; /* Auto, Keyboard & mouse, Gamepad */
    const char *kbm_actions[KBM_TEXT_ACTIONS]; /* per KbmBinding (kbm_config.h) */
    const char *kbm_key_descriptions[2]; /* a driving key, a menu key */
    const char *kbm_reset, *kbm_reset_description;
    const wchar_t *kbm_labels[KBM_LABEL_COUNT];
    const wchar_t *on, *off;
} KbmText;

/* The texts of the game's current language (English when unknown or when
   `game_image` is NULL). */
const KbmText *kbm_text(unsigned char *game_image);
const KbmText *kbm_text_for(const char *code);
unsigned kbm_text_count(void);
const KbmText *kbm_text_at(unsigned index);
#endif
