// Azimuth Graphics Options for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Values, captions (English and Polish, the languages of the fork's rows that
// Extra Options 0.3.2 also has) and keys of the Azimuth rows, from the fork's
// features/graphics_launcher.c.
#include "rows.h"

#include "common.h"

namespace azg {
namespace rows {

namespace {

const wchar_t *const off_on[] = {L"off", L"on"};
const wchar_t *const pacings[] = {L"fixed", L"real-time"};
const wchar_t *const resolutions[] = {L"540", L"720", L"900", L"1080", L"1260",
                                      L"1440", L"1620", L"1800", L"1980", L"2160"};
const wchar_t *const displays[] = {L"windowed", L"fullscreen"};
const wchar_t *const presets[] = {L"low", L"default", L"high", L"ultra", L"custom"};
const wchar_t *const qualities[] = {L"low", L"default", L"high", L"ultra"};
const wchar_t *const filterings[] = {L"default", L"2x", L"4x", L"8x", L"16x"};
const wchar_t *const antialiasing[] = {L"off", L"msaax2", L"msaax4"};

const wchar_t *const off_on_en[] = {L"Off", L"On"}, *const off_on_pl[] = {L"Wył.", L"Wł."};
const wchar_t *const pacing_en[] = {L"Fixed", L"Real-time"}, *const pacing_pl[] = {L"Stałe", L"Czas rzeczywisty"};
const wchar_t *const resolution_labels[] = {L"960 x 540",   L"1280 x 720",  L"1600 x 900",  L"1920 x 1080",
                                            L"2240 x 1260", L"2560 x 1440", L"2880 x 1620", L"3200 x 1800",
                                            L"3520 x 1980", L"3840 x 2160 (4K)"};
const wchar_t *const display_en[] = {L"Windowed", L"Fullscreen"}, *const display_pl[] = {L"Okno", L"Pełny ekran"};
const wchar_t *const preset_en[] = {L"Low", L"Default", L"High", L"Ultra", L"Custom"},
                     *const preset_pl[] = {L"Niska", L"Domyślna", L"Wysoka", L"Ultra", L"Własna"};
const wchar_t *const filtering_en[] = {L"Default", L"2x", L"4x", L"8x", L"16x"},
                     *const filtering_pl[] = {L"Domyślna", L"2x", L"4x", L"8x", L"16x"};
const wchar_t *const aa_en[] = {L"Off", L"2x MSAA", L"4x MSAA"}, *const aa_pl[] = {L"Wył.", L"2x MSAA", L"4x MSAA"};

#define RESTART_EN " Restart the game to apply."
#define RESTART_PL " Uruchom grę ponownie, aby zastosować."

struct RowDef {
    const char *property, *collection;
    const wchar_t *key; // in [title-1a96545b], or [launcher] for display
    const wchar_t *const *values;
    unsigned count, fallback;
    bool custom_detail; // choosing it sets graphics=custom
    const wchar_t *const *labels_en, *const *labels_pl;
    const char *name_en, *name_pl;
};

const RowDef table[kRowCount] = {
    {"GfxFrameRate", "GfxFrameRateChoices", L"frame_rate", nullptr, 0, 0, false, nullptr, nullptr,
     "Frame rate", "Liczba klatek"},
    {"GfxVSync", "GfxVSyncChoices", L"vsync", off_on, 2, 1, false, off_on_en, off_on_pl, "V-sync",
     "Synchronizacja pionowa"},
    {"GfxFramePacing", "GfxFramePacingChoices", L"frame_pacing", pacings, 2, 1, false, pacing_en, pacing_pl,
     "Frame pacing", "Taktowanie klatek"},
    {"GfxResolution", "GfxResolutionChoices", L"resolution", resolutions, 10, 3, false, resolution_labels,
     resolution_labels, "Base resolution (restart)", "Bazowa rozdzielczość (restart)"},
    {"GfxDisplay", "GfxDisplayChoices", L"display", displays, 2, 1, false, display_en, display_pl, "Display mode",
     "Tryb wyświetlania"},
    {"GfxPreset", "GfxPresetChoices", L"graphics", presets, 5, 1, false, preset_en, preset_pl, "Graphics",
     "Grafika"},
    {"GfxShadows", "GfxShadowsChoices", L"shadow_detail", qualities, 4, 1, true, preset_en, preset_pl, "Shadows",
     "Cienie"},
    {"GfxTextureFiltering", "GfxTextureFilteringChoices", L"texture_filtering", filterings, 5, 0, true,
     filtering_en, filtering_pl, "Texture filtering", "Filtrowanie tekstur"},
    {"GfxAntiAliasing", "GfxAntiAliasingChoices", L"antialiasing", antialiasing, 3, 2, false, aa_en, aa_pl,
     "Anti-aliasing", "Wygładzanie krawędzi"},
    {"GfxHdr", "GfxHdrChoices", L"hdr", off_on, 2, 0, false, off_on_en, off_on_pl, "HDR", "HDR"},
};

const wchar_t kTitle[] = L"title-1a96545b";
const wchar_t kLauncher[] = L"launcher";

std::wstring ini;
std::vector<int> rates = {30, 60};
std::vector<std::wstring> rate_labels = {L"30 FPS", L"60 FPS"};

// As Azimuth's launcher (0x10a80): the highest choice at or below the saved rate.
int rate_index(int saved) {
    int index = 0;
    for (size_t i = 0; i < rates.size(); ++i)
        if (rates[i] <= saved) index = (int)i;
    return index;
}

const wchar_t *section(unsigned row) { return row == kDisplay ? kLauncher : kTitle; }

bool has_section(const std::wstring &names, const wchar_t *wanted) {
    for (size_t at = 0; at < names.size() && names[at];) {
        size_t end = names.find(L'\0', at);
        if (end == std::wstring::npos) end = names.size();
        if (!lstrcmpiW(names.substr(at, end - at).c_str(), wanted)) return true;
        at = end + 1;
    }
    return false;
}

} // namespace

void set_file(const std::wstring &azimuth_ini) { ini = azimuth_ini; }

bool rates_set;

bool frame_rates_set() { return rates_set; }

void set_frame_rates(const std::vector<int> &list) {
    if (list.empty()) return;
    rates_set = true;
    rates = list;
    rate_labels.clear();
    for (int rate : rates) rate_labels.push_back(std::to_wstring(rate) + L" FPS");
}

const std::vector<int> &frame_rates() { return rates; }

int frame_rate_value(int index) { return index >= 0 && (size_t)index < rates.size() ? rates[(size_t)index] : 0; }

const std::wstring &file() { return ini; }

std::string check_layout() {
    if (ini.empty()) return "no data\\azimuth.ini path";
    DWORD attributes = GetFileAttributesW(ini.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
        return narrow(ini) + " does not exist";
    std::wstring names(32768, L'\0');
    DWORD n = GetPrivateProfileSectionNamesW(&names[0], (DWORD)names.size(), ini.c_str());
    names.resize(n);
    if (!has_section(names, kLauncher)) return narrow(ini) + " has no [launcher] section";
    if (!has_section(names, kTitle))
        return narrow(ini) + " has no [title-1a96545b] section (Azimuth 1.x keeps the game's settings there)";
    return std::string();
}

bool shown(unsigned row) { return row < kRowCount && row != kFramePacing; }

unsigned count(unsigned row) {
    if (row == kFrameRate) return (unsigned)rates.size();
    return row < kRowCount ? table[row].count : 0;
}

int get(unsigned row) {
    if (!shown(row)) return 0;
    const RowDef &r = table[row];
    if (row == kFrameRate) {
        // Azimuth's default when the key is absent or not a rate: 60.
        int saved = ini.empty() ? 60 : (int)GetPrivateProfileIntW(section(row), r.key, 60, ini.c_str());
        return rate_index(saved >= 30 && saved <= 1000 ? saved : 60);
    }
    wchar_t value[64];
    if (ini.empty() || !GetPrivateProfileStringW(section(row), r.key, L"", value, 64, ini.c_str()))
        return (int)r.fallback;
    for (unsigned i = 0; i < r.count; ++i)
        if (!lstrcmpiW(value, r.values[i])) return (int)i;
    return (int)r.fallback;
}

bool set(unsigned row, int value) {
    if (!shown(row) || value < 0 || (unsigned)value >= count(row) || ini.empty()) return false;
    // Frame rate: unchanged only when the saved rate is exactly this choice
    // (a saved rate between choices shows as the lower one).
    if (row == kFrameRate ? (int)GetPrivateProfileIntW(kTitle, L"frame_rate", 0, ini.c_str()) == rates[(size_t)value]
                          : get(row) == value)
        return false;
    const RowDef &r = table[row];
    std::wstring rate = row == kFrameRate ? std::to_wstring(rates[(size_t)value]) : std::wstring();
    const wchar_t *text = row == kFrameRate ? rate.c_str() : r.values[value];
    if (!WritePrivateProfileStringW(section(row), r.key, text, ini.c_str())) return false;
    if (row == kShadows) {
        WritePrivateProfileStringW(kTitle, L"shadow_distance", text, ini.c_str());
        WritePrivateProfileStringW(kTitle, L"shadow_redraw", text, ini.c_str());
    }
    if (r.custom_detail) WritePrivateProfileStringW(kTitle, L"graphics", L"custom", ini.c_str());
    return true;
}

int shadow_level() {
    int preset = get(kPreset);
    return preset == 4 ? get(kShadows) : preset;
}

const char *property(unsigned row) { return row < kRowCount ? table[row].property : nullptr; }

const char *collection(unsigned row) { return row < kRowCount ? table[row].collection : nullptr; }

const wchar_t *choice(unsigned row, unsigned index, Language language) {
    if (row >= kRowCount || index >= count(row)) return nullptr;
    if (row == kFrameRate) return rate_labels[index].c_str();
    return (language == kPolish ? table[row].labels_pl : table[row].labels_en)[index];
}

const char *name(unsigned row, Language language) {
    if (row >= kRowCount) return nullptr;
    return language == kPolish ? table[row].name_pl : table[row].name_en;
}

std::string description(unsigned row, Language language, const LiveState &live) {
    bool pl = language == kPolish;
    switch (row) {
    case kFrameRate:
        return live.frame_rate ? (pl ? "Docelowa liczba klatek. Działa od razu." : "Target frame rate. Applies at once.")
                               : (pl ? "Docelowa liczba klatek." RESTART_PL : "Target frame rate." RESTART_EN);
    case kVSync:
        return pl ? "Czekaj na odświeżenie ekranu. Wyłączona pozwala na 120 FPS na każdym ekranie." RESTART_PL
                  : "Wait for the display's refresh. Off allows 120 FPS on any screen." RESTART_EN;
    case kFramePacing:
        return pl ? "Czas rzeczywisty utrzymuje prawidłową szybkość gry powyżej 30/60 FPS." RESTART_PL
                  : "Real-time keeps game speed correct above 30/60 FPS." RESTART_EN;
    case kResolution:
        return pl ? "Bazowa rozdzielczość renderowania. Wyższa zwiększa obciążenie GPU. Obejmuje interfejs." RESTART_PL
                  : "Base rendering resolution. Higher resolution increases GPU load. Includes the interface."
                    RESTART_EN;
    case kDisplay:
        return live.display ? (pl ? "Okno lub pełny ekran. Działa od razu, jak Alt+Enter."
                                  : "Window or fullscreen. Applies at once, as Alt+Enter does.")
                            : (pl ? "Okno lub pełny ekran." RESTART_PL : "Window or fullscreen." RESTART_EN);
    case kPreset:
        if (live.shadow_distance && live.shadow_rate)
            return pl ? "Ogólne ustawienie jakości. Własna używa ustawień poniżej. Zasięg i odświeżanie cieni "
                        "działają od razu, reszta po ponownym uruchomieniu."
                      : "Overall quality preset. Custom uses the settings below. Shadow distance and shadow "
                        "update rate apply at once, the rest after a restart.";
        if (live.shadow_distance)
            return pl ? "Ogólne ustawienie jakości. Własna używa ustawień poniżej. Zasięg cieni działa od razu, "
                        "reszta po ponownym uruchomieniu."
                      : "Overall quality preset. Custom uses the settings below. Shadow distance applies at once, "
                        "the rest after a restart.";
        return pl ? "Ogólne ustawienie jakości. Własna używa ustawień poniżej." RESTART_PL
                  : "Overall quality preset. Custom uses the settings below." RESTART_EN;
    case kShadows:
        if (live.shadow_distance && live.shadow_rate)
            return pl ? "Jakość cieni. Ustawia grafikę na Własną. Zasięg i odświeżanie cieni działają od razu, "
                        "ostrość (rozmiar mapy cieni) po ponownym uruchomieniu."
                      : "Shadow quality. Sets Graphics to Custom. Shadow distance and update rate apply at once, "
                        "sharpness (shadow map size) after a restart.";
        if (live.shadow_distance)
            return pl ? "Jakość cieni. Ustawia grafikę na Własną. Zasięg cieni działa od razu, reszta po "
                        "ponownym uruchomieniu."
                      : "Shadow quality. Sets Graphics to Custom. Shadow distance applies at once, the rest "
                        "after a restart.";
        return pl ? "Jakość cieni. Ustawia grafikę na Własną." RESTART_PL
                  : "Shadow quality. Sets Graphics to Custom." RESTART_EN;
    case kTextureFiltering:
        return pl ? "Filtrowanie anizotropowe. Ustawia grafikę na Własną." RESTART_PL
                  : "Anisotropic filtering. Sets Graphics to Custom." RESTART_EN;
    case kAntiAliasing:
        return pl ? "Wygładzanie wielopróbkowe." RESTART_PL : "Multisample anti-aliasing." RESTART_EN;
    case kHdr:
        return pl ? "Obraz HDR na ekranie z obsługą HDR." RESTART_PL
                  : "High dynamic range output on an HDR display." RESTART_EN;
    }
    return std::string();
}

} // namespace rows
} // namespace azg
