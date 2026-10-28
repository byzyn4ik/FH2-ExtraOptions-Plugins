// Azimuth Graphics Options for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "live.h"

#include <cstring>

#include "common.h"

namespace azg {
namespace live {

namespace {

// Azimuth 1.0.2.
constexpr uint32_t kSetRates = 0x4cc50u, kSetRatesCall = 0x92f3u, kTargetRate = 0x8e7540u, kTitleRate = 0x8e7544u;
constexpr uint32_t kRateReader = 0x4fae7u; // cmp [rip+target],ebx in the per-frame interval
constexpr uint32_t kRateList = 0x105f8u;    // the launcher's 60..240 table
const unsigned char kRateListCode[] = {
    0xc7, 0x44, 0x24, 0x48, 0x3c, 0x00, 0x00, 0x00, 0xc7, 0x44, 0x24, 0x4c, 0x5a, 0x00, 0x00, 0x00,
    0xc7, 0x44, 0x24, 0x50, 0x78, 0x00, 0x00, 0x00, 0xc7, 0x44, 0x24, 0x54, 0x90, 0x00, 0x00, 0x00,
    0xc7, 0x44, 0x24, 0x58, 0xa5, 0x00, 0x00, 0x00, 0xc7, 0x44, 0x24, 0x5c, 0xf0, 0x00, 0x00, 0x00};
const int kLauncherRates[] = {60, 90, 120, 144, 165, 240};
constexpr uint32_t kToggleCheck = 0x6f5f8u, kFullscreen = 0x909d61u, kAltEnter = 0x6fb27u;
const unsigned char kAltEnterCode[] = {0x48, 0x83, 0xff, 0x0d, 0x75, 0x1d, 0x48, 0x0f, 0xba, 0xe6, 0x1d};

// FH2.
constexpr uint32_t kCascades = 0x2207288u;
constexpr uint32_t kRendererStore = 0x112d760u, kRendererPointer = 0x36609a0u, kRendererVtable = 0x2e82b88u;
constexpr uint32_t kRateRead = 0x1131c02u, kUpdates = 0x21bcu;
const unsigned char kRateReadCode[] = {0x41, 0x8b, 0x86, 0xbc, 0x21, 0x00, 0x00}; // mov eax,[r14+21BCh]
const unsigned char kAllCascadesCode[] = {0xb8, 0x04, 0x00, 0x00, 0x00, 0x90, 0x90}; // Extra Options' patch

// Low and Default (the console's), High, Ultra; [3] and [7] are other constants.
const float kCascadeLevels[3][8] = {
    {5.0f, 15.0f, 40.0f, 0, 1.6f, 15.0f, 35.0f, 0},
    {5.0f, 15.0f, 124.0f, 0, 1.6f, 15.0f, 120.0f, 0},
    {17.0f, 130.0f, 360.0f, 0, 4.0f, 60.0f, 270.0f, 0},
};

LiveState live_state;
volatile LONG *target_rate;
volatile unsigned char *fullscreen;
float *cascades;
unsigned char *game_image;

// "op [rip+disp32]" at `rva` whose target is `target`, with `prefix` before
// the displacement and `suffix` after it.
bool rip_code(const unsigned char *base, uint32_t rva, const unsigned char *prefix, size_t prefix_size,
              uint32_t target, const unsigned char *suffix, size_t suffix_size) {
    unsigned char expected[32];
    size_t size = prefix_size + 4 + suffix_size;
    int32_t displacement = (int32_t)(target - (rva + (uint32_t)size));
    memcpy(expected, prefix, prefix_size);
    memcpy(expected + prefix_size, &displacement, 4);
    if (suffix_size) memcpy(expected + prefix_size + 4, suffix, suffix_size);
    return bytes_equal(base + rva, expected, size);
}

bool rate_read_original() {
    return game_image && bytes_equal(game_image + kRateRead, kRateReadCode, sizeof(kRateReadCode));
}

bool rate_setter_known(const unsigned char *host) {
    // mov [rip+target],ecx; mov [rip+title],edx; ret
    static const unsigned char store_ecx[] = {0x89, 0x0d}, store_edx[] = {0x89, 0x15}, ret[] = {0xc3};
    static const unsigned char cmp_ebx[] = {0x39, 0x1d};
    unsigned char call[5];
    int32_t displacement;
    return rip_code(host, kSetRates, store_ecx, 2, kTargetRate, nullptr, 0) &&
           rip_code(host, kSetRates + 6, store_edx, 2, kTitleRate, nullptr, 0) &&
           bytes_equal(host + kSetRates + 12, ret, 1) && safe_read(host + kSetRatesCall, call, 5) &&
           call[0] == 0xe8 && (memcpy(&displacement, call + 1, 4), kSetRatesCall + 5 + (uint32_t)displacement == kSetRates) &&
           rip_code(host, kRateReader, cmp_ebx, 2, kTargetRate, nullptr, 0);
}

void apply_cascades() {
    if (!cascades) return;
    int quality = rows::shadow_level();
    unsigned level = quality == 3 ? 2 : quality == 2 ? 1 : 0;
    DWORD previous, ignored;
    if (!VirtualProtect(cascades, sizeof(kCascadeLevels[0]), PAGE_READWRITE, &previous)) return;
    for (unsigned i = 0; i < 8; ++i)
        if (i != 3 && i != 7) cascades[i] = kCascadeLevels[level][i];
    VirtualProtect(cascades, sizeof(kCascadeLevels[0]), previous, &ignored);
}

void apply_shadow_rate() {
    if (!live_state.shadow_rate) return;
    if (!rate_read_original()) {
        say("the shadow update read changed since start; shadow update rate left as it is");
        return;
    }
    unsigned char *renderer = nullptr;
    void *vtable = nullptr;
    LONG current = 0;
    if (!safe_read(game_image + kRendererPointer, &renderer, sizeof(renderer)) || !renderer ||
        !safe_read(renderer, &vtable, sizeof(vtable)) || vtable != game_image + kRendererVtable ||
        !safe_read(renderer + kUpdates, &current, sizeof(current)) || (current != 1 && current != 4)) {
        say("the shadow renderer is not ready or not as expected; shadow update rate unchanged until a restart");
        return;
    }
    InterlockedExchange((volatile LONG *)(renderer + kUpdates), rows::shadow_level() >= 2 ? 4 : 1);
}

void apply_display(int full) {
    if (!fullscreen || (*fullscreen != 0) == (full != 0)) return;
    HWND window = FindWindowW(L"Azimuth.Game.Win32", nullptr);
    DWORD process = 0;
    if (window && GetWindowThreadProcessId(window, &process) && process == GetCurrentProcessId())
        // Alt held (bit 29), first press, one repeat: Azimuth's Alt+Enter.
        PostMessageW(window, WM_SYSKEYDOWN, VK_RETURN, 0x20000001);
}

} // namespace

const char *check_host(const unsigned char *host) {
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    if (!host || !safe_read(host, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 64 ||
        dos.e_lfanew > 4096 || !safe_read(host + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return "the host process is not a 64-bit executable";
    if (nt.FileHeader.TimeDateStamp != kHostTimestamp || nt.OptionalHeader.SizeOfImage < kHostImageSize)
        return "the host is not Azimuth 1.0.2 (TimeDateStamp 0x6ae05dd1), an unknown build";
    if (!bytes_equal(host + kRateList, kRateListCode, sizeof(kRateListCode)))
        return "the host's launcher frame rate list (0x105f8) is not Azimuth 1.0.2's, an unknown build";
    return nullptr;
}

std::vector<int> frame_rates(unsigned refresh) {
    int top = refresh > 1 ? (int)refresh : 60;
    std::vector<int> rates = {30};
    for (int rate : kLauncherRates)
        if (rate <= top) rates.push_back(rate);
    if (top > rates.back() && top <= 1000) rates.push_back(top);
    return rates;
}

unsigned display_refresh() {
    HWND window = FindWindowW(L"Azimuth.Game.Win32", nullptr);
    DWORD process = 0;
    HMONITOR monitor = window && GetWindowThreadProcessId(window, &process) && process == GetCurrentProcessId()
                           ? MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY)
                           : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW info;
    info.cbSize = sizeof(info);
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    const wchar_t *device = monitor && GetMonitorInfoW(monitor, &info) ? info.szDevice : nullptr;
    if (!EnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, &mode)) return 60;
    return mode.dmDisplayFrequency;
}

void prepare(unsigned char *host, unsigned char *game) {
    game_image = game;
    LONG current = 0;
    if (rate_setter_known(host) && safe_read(host + kTargetRate, &current, sizeof(current))) {
        if (current >= 30 && current <= 1000) {
            target_rate = (volatile LONG *)(host + kTargetRate);
            live_state.frame_rate = true;
            say("frame rate applies at once (Azimuth's target rate at " + hex(kTargetRate) + ", now " +
                std::to_string(current) + " FPS)");
        } else {
            say("Azimuth's target frame rate holds " + std::to_string(current) +
                ", not a rate; frame rate applies after a restart");
        }
    } else {
        say("Azimuth's frame rate setter (0x4cc50) is not as expected; frame rate applies after a restart");
    }
    static const unsigned char cmp_byte[] = {0x80, 0x3d}, zero[] = {0x00};
    if (rip_code(host, kToggleCheck, cmp_byte, 2, kFullscreen, zero, 1) &&
        bytes_equal(host + kAltEnter, kAltEnterCode, sizeof(kAltEnterCode))) {
        fullscreen = host + kFullscreen;
        live_state.display = true;
        say("display mode applies at once (Azimuth's Alt+Enter)");
    } else {
        say("Azimuth's Alt+Enter handler is not as expected; display mode applies after a restart");
    }
    float values[8];
    if (safe_read(game + kCascades, values, sizeof(values))) {
        values[3] = values[7] = 0;
        for (const auto &level : kCascadeLevels)
            if (!memcmp(values, level, sizeof(values))) cascades = (float *)(game + kCascades);
    }
    live_state.shadow_distance = cascades != nullptr;
    say(cascades ? "shadow distance applies at once (cascades at " + hex(kCascades) + ")"
                 : std::string("the shadow cascade distances are not as expected; shadows apply after a restart"));
    static const unsigned char store[] = {0x48, 0x89, 0x0d}; // mov [rip+renderer],rcx
    bool renderer_known = rip_code(game, kRendererStore, store, 3, kRendererPointer, nullptr, 0);
    if (renderer_known && rate_read_original()) {
        live_state.shadow_rate = true;
        say("shadow update rate applies at once");
    } else if (renderer_known && bytes_equal(game + kRateRead, kAllCascadesCode, sizeof(kAllCascadesCode))) {
        say("Extra Options' Realtime shadows updates every cascade each frame; shadow update rate left to it");
    } else {
        say("the shadow renderer is not as expected; shadow update rate applies after a restart");
    }
}

const LiveState &state() { return live_state; }

void apply(unsigned row, int value) {
    if (row == kFrameRate && target_rate) {
        int rate = rows::frame_rate_value(value);
        if (rate >= 30 && rate <= 1000) InterlockedExchange(target_rate, rate);
    } else if (row == kDisplay) {
        apply_display(value);
    } else if (row == kPreset || row == kShadows) {
        apply_cascades();
        apply_shadow_rate();
    }
}

} // namespace live
} // namespace azg
