// Custom Radio for Forza Horizon 2 Extra Options: CustomRadio.ini and its keys.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.h"

static const char *const action_names[KEY_COUNT] = {"RadioStation", "RadioNext", "RadioRestart"};
static const wchar_t *const keyboard_keys[KEY_COUNT] = {L"RadioStationKeyboard", L"RadioNextKeyboard",
                                                        L"RadioRestartKeyboard"};
static const wchar_t *const xbox_keys[KEY_COUNT] = {L"RadioStationXbox", L"RadioNextXbox",
                                                    L"RadioRestartXbox"};
static const unsigned default_keyboard[KEY_COUNT] = {VK_F9, VK_F7, VK_F6};

const char *config_action_name(unsigned action) {
    return action < KEY_COUNT ? action_names[action] : "?";
}

typedef struct {
    const wchar_t *name;
    unsigned value;
} InputName;

static const InputName key_names[] = {
    {L"None", 0},          {L"Escape", VK_ESCAPE}, {L"Esc", VK_ESCAPE},     {L"Enter", VK_RETURN},
    {L"Space", VK_SPACE},  {L"Tab", VK_TAB},       {L"Backspace", VK_BACK}, {L"Insert", VK_INSERT},
    {L"Delete", VK_DELETE}, {L"Home", VK_HOME},    {L"End", VK_END},        {L"PageUp", VK_PRIOR},
    {L"PageDown", VK_NEXT}, {L"Up", VK_UP},        {L"Down", VK_DOWN},      {L"Left", VK_LEFT},
    {L"Right", VK_RIGHT},  {L"Shift", VK_SHIFT},   {L"Ctrl", VK_CONTROL},   {L"Alt", VK_MENU}};

// XInput's wButtons bits, as FH2PluginVehicleFrame::buttons carries them.
static const InputName button_names[] = {
    {L"DPadUp", 0x1},  {L"DPadDown", 0x2}, {L"DPadLeft", 0x4},    {L"DPadRight", 0x8},
    {L"Menu", 0x10},   {L"Start", 0x10},   {L"View", 0x20},       {L"Back", 0x20},
    {L"LeftStick", 0x40}, {L"LS", 0x40},   {L"RightStick", 0x80}, {L"RS", 0x80},
    {L"LB", 0x100},    {L"RB", 0x200},     {L"A", 0x1000},        {L"B", 0x2000},
    {L"X", 0x4000},    {L"Y", 0x8000}};

static void trim(wchar_t *text) {
    size_t start = 0, length = wide_length(text);
    while (text[start] == L' ' || text[start] == L'\t') {
        ++start;
    }
    while (length > start && (text[length - 1] == L' ' || text[length - 1] == L'\t')) {
        --length;
    }
    for (size_t i = 0; i < length - start; ++i) {
        text[i] = text[start + i];
    }
    text[length - start] = 0;
}

static int parse_number(const wchar_t *text, unsigned limit, unsigned *value) {
    unsigned result = 0, base = 10;
    if (text[0] == L'0' && (text[1] == L'x' || text[1] == L'X')) {
        base = 16;
        text += 2;
    }
    if (!*text) {
        return 0;
    }
    for (; *text; ++text) {
        unsigned digit;
        if (*text >= L'0' && *text <= L'9') {
            digit = (unsigned)(*text - L'0');
        } else if (base == 16 && *text >= L'a' && *text <= L'f') {
            digit = (unsigned)(*text - L'a' + 10);
        } else if (base == 16 && *text >= L'A' && *text <= L'F') {
            digit = (unsigned)(*text - L'A' + 10);
        } else {
            return 0;
        }
        result = result * base + digit;
        if (result > limit) {
            return 0;
        }
    }
    *value = result;
    return 1;
}

int config_parse_keyboard(const wchar_t *text, unsigned *key) {
    wchar_t value[32];
    size_t length = wide_length(text);
    if (length >= ARRAY_COUNT(value)) {
        return 0;
    }
    memory_copy(value, text, (length + 1) * sizeof(wchar_t));
    trim(value);
    for (size_t i = 0; i < ARRAY_COUNT(key_names); ++i) {
        if (wide_equal_ignore_case(value, key_names[i].name)) {
            *key = key_names[i].value;
            return 1;
        }
    }
    wchar_t first = value[0] >= L'a' && value[0] <= L'z' ? (wchar_t)(value[0] - 32) : value[0];
    if (value[0] && !value[1] && ((first >= L'A' && first <= L'Z') || (first >= L'0' && first <= L'9'))) {
        *key = first;  // VK_A..VK_Z and VK_0..VK_9 are the characters
        return 1;
    }
    unsigned number;
    if ((first == L'F') && parse_number(value + 1, 24, &number) && number >= 1 && value[1] != L'0') {
        *key = VK_F1 + number - 1;
        return 1;
    }
    if (parse_number(value, 254, &number) && number >= 1) {
        *key = number;
        return 1;
    }
    return 0;
}

