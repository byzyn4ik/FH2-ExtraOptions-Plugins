// Language Support for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The language pack (an Extra Options mod folder with a language.ini), the
// launcher's language and the Azimuth host. Ported from custom_language.c of
// the Extra Options fork (0.3.2-openbox), where the pack was mounted from
// <game>\Mount\languages\<id>\ by Extra Options itself; with a stock Extra
// Options 0.4.4 the pack is an ordinary mod folder and Extra Options mounts its
// media\ (see README.md).
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "lang_support.h"

// Azimuth language code -> the game's string table archive (media\stripped\stringtables\<archive>.zip).
static const struct {
    const wchar_t *slot, *archive;
} slots[] = {{L"en-US", L"EN"}, {L"en-GB", L"GB"}, {L"de-DE", L"DE"}, {L"fr-FR", L"FR"},
             {L"it-IT", L"IT"}, {L"es-ES", L"ES"}, {L"es-MX", L"MX"}, {L"pt-BR", L"br"},
             {L"pl-PL", L"PL"}, {L"ru-RU", L"RU"}, {L"ja-JP", L"JP"}, {L"ko-KR", L"KO"},
             {L"zh-TW", L"CHT"}, {L"zh-HK", L"CHT"}, {L"nl-NL", L"nl"}, {L"cs-CZ", L"cz"},
             {L"hu-HU", L"HU"}};

const wchar_t *slot_archive(const wchar_t *slot) {
    for (unsigned i = 0; slot && i < ARRAY_COUNT(slots); ++i)
        if (!_wcsicmp(slot, slots[i].slot)) return slots[i].archive;
    return NULL;
}

static int join(wchar_t *output, size_t capacity, const wchar_t *a, const wchar_t *b) {
    int n = _snwprintf(output, capacity, L"%ls\\%ls", a, b);
    if (n < 0 || (size_t)n >= capacity) {
        output[0] = 0;
        return 0;
    }
    return 1;
}

static int exists(const wchar_t *path, int directory) {
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !!(attributes & FILE_ATTRIBUTE_DIRECTORY) == directory;
}

static const char *read_pack(const wchar_t *mods, const wchar_t *name, LangPack *pack) {
    wchar_t ini[LANG_PATH];
    if (wcslen(name) >= ARRAY_COUNT(pack->name) || !join(pack->folder, ARRAY_COUNT(pack->folder), mods, name) ||
        !join(ini, ARRAY_COUNT(ini), pack->folder, L"language.ini"))
        return "the pack's path is too long";
    if (!exists(ini, 0)) return "the pack's folder in mods\\ has no language.ini";
    wcscpy(pack->name, name);
    GetPrivateProfileStringW(L"Language", L"Slot", L"", pack->slot, ARRAY_COUNT(pack->slot), ini);
    GetPrivateProfileStringW(L"Language", L"Name", L"", pack->language, ARRAY_COUNT(pack->language), ini);
    if (!pack->language[0]) lstrcpynW(pack->language, name, ARRAY_COUNT(pack->language));
    pack->archive = slot_archive(pack->slot);
    if (!pack->archive) return "its language.ini [Language] Slot is not a game language code such as ru-RU";
    return NULL;
}

const char *pack_find(const wchar_t *mods, const wchar_t *wanted, LangPack *pack) {
    memset(pack, 0, sizeof(*pack));
    if (!mods || !exists(mods, 1)) return "the mods folder was not found";
    if (wanted && wanted[0]) {
        if (wcschr(wanted, L'\\') || wcschr(wanted, L'/') || wcschr(wanted, L':') || !wcscmp(wanted, L".") ||
            !wcscmp(wanted, L".."))
            return "[LangSupport] Pack must be the name of a folder in mods\\";
        return read_pack(mods, wanted, pack);
    }
    // No Pack=: the one folder of mods\ that holds a language.ini.
    wchar_t pattern[LANG_PATH], found[260] = L"";
    unsigned count = 0;
    if (!join(pattern, ARRAY_COUNT(pattern), mods, L"*")) return "the mods folder's path is too long";
    WIN32_FIND_DATAW entry;
    HANDLE search = FindFirstFileW(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE) return "no folder in mods\\ holds a language.ini";
    do {
        wchar_t ini[LANG_PATH];
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || entry.cFileName[0] == L'.')
            continue;  // as Extra Options: no dot folders, no directory links
        if (_snwprintf(ini, ARRAY_COUNT(ini), L"%ls\\%ls\\language.ini", mods, entry.cFileName) < 0 ||
            !exists(ini, 0))
            continue;
        if (!count++) wcscpy(found, entry.cFileName);
    } while (FindNextFileW(search, &entry));
    FindClose(search);
    if (!count) return "no folder in mods\\ holds a language.ini";
    if (count > 1) return "several folders in mods\\ hold a language.ini; name one with [LangSupport] Pack";
    return read_pack(mods, found, pack);
}

