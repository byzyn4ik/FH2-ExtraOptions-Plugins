// Azimuth Graphics Options for Forza Horizon 2: an Extra Options mod-loader plugin.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Installed as mods\Azimuth Graphics Options\plugins\AzimuthGraphicsOptions.dll beside a stock
// Extra Options 0.3.2 (Azimuth 1.0.2). From FH2PluginInitialize it checks the
// host (Azimuth 1.0.2), the game image and Azimuth's data\azimuth.ini layout;
// then it adds Azimuth's graphics settings (frame rate, V-sync, resolution,
// display mode, graphics preset, shadows, texture filtering, anti-aliasing,
// HDR) as rows of the game's Video settings page, writes them to
// data\azimuth.ini and applies what it can at once (see live.h). With any
// mismatch it installs nothing and logs why. Settings are in
// AzimuthGraphicsOptions.ini beside the DLL; Extra Options' own INI is not touched.
#include <string>

#include "common.h"
#include "fh2_plugin.h"
#include "live.h"
#include "rows.h"
#include "video_page.h"

#ifdef AZG_TEST_EXPORTS
#include "call_chain.h"
#endif

#define PLUGIN_VERSION "1.0.0"
#define PLUGIN_EXPORT extern "C" __declspec(dllexport)

#ifdef AZG_TEST_EXPORTS
static unsigned char *test_host; // the host image the test build checks instead of its own process
#endif