int config_parse_xbox(const wchar_t *text, unsigned *buttons) {
    wchar_t value[128];
    size_t length = wide_length(text);
    unsigned result = 0;
    if (length >= ARRAY_COUNT(value)) {
        return 0;
    }
    memory_copy(value, text, (length + 1) * sizeof(wchar_t));
    trim(value);
    if (wide_equal_ignore_case(value, L"None")) {
        *buttons = 0;
        return 1;
    }
    wchar_t *part = value;
    for (;;) {
        wchar_t *end = part;
        while (*end && *end != L'+') {
            ++end;
        }
        wchar_t separator = *end;
        *end = 0;
        trim(part);
        unsigned bit = 0;
        for (size_t i = 0; i < ARRAY_COUNT(button_names) && !bit; ++i) {
            if (wide_equal_ignore_case(part, button_names[i].name)) {
                bit = button_names[i].value;
            }
        }
        if (!bit) {
            return 0;
        }
        result |= bit;
        if (!separator) {
            break;
        }
        part = end + 1;
    }
    *buttons = result;
    return 1;
}

void config_defaults(RadioConfig *config) {
    memory_zero(config, sizeof(*config));
    config->enabled = 1;
    config->pcm_decode = 1;
    config->volume = 100;
    for (unsigned i = 0; i < KEY_COUNT; ++i) {
        config->keyboard[i] = default_keyboard[i];
    }
}

static void log_bad(const wchar_t *section, const wchar_t *key, const wchar_t *value, const char *used) {
    // "[Section] Key=value" in UTF-8.
    char setting[384];
    int length = 0, part;
    setting[length++] = '[';
    part = WideCharToMultiByte(CP_UTF8, 0, section, -1, setting + length, 32, NULL, NULL);
    length = part > 0 ? length + part - 1 : -1;
    if (length > 0) {
        setting[length++] = ']';
        setting[length++] = ' ';
        part = WideCharToMultiByte(CP_UTF8, 0, key, -1, setting + length, 96, NULL, NULL);
        length = part > 0 ? length + part - 1 : -1;
    }
    if (length > 0) {
        setting[length++] = '=';
        part = WideCharToMultiByte(CP_UTF8, 0, value, -1, setting + length,
                                   (int)sizeof(setting) - length, NULL, NULL);
        length = part > 0 ? length + part - 1 : -1;
    }
    if (length <= 0) {
        log_append("CUSTOM RADIO: CustomRadio.ini has a value that is not valid; its default is used.\r\n");
        return;
    }
    log_append_parts("CUSTOM RADIO: ", setting, " is not valid; default used: ", used);
}

// A value; `present` is 0 when the key is missing or empty.
static void read_value(const wchar_t *ini, const wchar_t *section, const wchar_t *key, wchar_t *value,
                       DWORD capacity, int *present) {
    GetPrivateProfileStringW(section, key, L"", value, capacity, ini);
    trim(value);
    *present = value[0] != 0;
}

static int read_flag(const wchar_t *ini, const wchar_t *key, int fallback) {
    wchar_t value[16];
    int present;
    read_value(ini, L"CustomRadio", key, value, ARRAY_COUNT(value), &present);
    if (!present) {
        return fallback;
    }
    if (!lstrcmpW(value, L"0") || !lstrcmpW(value, L"1")) {
        return value[0] == L'1';
    }
    log_bad(L"CustomRadio", key, value, fallback ? "1" : "0");
    return fallback;
}

// Warns about keys this plugin does not know (a typo would otherwise be silent).
static void check_known(const wchar_t *ini, const wchar_t *section, const wchar_t *const *known,
                        unsigned count) {
    static wchar_t text[8192];
    DWORD size = GetPrivateProfileSectionW(section, text, ARRAY_COUNT(text), ini);
    for (wchar_t *entry = text; size && entry < text + size && *entry; entry += wide_length(entry) + 1) {
        wchar_t key[64];
        size_t length = 0;
        while (entry[length] && entry[length] != L'=' && length + 1 < ARRAY_COUNT(key)) {
            key[length] = entry[length];
            ++length;
        }
        key[length] = 0;
        trim(key);
        if (!key[0] || key[0] == L';' || key[0] == L'#') {
            continue;
        }
        int found = 0;
        for (unsigned i = 0; i < count && !found; ++i) {
            found = wide_equal_ignore_case(key, known[i]);
        }
        if (!found) {
            char key_text[96], section_text[32];
            if (WideCharToMultiByte(CP_UTF8, 0, key, -1, key_text, sizeof(key_text), NULL, NULL) > 0 &&
                WideCharToMultiByte(CP_UTF8, 0, section, -1, section_text, sizeof(section_text), NULL,
                                    NULL) > 0) {
                log_append_parts("CUSTOM RADIO: unknown key ignored: [", section_text, "] ", key_text);
            }
        }
    }
}

