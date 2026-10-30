// Photo Export for Forza Horizon 2: an Extra Options mod-loader plugin.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Installed as mods\Photo Export\plugins\PhotoExport.dll beside Extra Options
// 0.4.4 on Azimuth 1.0.4. Every photo taken in the game is saved by Azimuth as
// a save container (player-1\Photo_NNNN_<time>), and its image is already a
// JPEG; this plugin copies each new one, once, into a folder of JPG files
// (default Documents\My Games\Forza Horizon 2\Photos). It changes nothing in
// the game: one low-priority thread, started from FH2PluginInitialize, reads
// Azimuth's saves every 3 seconds (photos.c). Settings: PhotoExport.ini beside
// the DLL. Log lines start with "PHOTO EXPORT:".
#include <stdarg.h>
#include <stdio.h>

#include "fh2_game.h"
#include "fh2_plugin.h"
#include "photo_export.h"

#define PLUGIN_EXPORT __declspec(dllexport)

static void (*host_log)(const char *message);
static DWORD first_delay = 5000, interval = 3000;
static HANDLE stop_event;
#ifdef PHOTO_EXPORT_TEST
static const wchar_t *test_saves, *test_documents;
static const void *test_host;
static HANDLE test_thread;
#endif

void say(const char *format, ...) {
    char text[2048], line[2100];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    text[sizeof(text) - 1] = 0;
    snprintf(line, sizeof(line), "PHOTO EXPORT: %s.\r\n", text);
    if (host_log) host_log(line);
}

static DWORD WINAPI photo_thread(LPVOID unused) {
    (void)unused;
    if (!SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN))
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    if (WaitForSingleObject(stop_event, first_delay) == WAIT_OBJECT_0) return 0;
    photos_prepare();
    photos_scan();
    photos_summary();
    while (WaitForSingleObject(stop_event, interval) == WAIT_TIMEOUT) photos_scan();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}

PLUGIN_EXPORT BOOL WINAPI FH2PluginInitialize(const FH2PluginContext *context) {
    if (!context || context->size < sizeof(FH2PluginContext) || context->api_version < 1) return FALSE;
    host_log = context->log_utf8;
    if (!context->plugin_path) {
        say("the loader gave no plugin path; nothing is exported");
        return FALSE;
    }
    wchar_t ini[FH2_PATH_CAPACITY], folder_text[FH2_PATH_CAPACITY], folder[FH2_PATH_CAPACITY];
    wchar_t saves[FH2_PATH_CAPACITY], index[FH2_PATH_CAPACITY];
    char text[FH2_PATH_CAPACITY * 3], more[FH2_PATH_CAPACITY * 3];
    if (!fh2_plugin_ini_path(context->plugin_path, ini, FH2_PATH_CAPACITY)) {
        say("the plugin's path is too long; nothing is exported");
        return FALSE;
    }
    if (!GetPrivateProfileIntW(L"PhotoExport", L"Enabled", 1, ini)) {
        say("[PhotoExport] Enabled=0; nothing is exported");
        return TRUE;
    }
    say("plugin " PHOTO_EXPORT_VERSION " for Extra Options %s",
        context->extra_options_version ? context->extra_options_version : "?");
    if (context->adapter != FH2_PLUGIN_AZIMUTH) {
        say("the game does not run on Azimuth (no Azimuth saves to read); nothing is exported");
        return TRUE;
    }
    const void *host = GetModuleHandleW(NULL);
#ifdef PHOTO_EXPORT_TEST
    if (test_host) host = test_host;
#endif
    const char *build = fh2_azimuth_build(host);
    if (!build) {
        say("the Azimuth executable is not 1.0.1 to 1.0.4 (their save layout is the one this plugin reads); "
            "nothing is exported");
        return TRUE;
    }
    size_t saves_length = fh2_azimuth_saves(saves);
#ifdef PHOTO_EXPORT_TEST
    if (test_saves) saves_length = fh2_path_set(saves, test_saves);
#endif
    if (!saves_length) {
        say("Azimuth's saves folder cannot be resolved; nothing is exported");
        return TRUE;
    }
    // Folder=: empty for Documents\My Games\Forza Horizon 2\Photos.
    GetPrivateProfileStringW(L"PhotoExport", L"Folder", L"", folder_text, FH2_PATH_CAPACITY, ini);
    size_t folder_length = 0;
    if (folder_text[0]) {
        folder_length = fh2_folder_setting(folder_text, folder);
        if (!folder_length) {
            fh2_utf8(folder_text, text, sizeof(text));
            say("[PhotoExport] Folder=%s is not a full path (X:\\... or \\\\server\\share\\...); nothing is exported", text);
            return TRUE;
        }
    } else {
        folder_length = fh2_documents(folder);
#ifdef PHOTO_EXPORT_TEST
        if (test_documents) folder_length = fh2_path_set(folder, test_documents);
#endif
        folder_length = fh2_path_join(folder, folder_length, L"My Games\\Forza Horizon 2\\Photos");
        if (!folder_length) {
            say("Windows gives no Documents folder; set [PhotoExport] Folder=; nothing is exported");
            return TRUE;
        }
    }
    if (fh2_path_within(folder, saves) || fh2_path_within(saves, folder)) {
        say("the JPG folder and Azimuth's saves folder are inside each other; nothing is exported");
        return TRUE;
    }
    // The list of exported photos: a hidden PhotoExport.list in the JPG folder.
    size_t index_length = fh2_path_join(index, fh2_path_set(index, folder), L"PhotoExport.list");
    if (!index_length || !photos_configure(saves, folder, index)) {
        say("a path is too long; nothing is exported");
        return TRUE;
    }
    stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    // The thread runs code of this DLL until the game ends: keep it loaded.
    HMODULE self = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       (LPCWSTR)(void *)FH2PluginInitialize, &self);
    HANDLE thread = stop_event ? CreateThread(NULL, 0, photo_thread, NULL, 0, NULL) : NULL;
    if (!thread) {
        say("the export thread cannot be started (error %lu); nothing is exported", GetLastError());
        return TRUE;
    }
