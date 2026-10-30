// Saves in Documents (mirror) for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "fh2_saves.h"  // common/include: Azimuth's save layout, SHA-256, folders

#ifndef DOCUMENTS_SAVES_VERSION
#define DOCUMENTS_SAVES_VERSION "0.0.0"  // build.py passes mod/mod.json's version
#endif

// plugin.c: one line in Extra Options' log, "SAVES IN DOCUMENTS: <text>".
void say(const char *format, ...) __attribute__((format(printf, 1, 2)));

// mirror.c
// source: Azimuth's saves (only read). mirror: the copy in Documents. keep: 0
// keeps every revision ever copied; n >= 2 keeps the newest n of a container.
// Resets what the passes remember. 1 = ok.
int mirror_configure(const wchar_t *source, const wchar_t *mirror, unsigned keep);
// Finishes or removes copies cut short (the game closed during one), warns
// when Documents holds newer saves than the game folder.
void mirror_prepare(void);
// One pass: every committed revision of the game folder's saves not yet in
// the mirror is copied and verified. The number of revisions copied.
int mirror_pass(void);
void mirror_summary(void);

#ifdef DOCUMENTS_SAVES_TEST
// Called at the copy's steps (0: after each chunk of a blob, 1: blobs and
// commit written, 2: before the rename into place); returning 1 stops the copy
// there as if the game had closed (nothing is cleaned up).
typedef int (*MirrorFault)(int step, const wchar_t *path);
void mirror_test_hooks(MirrorFault fault, unsigned chunk, unsigned long long free_space);
#endif
