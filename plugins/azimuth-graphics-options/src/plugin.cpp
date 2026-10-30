// Azimuth Graphics Options for Forza Horizon 2: an Extra Options mod-loader plugin.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Installed as mods\Azimuth Graphics Options\plugins\AzimuthGraphicsOptions.dll
// beside a stock Extra Options (0.4.4 on Azimuth 1.0.4; 0.3.2 on 1.0.2 works
// the same way). From FH2PluginInitialize it identifies the host (Azimuth
// 1.0.4 or 1.0.2), checks the game image and Azimuth's data\azimuth.ini
// layout; then it adds Azimuth's graphics settings (frame rate, V-sync,
// resolution, display mode, graphics preset, shadows, texture filtering,
// anti-aliasing, HDR) as rows of the game's Video settings page and its sound
// settings (audio output, volume) as rows of the Audio page, writes them to
// data\azimuth.ini and applies what it can at once (see live.h). With any
// mismatch it installs nothing and logs why. Settings are in
// AzimuthGraphicsOptions.ini beside the DLL; Extra Options' own INI is not
// touched (only read for its custom language, see language.h).
#include <string>

#include "common.h"
#include "fh2_game.h"
#include "fh2_plugin.h"
#include "language.h"
#include "live.h"
#include "rows.h"
#include "settings_pages.h"

#ifdef AZG_TEST_EXPORTS
#include "call_chain.h"
#endif

#define PLUGIN_VERSION "0.0.1"
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

std::wstring trimmed(const wchar_t *value) {
    std::wstring text = value;
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t')) text.pop_back();
    return text;
}

