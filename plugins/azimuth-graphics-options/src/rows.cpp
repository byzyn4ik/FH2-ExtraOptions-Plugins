// Azimuth Graphics Options for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Values, captions (English, Polish and Ukrainian, as the Extra Options fork's
// features/graphics_launcher.c had them) and keys of the Azimuth rows.
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
// Azimuth's audio values (its reader takes exactly these three).
const wchar_t *const audio_outputs[] = {L"stereo", L"system", L"multichannel"};

const wchar_t *const off_on_en[] = {L"Off", L"On"}, *const off_on_pl[] = {L"Wył.", L"Wł."},
                     *const off_on_uk[] = {L"Вимк", L"Увімк"};
const wchar_t *const pacing_en[] = {L"Fixed", L"Real-time"}, *const pacing_pl[] = {L"Stałe", L"Czas rzeczywisty"},
                     *const pacing_uk[] = {L"Фіксований", L"Реальний час"};
const wchar_t *const resolution_labels[] = {L"960 x 540",   L"1280 x 720",  L"1600 x 900",  L"1920 x 1080",
                                            L"2240 x 1260", L"2560 x 1440", L"2880 x 1620", L"3200 x 1800",
                                            L"3520 x 1980", L"3840 x 2160 (4K)"};
const wchar_t *const display_en[] = {L"Windowed", L"Fullscreen"}, *const display_pl[] = {L"Okno", L"Pełny ekran"},
                     *const display_uk[] = {L"У вікні", L"Повний екран"};
const wchar_t *const preset_en[] = {L"Low", L"Default", L"High", L"Ultra", L"Custom"},
                     *const preset_pl[] = {L"Niska", L"Domyślna", L"Wysoka", L"Ultra", L"Własna"},
                     *const preset_uk[] = {L"Низька", L"Стандартна", L"Висока", L"Ультра", L"Власна"};
const wchar_t *const filtering_en[] = {L"Default", L"2x", L"4x", L"8x", L"16x"},
                     *const filtering_pl[] = {L"Domyślna", L"2x", L"4x", L"8x", L"16x"},
                     *const filtering_uk[] = {L"Стандартна", L"2x", L"4x", L"8x", L"16x"};
const wchar_t *const aa_en[] = {L"Off", L"2x MSAA", L"4x MSAA"}, *const aa_pl[] = {L"Wył.", L"2x MSAA", L"4x MSAA"},
                     *const aa_uk[] = {L"Вимк", L"2x MSAA", L"4x MSAA"};
const wchar_t *const audio_en[] = {L"Stereo", L"System (Windows)", L"Multichannel"},
                     *const audio_pl[] = {L"Stereo", L"System (Windows)", L"Wielokanałowe"},
                     *const audio_uk[] = {L"Стерео", L"Система (Windows)", L"Багатоканальний"};

#define RESTART_EN " Restart the game to apply."
#define RESTART_PL " Uruchom grę ponownie, aby zastosować."
#define RESTART_UK " Перезапустіть гру, щоб застосувати."

struct RowDef {
    const char *property, *collection;
    const wchar_t *key; // in [title-1a96545b], or [launcher] for display
    const wchar_t *const *values;
    unsigned count, fallback;
    bool custom_detail; // choosing it sets graphics=custom
    const wchar_t *const *labels[3];
    const char *names[3];
};

