// Performance for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common.h"

#include <cstring>

#include "fh2_guard.h"

namespace perf {

namespace {
void (*host_log)(const char *);
SRWLOCK log_lock = SRWLOCK_INIT;
} // namespace

void set_log(void (*host)(const char *)) {
    AcquireSRWLockExclusive(&log_lock);
    host_log = host;
    ReleaseSRWLockExclusive(&log_lock);
}

void say(const std::string &text) {
    std::string line = "PERFORMANCE: " + text + ".\r\n";
    AcquireSRWLockExclusive(&log_lock);
    if (host_log) host_log(line.c_str());
    ReleaseSRWLockExclusive(&log_lock);
    OutputDebugStringA(line.c_str());
}

bool safe_read(const void *address, void *output, size_t size) { return fh2_safe_read(address, output, size) != 0; }

bool bytes_equal(const void *address, const void *expected, size_t size) {
    return fh2_bytes_equal(address, expected, size) != 0;
}

bool is_code(const void *address) { return fh2_is_code(address) != 0; }

const char *find(const char *text, size_t length, const char *needle) {
    size_t n = strlen(needle);
    if (!text || !n || n > length) return nullptr;
    for (size_t i = 0; i + n <= length; ++i)
        if (text[i] == needle[0] && !memcmp(text + i, needle, n)) return text + i;
    return nullptr;
}

const char *find_in(const char *begin, const char *end, const char *needle) {
    return begin && end && begin <= end ? find(begin, (size_t)(end - begin), needle) : nullptr;
}

std::string narrow(const std::wstring &text) {
    if (text.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), &out[0], n, nullptr, nullptr);
    return out;
}

std::string hex(uint64_t value) {
    char text[24];
    wsprintfA(text, "0x%I64x", value);
    return text;
}

std::string module_name(const void *address) {
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH];
    if (!address ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)address, &module) ||
        !GetModuleFileNameW(module, path, MAX_PATH))
        return "?";
    const wchar_t *name = wcsrchr(path, L'\\');
    return narrow(name ? name + 1 : path);
}

} // namespace perf