std::wstring ini_text(const wchar_t *section, const wchar_t *key, const wchar_t *fallback) {
    wchar_t value[1024];
    GetPrivateProfileStringW(section, key, fallback, value, 1024, ini_path.c_str());
    return trimmed(value);
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

// The game's Mount folder: the game executable's folder, else the title's
// folder in azimuth.ini (relative to the launcher) + \Mount.
std::wstring mount_folder(void *game, const std::wstring &launcher, const std::wstring &azimuth_ini) {
    std::wstring folder = module_folder((HMODULE)game);
    if (!folder.empty() && folder != launcher) return folder;
    wchar_t value[1024];
    GetPrivateProfileStringW(L"title-1a96545b", L"folder", L"", value, 1024, azimuth_ini.c_str());
    std::wstring title = trimmed(value);
    if (title.empty()) return std::wstring();
    return (absolute(title) || launcher.empty() ? title : launcher + L"\\" + title) + L"\\Mount";
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
    std::wstring ini(dll.size() + 5, L' ');
    ini_path = fh2_plugin_ini_path(dll.c_str(), &ini[0], ini.size()) ? ini.c_str() : dll + L".ini";
    set_log(context->log_utf8,
            ini_flag(L"Logging", L"Enabled", true) ? folder_of(dll) + L"\\AzimuthGraphicsOptions.log" : std::wstring());
    bool video = ini_flag(L"Graphics", L"Enabled", true), audio = ini_flag(L"Audio", L"Enabled", true);
    if (!video && !audio) {
        say("[Graphics] and [Audio] Enabled=0; nothing is installed");
        return TRUE;
    }
    rows::enable_page(kVideoPage, video);
    rows::enable_page(kAudioPage, audio);
    say("plugin " PLUGIN_VERSION " for Extra Options " +
        std::string(context->extra_options_version ? context->extra_options_version : "?"));
    if (context->adapter != FH2_PLUGIN_AZIMUTH) {
        say("the game does not run on Azimuth; nothing is installed");
        return TRUE;
    }
    unsigned char *host = host_image();
    const char *problem = nullptr;
    const live::Host *build = live::identify(host, &problem);
    if (!build) {
        say(std::string(problem) + "; nothing is installed");
        return TRUE;
    }
    say(std::string("host: Azimuth ") + live::host_name(build));
    unsigned char *game = (unsigned char *)context->game_image;
    const char *target = nullptr;
    if (const char *mismatch = pages::check_game(game, context->game_image_size, &target)) {
        say(std::string(mismatch) + "; nothing is installed and the game is left as it is");
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
    language::configure(language::parse(ini_text(L"Captions", L"Language", L"auto")), launcher,
                        mount_folder(context->game_image, launcher, azimuth_ini));

    bool apply_live = ini_flag(L"Graphics", L"LiveApply", true);
    LiveState text = {};
    if (apply_live) {
        live::prepare(build, host, game);
        text = live::state();
    } else {
        say("[Graphics] LiveApply=0: every change applies after a restart");
    }
    if (!pages::install(game, text, apply_live)) return TRUE;
    say(std::string(video && audio ? "Video and Audio page rows" : video ? "Video page rows" : "Audio page rows") +
        " ready; settings in " + narrow(azimuth_ini) + "; the resource call at 0x16ebb37 is chained to " +
        (target ? target : "?"));
    return TRUE;
}

#ifdef AZG_TEST_EXPORTS
// Test build only (out\tests\AzimuthGraphicsOptionsTest.dll): the same code with
// entry points for tests\run_tests.py and tests\plugin_test.cpp.
PLUGIN_EXPORT void AzgTestUseHost(unsigned char *host) { test_host = host; }

// nullptr when `host` is a known build, else why not.
PLUGIN_EXPORT const char *AzgTestCheckHost(const unsigned char *host) {
    const char *why = nullptr;
    return azg::live::identify(host, &why) ? nullptr : why;
}
PLUGIN_EXPORT const char *AzgTestHostName(const unsigned char *host) {
    const char *why = nullptr;
    const azg::live::Host *build = azg::live::identify(host, &why);
    return build ? azg::live::host_name(build) : nullptr;
}
PLUGIN_EXPORT uint32_t AzgTestHostSite(const unsigned char *host, unsigned which) {
    const char *why = nullptr;
    return azg::live::test_site(azg::live::identify(host, &why), which);
}

PLUGIN_EXPORT const char *AzgTestCheckGame(const unsigned char *game, uint32_t size) {
    const char *target = nullptr;
    const char *problem = azg::pages::check_game(game, size, &target);
    return problem ? problem : target;
}

// live: bit 0 frame rate, 1 display, 2 shadow distance, 3 shadow rate, 4 volume.
PLUGIN_EXPORT size_t AzgTestExtendXaml(int page, const char *text, size_t length, char *output, size_t capacity,
                                       int language, int live) {
    azg::LiveState state = {(live & 1) != 0, (live & 2) != 0, (live & 4) != 0, (live & 8) != 0, (live & 16) != 0};
    const char *why = nullptr;
    azg::Language lang = (azg::Language)(language >= 0 && language <= 2 ? language : 0);
    return page ? azg::pages::extend_audio(text, length, output, capacity, lang, state, &why)
                : azg::pages::extend_video(text, length, output, capacity, lang, state, &why);
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
PLUGIN_EXPORT const wchar_t *AzgTestRowChoice(unsigned row, unsigned index, int language) {
    return azg::rows::choice(row, index, (azg::Language)(language >= 0 && language <= 2 ? language : 0));
}
PLUGIN_EXPORT unsigned AzgTestRowCount(unsigned row) { return azg::rows::count(row); }

// choice: 0 auto, 1 en, 2 pl, 3 uk; pack: a Ukrainian pack in the ru-RU slot.
PLUGIN_EXPORT void AzgTestLanguage(int choice, int pack) {
    azg::language::test_set((azg::language::Choice)choice, pack != 0);
}
PLUGIN_EXPORT int AzgTestCurrentLanguage(const unsigned char *game) { return (int)azg::language::current(game); }
PLUGIN_EXPORT int AzgTestConfigureLanguage(const wchar_t *launcher, const wchar_t *mount) {
    azg::language::configure(azg::language::kAuto, launcher, mount);
    return azg::language::ukrainian_pack();
}

PLUGIN_EXPORT int AzgTestChain(unsigned char *site, void *handler, void **previous) {
    return (int)azg::chain_call(site, handler, previous);
}
#endif
