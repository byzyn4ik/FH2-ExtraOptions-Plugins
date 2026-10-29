// Azimuth Graphics Options for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "live.h"

#include <cstring>

#include "common.h"

namespace azg {
namespace live {

struct Host {
    const char *name;
    uint32_t timestamp, image_size;
    uint32_t rate_list;                  // the launcher's "mov dword [rsp+48h],60 ... [rsp+5Ch],240"
    uint32_t set_rates, set_rates_call;  // the rate setter and its call at startup
    uint32_t target_rate, title_rate;    // what the setter stores
    uint32_t rate_reader;                // "cmp [rip+target],ebx" in the per-frame interval
    uint32_t toggle_check, fullscreen;   // "cmp byte [rip+fullscreen],0" at the toggle's start
    uint32_t alt_enter;                  // "cmp rdi,0Dh; jne; bt rsi,1Dh"
    uint32_t volume_setter, volume_call; // Azimuth's live volume setter and its call at startup
    uint32_t volume;                     // the volume it stores (0..100)
};

namespace {

const Host kHosts[] = {
    {"1.0.4", 0x6ae393ebu, 0x8a2000u, 0x10528u, 0x4ba30u, 0x945fu, 0x7d8554u, 0x7d8550u, 0x4e8e7u, 0x6e868u,
     0x7fcbc1u, 0x6eda7u, 0xc37f0u, 0x938bu, 0x7da2e0u},
    {"1.0.2", 0x6ae05dd1u, 0x9af000u, 0x105f8u, 0x4cc50u, 0x92f3u, 0x8e7540u, 0x8e7544u, 0x4fae7u, 0x6f5f8u,
     0x909d61u, 0x6fb27u, 0xbaea0u, 0x921fu, 0x8e7be8u},
};

const unsigned char kRateListCode[] = {
    0xc7, 0x44, 0x24, 0x48, 0x3c, 0x00, 0x00, 0x00, 0xc7, 0x44, 0x24, 0x4c, 0x5a, 0x00, 0x00, 0x00,
    0xc7, 0x44, 0x24, 0x50, 0x78, 0x00, 0x00, 0x00, 0xc7, 0x44, 0x24, 0x54, 0x90, 0x00, 0x00, 0x00,
    0xc7, 0x44, 0x24, 0x58, 0xa5, 0x00, 0x00, 0x00, 0xc7, 0x44, 0x24, 0x5c, 0xf0, 0x00, 0x00, 0x00};
const int kLauncherRates[] = {60, 90, 120, 144, 165, 240};
const unsigned char kAltEnterCode[] = {0x48, 0x83, 0xff, 0x0d, 0x75, 0x1d, 0x48, 0x0f, 0xba, 0xe6, 0x1d};

// The volume setter's first 0x69 bytes, the same in both builds apart from
// six rip-relative displacements (zero here, listed in kVolumeFields):
//   mov [rsp+8],rbx; mov [rsp+20h],rsi; push rdi; sub rsp,30h; movaps [rsp+20h],xmm6
//   mov ebx,ecx; lea rsi,[lock]; mov [rsp+50h],rsi; mov rcx,rsi; call lock()
//   test eax,eax; jnz; cmp [guard],7FFFFFFFh; je
//   mov eax,64h; cmp ebx,eax; cmova ebx,eax          (clamp to 100)
//   xchg [volume],ebx; mov eax,[volume]; nop
//   xorps xmm6,xmm6; cvtsi2ss xmm6,rax; divss xmm6,[100.0]
const unsigned char kVolumeCode[0x69] = {
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xec, 0x30, 0x0f,
    0x29, 0x74, 0x24, 0x20, 0x8b, 0xd9, 0x48, 0x8d, 0x35, 0x00, 0x00, 0x00, 0x00, 0x48, 0x89, 0x74,
    0x24, 0x50, 0x48, 0x8b, 0xce, 0xe8, 0x00, 0x00, 0x00, 0x00, 0x85, 0xc0, 0x0f, 0x85, 0xab, 0x00,
    0x00, 0x00, 0x81, 0x3d, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x7f, 0x0f, 0x84, 0x80, 0x00,
    0x00, 0x00, 0xb8, 0x64, 0x00, 0x00, 0x00, 0x3b, 0xd8, 0x0f, 0x47, 0xd8, 0x87, 0x1d, 0x00, 0x00,
    0x00, 0x00, 0x8b, 0x05, 0x00, 0x00, 0x00, 0x00, 0x90, 0x0f, 0x57, 0xf6, 0xf3, 0x48, 0x0f, 0x2a,
    0xf0, 0xf3, 0x0f, 0x5e, 0x35, 0x00, 0x00, 0x00, 0x00};
// The displacement fields (offset, end of their instruction); the xchg and
// mov ones must point at the build's volume.
const struct {
    uint8_t offset, end;
    bool volume;
} kVolumeFields[] = {{0x19, 0x1d, false}, {0x26, 0x2a, false}, {0x34, 0x3c, false},
                     {0x4e, 0x52, true},  {0x54, 0x58, true},  {0x65, 0x69, false}};

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
void (*set_volume)(int);
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

// "call rel32" at `rva` to `target`.
bool calls(const unsigned char *base, uint32_t rva, uint32_t target) {
    unsigned char call[5];
    int32_t displacement;
    if (!safe_read(base + rva, call, sizeof(call)) || call[0] != 0xe8) return false;
    memcpy(&displacement, call + 1, 4);
    return rva + 5 + (uint32_t)displacement == target;
}

bool rate_read_original() {
    return game_image && bytes_equal(game_image + kRateRead, kRateReadCode, sizeof(kRateReadCode));
}

bool rate_setter_known(const Host &h, const unsigned char *host) {
    // mov [rip+target],ecx; mov [rip+title],edx; ret
    static const unsigned char store_ecx[] = {0x89, 0x0d}, store_edx[] = {0x89, 0x15}, ret[] = {0xc3};
    static const unsigned char cmp_ebx[] = {0x39, 0x1d};
    return rip_code(host, h.set_rates, store_ecx, 2, h.target_rate, nullptr, 0) &&
           rip_code(host, h.set_rates + 6, store_edx, 2, h.title_rate, nullptr, 0) &&
           bytes_equal(host + h.set_rates + 12, ret, 1) && calls(host, h.set_rates_call, h.set_rates) &&
           rip_code(host, h.rate_reader, cmp_ebx, 2, h.target_rate, nullptr, 0);
}

bool volume_setter_known(const Host &h, const unsigned char *host) {
    unsigned char code[sizeof(kVolumeCode)];
    if (!safe_read(host + h.volume_setter, code, sizeof(code)) || !calls(host, h.volume_call, h.volume_setter))
        return false;
    for (const auto &field : kVolumeFields) {
        int32_t displacement;
        memcpy(&displacement, code + field.offset, 4);
        if (field.volume && h.volume_setter + field.end + (uint32_t)displacement != h.volume) return false;
        memset(code + field.offset, 0, 4);
    }
    return !memcmp(code, kVolumeCode, sizeof(code));
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

HWND azimuth_window() {
    HWND window = FindWindowW(L"Azimuth.Game.Win32", nullptr);
    DWORD process = 0;
    return window && GetWindowThreadProcessId(window, &process) && process == GetCurrentProcessId() ? window
                                                                                                   : nullptr;
}

void apply_display(int full) {
    if (!fullscreen || (*fullscreen != 0) == (full != 0)) return;
    if (HWND window = azimuth_window())
        // Alt held (bit 29), first press, one repeat: Azimuth's Alt+Enter.
        PostMessageW(window, WM_SYSKEYDOWN, VK_RETURN, 0x20000001);
}

} // namespace

const Host *identify(const unsigned char *host, const char **why) {
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    *why = nullptr;
    if (!host || !safe_read(host, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 64 ||
        dos.e_lfanew > 4096 || !safe_read(host + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        *why = "the host process is not a 64-bit executable";
        return nullptr;
    }
    for (const Host &h : kHosts) {
        if (nt.FileHeader.TimeDateStamp != h.timestamp) continue;
        static std::string problem;
        if (nt.OptionalHeader.SizeOfImage < h.image_size) {
            problem = std::string("the host has Azimuth ") + h.name + "'s TimeDateStamp but a smaller image; an unknown build";
        } else if (!bytes_equal(host + h.rate_list, kRateListCode, sizeof(kRateListCode))) {
            problem = std::string("the host's launcher frame rate list (") + hex(h.rate_list) + ") is not Azimuth " +
                      h.name + "'s; an unknown build";
        } else {
            return &h;
        }
        *why = problem.c_str();
        return nullptr;
    }
    static std::string unknown;
    unknown = "the host is not Azimuth 1.0.4 or 1.0.2 (TimeDateStamp " + hex(nt.FileHeader.TimeDateStamp) +
              "); an unknown build";
    *why = unknown.c_str();
    return nullptr;
}

const char *host_name(const Host *build) { return build ? build->name : "?"; }

std::vector<int> frame_rates(unsigned refresh) {
    int top = refresh > 1 ? (int)refresh : 60;
    std::vector<int> rates = {30};
    for (int rate : kLauncherRates)
        if (rate <= top) rates.push_back(rate);
    if (top > rates.back() && top <= 1000) rates.push_back(top);
    return rates;
}

unsigned display_refresh() {
    HWND window = azimuth_window();
    HMONITOR monitor = window ? MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY)
                              : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW info;
    info.cbSize = sizeof(info);
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    const wchar_t *device = monitor && GetMonitorInfoW(monitor, &info) ? info.szDevice : nullptr;
    if (!EnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, &mode)) return 60;
    return mode.dmDisplayFrequency;
}

void prepare(const Host *build, unsigned char *host, unsigned char *game) {
    game_image = game;
    const Host &h = *build;
    LONG current = 0;
    if (rate_setter_known(h, host) && safe_read(host + h.target_rate, &current, sizeof(current))) {
        if (current >= 30 && current <= 1000) {
            target_rate = (volatile LONG *)(host + h.target_rate);
            live_state.frame_rate = true;
            say("frame rate applies at once (Azimuth's target rate at " + hex(h.target_rate) + ", now " +
                std::to_string(current) + " FPS)");
        } else {
            say("Azimuth's target frame rate holds " + std::to_string(current) +
                ", not a rate; frame rate applies after a restart");
        }
    } else {
        say("Azimuth's frame rate setter (" + hex(h.set_rates) + ") is not as expected; frame rate applies after a restart");
    }
    static const unsigned char cmp_byte[] = {0x80, 0x3d}, zero[] = {0x00};
    if (rip_code(host, h.toggle_check, cmp_byte, 2, h.fullscreen, zero, 1) &&
        bytes_equal(host + h.alt_enter, kAltEnterCode, sizeof(kAltEnterCode))) {
        fullscreen = host + h.fullscreen;
        live_state.display = true;
        say("display mode applies at once (Azimuth's Alt+Enter)");
    } else {
        say("Azimuth's Alt+Enter handler is not as expected; display mode applies after a restart");
    }
    LONG volume = -1;
    if (volume_setter_known(h, host) && safe_read(host + h.volume, &volume, sizeof(volume)) && volume >= 0 &&
        volume <= 100) {
        set_volume = (void (*)(int))(void *)(host + h.volume_setter);
        live_state.volume = true;
        say("volume applies at once (Azimuth's volume setter " + hex(h.volume_setter) + ", now " +
            std::to_string(volume) + ")");
    } else {
        say("Azimuth's volume setter (" + hex(h.volume_setter) + ") is not as expected; volume applies after a restart");
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
    } else if (row == kVolume && set_volume && value >= 0 && value <= 100) {
        set_volume(value);
    }
}

#ifdef AZG_TEST_EXPORTS
uint32_t test_site(const Host *build, unsigned which) {
    if (!build) return 0;
    switch (which) {
    case 0: return build->target_rate;
    case 1: return build->volume_setter;
    case 2: return build->fullscreen;
    case 3: return build->rate_list;
    case 4: return build->volume;
    }
    return 0;
}
#endif

} // namespace live
} // namespace azg
