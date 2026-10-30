// Keyboard & Mouse for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared helpers: fault-tolerant memory reads, text search, the log (Extra
// Options' FH2_ExtraOptions.log through the plugin context), the plugin's INI
// path and the game addresses several modules use.
#ifndef KBM_COMMON_H
#define KBM_COMMON_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#ifndef KBM_VERSION
#define KBM_VERSION "0.0.0"  // build.py passes mod/mod.json's version
#endif

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define PATH_CAPACITY 1024

// Forza Horizon 2 1.0.0.64 (forza_durango_release_final.exe), the only
// build Extra Options supports.
#define GAME_IMAGE_SIZE 0x3a18000u
enum {
    GAME_RVA_PLAYER_MANAGER = 0x34aba60,
    GAME_RVA_UI_MANAGER = 0x36e0ae8,
    GAME_RVA_CREATE_PIXEL_BUFFER = 0x165e600,
    GAME_RVA_QUEUE_BITMAP_UPDATE = 0x16e2dc0,
    GAME_RVA_DESTROY_UI_OBJECT = 0x257730,
};

void memory_copy(void *destination, const void *source, size_t size);
int memory_equal(const void *left, const void *right, size_t size);
// 0 when any byte cannot be read (access violation, guard page, unmapped).
int memory_read(const void *address, void *output, size_t size);
int memory_read_pointer(const void *address, void **value);
int memory_is_writable(const void *address, size_t size);
int float_is_finite(float value);
size_t text_length(const char *text);
const char *text_find(const char *text, size_t length, const char *needle);
// The only occurrence, or NULL (none or several).
const char *text_find_unique(const char *text, size_t length, const char *needle);
int text_equal_ignore_case(const char *left, const char *right);

// Lines go to Extra Options' log (FH2_ExtraOptions.log) as they are; each
// ends with "\r\n".
void log_set_host(void (*host)(const char *message));
void log_append(const char *message);
void log_append_value(const char *label, int value);
// printf-style; the line break is added.
void log_line(const char *format, ...);

// KeyboardMouse.ini beside the DLL (plugin.c sets it before anything runs).
void kbm_set_ini_path(const wchar_t *path);
const wchar_t *kbm_ini_path(void);
#endif
