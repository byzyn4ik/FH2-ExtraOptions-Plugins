// Saves in Documents (mirror) for Forza Horizon 2: an Extra Options mod-loader plugin.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Installed as mods\Saves in Documents (mirror)\plugins\DocumentsSaves.dll
// beside Extra Options 0.4.4 on Azimuth 1.0.4. Azimuth keeps the saves in its
// own folder (data\titles\1A96545B\saves) and reads and writes them there; this
// plugin keeps a verified copy of every save in Documents\My Games\Forza
// Horizon 2\Saves\1A96545B (mirror.c). It does not redirect anything: Extra
// Options starts plugins only once the game runs, after Azimuth has prepared
// its saves folder and its launcher has shown (and could back up or restore)
// the saves in it. One low-priority thread, started from FH2PluginInitialize,
// looks for new saves every 5 seconds. Settings: DocumentsSaves.ini beside the
// DLL. Log lines start with "SAVES IN DOCUMENTS:".
#include <stdarg.h>
#include <stdio.h>

#include "documents_saves.h"
#include "fh2_game.h"
#include "fh2_plugin.h"

#define PLUGIN_EXPORT __declspec(dllexport)

static void (*host_log)(const char *message);
static DWORD first_delay = 10000, interval = 5000;
static HANDLE stop_event;
#ifdef DOCUMENTS_SAVES_TEST
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
    snprintf(line, sizeof(line), "SAVES IN DOCUMENTS: %s.\r\n", text);
    if (host_log) host_log(line);
}

static DWORD WINAPI mirror_thread(LPVOID unused) {
    (void)unused;
    if (!SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN))
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    if (WaitForSingleObject(stop_event, first_delay) == WAIT_OBJECT_0) return 0;
    mirror_prepare();
    mirror_pass();
    mirror_summary();
    while (WaitForSingleObject(stop_event, interval) == WAIT_TIMEOUT) mirror_pass();
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
        say("the loader gave no plugin path; nothing is copied");
        return FALSE;
    }
    wchar_t ini[FH2_PATH_CAPACITY], folder_text[FH2_PATH_CAPACITY], mirror[FH2_PATH_CAPACITY], saves[FH2_PATH_CAPACITY];
    char text[FH2_PATH_CAPACITY * 3], more[FH2_PATH_CAPACITY * 3];
    if (!fh2_plugin_ini_path(context->plugin_path, ini, FH2_PATH_CAPACITY)) {
        say("the plugin's path is too long; nothing is copied");
        return FALSE;
    }
    if (!GetPrivateProfileIntW(L"DocumentsSaves", L"Enabled", 1, ini)) {
        say("[DocumentsSaves] Enabled=0; nothing is copied");
        return TRUE;
    }
    say("plugin " DOCUMENTS_SAVES_VERSION " for Extra Options %s",
        context->extra_options_version ? context->extra_options_version : "?");
    if (context->adapter != FH2_PLUGIN_AZIMUTH) {
        say("the game does not run on Azimuth (no Azimuth saves to copy); nothing is copied");
        return TRUE;
    }
    const void *host = GetModuleHandleW(NULL);
#ifdef DOCUMENTS_SAVES_TEST
    if (test_host) host = test_host;
#endif
    const char *build = fh2_azimuth_build(host);
    if (!build) {
        say("the Azimuth executable is not 1.0.1 to 1.0.4 (their save layout is the one this plugin copies); "
            "nothing is copied");
        return TRUE;
    }
    size_t saves_length = fh2_azimuth_saves(saves);
#ifdef DOCUMENTS_SAVES_TEST
    if (test_saves) saves_length = fh2_path_set(saves, test_saves);