#ifdef PHOTO_EXPORT_TEST
    test_thread = thread;
#else
    CloseHandle(thread);
#endif
    fh2_utf8(saves, text, sizeof(text));
    fh2_utf8(folder, more, sizeof(more));
    say("Azimuth %s; photos in %s (read only, checked every %lu s) are exported as JPG to %s", build, text,
        interval / 1000, more);
    return TRUE;
}

#ifdef PHOTO_EXPORT_TEST
// Test build only (tests\PhotoExportTest.dll): the same code with entry points
// for tests\run_tests.py. Nothing here runs in the shipped DLL.
PLUGIN_EXPORT int PhotoExportTestConfigure(const wchar_t *saves, const wchar_t *folder, const wchar_t *index,
                                           void (*log)(const char *)) {
    host_log = log;
    if (!photos_configure(saves, folder, index)) return 0;
    photos_prepare();
    return 1;
}

PLUGIN_EXPORT int PhotoExportTestScan(void) { return photos_scan(); }

PLUGIN_EXPORT void PhotoExportTestSummary(void) { photos_summary(); }

// FH2PluginInitialize then reads saves (instead of <exe folder>\data\titles\...),
// documents (instead of the known folder) and host (instead of the process' executable).
PLUGIN_EXPORT void PhotoExportTestOverride(const wchar_t *saves, const wchar_t *documents, const void *host,
                                           unsigned delay_ms, unsigned interval_ms) {
    test_saves = saves;
    test_documents = documents;
    test_host = host;
    first_delay = delay_ms;
    interval = interval_ms;
}

// Stops the thread FH2PluginInitialize started; 1 when it ended.
PLUGIN_EXPORT int PhotoExportTestStop(void) {
    if (!test_thread) return 1;
    SetEvent(stop_event);
    int ended = WaitForSingleObject(test_thread, 10000) == WAIT_OBJECT_0;
    CloseHandle(test_thread);
    CloseHandle(stop_event);
    test_thread = NULL;
    stop_event = NULL;
    return ended;
}

PLUGIN_EXPORT const char *PhotoExportTestBuild(const void *host) { return fh2_azimuth_build(host); }

PLUGIN_EXPORT const char *PhotoExportTestCommit(const char *text, size_t size, unsigned *count) {
    static Fh2Commit parsed;
    const char *why = fh2_parse_commit(text, size, &parsed);
    *count = parsed.count;
    return why;
}

PLUGIN_EXPORT size_t PhotoExportTestFolder(const wchar_t *text, wchar_t *output) {
    return fh2_folder_setting(text, output);
}

PLUGIN_EXPORT size_t PhotoExportTestDocuments(wchar_t *output) { return fh2_documents(output); }
#endif