static void resolve_folder(const wchar_t *value, const wchar_t *base, wchar_t *folder) {
    static wchar_t joined[FOLDER_CAPACITY * 2];
    int absolute = (value[0] && value[1] == L':') || value[0] == L'\\' || value[0] == L'/';
    joined[0] = 0;
    if (!absolute && base && base[0]) {
        wide_append(joined, ARRAY_COUNT(joined), base);
        size_t length = wide_length(joined);
        if (length && joined[length - 1] != L'\\' && joined[length - 1] != L'/') {
            wide_append(joined, ARRAY_COUNT(joined), L"\\");
        }
    }
    wide_append(joined, ARRAY_COUNT(joined), value);
    DWORD length = GetFullPathNameW(joined, FOLDER_CAPACITY, folder, NULL);
    if (!length || length >= FOLDER_CAPACITY) {
        folder[0] = 0;
        return;
    }
    while (length > 3 && (folder[length - 1] == L'\\' || folder[length - 1] == L'/')) {
        folder[--length] = 0;  // "C:\" keeps its separator
    }
}

void config_load(const wchar_t *ini, const wchar_t *base, RadioConfig *config) {
    static const wchar_t *const radio_keys[] = {L"Enabled", L"Folder", L"Name", L"Volume",
                                                L"UnlockAllStations", L"PcmDecode"};
    static const wchar_t *const key_keys[] = {L"RadioStationKeyboard", L"RadioNextKeyboard",
                                              L"RadioRestartKeyboard", L"RadioStationXbox",
                                              L"RadioNextXbox", L"RadioRestartXbox"};
    wchar_t value[FOLDER_CAPACITY];
    int present;
    config_defaults(config);
    check_known(ini, L"CustomRadio", radio_keys, ARRAY_COUNT(radio_keys));
    check_known(ini, L"Keys", key_keys, ARRAY_COUNT(key_keys));
    config->enabled = read_flag(ini, L"Enabled", 1);
    config->unlock_all = read_flag(ini, L"UnlockAllStations", 0);
    config->pcm_decode = read_flag(ini, L"PcmDecode", 1);

    read_value(ini, L"CustomRadio", L"Volume", value, ARRAY_COUNT(value), &present);
    unsigned volume;
    if (present) {
        if (value[1] != L'x' && value[1] != L'X' && parse_number(value, 400, &volume)) {
            config->volume = (int)volume;
        } else {
            log_bad(L"CustomRadio", L"Volume", value, "100");
        }
    }

    read_value(ini, L"CustomRadio", L"Folder", value, ARRAY_COUNT(value), &present);
    resolve_folder(present ? value : L"Music", base, config->folder);
    if (!config->folder[0]) {
        log_bad(L"CustomRadio", L"Folder", value, "Music");
        resolve_folder(L"Music", base, config->folder);
    }

    read_value(ini, L"CustomRadio", L"Name", value, NAME_CAPACITY / 3, &present);
    if (present && WideCharToMultiByte(CP_UTF8, 0, value, -1, config->name, NAME_CAPACITY, NULL, NULL) <= 0) {
        config->name[0] = 0;
    }

    for (unsigned i = 0; i < KEY_COUNT; ++i) {
        unsigned parsed;
        read_value(ini, L"Keys", keyboard_keys[i], value, ARRAY_COUNT(value), &present);
        if (present) {
            if (config_parse_keyboard(value, &parsed)) {
                config->keyboard[i] = parsed;
            } else {
                log_bad(L"Keys", keyboard_keys[i], value,
                        i == KEY_STATION ? "F9" : i == KEY_NEXT ? "F7" : "F6");
            }
        }
        read_value(ini, L"Keys", xbox_keys[i], value, ARRAY_COUNT(value), &present);
        if (present) {
            if (config_parse_xbox(value, &parsed)) {
                config->xbox[i] = parsed;
            } else {
                log_bad(L"Keys", xbox_keys[i], value, "None");
            }
        }
    }
}

unsigned keys_pressed(KeyEdges *edges, unsigned down, int enabled) {
    unsigned pressed = enabled ? down & ~edges->held : 0;
    edges->held = down;
    return pressed;
}

unsigned keys_down(const RadioConfig *config, KeyStateFn key_state, unsigned buttons) {
    unsigned down = 0;
    for (unsigned i = 0; i < KEY_COUNT; ++i) {
        int key = config->keyboard[i] && key_state && (key_state((int)config->keyboard[i]) & 0x8000);
        int pad = config->xbox[i] && (buttons & config->xbox[i]) == config->xbox[i];
        if (key || pad) {
            down |= 1u << i;
        }
    }
    return down;
}