#endif
    if (!saves_length) {
        say("Azimuth's saves folder cannot be resolved; nothing is copied");
        return TRUE;
    }
    // Folder=: empty for Documents\My Games\Forza Horizon 2\Saves\1A96545B.
    GetPrivateProfileStringW(L"DocumentsSaves", L"Folder", L"", folder_text, FH2_PATH_CAPACITY, ini);
    size_t mirror_length;
    if (folder_text[0]) {
        mirror_length = fh2_folder_setting(folder_text, mirror);
        if (!mirror_length) {
            fh2_utf8(folder_text, text, sizeof(text));
            say("[DocumentsSaves] Folder=%s is not a full path (X:\\... or \\\\server\\share\\...); nothing is copied",
                text);
            return TRUE;
        }
    } else {
        mirror_length = fh2_documents(mirror);
#ifdef DOCUMENTS_SAVES_TEST
        if (test_documents) mirror_length = fh2_path_set(mirror, test_documents);
#endif
        mirror_length = fh2_path_join(mirror, mirror_length, L"My Games\\Forza Horizon 2\\Saves\\" FH2_TITLE_ID);
        if (!mirror_length) {
            say("Windows gives no Documents folder; set [DocumentsSaves] Folder=; nothing is copied");
            return TRUE;
        }
    }
    if (fh2_path_within(mirror, saves) || fh2_path_within(saves, mirror)) {
        say("the mirror folder and Azimuth's saves folder are inside each other; nothing is copied");
        return TRUE;
    }
    DWORD attributes = GetFileAttributesW(mirror);
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))) {
        fh2_utf8(mirror, text, sizeof(text));
        say("%s is a link or a file, not a folder; nothing is copied (links are never followed)", text);
        return TRUE;
    }
    unsigned keep = GetPrivateProfileIntW(L"DocumentsSaves", L"KeepRevisions", 0, ini);
    if (!mirror_configure(saves, mirror, keep)) {
        say("a path is too long; nothing is copied");
        return TRUE;
    }
    stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    // The thread runs code of this DLL until the game ends: keep it loaded.
    HMODULE self = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       (LPCWSTR)(void *)FH2PluginInitialize, &self);
    HANDLE thread = stop_event ? CreateThread(NULL, 0, mirror_thread, NULL, 0, NULL) : NULL;
    if (!thread) {
        say("the copy thread cannot be started (error %lu); nothing is copied", GetLastError());
        return TRUE;
    }
#ifdef DOCUMENTS_SAVES_TEST
    test_thread = thread;
#else
    CloseHandle(thread);
#endif
    fh2_utf8(saves, text, sizeof(text));
    fh2_utf8(mirror, more, sizeof(more));
    say("Azimuth %s; the saves stay in %s (only read, checked every %lu s); each new committed revision is copied "
        "to %s and verified (SHA-256); %s",
        build, text, interval / 1000, more,
        keep ? "KeepRevisions: the mirror keeps the newest revisions of each container" : "nothing in the mirror is ever deleted");
    return TRUE;
}

#ifdef DOCUMENTS_SAVES_TEST
// Test build only (tests\DocumentsSavesTest.dll): the same code with entry
// points for tests\run_tests.py. Nothing here runs in the shipped DLL.
PLUGIN_EXPORT int DocumentsSavesTestConfigure(const wchar_t *source, const wchar_t *mirror, unsigned keep,
                                              void (*log)(const char *)) {
    host_log = log;
    return mirror_configure(source, mirror, keep);
}

PLUGIN_EXPORT void DocumentsSavesTestPrepare(void) { mirror_prepare(); }

PLUGIN_EXPORT int DocumentsSavesTestPass(void) { return mirror_pass(); }

PLUGIN_EXPORT void DocumentsSavesTestSummary(void) { mirror_summary(); }

PLUGIN_EXPORT void DocumentsSavesTestHooks(MirrorFault fault, unsigned chunk, unsigned long long free_space) {
    mirror_test_hooks(fault, chunk, free_space);
}

PLUGIN_EXPORT void DocumentsSavesTestOverride(const wchar_t *saves, const wchar_t *documents, const void *host,
                                              unsigned delay_ms, unsigned interval_ms) {
    test_saves = saves;
    test_documents = documents;
    test_host = host;
    first_delay = delay_ms;
    interval = interval_ms;
}

PLUGIN_EXPORT int DocumentsSavesTestStop(void) {
    if (!test_thread) return 1;
    SetEvent(stop_event);
    int ended = WaitForSingleObject(test_thread, 30000) == WAIT_OBJECT_0;
    CloseHandle(test_thread);
    CloseHandle(stop_event);
    test_thread = NULL;
    stop_event = NULL;
    return ended;
}
#endif