const RowDef table[kRowCount] = {
    {"GfxFrameRate", "GfxFrameRateChoices", L"frame_rate", nullptr, 0, 0, false, {nullptr, nullptr, nullptr},
     {"Frame rate", "Liczba klatek", "Частота кадрів"}},
    {"GfxVSync", "GfxVSyncChoices", L"vsync", off_on, 2, 1, false, {off_on_en, off_on_pl, off_on_uk},
     {"V-sync", "Synchronizacja pionowa", "Вертикальна синхронізація"}},
    {"GfxFramePacing", "GfxFramePacingChoices", L"frame_pacing", pacings, 2, 1, false,
     {pacing_en, pacing_pl, pacing_uk}, {"Frame pacing", "Taktowanie klatek", "Тактування кадрів"}},
    {"GfxResolution", "GfxResolutionChoices", L"resolution", resolutions, 10, 3, false,
     {resolution_labels, resolution_labels, resolution_labels},
     {"Base resolution (restart)", "Bazowa rozdzielczość (restart)", "Базова роздільність (перезапуск)"}},
    {"GfxDisplay", "GfxDisplayChoices", L"display", displays, 2, 1, false, {display_en, display_pl, display_uk},
     {"Display mode", "Tryb wyświetlania", "Режим екрана"}},
    {"GfxPreset", "GfxPresetChoices", L"graphics", presets, 5, 1, false, {preset_en, preset_pl, preset_uk},
     {"Graphics", "Grafika", "Графіка"}},
    {"GfxShadows", "GfxShadowsChoices", L"shadow_detail", qualities, 4, 1, true, {preset_en, preset_pl, preset_uk},
     {"Shadows", "Cienie", "Тіні"}},
    {"GfxTextureFiltering", "GfxTextureFilteringChoices", L"texture_filtering", filterings, 5, 0, true,
     {filtering_en, filtering_pl, filtering_uk}, {"Texture filtering", "Filtrowanie tekstur", "Фільтрація текстур"}},
    {"GfxAntiAliasing", "GfxAntiAliasingChoices", L"antialiasing", antialiasing, 3, 2, false, {aa_en, aa_pl, aa_uk},
     {"Anti-aliasing", "Wygładzanie krawędzi", "Згладжування"}},
    {"GfxHdr", "GfxHdrChoices", L"hdr", off_on, 2, 0, false, {off_on_en, off_on_pl, off_on_uk},
     {"HDR", "HDR", "HDR"}},
    {"AzAudioOutput", "AzAudioOutputChoices", L"audio", audio_outputs, 3, 0, false, {audio_en, audio_pl, audio_uk},
     {"Audio output (Azimuth)", "Wyjście audio (Azimuth)", "Аудіовихід (Azimuth)"}},
    {"AzVolume", nullptr, L"volume", nullptr, 0, 0, false, {nullptr, nullptr, nullptr},
     {"Volume (Azimuth)", "Głośność (Azimuth)", "Гучність (Azimuth)"}},
};

const wchar_t kTitle[] = L"title-1a96545b";
const wchar_t kLauncher[] = L"launcher";
// Azimuth's frame rate when frame_rate is absent or not a rate: the title's
// own rate (its title table: 30 for FH2). Its volume default: 100.
constexpr int kTitleRate = 30, kDefaultVolume = 100;

std::wstring ini;
std::vector<int> rates = {30, 60};
std::vector<std::wstring> rate_labels = {L"30 FPS", L"60 FPS"};
bool rates_set;
bool page_on[2] = {true, true};

// As Azimuth's launcher: the highest choice at or below the saved rate.
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

// A whole decimal number of up to 4 digits (as Azimuth reads frame_rate and
// volume), else -1.
int read_number(const wchar_t *key) {
    wchar_t value[16];
    if (ini.empty() || !GetPrivateProfileStringW(kTitle, key, L"", value, 16, ini.c_str()) || !value[0]) return -1;
    int number = 0, digits = 0;
    for (const wchar_t *c = value; *c; ++c, ++digits) {
        if (*c < L'0' || *c > L'9' || digits >= 4) return -1;
        number = number * 10 + (*c - L'0');
    }
    return number;
}

int saved_rate() {
    int saved = read_number(L"frame_rate");
    return saved >= 30 && saved <= 1000 ? saved : kTitleRate;
}

int saved_volume() {
    int saved = read_number(L"volume");
    return saved >= 0 && saved <= 100 ? saved : kDefaultVolume;
}

} // namespace

void set_file(const std::wstring &azimuth_ini) { ini = azimuth_ini; }

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

void enable_page(Page which, bool on) { page_on[which] = on; }

Page page(unsigned row) { return row == kAudioOutput || row == kVolume ? kAudioPage : kVideoPage; }

bool shown(unsigned row) { return row < kRowCount && row != kFramePacing && page_on[page(row)]; }

bool slider(unsigned row) { return row == kVolume; }

