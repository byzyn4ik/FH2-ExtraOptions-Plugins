// Keyboard & Mouse for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Extra Options 0.4.4's own keyboard bindings (FH2_ExtraOptions.ini
// [Bindings] *Keyboard; it polls them with GetAsyncKeyState, whatever this
// plugin does). Its defaults: ToggleUI F1, Autodrive F2, Headlights F3,
// Roof F4, JumpToManufacturer and RimJumpToManufacturer Tab (car lists),
// CancelAutodrive Escape (None in the INI it ships). A key of ours that one
// of them also uses, in the same situation (driving, or menus), would do both
// things: such keys are reported at start and refused on the in-game page.
#ifndef KBM_EO_BINDINGS_H
#define KBM_EO_BINDINGS_H
#include "common.h"
#include "kbm_config.h"

// Reads FH2_ExtraOptions.ini (NULL: Extra Options' folder, beside its
// winmm.dll); without the file Extra Options' defaults are assumed.
void eo_bindings_load(const wchar_t *eo_ini);
// The Extra Options action whose keyboard binding is `scan_code` (a
// physical key, as in kbm_config.h) where `binding` acts, or NULL.
const char *eo_bindings_conflict(unsigned binding, unsigned scan_code);
// Logs one line per clash in `settings`; returns how many.
unsigned eo_bindings_report(const KbmSettings *settings);
#endif
