// Keyboard & Mouse for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef KBM_CONTROLLER_PAUSE_H
#define KBM_CONTROLLER_PAUSE_H
#include "common.h"
/* The game's "controller lost: pause" rule. [Controller] PauseOnlyOnUnplug=1:
   it fires only when a pad that was attached goes away, not over and over
   while no pad is paired. Keyboard & mouse also installs it (unplug_only 0
   keeps the game's rule otherwise) to skip the pause while it is the active
   source with no pad. Installing twice only updates the options. */
int controller_pause_install(unsigned char *game_image, int unplug_only);
/* Keyboard & mouse: while the callback returns 1 the pause is never fired. */
void controller_pause_set_suppressed(int (*callback)(void));
#endif
