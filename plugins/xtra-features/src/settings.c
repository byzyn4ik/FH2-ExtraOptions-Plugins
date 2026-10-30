// Xtra Features for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// XtraFeatures.ini beside the DLL, read once at start:
//   [XtraFeatures]      MapZoom, GarageThumbnails, PhotoScan, ExitGame: 0 or 1
//                       (default 1)
//   [MapZoom]           MapStartZoom 25-400 (150), MapMaxZoomOut 100-400 (225),
//                       MapIslandStartZoom 25-400 (100), MapIslandMaxZoomOut
//                       100-400 (175): percent of the game's distances
//   [GarageThumbnails]  CarPreviewScale 1-4 (3)
// The keys and ranges of the Extra Options fork the first three come from. A
// missing key keeps its default; a bad value turns its feature off.
#include <stdio.h>

#include "xtra.h"

void settings_defaults(XtraSettings *settings) {
    *settings = (XtraSettings){0};
    settings->map_zoom = settings->thumbnails = settings->photo_scan = settings->exit_game = 1;
    settings->start = 150;
    settings->max = 225;
    settings->island_start = 100;
    settings->island_max = 175;
    settings->scale = 3;
}

// The value without blanks and a trailing "; comment"; 0 when the key is absent.
static int read_value(const wchar_t *ini, const wchar_t *section, const wchar_t *key, char *out, size_t size) {
    wchar_t value[64];
    DWORD length = GetPrivateProfileStringW(section, key, L"\x01", value, ARRAY_COUNT(value), ini);
    if (length == 1 && value[0] == 1) return 0;
    size_t used = 0;
    for (DWORD i = 0; i < length && value[i] != L';' && used + 1 < size; ++i) {
        wchar_t c = value[i];
        if (c == L' ' || c == L'\t') continue;
        out[used++] = c < 0x80 ? (char)c : '?';
    }
    out[used] = 0;
    return 1;
}

// A whole number in [low, high].
static int parse_number(const char *text, int low, int high, int *value) {
    int result = 0;
    if (!*text) return 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9' || result > 100000) return 0;
        result = result * 10 + (*text - '0');
    }
    if (result < low || result > high) return 0;
    *value = result;
    return 1;
}

// One key; 0 (problem set) when it holds a bad value.
static int read_number(const wchar_t *ini, const wchar_t *section, const wchar_t *key, int low, int high, int *value,
                       char *problem, size_t size) {
    char text[64];
    if (!read_value(ini, section, key, text, sizeof(text))) return 1;
    if (parse_number(text, low, high, value)) return 1;
    snprintf(problem, size, "[%ls] %ls=%s is not a whole number from %d to %d", section, key, text, low, high);
    return 0;
}

void settings_read(const wchar_t *ini, XtraSettings *settings) {
    settings_defaults(settings);
    static const wchar_t *const switches[FEATURE_COUNT] = {L"MapZoom", L"GarageThumbnails", L"PhotoScan",
                                                           L"ExitGame"};
    int *const enabled[FEATURE_COUNT] = {&settings->map_zoom, &settings->thumbnails, &settings->photo_scan,
                                         &settings->exit_game};
    for (unsigned i = 0; i < FEATURE_COUNT; ++i)
        if (!read_number(ini, L"XtraFeatures", switches[i], 0, 1, enabled[i], settings->problem[i],
                         sizeof(settings->problem[i])))
            *enabled[i] = 0;

    char *problem = settings->problem[FEATURE_MAP_ZOOM];
    size_t size = sizeof(settings->problem[FEATURE_MAP_ZOOM]);
    if (settings->map_zoom &&
        (!read_number(ini, L"MapZoom", L"MapStartZoom", PERCENT_MIN, PERCENT_MAX, &settings->start, problem, size) ||
         !read_number(ini, L"MapZoom", L"MapMaxZoomOut", 100, PERCENT_MAX, &settings->max, problem, size) ||
         !read_number(ini, L"MapZoom", L"MapIslandStartZoom", PERCENT_MIN, PERCENT_MAX, &settings->island_start,
                      problem, size) ||
         !read_number(ini, L"MapZoom", L"MapIslandMaxZoomOut", 100, PERCENT_MAX, &settings->island_max, problem,
                      size)))
        settings->map_zoom = 0;

    if (settings->thumbnails &&
        !read_number(ini, L"GarageThumbnails", L"CarPreviewScale", 1, 4, &settings->scale,
                     settings->problem[FEATURE_THUMBNAILS], sizeof(settings->problem[FEATURE_THUMBNAILS])))
        settings->thumbnails = 0;
}
