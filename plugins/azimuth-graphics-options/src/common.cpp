// Azimuth Graphics Options for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common.h"

#include <cstring>

#include "fh2_guard.h"

namespace azg {

namespace {

void (*host_log)(const char *);
std::wstring log_file;
CRITICAL_SECTION log_lock;
INIT_ONCE log_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK init_lock(PINIT_ONCE, PVOID, PVOID *) {
    InitializeCriticalSection(&log_lock);
    return TRUE;
}

} // namespace

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

void set_log(void (*host)(const char *), const std::wstring &own_file) {
    InitOnceExecuteOnce(&log_once, init_lock, nullptr, nullptr);
    host_log = host;
    log_file = own_file;
    if (!log_file.empty()) DeleteFileW(log_file.c_str());
}

void say(const std::string &text) {
    InitOnceExecuteOnce(&log_once, init_lock, nullptr, nullptr);
    std::string line = "AZIMUTH GRAPHICS OPTIONS: " + text + "\r\n";
    EnterCriticalSection(&log_lock);
    if (host_log) host_log(line.c_str());
    if (!log_file.empty()) {
        HANDLE file = CreateFileW(log_file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            SYSTEMTIME now;
            GetLocalTime(&now);
            char stamp[32];
            int n = wsprintfA(stamp, "%02u:%02u:%02u.%03u ", now.wHour, now.wMinute, now.wSecond,
                              now.wMilliseconds);
            DWORD written;
            WriteFile(file, stamp, (DWORD)n, &written, nullptr);
            WriteFile(file, text.data(), (DWORD)text.size(), &written, nullptr);
            WriteFile(file, "\r\n", 2, &written, nullptr);
            CloseHandle(file);
        }
    }
    LeaveCriticalSection(&log_lock);
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

} // namespace azg
