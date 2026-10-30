// Xtra Features for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared declarations: the log, guarded reads and byte checks of the game
// image, the settings (settings.c), the code patches (patch.c) and the four
// features (map_zoom.c, thumbnails.c, photo_scan.c, exit_game.c).
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#include "fh2_guard.h"  // common/include: guarded reads, chained hooks

#ifndef XTRA_VERSION
#define XTRA_VERSION "0.0.0"  // build.py passes mod/mod.json's version
#endif
#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

// Forza Horizon 2 1.0.0.64 (forza_durango_release_final.exe), the only
// build Extra Options supports.
#define GAME_IMAGE_SIZE 0x3a18000u
// The end of the game's .text section (code lies in [0x1000, TEXT_END)).
#define GAME_TEXT_END 0x1dcc000u

// ------------------------------------------------------------ plugin.c
// One line in Extra Options' log: "XTRA FEATURES: <text>" (printf-style; the
// line break is added).
void say(const char *format, ...);

// ------------------------------------------------------------ guards
typedef struct {
    uint32_t rva;
    const char *name;
    unsigned length;
    const unsigned char *bytes;
} Code;
#define CODE(rva, name, ...) \
    {rva, name, sizeof((const unsigned char[]){__VA_ARGS__}), (const unsigned char[]){__VA_ARGS__}}

// {slot rva, the game function it holds}: a pointer in the game's tables.
typedef struct {
    uint32_t slot, target;
    const char *name;
} Slot;

// NULL when every block is as inspected, else "<name> at 0x<rva>" in `why`.
const char *check_code(const unsigned char *game, const Code *code, unsigned count, char *why, size_t size);
const char *check_slots(const unsigned char *game, const Slot *slots, unsigned count, char *why, size_t size);
void *read_pointer(const void *address);
int in_game(const unsigned char *game, const void *address);
int in_game_text(const unsigned char *game, const void *address);
// "XtraFeatures.dll+0x1234" (a module) or the address, for log lines.
void describe(const void *address, char *out, size_t size);

// ------------------------------------------------------------ settings.c
enum { PERCENT_MIN = 25, PERCENT_MAX = 400 };
typedef struct {
    int map_zoom, thumbnails, photo_scan, exit_game;  // [XtraFeatures]
    int start, max, island_start, island_max;      // [MapZoom], percent
    int scale;                                     // [GarageThumbnails] CarPreviewScale 1-4
    char problem[4][200];                          // why a feature was turned off ("" none)
} XtraSettings;
enum { FEATURE_MAP_ZOOM, FEATURE_THUMBNAILS, FEATURE_PHOTO_SCAN, FEATURE_EXIT_GAME, FEATURE_COUNT };

void settings_defaults(XtraSettings *settings);
// Reads the INI (missing file or keys: the defaults). A bad value turns its
// feature off and says why in problem[feature].
void settings_read(const wchar_t *ini_path, XtraSettings *settings);

// ------------------------------------------------------------ patch.c
// Takes over the "call rel32" at `site`, whatever its alignment: the call is
// pointed at a jump to `handler` placed within 2 GB of it, by swapping its
// displacement inside the smallest aligned 4-, 8- or 16-byte block holding it
// (one compare-exchange). The call must go to `native` or to another
// module's code outside the game image (then chained: *previous is what it
// called). NULL when done, else why not (nothing changed).
const char *chain_call(unsigned char *site, const void *native, const unsigned char *game, void *handler,
                       void **previous);
// The same for an 8-byte function pointer (fh2_chain_slot).
const char *chain_slot(void **slot, const void *native, const unsigned char *game, void *handler, void **previous);
// Replaces `length` bytes (at most 64) that must still be `expected` with
// `bytes`, with every other thread of the process suspended and none of them
// executing inside them. NULL when done, else why not.
const char *write_code(unsigned char *at, const unsigned char *expected, const unsigned char *bytes, size_t length);

// ------------------------------------------------------------ features
// Each check: NULL when the game is as inspected, else what differs. Each
// install logs what it did; 1 when installed.
const char *map_zoom_check(const unsigned char *game, char *why, size_t size);
int map_zoom_install(unsigned char *game, const XtraSettings *settings);

const char *thumbnails_check(const unsigned char *game, char *why, size_t size);
int thumbnails_install(unsigned char *game, const XtraSettings *settings);

const char *photo_scan_check(const unsigned char *game, char *why, size_t size);
int photo_scan_install(unsigned char *game);

const char *exit_game_check(const unsigned char *game, char *why, size_t size);
int exit_game_install(unsigned char *game);
