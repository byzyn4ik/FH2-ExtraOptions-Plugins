// Keyboard & Mouse for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common.h"

#include <stdarg.h>
#include <stdio.h>

static void (*host_log)(const char *message);
static wchar_t ini_path[PATH_CAPACITY];

void memory_copy(void *destination, const void *source, size_t size) {
    volatile unsigned char *to = (volatile unsigned char *)destination;
    const unsigned char *from = (const unsigned char *)source;
    while (size--) *to++ = *from++;
}

int memory_equal(const void *left, const void *right, size_t size) {
    const unsigned char *a = left, *b = right;
    while (size--)
        if (*a++ != *b++) return 0;
    return 1;
}

size_t text_length(const char *text) {
    size_t length = 0;
    while (text[length]) ++length;
    return length;
}

int text_equal_ignore_case(const char *left, const char *right) {
    while (*left && *right) {
        char a = *left++, b = *right++;
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return 0;
    }
    return *left == *right;
}

const char *text_find(const char *text, size_t length, const char *needle) {
    size_t needle_length = text_length(needle);
    if (needle_length > length) return NULL;
    if (!needle_length) return text;
    for (size_t offset = 0; offset <= length - needle_length; ++offset)
        if (text[offset] == needle[0] && memory_equal(text + offset, needle, needle_length)) return text + offset;
    return NULL;
}

const char *text_find_unique(const char *text, size_t length, const char *needle) {
    if (!*needle) return NULL;
    const char *found = text_find(text, length, needle);
    if (!found || text_find(found + 1, length - (size_t)(found + 1 - text), needle)) return NULL;
    return found;
}

// A guard page hit while reading is put back, so the owner still sees it.
static int read_fault(EXCEPTION_POINTERS *exception) {
    const EXCEPTION_RECORD *record = exception->ExceptionRecord;
    DWORD code = record->ExceptionCode;
    if (code == EXCEPTION_GUARD_PAGE && record->NumberParameters >= 2) {
        void *address = (void *)record->ExceptionInformation[1];
        MEMORY_BASIC_INFORMATION region;
        DWORD previous;
        if (VirtualQuery(address, &region, sizeof(region)) == sizeof(region))
            VirtualProtect(address, 1, region.Protect | PAGE_GUARD, &previous);
    }
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR || code == EXCEPTION_GUARD_PAGE
               ? EXCEPTION_EXECUTE_HANDLER
               : EXCEPTION_CONTINUE_SEARCH;
}

int memory_read(const void *address, void *output, size_t size) {
    if (!address || !output || size > UINTPTR_MAX - (uintptr_t)address) return 0;
    __try {
        memory_copy(output, address, size);
        return 1;
    } __except (read_fault(GetExceptionInformation())) {
        return 0;
    }
}

int memory_read_pointer(const void *address, void **value) {
    return memory_read(address, value, sizeof(*value)) && *value;
}

int float_is_finite(float value) {
    uint32_t bits;
    memory_copy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000) != 0x7f800000;
}

int memory_is_writable(const void *address, size_t size) {
    uintptr_t start = (uintptr_t)address, end = start + size;
    if (!start || end < start) return 0;
    while (start < end) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((void *)start, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return 0;
        DWORD protection = info.Protect & 0xff;
        if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY && protection != PAGE_EXECUTE_READWRITE &&
            protection != PAGE_EXECUTE_WRITECOPY)
            return 0;
        uintptr_t next = (uintptr_t)info.BaseAddress + info.RegionSize;
        if (next <= start) return 0;
        start = next;
    }
    return 1;
}

void log_set_host(void (*host)(const char *message)) { host_log = host; }

void log_append(const char *message) {
    if (host_log && message) host_log(message);
}

void log_append_value(const char *label, int value) {
    char line[512];
    snprintf(line, sizeof(line), "%s%d\r\n", label, value);
    log_append(line);
}

void log_line(const char *format, ...) {
    char line[768];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(line, sizeof(line) - 2, format, arguments);
    va_end(arguments);
    if (length < 0) return;
    if ((size_t)length > sizeof(line) - 3) length = (int)sizeof(line) - 3;
    line[length] = '\r';
    line[length + 1] = '\n';
    line[length + 2] = 0;
    log_append(line);
}

void kbm_set_ini_path(const wchar_t *path) {
    size_t length = path ? wcslen(path) : 0;
    if (length >= ARRAY_COUNT(ini_path)) length = 0;
    memcpy(ini_path, path ? path : L"", length * sizeof(wchar_t));
    ini_path[length] = 0;
}

const wchar_t *kbm_ini_path(void) { return ini_path; }

#ifdef KBM_TEST
__declspec(dllexport) void KbmTestSetIni(const wchar_t *path) { kbm_set_ini_path(path); }
// Log lines go to `host` (the tests collect them).
__declspec(dllexport) void KbmTestSetLog(void (*host)(const char *message)) { log_set_host(host); }
#endif
