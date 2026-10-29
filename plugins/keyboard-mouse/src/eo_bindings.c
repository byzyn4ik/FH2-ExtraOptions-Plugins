// Keyboard & Mouse for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eo_bindings.h"

#include <stdio.h>
#include <string.h>

enum { DRIVING = 1, MENUS = 2 };

// Extra Options 0.4.4 features/bindings.c: actions, their default keys
// (virtual-key codes) and where they act.
static struct {
    const char *action;
    unsigned key, where;
} actions[] = {
    {"Autodrive", VK_F2, DRIVING},
    {"CancelAutodrive", VK_ESCAPE, DRIVING},
    {"Headlights", 0, DRIVING},
    {"Roof", 0, DRIVING},
    {"SignalLeft", 0, DRIVING},
    {"SignalRight", 0, DRIVING},
    {"Hazards", 0, DRIVING},
    {"PhotoMode", 0, DRIVING},
    {"ShowroomPhoto", 0, MENUS},
    {"ReturnToGarage", 0, MENUS},
    {"JumpToManufacturer", VK_TAB, MENUS},
    {"ToggleUI", VK_F1, DRIVING | MENUS},
    {"RimJumpToManufacturer", VK_TAB, MENUS},
    {"CarMeetPhoto", 0, MENUS},
};
static unsigned keys[ARRAY_COUNT(actions)];
static int loaded;

static const struct {
    const char *name;
    unsigned key;
} names[] = {{"None", 0},           {"Escape", VK_ESCAPE}, {"Esc", VK_ESCAPE},     {"Enter", VK_RETURN},
             {"Space", VK_SPACE},   {"Tab", VK_TAB},       {"Backspace", VK_BACK}, {"Insert", VK_INSERT},
             {"Delete", VK_DELETE}, {"Home", VK_HOME},     {"End", VK_END},        {"PageUp", VK_PRIOR},
             {"PageDown", VK_NEXT}, {"Up", VK_UP},         {"Down", VK_DOWN},      {"Left", VK_LEFT},
             {"Right", VK_RIGHT},   {"Shift", VK_SHIFT},   {"Ctrl", VK_CONTROL},   {"Alt", VK_MENU}};

// As Extra Options parses a keyboard binding; 0 = none or not understood.
static unsigned key_value(const char *text) {
    size_t length = strlen(text);
    for (unsigned i = 0; i < ARRAY_COUNT(names); ++i)
        if (text_equal_ignore_case(text, names[i].name)) return names[i].key;
    if (length == 1) {
        unsigned ch = (unsigned char)text[0];
        if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) return ch;
    }
    if (length >= 2 && length <= 3 && (text[0] == 'F' || text[0] == 'f')) {
        unsigned number = 0;
        for (size_t i = 1; i < length; ++i) {
            if (text[i] < '0' || text[i] > '9') return 0;
            number = number * 10 + (unsigned)(text[i] - '0');
        }
        return number >= 1 && number <= 24 ? VK_F1 + number - 1 : 0;
    }
    if (length >= 3 && length <= 4 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        unsigned number = 0;
        for (size_t i = 2; i < length; ++i) {
            unsigned ch = (unsigned char)text[i], digit;
            if (ch >= '0' && ch <= '9') digit = ch - '0';
            else if (ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10;
            else return 0;
            number = number * 16 + digit;
        }
        return number < 256 ? number : 0;
    }
    return 0;
}

static int default_path(wchar_t *path, size_t capacity) {
    HMODULE module = GetModuleHandleW(L"winmm.dll");
    DWORD length = GetModuleFileNameW(module ? module : NULL, path, (DWORD)capacity);
    wchar_t *slash = length && length < capacity ? wcsrchr(path, L'\\') : NULL;
    if (!slash || (size_t)(slash - path) + 22 >= capacity) return 0;
    wcscpy(slash + 1, L"FH2_ExtraOptions.ini");
    return 1;
}

void eo_bindings_load(const wchar_t *eo_ini) {
    wchar_t path[MAX_PATH * 2];
    int have = 0;
    if (eo_ini && wcslen(eo_ini) < ARRAY_COUNT(path)) {
        wcscpy(path, eo_ini);
        have = 1;
    } else if (!eo_ini) {
        have = default_path(path, ARRAY_COUNT(path));
    }
    int exists = have && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    for (unsigned i = 0; i < ARRAY_COUNT(actions); ++i) {
        keys[i] = actions[i].key;  // Extra Options' default (its INI ships Headlights F3, Roof F4)
        if (!exists) continue;
        wchar_t name[64], value[64];
        char narrow[64];
        swprintf(name, ARRAY_COUNT(name), L"%hsKeyboard", actions[i].action);
        // Extra Options keeps its own default for a missing key.
        if (!GetPrivateProfileStringW(L"Bindings", name, L"\x1", value, ARRAY_COUNT(value), path) ||
            value[0] == 1) {
            keys[i] = actions[i].key;
            continue;
        }
        for (wchar_t *c = value; *c; ++c)
            if (*c == L';' || *c == L'#') *c = 0;
        size_t length = wcslen(value);
        while (length && (value[length - 1] == L' ' || value[length - 1] == L'\t')) value[--length] = 0;
        keys[i] = WideCharToMultiByte(CP_UTF8, 0, value, -1, narrow, sizeof(narrow), NULL, NULL) ? key_value(narrow)
                                                                                                  : 0;
    }
    loaded = 1;
}

// The virtual key Extra Options' GetAsyncKeyState sees for a physical key.
static unsigned virtual_key(unsigned scan_code) {
    unsigned scan = (scan_code & 0x7f) | (scan_code & 0x80 ? 0xe000 : 0);
    unsigned vk = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX);
    return vk;
}

static int same_key(unsigned eo, unsigned vk) {
    if (!eo || !vk) return 0;
    if (eo == vk) return 1;
    if (eo == VK_SHIFT) return vk == VK_LSHIFT || vk == VK_RSHIFT;
    if (eo == VK_CONTROL) return vk == VK_LCONTROL || vk == VK_RCONTROL;
    if (eo == VK_MENU) return vk == VK_LMENU || vk == VK_RMENU;
    return 0;
}

const char *eo_bindings_conflict(unsigned binding, unsigned scan_code) {
    if (!loaded) eo_bindings_load(NULL);
    unsigned where = binding < KBM_FIRST_MENU_BINDING ? DRIVING : MENUS;
    unsigned vk = virtual_key(scan_code);
    for (unsigned i = 0; i < ARRAY_COUNT(actions); ++i)
        if ((actions[i].where & where) && same_key(keys[i], vk)) return actions[i].action;
    return NULL;
}

unsigned eo_bindings_report(const KbmSettings *settings) {
    unsigned found = 0;
    for (unsigned binding = 0; binding < KBM_BINDING_COUNT; ++binding)
        for (unsigned k = 0; k < KBM_KEYS_PER_BINDING; ++k) {
            unsigned code = settings->keys[binding][k];
            const char *action = code ? eo_bindings_conflict(binding, code) : NULL;
            if (!action) continue;
            ++found;
            log_line("KBM: warning: %s=%s is also Extra Options' %sKeyboard ([Bindings] in FH2_ExtraOptions.ini); "
                     "that key does both. Choose another key for one of them.",
                     kbm_config_binding_name(binding), kbm_config_key_name(code), action);
        }
    return found;
}

#ifdef KBM_TEST
__declspec(dllexport) const char *KbmTestEoConflict(const wchar_t *eo_ini, unsigned binding, unsigned scan_code) {
    eo_bindings_load(eo_ini);
    return eo_bindings_conflict(binding, scan_code);
}
#endif
