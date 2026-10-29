// Custom Radio for Forza Horizon 2 Extra Options: shared helpers.
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CUSTOM_RADIO_SUPPORT_H
#define CUSTOM_RADIO_SUPPORT_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define PATH_CAPACITY 1024

void memory_copy(void *output, const void *input, size_t size);
void memory_zero(void *output, size_t size);
int memory_equal(const void *a, const void *b, size_t size);
size_t text_length(const char *text);
int text_equal_ignore_case(const char *a, const char *b);
size_t wide_length(const wchar_t *text);
int wide_equal_ignore_case(const wchar_t *a, const wchar_t *b);
// Appends `text` to the NUL-terminated `output`; 0 (and a truncated result) if it does not fit.
int wide_append(wchar_t *output, size_t capacity, const wchar_t *text);

// Reads foreign (game) memory without faulting: 0 if any byte is unreadable.
int memory_read(const void *address, void *output, size_t size);
int memory_is_writable(const void *address, size_t size);
int memory_is_code(const void *address);

// The Extra Options log (FH2PluginContext::log_utf8). Every message is one
// complete line; the helpers build it first so threads do not interleave.
void log_bind(void (*sink)(const char *message));
void log_append(const char *line);  // includes its own CR LF
void log_append_value(const char *label, int value);
// Joins the parts (any may be NULL) and ends the line with CR LF if it has none.
void log_append_parts(const char *a, const char *b, const char *c, const char *d);
void log_append_text(const char *label, const char *text, const char *tail);
void log_append_wide(const char *label, const wchar_t *text);
#endif
