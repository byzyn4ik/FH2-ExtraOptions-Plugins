// Language Support for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#include "fh2_guard.h"  // common/include: guarded reads

#ifndef LANG_SUPPORT_VERSION
#define LANG_SUPPORT_VERSION "0.0.0"  // build.py passes mod/mod.json's version
#endif
#define LANG_PATH 1024                   // wide characters
#define GAME_IMAGE_SIZE 0x3a18000u       // forza_durango_release_final.exe
#define EXTRA_OPTIONS_VERSION "0.4.4"    // the mod loader this plugin is built and tested for
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

// plugin.c
// One line in Extra Options' log: "LANG SUPPORT: <text>".
void say(const char *format, ...);

// pack.c ------------------------------------------------------------------

typedef struct {
    wchar_t folder[LANG_PATH];  // the pack's mod folder: <mods>\<name>
    wchar_t name[260];          // its folder name (as in loadorder.txt)
    wchar_t language[64];       // language.ini [Language] Name (default: the folder name)
    wchar_t slot[16];           // [Language] Slot: an Azimuth language code, e.g. ru-RU
    const wchar_t *archive;     // the slot's string table archive, e.g. RU
} LangPack;

// The game's string table archive (media\stripped\stringtables\<archive>.zip)
// of an Azimuth language code ("ru-RU" -> "RU"); NULL when the game has none.
const wchar_t *slot_archive(const wchar_t *slot);
// The pack in <mods>: the folder `wanted` (empty or NULL: the one mod folder
// that holds a language.ini). NULL when found, else why not.
const char *pack_find(const wchar_t *mods, const wchar_t *wanted, LangPack *pack);
// The folder in mods\loadorder.txt: 1 listed, 0 commented out ("# name"),
// -1 not listed or no file (Extra Options lists new folders itself).
int pack_load_order(const wchar_t *mods, const wchar_t *name);
// The language Azimuth runs the game in: azimuth.ini [launcher] game names
// the title, [title-<game>] language its language (Azimuth 1.0.0 and later;
// [launcher] language before). "system" or none: the Windows locale.
// 1 when the INI names a language, 0 when the locale was used.
int launcher_language(const wchar_t *azimuth_ini, wchar_t *language, size_t capacity);

typedef enum { AZIMUTH_UNKNOWN, AZIMUTH_102, AZIMUTH_104 } AzimuthBuild;
// azimuth.exe 1.0.4 (TimeDateStamp 0x6ae393eb) or 1.0.2 (0x6ae05dd1), by its PE header.
AzimuthBuild azimuth_build(const unsigned char *base);
const char *azimuth_name(AzimuthBuild build);
// The process' main image (a test build can set another).
const unsigned char *azimuth_host(void);

// voice_subtitles.c -------------------------------------------------------

// NULL when the game image has the five speech hook sites and SetText as
// inspected (forza_durango_release_final.exe, SHA-256 3ec04c61...), else what differs.
const char *voice_check(const unsigned char *game, uint32_t size);
// Reads VoiceSubtitles.str (loose <stringtables>\<archive>\, else a stored
// entry of <stringtables>\<archive>.zip), places the hook code and table near
// the game image and hooks the five sites. Returns the number of lines; 0
// (with *why) leaves the game as it was.
unsigned voice_install(unsigned char *game, const wchar_t *stringtables, const wchar_t *archive, const char **why);
// Where the hook code went (NULL before voice_install succeeded).
unsigned char *voice_cave(void);

#ifdef LANG_SUPPORT_TEST
void azimuth_set_test_host(const unsigned char *base);

unsigned voice_test_table(const wchar_t *stringtables, const wchar_t *archive, const char **why);
int voice_test_entry(unsigned index, unsigned long long *hash, const wchar_t **text, unsigned *duration);
#endif
