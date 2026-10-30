// Photo Export for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "fh2_saves.h"  // common/include: Azimuth's save layout, SHA-256, folders

#ifndef PHOTO_EXPORT_VERSION
#define PHOTO_EXPORT_VERSION "0.0.0"  // build.py passes mod/mod.json's version
#endif

// plugin.c: one line in Extra Options' log, "PHOTO EXPORT: <text>".
void say(const char *format, ...) __attribute__((format(printf, 1, 2)));

// photos.c
// The folders: Azimuth's saves (read only), the JPG folder, the list of
// photos already exported (beside the plugin's INI; "" = none kept).
// Resets what the scans remember. 1 = ok.
int photos_configure(const wchar_t *saves, const wchar_t *folder, const wchar_t *index);
// Deletes temporary files of an export cut short, reads the list of exported photos.
void photos_prepare(void);
// One pass over <saves>\player-*\Photo_*; the number of JPGs written.
int photos_scan(void);
// "first scan: ..." line.
void photos_summary(void);
