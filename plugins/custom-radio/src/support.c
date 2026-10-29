// Custom Radio for Forza Horizon 2 Extra Options: shared helpers.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "support.h"

#include "fh2_guard.h"  // common/include

void memory_copy(void *output, const void *input, size_t size) {
    unsigned char *to = output;
    const unsigned char *from = input;
    while (size--) {
        *to++ = *from++;
    }
}

void memory_zero(void *output, size_t size) {
    volatile unsigned char *to = output;
    while (size--) {
        *to++ = 0;
    }
}

int memory_equal(const void *a, const void *b, size_t size) {
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < size; ++i) {
        if (x[i] != y[i]) {
            return 0;
        }
    }
    return 1;
}

size_t text_length(const char *text) {
    size_t length = 0;
    while (text[length]) {
        ++length;
    }
    return length;
}

static int lower_ascii(int character) {
    return character >= 'A' && character <= 'Z' ? character + 'a' - 'A' : character;
}

int text_equal_ignore_case(const char *a, const char *b) {
    for (;; ++a, ++b) {
        if (lower_ascii((unsigned char)*a) != lower_ascii((unsigned char)*b)) {
            return 0;
        }
        if (!*a) {
            return 1;
        }
    }
}

size_t wide_length(const wchar_t *text) {
    size_t length = 0;
    while (text[length]) {
        ++length;
    }
    return length;
}

int wide_equal_ignore_case(const wchar_t *a, const wchar_t *b) {
    for (;; ++a, ++b) {
        int x = lower_ascii(*a), y = lower_ascii(*b);
        if (x != y) {
            return 0;
        }
        if (!x) {
            return 1;
        }
    }
}

int wide_append(wchar_t *output, size_t capacity, const wchar_t *text) {
    size_t length = wide_length(output);
    for (size_t index = 0;; ++index) {
        if (length >= capacity) {
            output[capacity - 1] = 0;
            return 0;
        }
        output[length++] = text[index];
        if (!text[index]) {
            return 1;
        }
    }
}

int memory_read(const void *address, void *output, size_t size) {
    if (!address || !output || size > UINTPTR_MAX - (uintptr_t)address) {
        return 0;
    }
    if (!size) {
        return 1;
    }
    // No exception on unmapped memory, unlike a plain copy.
    return fh2_safe_read(address, output, size);
}

int memory_is_writable(const void *address, size_t size) {
    uintptr_t start = (uintptr_t)address, end = start + size;
    if (!start || end < start) {
        return 0;
    }
    while (start < end) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((void *)start, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            return 0;
        }
        DWORD protection = info.Protect & 0xff;
        if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
            protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) {
            return 0;
        }
        uintptr_t next = (uintptr_t)info.BaseAddress + info.RegionSize;
        if (next <= start) {
            return 0;
        }
        start = next;
    }
    return 1;
}

int memory_is_code(const void *address) {
    return fh2_is_code(address);
}

// ------------------------------------------------------------------ log

static void (*volatile log_sink)(const char *message);

void log_bind(void (*sink)(const char *message)) {
    log_sink = sink;
}

void log_append(const char *line) {
    void (*sink)(const char *) = log_sink;
    if (sink && line) {
        sink(line);
    }
}

typedef struct {
    char text[1024];
    size_t length;
} Line;

static void line_put(Line *line, const char *text) {
    while (text && *text && line->length + 3 < sizeof(line->text)) {
        line->text[line->length++] = *text++;
    }
    line->text[line->length] = 0;
}

void log_append_value(const char *label, int value) {
    Line line = {{0}, 0};
    char digits[16], *cursor = digits + sizeof(digits) - 1;
    unsigned magnitude = value < 0 ? 0u - (unsigned)value : (unsigned)value;
    *cursor = 0;
    do {
        *--cursor = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude);
    if (value < 0) {
        *--cursor = '-';
    }
    line_put(&line, label);
    line_put(&line, cursor);
    line_put(&line, "\r\n");
    log_append(line.text);
}

void log_append_parts(const char *a, const char *b, const char *c, const char *d) {
    Line line = {{0}, 0};
    line_put(&line, a);
    line_put(&line, b);
    line_put(&line, c);
    line_put(&line, d);
    if (!line.length || line.text[line.length - 1] != '\n') {
        line.text[line.length++] = '\r';
        line.text[line.length++] = '\n';
        line.text[line.length] = 0;
    }
    log_append(line.text);
}

void log_append_text(const char *label, const char *text, const char *tail) {
    log_append_parts(label, text, tail, NULL);
}

void log_append_wide(const char *label, const wchar_t *text) {
    char utf8[MAX_PATH * 3 + 4];
    utf8[0] = 0;
    if (text && WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, (int)sizeof(utf8), NULL, NULL) <= 0) {
        utf8[0] = 0;
    }
    log_append_text(label, utf8, NULL);
}
