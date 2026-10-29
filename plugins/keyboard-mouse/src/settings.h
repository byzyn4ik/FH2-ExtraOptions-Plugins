// Keyboard & Mouse for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// KeyboardMouse.ini beside the DLL, read once at start (the in-game page
// writes [KeyboardMouse] keys back and applies them at once):
//   [KeyboardMouse]  Enabled (default 1), Mode, MouseLook, ..., one key per
//                    action (kbm_config.c); an unknown key, a bad value or a
//                    key bound twice in one group refuses the section and
//                    keyboard & mouse stays off (the log names the line).
//   [Controller]     PauseOnlyOnUnplug (default 1): the controller-missing
//                    pause needs a real unplug (controller_pause.c).
#ifndef KBM_SETTINGS_H
#define KBM_SETTINGS_H
#include "kbm_config.h"

typedef struct {
    KbmSettings kbm;
    int kbm_valid;            // 0: [KeyboardMouse] refused (kbm_problem says why)
    int pause_only_on_unplug;
    int controller_valid;     // 0: [Controller] refused (controller_problem says why)
    char kbm_problem[200], controller_problem[200];
} PluginSettings;

void settings_read(const wchar_t *ini_path, PluginSettings *settings);
#endif
