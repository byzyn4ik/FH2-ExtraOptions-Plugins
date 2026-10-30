// Xtra Features for Forza Horizon 2: an Extra Options mod-loader plugin.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Installed as mods\Xtra Features\plugins\XtraFeatures.dll beside Extra
// Options 0.4.4 on Azimuth 1.0.4. Three features, each with its own switch in
// XtraFeatures.ini ([XtraFeatures]):
//   MapZoom           the pause map opens farther out and zooms out farther
//                     (map_zoom.c);
//   GarageThumbnails  garage thumbnails rendered 2-4 times the console's size,
//                     F8 re-renders them (thumbnails.c);
//   PhotoScan         RB in photo mode credits the cars in view to the photo
//                     collection (photo_scan.c);
//   ExitGame          an Exit Game tile on the pause menu's Home tab that closes
//                     the game (exit_game.c, pause_xaml.c: the Exit Game plugin).
// All four change only the game image (no Azimuth or Extra Options code).
// From FH2PluginInitialize (on Extra Options' loader worker, after the game
// started) each feature checks its game code byte for byte and chains onto
// whatever is at its sites; on any mismatch that feature changes nothing and
// the log (FH2_ExtraOptions.log, "XTRA FEATURES: ...") says why.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "fh2_game.h"
#include "fh2_plugin.h"
#include "xtra.h"

#define PLUGIN_EXPORT __declspec(dllexport)

static void (*host_log)(const char *message);

void say(const char *format, ...) {
    char line[640];
    int used = snprintf(line, sizeof(line), "XTRA FEATURES: ");
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line + used, sizeof(line) - (size_t)used - 3, format, arguments);
    va_end(arguments);
    strcat(line, "\r\n");
    if (host_log) host_log(line);
}

void *read_pointer(const void *address) {
    void *value = NULL;
    return fh2_safe_read(address, &value, sizeof(value)) ? value : NULL;
}

int in_game(const unsigned char *game, const void *address) {
    return (const unsigned char *)address >= game && (const unsigned char *)address < game + GAME_IMAGE_SIZE;
}

int in_game_text(const unsigned char *game, const void *address) {
    return (const unsigned char *)address >= game + 0x1000 && (const unsigned char *)address < game + GAME_TEXT_END;
}

void describe(const void *address, char *out, size_t size) {
    HMODULE module = NULL;
    wchar_t path[MAX_PATH];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)address, &module) &&
        GetModuleFileNameW(module, path, MAX_PATH)) {
        const wchar_t *name = wcsrchr(path, L'\\');
        snprintf(out, size, "%ls+0x%llx", name ? name + 1 : path,
                 (unsigned long long)((const unsigned char *)address - (const unsigned char *)module));
    } else {
        snprintf(out, size, "0x%llx", (unsigned long long)(uintptr_t)address);
    }
}

const char *check_code(const unsigned char *game, const Code *code, unsigned count, char *why, size_t size) {
    for (unsigned i = 0; i < count; ++i) {
        for (unsigned done = 0; done < code[i].length; done += 64) {
            unsigned chunk = code[i].length - done < 64 ? code[i].length - done : 64;
            if (!fh2_bytes_equal(game + code[i].rva + done, code[i].bytes + done, chunk)) {
                snprintf(why, size, "%s at 0x%x is not as inspected", code[i].name, (unsigned)code[i].rva);
                return why;
            }
        }
    }
    return NULL;
}

const char *check_slots(const unsigned char *game, const Slot *slots, unsigned count, char *why, size_t size) {
    for (unsigned i = 0; i < count; ++i) {
        if (read_pointer(game + slots[i].slot) != game + slots[i].target) {
            snprintf(why, size, "%s (slot 0x%x) does not hold 0x%x", slots[i].name, (unsigned)slots[i].slot,
                     (unsigned)slots[i].target);
            return why;
        }
    }
    return NULL;
}

static void keep_loaded(void) {
    HMODULE self = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       (LPCWSTR)(void *)keep_loaded, &self);
}

static const char *const feature_names[FEATURE_COUNT] = {"MapZoom", "GarageThumbnails", "PhotoScan", "ExitGame"};

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}

