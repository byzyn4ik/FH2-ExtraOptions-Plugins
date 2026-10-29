// Keyboard & Mouse for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings.h"

#include <stdio.h>
#include <string.h>

// One "key=value" line of a section: the key and the value as Extra Options'
// own INI reader takes them (an inline ; or # starts a comment; spaces
// around both are dropped). 0 for a line without '='.
static int split(const wchar_t *line, char *key, size_t key_size, char *value, size_t value_size) {
    char narrow[512];
    if (!WideCharToMultiByte(CP_UTF8, 0, line, -1, narrow, sizeof(narrow), NULL, NULL)) return 0;
    char *equals = strchr(narrow, '=');
    if (!equals) return 0;
    *equals = 0;
    char *v = equals + 1;
    for (char *c = v; *c; ++c)
        if (*c == ';' || *c == '#') {
            *c = 0;
            break;
        }
    char *k = narrow;
    while (*k == ' ' || *k == '\t') ++k;
    while (*v == ' ' || *v == '\t') ++v;
    for (char *parts[2] = {k, v}, **part = parts; part < parts + 2; ++part) {
        size_t length = strlen(*part);
        while (length && ((*part)[length - 1] == ' ' || (*part)[length - 1] == '\t' || (*part)[length - 1] == '\r'))
            (*part)[--length] = 0;
    }
    if (strlen(k) >= key_size || strlen(v) >= value_size) return 0;
    strcpy(key, k);
    strcpy(value, v);
    return 1;
}

// Calls `take` for every key of the section; the first refused line is
// described in `problem` and 0 returned.
static int read_section(const wchar_t *ini_path, const wchar_t *section, void *target,
                        int (*take)(void *target, const char *key, const char *value), char *problem,
                        size_t problem_size) {
    static wchar_t buffer[32768];
    DWORD length = GetPrivateProfileSectionW(section, buffer, ARRAY_COUNT(buffer), ini_path);
    for (const wchar_t *line = buffer; length && *line; line += wcslen(line) + 1) {
        char key[64], value[256];
        if (line[0] == L';' || line[0] == L'#') continue;
        if (!split(line, key, sizeof(key), value, sizeof(value)) || !key[0]) {
            snprintf(problem, problem_size, "[%ls] line \"%ls\" is not key=value", section, line);
            return 0;
        }
        if (!take(target, key, value)) {
            snprintf(problem, problem_size, "[%ls] %s=%s: unknown key or invalid value", section, key, value);
            return 0;
        }
    }
    return 1;
}

static int take_kbm(void *target, const char *key, const char *value) {
    return kbm_config_parse((KbmSettings *)target, key, value);
}

static int take_controller(void *target, const char *key, const char *value) {
    if (!text_equal_ignore_case(key, "PauseOnlyOnUnplug") ||
        (!text_equal_ignore_case(value, "0") && !text_equal_ignore_case(value, "1")))
        return 0;
    *(int *)target = value[0] - '0';
    return 1;
}

void settings_read(const wchar_t *ini_path, PluginSettings *settings) {
    memset(settings, 0, sizeof(*settings));
    kbm_config_defaults(&settings->kbm);
    settings->pause_only_on_unplug = 1;
    settings->kbm_valid =
        read_section(ini_path, L"KeyboardMouse", &settings->kbm, take_kbm, settings->kbm_problem,
                     sizeof(settings->kbm_problem));
    if (settings->kbm_valid && !kbm_config_validate(&settings->kbm)) {
        settings->kbm_valid = 0;
        snprintf(settings->kbm_problem, sizeof(settings->kbm_problem),
                 "[KeyboardMouse] binds one key to two actions of the same group (driving or menus)");
    }
    if (!settings->kbm_valid) settings->kbm.enabled = 0;
    int pause = 1;
    settings->controller_valid = read_section(ini_path, L"Controller", &pause, take_controller,
                                              settings->controller_problem, sizeof(settings->controller_problem));
    settings->pause_only_on_unplug = settings->controller_valid ? pause : 0;
}

#ifdef KBM_TEST
// Test build: reads `ini` as at start; returns kbm enabled (0/1), -1 when
// [KeyboardMouse] is refused. *pause gets PauseOnlyOnUnplug (-1 refused).
__declspec(dllexport) int KbmTestReadSettings(const wchar_t *ini, int *pause, char *problem, size_t size,
                                              KbmSettings *out) {
    static PluginSettings settings;
    settings_read(ini, &settings);
    *pause = settings.controller_valid ? settings.pause_only_on_unplug : -1;
    snprintf(problem, size, "%s%s%s", settings.kbm_problem, settings.kbm_problem[0] ? "; " : "",
             settings.controller_problem);
    if (out) *out = settings.kbm;
    return settings.kbm_valid ? settings.kbm.enabled : -1;
}
#endif
