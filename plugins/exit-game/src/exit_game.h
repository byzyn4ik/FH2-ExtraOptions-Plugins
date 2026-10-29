// Exit Game for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#include "fh2_guard.h"  // common/include: guarded reads, chained hooks

#ifndef EXIT_GAME_VERSION
#define EXIT_GAME_VERSION "0.0.0"  // build.py passes mod/mod.json's version
#endif
// Azimuth's game window class (azimuth.exe 1.0.2 and 1.0.4: RegisterClassExW
// and CreateWindowExW). Its window procedure answers WM_CLOSE with
// DestroyWindow, as for the title bar's close button.
#define AZIMUTH_WINDOW_CLASS L"Azimuth.Game.Win32"

// plugin.c
// One line in Extra Options' log: "EXIT GAME: <text>".
void say(const char *text);
// The caption in the game's current language (English when unknown).
const wchar_t *exit_caption(const unsigned char *game);
const wchar_t *caption_for(const char *code);
// The tile's action: closes Azimuth's game window of this process.
void choose_exit_game(void *page);

// hooks.c
// NULL when the game image is as inspected and both sites can be chained;
// else what differs. *init_target / *resource_target (when given) describe
// what the sites call now.
const char *hooks_check(const unsigned char *game, uint32_t size, const char **init_target,
                        const char **resource_target);
// Chains the pause page's init (vtable slot) and the XAML resource call;
// 0 (logged) when it cannot.
int hooks_install(unsigned char *game);

