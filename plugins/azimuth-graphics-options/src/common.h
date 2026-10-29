// Azimuth Graphics Options for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Small helpers shared by the plugin's parts: guarded memory reads, text
// search in XAML, and the log (Extra Options' log plus the plugin's own).
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace azg {

// Reads `size` bytes at `address` without faulting; false if unreadable.
bool safe_read(const void *address, void *output, size_t size);
bool bytes_equal(const void *address, const void *expected, size_t size);
// True when `address` is committed, executable memory.
bool is_code(const void *address);

// First occurrence of `needle` in text[0, length), or nullptr.
const char *find(const char *text, size_t length, const char *needle);
// The same between two pointers.
const char *find_in(const char *begin, const char *end, const char *needle);

// One log line: "AZIMUTH GRAPHICS OPTIONS: <text>" in FH2_ExtraOptions.log and in
// AzimuthGraphicsOptions.log (when enabled).
void say(const std::string &text);
void set_log(void (*host)(const char *), const std::wstring &own_file);

std::string narrow(const std::wstring &text);
std::string hex(uint64_t value);

} // namespace azg
