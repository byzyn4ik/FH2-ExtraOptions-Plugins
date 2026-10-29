// Azimuth Graphics Options for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "language.h"

#include <cstring>

#include "common.h"

namespace azg {
namespace language {

namespace {

// The game's text table manager; its language code is a std::string at +0x30
// (the same read Extra Options and its Weather Control plugin use).
constexpr uint32_t kTextTables = 0x3662990u;

Choice forced = kAuto;
bool pack;

std::wstring profile(const std::wstring &file, const wchar_t *section, const wchar_t *key) {
    wchar_t value[260];
    value[0] = 0;
    GetPrivateProfileStringW(section, key, L"", value, 260, file.c_str());
    std::wstring text = value;
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t')) text.pop_back();
    return text;
}

bool valid_id(const std::wstring &id) {
    if (id.empty() || id.size() > 32) return false;
    for (wchar_t c : id)
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'-' ||
              c == L'_'))
            return false;
    return true;
}

} // namespace

Choice parse(const std::wstring &text) {
    if (!lstrcmpiW(text.c_str(), L"en")) return kForceEnglish;
    if (!lstrcmpiW(text.c_str(), L"pl")) return kForcePolish;
    if (!lstrcmpiW(text.c_str(), L"uk")) return kForceUkrainian;
    return kAuto;
}

void configure(Choice choice, const std::wstring &launcher, const std::wstring &mount) {
    forced = choice;
    pack = false;
    if (choice != kAuto) {
        say(std::string("captions: ") + (choice == kForcePolish ? "Polish" : choice == kForceUkrainian ? "Ukrainian" : "English") +
            " ([Captions] Language)");
        return;
    }
    if (launcher.empty()) return;
    std::wstring id = profile(launcher + L"\\FH2_ExtraOptions.ini", L"Language", L"Custom");
    if (id.empty()) return;
    if (!valid_id(id) || mount.empty()) {
        say("FH2_ExtraOptions.ini names a custom language the plugin cannot locate; captions follow the game's language");
        return;
    }
    std::wstring file = mount + L"\\languages\\" + id + L"\\language.ini";
    std::wstring slot = profile(file, L"Language", L"Slot"), ui = profile(file, L"Language", L"Ui");
    if (ui.empty()) ui = id;
    pack = !lstrcmpiW(slot.c_str(), L"ru-RU") && !lstrcmpiW(ui.c_str(), L"uk");
    if (pack) say("custom language " + narrow(id) + " holds Ukrainian in the ru-RU slot: Ukrainian captions there");
}

bool ukrainian_pack() { return pack; }

std::string game_code(const unsigned char *game) {
    void *manager = nullptr;
    unsigned char text[32];
    if (!game || !safe_read(game + kTextTables, &manager, sizeof(manager)) || !manager ||
        !safe_read((unsigned char *)manager + 0x30, text, sizeof(text)))
        return std::string();
    size_t length, capacity;
    memcpy(&length, text + 16, sizeof(length));
    memcpy(&capacity, text + 24, sizeof(capacity));
    // An inline (short) std::string of 2 or 3 letters.
    if (capacity != 15 || length < 2 || length > 3 || text[length] != 0) return std::string();
    std::string code((const char *)text, length);
    for (char &c : code) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return std::string();
        if (c >= 'a') c = (char)(c - 'a' + 'A');
    }
    return code;
}

Language current(const unsigned char *game) {
    switch (forced) {
    case kForceEnglish: return kEnglish;
    case kForcePolish: return kPolish;
    case kForceUkrainian: return kUkrainian;
    case kAuto: break;
    }
    std::string code = game_code(game);
    if (code == "PL") return kPolish;
    if (code == "RU" && pack) return kUkrainian;
    return kEnglish;
}

#ifdef AZG_TEST_EXPORTS
void test_set(Choice choice, bool has_pack) {
    forced = choice;
    pack = has_pack;
}
#endif

} // namespace language
} // namespace azg
