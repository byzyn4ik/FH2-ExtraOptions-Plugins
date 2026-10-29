// Custom Radio for Forza Horizon 2 Extra Options: the plugin's entry points.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// An Extra Options mod-loader plugin (tools/PLUGIN_API.md of Extra Options):
// FH2PluginInitialize reads CustomRadio.ini beside this DLL and installs the
// station's hooks; FH2PluginVehicleUpdate (the local car's update, on the game
// thread) reads the three keys while gameplay input is enabled and the game
// window has focus.
#include "fh2_plugin.h"
#include "config.h"
#include "radio.h"
#include "sites.h"

#define CUSTOM_RADIO_VERSION "1.0.0"

static RadioConfig config;
static KeyEdges edges;
static KeyStateFn key_state;  // NULL: GetAsyncKeyState
static int focus_override = -1;  // test build: -1 = ask Windows

static int game_focused(void) {
    if (focus_override >= 0) {
        return focus_override;
    }
    HWND window = GetForegroundWindow();
    DWORD process = 0;
    if (window) {
        GetWindowThreadProcessId(window, &process);
    }
    return process == GetCurrentProcessId();
}

// <folder of the DLL>\CustomRadio.ini: the DLL path with its extension replaced.
static int ini_path_for(const wchar_t *plugin_path, wchar_t *output, size_t capacity) {
    size_t length = wide_length(plugin_path), dot = length;
    for (size_t i = length; i > 0; --i) {
        if (plugin_path[i - 1] == L'.') {
            dot = i - 1;
            break;
        }
        if (plugin_path[i - 1] == L'\\' || plugin_path[i - 1] == L'/') {
            break;
        }
    }
    if (dot + 5 > capacity || dot == length) {
        return 0;
    }
    memory_copy(output, plugin_path, dot * sizeof(wchar_t));
    memory_copy(output + dot, L".ini", 5 * sizeof(wchar_t));
    return 1;
}

// The folder of the game's launcher (Azimuth.exe): relative Folder= values start here.
static void launcher_folder(wchar_t *output, size_t capacity) {
    DWORD length = GetModuleFileNameW(NULL, output, (DWORD)capacity);
    if (!length || length >= capacity) {
        output[0] = 0;
        return;
    }
    while (length && output[length - 1] != L'\\' && output[length - 1] != L'/') {
        --length;
    }
    output[length ? length - 1 : 0] = 0;
}

static void log_keys(void) {
    for (unsigned i = 0; i < KEY_COUNT; ++i) {
        char text[64];
        unsigned key = config.keyboard[i], at = 0;
        const char *label = config_action_name(i);
        if (!key) {
            memory_copy(text, "keyboard none", 14);
        } else if (key >= VK_F1 && key <= VK_F24) {
            memory_copy(text, "keyboard F", 10);
            at = 10;
            unsigned number = key - VK_F1 + 1;
            if (number >= 10) {
                text[at++] = (char)('0' + number / 10);
            }
            text[at++] = (char)('0' + number % 10);
            text[at] = 0;
        } else {
            static const char hex[] = "0123456789ABCDEF";
            memory_copy(text, "keyboard 0x", 11);
            text[11] = hex[key >> 4 & 15];
            text[12] = hex[key & 15];
            text[13] = 0;
        }
        log_append_parts("CUSTOM RADIO: ", label, ": ", text);
    }
}

__declspec(dllexport) BOOL WINAPI FH2PluginInitialize(const FH2PluginContext *context) {
    static wchar_t ini[PATH_CAPACITY], base[PATH_CAPACITY];
    if (!context || context->size < sizeof(*context) || context->api_version < 1) {
        return FALSE;
    }
    log_bind(context->log_utf8);
    log_append("CUSTOM RADIO " CUSTOM_RADIO_VERSION ": starting.\r\n");
    if (!context->game_image || context->game_image_size != GAME_IMAGE_SIZE || !context->plugin_path ||
        !ini_path_for(context->plugin_path, ini, ARRAY_COUNT(ini))) {
        log_append("CUSTOM RADIO: unsupported game image or plugin path; nothing installed.\r\n");
        return FALSE;
    }
    launcher_folder(base, ARRAY_COUNT(base));
    config_load(ini, base, &config);
    log_append_wide("CUSTOM RADIO: settings ", ini);
    log_append_wide("CUSTOM RADIO: music folder ", config.folder);
    log_keys();
    if (!radio_install(context->game_image, &config)) {
        return FALSE;
    }
    return TRUE;
}

__declspec(dllexport) void WINAPI FH2PluginVehicleUpdate(const FH2PluginVehicleFrame *frame) {
    if (!frame || frame->size < sizeof(*frame) || !radio_installed()) {
        return;
    }
    int enabled = frame->input_enabled && game_focused();
    unsigned down = keys_down(&config, key_state ? key_state : GetAsyncKeyState, frame->buttons);
    radio_game_tick(keys_pressed(&edges, down, enabled), enabled);
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

#ifdef CUSTOM_RADIO_TEST
// Test build only: the key state and window focus the vehicle update sees.
__declspec(dllexport) void CustomRadioTestInput(KeyStateFn state, int focused) {
    key_state = state;
    focus_override = focused;
}
__declspec(dllexport) unsigned CustomRadioTestKeyEdges(unsigned down, int enabled) {
    static KeyEdges test_edges;
    return keys_pressed(&test_edges, down, enabled);
}
__declspec(dllexport) int CustomRadioTestConfig(const wchar_t *ini_path, const wchar_t *base_directory,
                                                RadioConfig *output, unsigned size) {
    if (size != sizeof(RadioConfig)) {
        return 0;
    }
    config_load(ini_path, base_directory, output);
    return 1;
}
__declspec(dllexport) int CustomRadioTestParseKey(const wchar_t *text, int xbox, unsigned *value) {
    return xbox ? config_parse_xbox(text, value) : config_parse_keyboard(text, value);
}
__declspec(dllexport) unsigned CustomRadioTestConfigSize(void) {
    return sizeof(RadioConfig);
}
__declspec(dllexport) void CustomRadioTestBindLog(void (*sink)(const char *message)) {
    log_bind(sink);
}
#endif