namespace {

std::wstring ini_path;

std::wstring folder_of(const std::wstring &path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

bool absolute(const std::wstring &path) {
    return (path.size() > 1 && path[1] == L':') || (!path.empty() && (path[0] == L'\\' || path[0] == L'/'));
}

std::wstring ini_text(const wchar_t *section, const wchar_t *key, const wchar_t *fallback) {
    wchar_t value[1024];
    GetPrivateProfileStringW(section, key, fallback, value, 1024, ini_path.c_str());
    std::wstring text = value;
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t')) text.pop_back();
    return text;
}

bool ini_flag(const wchar_t *section, const wchar_t *key, bool fallback) {
    std::wstring value = ini_text(section, key, fallback ? L"1" : L"0");
    return !(value == L"0" || value == L"false" || value == L"False" || value == L"off" || value == L"no");
}

unsigned char *host_image() {
#ifdef AZG_TEST_EXPORTS
    if (test_host) return test_host;
#endif
    return (unsigned char *)GetModuleHandleW(nullptr);
}

std::wstring module_folder(HMODULE module) {
    wchar_t path[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(module, path, MAX_PATH * 4);
    return n && n < MAX_PATH * 4 ? folder_of(std::wstring(path, n)) : std::wstring();
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

PLUGIN_EXPORT BOOL WINAPI FH2PluginInitialize(const FH2PluginContext *context) {
    using namespace azg;
    if (!context || context->size < sizeof(FH2PluginContext) || context->api_version < 1) return FALSE;
    if (!context->plugin_path || !context->game_image) {
        set_log(context->log_utf8, L"");
        say("the loader gave no plugin path or game image; nothing is installed");
        return FALSE;
    }
    std::wstring dll = context->plugin_path;
    size_t dot = dll.find_last_of(L'.');
    ini_path = (dot == std::wstring::npos ? dll : dll.substr(0, dot)) + L".ini";
    set_log(context->log_utf8,
            ini_flag(L"Logging", L"Enabled", true) ? folder_of(dll) + L"\\AzimuthGraphicsOptions.log" : std::wstring());
    if (!ini_flag(L"Graphics", L"Enabled", true)) {
        say("[Graphics] Enabled=0; nothing is installed");
        return TRUE;
    }
    say("plugin " PLUGIN_VERSION " for Extra Options " +
        std::string(context->extra_options_version ? context->extra_options_version : "?"));
    if (context->adapter != FH2_PLUGIN_AZIMUTH) {
        say("the game does not run on Azimuth; nothing is installed");
        return TRUE;
    }
    unsigned char *host = host_image();
    if (const char *problem = live::check_host(host)) {
        say(std::string(problem) + "; nothing is installed");
        return TRUE;
    }
    unsigned char *game = (unsigned char *)context->game_image;
    const char *target = nullptr;
    if (const char *problem = video::check_game(game, context->game_image_size, &target)) {
        say(std::string(problem) + "; nothing is installed and the game is left as it is");
        return TRUE;
    }

    std::wstring azimuth_ini = ini_text(L"Graphics", L"AzimuthIni", L"");
    std::wstring launcher = module_folder(nullptr);
    if (azimuth_ini.empty())
        azimuth_ini = launcher.empty() ? std::wstring() : launcher + L"\\data\\azimuth.ini";
    else if (!absolute(azimuth_ini) && !launcher.empty())
        azimuth_ini = launcher + L"\\" + azimuth_ini;
    rows::set_file(azimuth_ini);
    std::string layout = rows::check_layout();
    if (!layout.empty()) {
        say(layout + "; nothing is installed");
        return TRUE;
    }

    bool apply_live = ini_flag(L"Graphics", L"LiveApply", true);
    LiveState text = {};
    if (apply_live) {
        live::prepare(host, game);
        text = live::state();
    } else {
        say("[Graphics] LiveApply=0: every change applies after a restart");
    }
    if (!video::install(game, text, apply_live)) return TRUE;
    say("Video page rows ready; settings in " + narrow(azimuth_ini) + "; the resource call at 0x16ebb37 is chained to " +
        (target ? target : "?"));
    return TRUE;
}

#ifdef AZG_TEST_EXPORTS
// Test build only (out\tests\AzimuthGraphicsOptionsTest.dll): the same code with
// entry points for tests\run_tests.py and tests\plugin_test.cpp.
PLUGIN_EXPORT void AzgTestUseHost(unsigned char *host) { test_host = host; }

PLUGIN_EXPORT const char *AzgTestCheckHost(const unsigned char *host) { return azg::live::check_host(host); }

PLUGIN_EXPORT const char *AzgTestCheckGame(const unsigned char *game, uint32_t size) {
    const char *target = nullptr;
    const char *problem = azg::video::check_game(game, size, &target);
    return problem ? problem : target;
}

PLUGIN_EXPORT size_t AzgTestExtendXaml(const char *text, size_t length, char *output, size_t capacity, int polish,
                                       int live) {
    azg::LiveState state = {(live & 1) != 0, (live & 2) != 0, (live & 4) != 0, (live & 8) != 0};
    const char *why = nullptr;
    return azg::video::extend_xaml(text, length, output, capacity, polish ? azg::kPolish : azg::kEnglish, state, &why);
}

PLUGIN_EXPORT void AzgTestRowsFile(const wchar_t *path) { azg::rows::set_file(path); }
PLUGIN_EXPORT const char *AzgTestRowsLayout() {
    static std::string text;
    text = azg::rows::check_layout();
    return text.c_str();
}
PLUGIN_EXPORT int AzgTestRowGet(unsigned row) { return azg::rows::get(row); }
PLUGIN_EXPORT int AzgTestRowSet(unsigned row, int value) { return azg::rows::set(row, value); }
PLUGIN_EXPORT int AzgTestRowShown(unsigned row) { return azg::rows::shown(row); }

PLUGIN_EXPORT int AzgTestFrameRates(unsigned refresh, int *out, int capacity) {
    std::vector<int> rates = azg::live::frame_rates(refresh);
    for (size_t i = 0; i < rates.size() && (int)i < capacity; ++i) out[i] = rates[i];
    return (int)rates.size();
}
PLUGIN_EXPORT void AzgTestSetFrameRates(const int *rates, int count) {
    azg::rows::set_frame_rates(std::vector<int>(rates, rates + count));
}
PLUGIN_EXPORT const wchar_t *AzgTestRowChoice(unsigned row, unsigned index, int polish) {
    return azg::rows::choice(row, index, polish ? azg::kPolish : azg::kEnglish);
}
PLUGIN_EXPORT unsigned AzgTestRowCount(unsigned row) { return azg::rows::count(row); }

PLUGIN_EXPORT int AzgTestChain(unsigned char *site, void *handler, void **previous) {
    return (int)azg::chain_call(site, handler, previous);
}
#endif
