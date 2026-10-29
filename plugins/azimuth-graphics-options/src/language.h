// Azimuth Graphics Options for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The language of the plugin's captions:
//   - Polish while the game's text is Polish (its text tables' code "PL");
//   - Ukrainian while the game's text is the ru-RU slot ("RU") and that slot
//     holds a custom Ukrainian language pack, as the Extra Options fork's
//     custom languages lay it out: FH2_ExtraOptions.ini [Language] Custom=<id>
//     beside the launcher, and <game>\Mount\languages\<id>\language.ini with
//     [Language] Slot=ru-RU and Ui=uk (Ui defaults to the id);
//   - English otherwise.
// The plugin's INI can force one: [Captions] Language=en, pl or uk.
#pragma once
#include <string>

#include "rows.h"

namespace azg {
namespace language {

enum Choice { kAuto, kForceEnglish, kForcePolish, kForceUkrainian };

// "auto", "en", "pl", "uk"; anything else is kAuto.
Choice parse(const std::wstring &text);

// Looks for a Ukrainian pack in the ru-RU slot (once, at start) and logs it.
// `launcher`: the folder of the Azimuth executable; `mount`: the game's Mount
// folder (empty when unknown).
void configure(Choice choice, const std::wstring &launcher, const std::wstring &mount);
bool ukrainian_pack();

// The language for the game's current text language.
Language current(const unsigned char *game_image);
// The text tables' language code ("PL", "RU", ...), empty when unreadable.
std::string game_code(const unsigned char *game_image);

#ifdef AZG_TEST_EXPORTS
void test_set(Choice choice, bool pack);
#endif

} // namespace language
} // namespace azg