int pack_load_order(const wchar_t *mods, const wchar_t *name) {
    wchar_t path[LANG_PATH];
    if (!join(path, ARRAY_COUNT(path), mods, L"loadorder.txt")) return -1;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER size;
    char *text = NULL;
    DWORD got = 0;
    // Extra Options reads at most 64 KiB of load order.
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= 65536) {
        text = (char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)size.QuadPart + 1);
        if (text && (!ReadFile(file, text, (DWORD)size.QuadPart, &got, NULL) || got != (DWORD)size.QuadPart)) got = 0;
    }
    CloseHandle(file);
    if (!text) return -1;
    text[got] = 0;
    int result = -1;
    char *line = text;
    if ((unsigned char)line[0] == 0xef && (unsigned char)line[1] == 0xbb && (unsigned char)line[2] == 0xbf) line += 3;
    while (*line && result != 1) {
        char *end = line;
        while (*end && *end != '\n') ++end;
        char *next = *end ? end + 1 : end;
        while (end > line && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) --end;
        while (line < end && (*line == ' ' || *line == '\t')) ++line;
        int commented = line < end && *line == '#';
        if (commented) {
            ++line;
            while (line < end && (*line == ' ' || *line == '\t')) ++line;
        }
        wchar_t entry[260];
        int n = end > line && end - line < 1024
                    ? MultiByteToWideChar(CP_UTF8, 0, line, (int)(end - line), entry, ARRAY_COUNT(entry) - 1)
                    : 0;
        if (n > 0) {
            entry[n] = 0;
            if (CompareStringOrdinal(entry, -1, name, -1, TRUE) == CSTR_EQUAL) result = commented ? 0 : 1;
        }
        line = next;
    }
    HeapFree(GetProcessHeap(), 0, text);
    return result;
}

int launcher_language(const wchar_t *azimuth_ini, wchar_t *language, size_t capacity) {
    wchar_t game[64], section[80];
    language[0] = 0;
    if (azimuth_ini && exists(azimuth_ini, 0)) {
        GetPrivateProfileStringW(L"launcher", L"game", L"", game, ARRAY_COUNT(game), azimuth_ini);
        if (game[0] && _snwprintf(section, ARRAY_COUNT(section), L"title-%ls", game) > 0)
            GetPrivateProfileStringW(section, L"language", L"", language, (DWORD)capacity, azimuth_ini);
        if (!language[0])  // Azimuth before 1.0.0 kept it under [launcher]
            GetPrivateProfileStringW(L"launcher", L"language", L"", language, (DWORD)capacity, azimuth_ini);
    }
    if (language[0] && _wcsicmp(language, L"system")) return 1;
    if (!GetUserDefaultLocaleName(language, (int)capacity)) language[0] = 0;
    return 0;
}

// ------------------------------------------------------------------- host

static const struct {
    AzimuthBuild build;
    const char *name;
    uint32_t timestamp, image_size;
} hosts[] = {{AZIMUTH_104, "1.0.4", 0x6ae393ebu, 0x8a2000u}, {AZIMUTH_102, "1.0.2", 0x6ae05dd1u, 0x9af000u}};

#ifdef LANG_SUPPORT_TEST
static const unsigned char *test_host;
void azimuth_set_test_host(const unsigned char *base) { test_host = base; }
#endif

const unsigned char *azimuth_host(void) {
#ifdef LANG_SUPPORT_TEST
    if (test_host) return test_host;
#endif
    return (const unsigned char *)GetModuleHandleW(NULL);
}

AzimuthBuild azimuth_build(const unsigned char *base) {
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    if (!base || !fh2_safe_read(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew <= 0 || dos.e_lfanew > 0x1000 || !fh2_safe_read(base + dos.e_lfanew, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE)
        return AZIMUTH_UNKNOWN;
    // A branded copy that only adds a section (the Forza Horizon 2 Builder's exe) is larger.
    for (unsigned i = 0; i < ARRAY_COUNT(hosts); ++i)
        if (nt.FileHeader.TimeDateStamp == hosts[i].timestamp && nt.OptionalHeader.SizeOfImage >= hosts[i].image_size)
            return hosts[i].build;
    return AZIMUTH_UNKNOWN;
}

const char *azimuth_name(AzimuthBuild build) {
    for (unsigned i = 0; i < ARRAY_COUNT(hosts); ++i)
        if (hosts[i].build == build) return hosts[i].name;
    return "unknown";
}