PLUGIN_EXPORT BOOL WINAPI FH2PluginInitialize(const FH2PluginContext *context) {
    if (!context || context->size < sizeof(FH2PluginContext) || context->api_version < 1) return FALSE;
    host_log = context->log_utf8;
    wchar_t ini[MAX_PATH * 2];
    if (!context->plugin_path || !context->game_image ||
        !fh2_plugin_ini_path(context->plugin_path, ini, ARRAY_COUNT(ini))) {
        say("the loader gave no plugin path or game image; nothing is installed.");
        return FALSE;
    }
    say("Xtra Features " XTRA_VERSION " for Extra Options %s.",
        context->extra_options_version ? context->extra_options_version : "?");
    if (context->game_image_size != GAME_IMAGE_SIZE) {
        say("the game image is not the supported Forza Horizon 2 build (size 0x%x, not 0x3a18000); nothing is "
            "installed.", (unsigned)context->game_image_size);
        return TRUE;
    }
    if (context->adapter != FH2_PLUGIN_AZIMUTH) {
        say("the game does not run on Azimuth (adapter %u); nothing is installed.", (unsigned)context->adapter);
        return TRUE;
    }
    XtraSettings settings;
    settings_read(ini, &settings);
    const int wanted[FEATURE_COUNT] = {settings.map_zoom, settings.thumbnails, settings.photo_scan,
                                       settings.exit_game};
    for (unsigned i = 0; i < FEATURE_COUNT; ++i) {
        if (settings.problem[i][0])
            say("%s is off: %s.", feature_names[i], settings.problem[i]);
        else if (!wanted[i])
            say("%s is off ([XtraFeatures] %s=0).", feature_names[i], feature_names[i]);
    }
    // Callbacks into this DLL stay in the game: keep it loaded.
    if (wanted[0] || wanted[1] || wanted[2] || wanted[3]) keep_loaded();
    unsigned char *game = (unsigned char *)context->game_image;
    char why[200];
    const char *problem;
    if (wanted[FEATURE_MAP_ZOOM]) {
        if ((problem = map_zoom_check(game, why, sizeof(why))) != NULL)
            say("MapZoom: %s; the pause map is left as it is.", problem);
        else
            map_zoom_install(game, &settings);
    }
    if (wanted[FEATURE_THUMBNAILS]) {
        if ((problem = thumbnails_check(game, why, sizeof(why))) != NULL)
            say("GarageThumbnails: %s; the thumbnails are left as they are.", problem);
        else
            thumbnails_install(game, &settings);
    }
    if (wanted[FEATURE_PHOTO_SCAN]) {
        if ((problem = photo_scan_check(game, why, sizeof(why))) != NULL)
            say("PhotoScan: %s; RB in photo mode is left as it is.", problem);
        else
            photo_scan_install(game);
    }
    if (wanted[FEATURE_EXIT_GAME]) {
        if ((problem = exit_game_check(game, why, sizeof(why))) != NULL)
            say("ExitGame: %s; nothing is installed and the pause menu is left as it is.", problem);
        else
            exit_game_install(game);
    }
    return TRUE;
}

#ifdef XTRA_TEST
// Test build only (out\tests\XtraFeaturesTest.dll): the same code with entry
// points for tests\run_tests.py.
PLUGIN_EXPORT void XtraTestSettings(const wchar_t *ini_path, XtraSettings *out, unsigned size) {
    XtraSettings settings;
    settings_read(ini_path, &settings);
    memcpy(out, &settings, size < sizeof(settings) ? size : sizeof(settings));
}
PLUGIN_EXPORT unsigned XtraTestSettingsSize(void) { return sizeof(XtraSettings); }

// 0 map zoom, 1 thumbnails, 2 photo scan, 3 exit game: NULL or what differs.
PLUGIN_EXPORT const char *XtraTestCheck(const unsigned char *game, int feature) {
    static char why[200];
    return feature == 0   ? map_zoom_check(game, why, sizeof(why))
           : feature == 1 ? thumbnails_check(game, why, sizeof(why))
           : feature == 2 ? photo_scan_check(game, why, sizeof(why))
                          : exit_game_check(game, why, sizeof(why));
}

PLUGIN_EXPORT const char *XtraTestChainCall(unsigned char *site, const void *native, const unsigned char *game,
                                            void *handler, void **previous) {
    return chain_call(site, native, game, handler, previous);
}

PLUGIN_EXPORT const char *XtraTestWriteCode(unsigned char *at, const unsigned char *expected,
                                            const unsigned char *bytes, size_t length) {
    return write_code(at, expected, bytes, length);
}
#endif