unsigned count(unsigned row) {
    if (row == kFrameRate) return (unsigned)rates.size();
    if (row == kVolume) return 101;
    return row < kRowCount ? table[row].count : 0;
}

int get(unsigned row) {
    if (!shown(row)) return 0;
    if (row == kFrameRate) return rate_index(saved_rate());
    if (row == kVolume) return saved_volume();
    const RowDef &r = table[row];
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
    // (a saved rate between choices shows as the lower one). Volume: when the
    // saved key holds this number.
    if (row == kFrameRate   ? read_number(L"frame_rate") == rates[(size_t)value]
        : row == kVolume    ? read_number(L"volume") == value
                            : get(row) == value)
        return false;
    const RowDef &r = table[row];
    std::wstring number = row == kFrameRate ? std::to_wstring(rates[(size_t)value])
                          : row == kVolume  ? std::to_wstring(value)
                                            : std::wstring();
    const wchar_t *text = number.empty() ? r.values[value] : number.c_str();
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
    if (row >= kRowCount || slider(row) || index >= count(row)) return nullptr;
    if (row == kFrameRate) return rate_labels[index].c_str();
    return table[row].labels[language][index];
}

const char *name(unsigned row, Language language) { return row < kRowCount ? table[row].names[language] : nullptr; }

std::string description(unsigned row, Language language, const LiveState &live) {
    // English, Polish, Ukrainian.
    auto pick = [language](const char *en, const char *pl, const char *uk) {
        return std::string(language == kPolish ? pl : language == kUkrainian ? uk : en);
    };
    switch (row) {
    case kFrameRate:
        return live.frame_rate ? pick("Target frame rate. Applies at once.", "Docelowa liczba klatek. Działa od razu.",
                                      "Цільова частота кадрів. Діє одразу.")
                               : pick("Target frame rate." RESTART_EN, "Docelowa liczba klatek." RESTART_PL,
                                      "Цільова частота кадрів." RESTART_UK);
    case kVSync:
        return pick("Wait for the display's refresh. Off allows 120 FPS on any screen." RESTART_EN,
                    "Czekaj na odświeżenie ekranu. Wyłączona pozwala na 120 FPS na każdym ekranie." RESTART_PL,
                    "Чекати на оновлення екрана. Вимк дозволяє 120 FPS на будь-якому екрані." RESTART_UK);
    case kFramePacing:
        return pick("Real-time keeps game speed correct above 30/60 FPS." RESTART_EN,
                    "Czas rzeczywisty utrzymuje prawidłową szybkość gry powyżej 30/60 FPS." RESTART_PL,
                    "Реальний час зберігає правильну швидкість гри понад 30/60 FPS." RESTART_UK);
    case kResolution:
        return pick("Base rendering resolution. Higher resolution increases GPU load. Includes the interface." RESTART_EN,
                    "Bazowa rozdzielczość renderowania. Wyższa zwiększa obciążenie GPU. Obejmuje interfejs." RESTART_PL,
                    "Базова роздільність рендерингу. Вища роздільність більше навантажує GPU. Включає "
                    "інтерфейс." RESTART_UK);
    case kDisplay:
        return live.display ? pick("Window or fullscreen. Applies at once, as Alt+Enter does.",
                                   "Okno lub pełny ekran. Działa od razu, jak Alt+Enter.",
                                   "У вікні або на весь екран. Діє одразу, як Alt+Enter.")
                            : pick("Window or fullscreen." RESTART_EN, "Okno lub pełny ekran." RESTART_PL,
                                   "У вікні або на весь екран." RESTART_UK);
    case kPreset:
        if (live.shadow_distance && live.shadow_rate)
            return pick("Overall quality preset. Custom uses the settings below. Shadow distance and shadow update "
                        "rate apply at once, the rest after a restart.",
                        "Ogólne ustawienie jakości. Własna używa ustawień poniżej. Zasięg i odświeżanie cieni działają "
                        "od razu, reszta po ponownym uruchomieniu.",
                        "Загальний рівень якості. Власна використовує налаштування нижче. Дальність і частота "
                        "оновлення тіней діють одразу, решта після перезапуску.");
        if (live.shadow_distance)
            return pick("Overall quality preset. Custom uses the settings below. Shadow distance applies at once, the "
                        "rest after a restart.",
                        "Ogólne ustawienie jakości. Własna używa ustawień poniżej. Zasięg cieni działa od razu, reszta "
                        "po ponownym uruchomieniu.",
                        "Загальний рівень якості. Власна використовує налаштування нижче. Дальність тіней діє одразу, "
                        "решта після перезапуску.");
        return pick("Overall quality preset. Custom uses the settings below." RESTART_EN,
                    "Ogólne ustawienie jakości. Własna używa ustawień poniżej." RESTART_PL,
                    "Загальний рівень якості. Власна використовує налаштування нижче." RESTART_UK);
    case kShadows:
        if (live.shadow_distance && live.shadow_rate)
            return pick("Shadow quality. Sets Graphics to Custom. Shadow distance and update rate apply at once, "
                        "sharpness (shadow map size) after a restart.",
                        "Jakość cieni. Ustawia grafikę na Własną. Zasięg i odświeżanie cieni działają od razu, "
                        "ostrość (rozmiar mapy cieni) po ponownym uruchomieniu.",
                        "Якість тіней. Встановлює графіку на Власну. Дальність і частота оновлення тіней діють "
                        "одразу, чіткість (розмір карти тіней) після перезапуску.");
        if (live.shadow_distance)
            return pick("Shadow quality. Sets Graphics to Custom. Shadow distance applies at once, the rest after a "
                        "restart.",
                        "Jakość cieni. Ustawia grafikę na Własną. Zasięg cieni działa od razu, reszta po ponownym "
                        "uruchomieniu.",
                        "Якість тіней. Встановлює графіку на Власну. Дальність тіней діє одразу, решта після "
                        "перезапуску.");
        return pick("Shadow quality. Sets Graphics to Custom." RESTART_EN,
                    "Jakość cieni. Ustawia grafikę na Własną." RESTART_PL,
                    "Якість тіней. Встановлює графіку на Власну." RESTART_UK);
    case kTextureFiltering:
        return pick("Anisotropic filtering. Sets Graphics to Custom." RESTART_EN,
                    "Filtrowanie anizotropowe. Ustawia grafikę na Własną." RESTART_PL,
                    "Анізотропна фільтрація. Встановлює графіку на Власну." RESTART_UK);
    case kAntiAliasing:
        return pick("Multisample anti-aliasing." RESTART_EN, "Wygładzanie wielopróbkowe." RESTART_PL,
                    "Багатовибіркове згладжування." RESTART_UK);
    case kHdr:
        return pick("High dynamic range output on an HDR display." RESTART_EN,
                    "Obraz HDR na ekranie z obsługą HDR." RESTART_PL,
                    "Зображення з високим динамічним діапазоном на HDR-екрані." RESTART_UK);
    case kAudioOutput:
        return pick("Stereo: the game mixes 2 channels. System: as the Windows output (5.1 or 7.1 when it has them). "
                    "Multichannel: 5.1/7.1 even on a stereo output." RESTART_EN,
                    "Stereo: gra miksuje 2 kanały. System: jak wyjście Windows (5.1 lub 7.1, jeśli je ma). "
                    "Wielokanałowe: 5.1/7.1 nawet na wyjściu stereo." RESTART_PL,
                    "Стерео: гра зводить 2 канали. Система: як вихід Windows (5.1 або 7.1, якщо він їх має). "
                    "Багатоканальний: 5.1/7.1 навіть на стереовиході." RESTART_UK);
    case kVolume:
        return live.volume ? pick("Azimuth's output volume, on top of the game's own volumes. Applies at once.",
                                  "Głośność wyjścia Azimuth, niezależna od głośności gry. Działa od razu.",
                                  "Гучність виходу Azimuth, окремо від гучностей гри. Діє одразу.")
                           : pick("Azimuth's output volume, on top of the game's own volumes." RESTART_EN,
                                  "Głośność wyjścia Azimuth, niezależna od głośności gry." RESTART_PL,
                                  "Гучність виходу Azimuth, окремо від гучностей гри." RESTART_UK);
    }
    return std::string();
}

} // namespace rows
} // namespace azg
