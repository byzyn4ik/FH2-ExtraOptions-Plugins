// FH2 Extra Options plugins: all plugins together in one process.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Offline: never runs the game and never touches its files (the executables
// are only read, into private memory). Driven by tests/run_combined.py:
//
//   combined_test <order> <variant> <work> <game exe> <azimuth exe>
//                 <graphics dll> <exit game dll> <radio dll> <video xaml> <audio xaml> <pause xaml>
//
// order: the install order, a permutation of "g" (Azimuth Graphics Options),
// "e" (Exit Game) and "r" (Custom Radio), as Extra Options' loader would call
// FH2PluginInitialize one plugin after the other. variant: "stock" (the XAML
// resource call at game+0x16ebb37 still calls the game's reader) or "eo" (it
// calls Extra Options' thunk at 0x13fe354, as Extra Options 0.4.4 patches it).
// The game image is mapped and relocated as the loader maps it; the pause
// page init slot holds a stand-in for Extra Options' hook (the game's own init
// cannot run here). After every plugin is in, the game's native functions are
// replaced by fakes and the Video, Audio and pause pages are read through the
// one resource call both Graphics and Exit Game chain; the pages come back in
// <work>\video_out.xaml, audio_out.xaml and pause_out.xaml for run_combined.py.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fh2_plugin.h"

namespace {

constexpr uint32_t kImageSize = 0x3a18000u;
constexpr uint32_t kCallSite = 0x16ebb37u, kThunk = 0x13fe354u, kGetResource = 0xaabec0u;
constexpr uint32_t kPauseInitSlot = 0x2e14310u, kBindButton = 0xd9f9f0u;
// Custom Radio's hooks (its src/sites.h): two vtable slots and two calls of its core group.
constexpr uint32_t kRadioSlots[] = {0x2ec4fd0u, 0x2e76248u};
constexpr uint32_t kRadioCalls[] = {0x10189a0u, 0x10221f2u};
// Azimuth's volume setter (1.0.4 and 1.0.2): the Graphics plugin calls it from the Audio page.
struct HostSites {
    uint32_t timestamp, volume_setter;
    const char *name;
};
const HostSites kHosts[] = {{0x6ae393ebu, 0xc37f0u, "1.0.4"}, {0x6ae05dd1u, 0xbaea0u, "1.0.2"}};

int failures;
std::wstring work;

void check(bool ok, const std::string &what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    fflush(stdout);
    if (!ok) ++failures;
}

std::string read_file(const std::wstring &path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::string();
    std::string text;
    char buffer[65536];
    DWORD got;
    while (ReadFile(file, buffer, sizeof(buffer), &got, nullptr) && got) text.append(buffer, got);
    CloseHandle(file);
    return text;
}

void write_file(const std::wstring &path, const std::string &data) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD written;
    if (file != INVALID_HANDLE_VALUE) {
        WriteFile(file, data.data(), (DWORD)data.size(), &written, nullptr);
        CloseHandle(file);
    }
}

std::string host_log_text;
CRITICAL_SECTION log_lock;

void host_log(const char *message) {
    EnterCriticalSection(&log_lock);
    host_log_text += message;
    LeaveCriticalSection(&log_lock);
}

bool logged(const char *what) {
    EnterCriticalSection(&log_lock);
    bool found = host_log_text.find(what) != std::string::npos;
    LeaveCriticalSection(&log_lock);
    return found;
}

// A PE file laid out as the loader maps it, in private read-write-execute
// memory; with `relocate`, its 64-bit base relocations applied (vtable slots
// then hold the mapped functions, as in the game).
unsigned char *map_image(const std::wstring &path, bool relocate, uint32_t *size, uint32_t *timestamp) {
    std::string file = read_file(path);
    if (file.size() < 0x400) return nullptr;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)file.data();
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(file.data() + dos->e_lfanew);
    *size = nt->OptionalHeader.SizeOfImage;
    if (timestamp) *timestamp = nt->FileHeader.TimeDateStamp;
    unsigned char *image =
        (unsigned char *)VirtualAlloc(nullptr, *size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!image) return nullptr;
    memcpy(image, file.data(), nt->OptionalHeader.SizeOfHeaders);
    const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        size_t raw = std::min<size_t>(section->SizeOfRawData, section->Misc.VirtualSize ? section->Misc.VirtualSize
                                                                                         : section->SizeOfRawData);
        if (section->PointerToRawData + raw <= file.size() && section->VirtualAddress + raw <= *size)
            memcpy(image + section->VirtualAddress, file.data() + section->PointerToRawData, raw);
    }
    if (relocate) {
        const IMAGE_DATA_DIRECTORY &directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        uint64_t delta = (uint64_t)(uintptr_t)image - nt->OptionalHeader.ImageBase;
        for (uint32_t at = 0; at + 8 <= directory.Size;) {
            const IMAGE_BASE_RELOCATION *block = (const IMAGE_BASE_RELOCATION *)(image + directory.VirtualAddress + at);
            if (block->SizeOfBlock < 8) break;
            const uint16_t *entries = (const uint16_t *)(block + 1);
            for (uint32_t i = 0; i < (block->SizeOfBlock - 8) / 2; ++i) {
                if (entries[i] >> 12 != IMAGE_REL_BASED_DIR64) continue;
                uint64_t *value = (uint64_t *)(image + block->VirtualAddress + (entries[i] & 0xfff));
                *value += delta;
            }
            at += block->SizeOfBlock;
        }
    }
    return image;
}

// ---------------------------------------------------------------- fakes
// The game's native functions the plugins call while a page is read
// (Graphics' tests/plugin_test.cpp has the same ones).

unsigned char *image;

struct Fake {
    void **vtable;
    unsigned char pad[8];
    volatile LONG references;
    unsigned char rest[0x60];
};

int deleted_originals, deleted_strings, reads;
std::vector<std::string> events;

void *delete_original(Fake *object, unsigned) {
    ++deleted_originals;
    return object;
}
void *delete_string(Fake *object, unsigned) {
    ++deleted_strings;
    return object;
}
void *original_vtable[8] = {nullptr, (void *)(uintptr_t)delete_original};
void *string_vtable[8] = {nullptr, (void *)(uintptr_t)delete_string};

void *fake_allocate(size_t size) { return calloc(1, size); }
void fake_free(void *p) { free(p); }
void fake_object_init(void *object) { ((Fake *)object)->references = 1; }
void fake_object_cleanup(void *) {}
void fake_delete_object(void *object) {
    void **vtable = ((Fake *)object)->vtable;
    ((void *(*)(void *, unsigned))vtable[0])(object, 1);
}
void *fake_collection_type(void *) { return (void *)(uintptr_t)0x7777; }
void *fake_allocate_typed(size_t size, void *) { return calloc(1, size < sizeof(Fake) ? sizeof(Fake) : size); }
void fake_collection_init(void *object) { ((Fake *)object)->references = 1; }
void **fake_make_string(void **out, const wchar_t *) {
    Fake *object = (Fake *)calloc(1, sizeof(Fake));
    object->vtable = string_vtable + 1;
    object->references = 1;
    *out = object;
    return out;
}
void fake_insert_item(void *, int, void *label) { InterlockedIncrement(&((Fake *)label)->references); }
void fake_register_type() {}
unsigned char int_descriptor[0x58], float_descriptor[0x58], list_descriptor[0x58];
void *fake_int_property() { return int_descriptor; }
void *fake_float_property() { return float_descriptor; }
void *fake_collection_property() { return list_descriptor; }
void **fake_box(void **out, const int *value) {
    unsigned char *box = (unsigned char *)calloc(1, 0x40);
    memcpy(box + 0x28, value, 4);
    *out = box;
    return out;
}
void fake_property_changed(void *, void *) {}
void fake_set_volume(int) {}
struct MemberHandler {
    void *method;
    int32_t adjust, unused;
};
std::vector<std::pair<std::string, MemberHandler>> bound;
void fake_bind_button(void *page, const char *name, const MemberHandler *handler) {
    events.push_back("bind " + std::to_string((uintptr_t)page) + " " + name);
    bound.push_back({name, *handler});
}
void fake_extra_options_init(void *page) { events.push_back("extra options init " + std::to_string((uintptr_t)page)); }

std::string next_resource;
Fake *last_original;

// The game's reader (stock) or Extra Options' hook (eo): the page as it was read.
void **fake_reader(void *, void **output) {
    ++reads;
    unsigned char *object = (unsigned char *)calloc(1, 0x40 + next_resource.size() + 1);
    Fake *resource = (Fake *)object;
    resource->vtable = original_vtable + 1;
    resource->references = 1;
    char *bytes = (char *)(object + 0x40);
    memcpy(bytes, next_resource.data(), next_resource.size());
    memcpy(object + 0x28, &bytes, sizeof(bytes));
    int length = (int)next_resource.size();
    memcpy(object + 0x30, &length, 4);
    memcpy(object + 0x34, &length, 4);
    last_original = resource;
    *output = resource;
    return output;
}

// mov rax, target; jmp rax
void jump_stub(unsigned char *base, uint32_t rva, const void *target) {
    unsigned char code[12] = {0x48, 0xb8};
    uintptr_t address = (uintptr_t)target;
    memcpy(code + 2, &address, 8);
    code[10] = 0xff;
    code[11] = 0xe0;
    memcpy(base + rva, code, sizeof(code));
    FlushInstructionCache(GetCurrentProcess(), base + rva, sizeof(code));
}

void install_fakes(unsigned char *host, const HostSites *sites) {
    const struct {
        uint32_t rva;
        const void *fake;
    } fakes[] = {
        {0x3229c0, (void *)(uintptr_t)fake_allocate},        {0x31c7b0, (void *)(uintptr_t)fake_free},
        {0x143eb80, (void *)(uintptr_t)fake_object_init},    {0x1442050, (void *)(uintptr_t)fake_object_cleanup},
        {0x257730, (void *)(uintptr_t)fake_delete_object},   {0x1476110, (void *)(uintptr_t)fake_collection_type},
        {0x14424e0, (void *)(uintptr_t)fake_allocate_typed}, {0xaefd60, (void *)(uintptr_t)fake_collection_init},
        {0x13fe8e0, (void *)(uintptr_t)fake_make_string},    {0xb52170, (void *)(uintptr_t)fake_insert_item},
        {0xeca710, (void *)(uintptr_t)fake_register_type},   {0xc803c0, (void *)(uintptr_t)fake_register_type},
        {0xed81f0, (void *)(uintptr_t)fake_int_property},    {0xed6e90, (void *)(uintptr_t)fake_float_property},
        {0xed77d0, (void *)(uintptr_t)fake_collection_property},
        {0x4702d0, (void *)(uintptr_t)fake_box},             {0x3f5990, (void *)(uintptr_t)fake_box},
        {0xcf9c60, (void *)(uintptr_t)fake_property_changed},
        {kBindButton, (void *)(uintptr_t)fake_bind_button},
    };
    for (const auto &f : fakes) jump_stub(image, f.rva, f.fake);
    // register_property (0x155ee30) keeps the game's own code: it only links
    // the descriptor into the registry's list.
    void *vtable = (void *)(uintptr_t)0x1234;
    memcpy(int_descriptor, &vtable, 8);
    memcpy(float_descriptor, &vtable, 8);
    memcpy(list_descriptor, &vtable, 8);
    jump_stub(host, sites->volume_setter, (void *)(uintptr_t)fake_set_volume);
}

void *call_target(uint32_t rva) {
    int32_t displacement;
    memcpy(&displacement, image + rva + 1, 4);
    return image + rva + 5 + displacement;
}

// The module a jump "jmp [rip+0]; dq x" (or code) leads into; nullptr outside every module.
HMODULE module_of(const void *address) {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)address, &module);
    return module;
}

HMODULE jump_module(const void *jump) {
    const unsigned char *code = (const unsigned char *)jump;
    if (code[0] != 0xff || code[1] != 0x25 || memcmp(code + 2, "\0\0\0\0", 4)) return nullptr;
    void *destination;
    memcpy(&destination, code + 6, 8);
    return module_of(destination);
}

// Reads a page through the resource call site; the XAML that comes back.
std::string read_page(const std::string &xaml, bool *replaced) {
    void *output = nullptr;
    next_resource = xaml;
    reads = 0;
    typedef void **(*Reader)(void *, void **);
    ((Reader)call_target(kCallSite))(nullptr, &output);
    check(reads == 1, "the page is read once, through every hook of the chain");
    *replaced = output && output != last_original;
    std::string text;
    if (output) {
        char *bytes;
        int length;
        memcpy(&bytes, (unsigned char *)output + 0x28, 8);
        memcpy(&length, (unsigned char *)output + 0x30, 4);
        text.assign(bytes, (size_t)length);
    }
    return text;
}

struct Plugin {
    char letter;
    const char *name, *log_prefix;
    std::wstring path;
    HMODULE module;
};

} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc < 12) {
        fprintf(stderr, "usage: combined_test <order> <variant> <work> <game exe> <azimuth exe> <graphics dll> "
                        "<exit game dll> <radio dll> <video xaml> <audio xaml> <pause xaml>\n");
        return 2;
    }
    InitializeCriticalSection(&log_lock);
    char order[8] = {}, variant_text[8] = {};
    WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, order, sizeof(order) - 1, nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, variant_text, sizeof(variant_text) - 1, nullptr, nullptr);
    bool eo = !strcmp(variant_text, "eo");
    work = argv[3];
    Plugin plugins[] = {{'g', "Azimuth Graphics Options", "AZIMUTH GRAPHICS OPTIONS: ", argv[6], nullptr},
                        {'e', "Exit Game", "EXIT GAME: ", argv[7], nullptr},
                        {'r', "Custom Radio", "CUSTOM RADIO", argv[8], nullptr}};
    std::string video = read_file(argv[9]), audio = read_file(argv[10]), pause = read_file(argv[11]);
    printf("combined_test %s %s\n", order, variant_text);

    uint32_t game_size = 0, host_size = 0, timestamp = 0;
    image = map_image(argv[4], true, &game_size, nullptr);
    unsigned char *host = map_image(argv[5], false, &host_size, &timestamp);
    const HostSites *sites = nullptr;
    for (const HostSites &h : kHosts)
        if (h.timestamp == timestamp) sites = &h;
    check(image && game_size == kImageSize, "the game's image is mapped and relocated");
    check(host && sites, std::string("Azimuth's azimuth.exe is mapped (") + (sites ? sites->name : "?") + ")");
    check(!video.empty() && !audio.empty() && !pause.empty(), "the Video, Audio and pause page XAML are read");
    if (!image || !host || !sites || video.empty() || audio.empty() || pause.empty()) return 1;

    // As Extra Options leaves the game: its pause page init hook, and in the
    // eo variant its resource thunk.
    void *eo_init = (void *)(uintptr_t)fake_extra_options_init;
    memcpy(image + kPauseInitSlot, &eo_init, 8);
    if (eo) {
        jump_stub(image, kThunk, (void *)(uintptr_t)fake_reader);
        int32_t displacement = (int32_t)(kThunk - (kCallSite + 5));
        memcpy(image + kCallSite + 1, &displacement, 4);
    }
    void *first_target = call_target(kCallSite);
    unsigned char radio_before[4][8];
    for (int i = 0; i < 2; ++i) memcpy(radio_before[i], image + kRadioSlots[i], 8);
    for (int i = 0; i < 2; ++i) memcpy(radio_before[2 + i], image + kRadioCalls[i], 5);

    // The loader: FH2PluginInitialize of each plugin, in `order`.
    std::vector<Plugin *> resource_chain;  // Graphics and Exit Game, in the order they chained the call
    for (const char *letter = order; *letter; ++letter) {
        Plugin *plugin = nullptr;
        for (Plugin &p : plugins)
            if (p.letter == *letter) plugin = &p;
        if (!plugin) return 2;
        plugin->module = LoadLibraryExW(plugin->path.c_str(), nullptr,
                                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        check(plugin->module != nullptr, std::string(plugin->name) + ": the DLL loads");
        if (!plugin->module) return 1;
        if (plugin->letter == 'g') {
            auto use = (void (*)(unsigned char *))(uintptr_t)GetProcAddress(plugin->module, "AzgTestUseHost");
            if (use) use(host);
        }
        auto init = (FH2PluginInitializeFn)(uintptr_t)GetProcAddress(plugin->module, "FH2PluginInitialize");
        std::wstring folder = plugin->path.substr(0, plugin->path.find_last_of(L"\\/"));
        FH2PluginContext context = {};
        context.size = sizeof(context);
        context.api_version = FH2_PLUGIN_API_VERSION;
        context.adapter = FH2_PLUGIN_AZIMUTH;
        context.game_image_size = kImageSize;
        context.game_image = image;
        context.plugin_module = plugin->module;
        context.plugins_directory = folder.c_str();
        context.plugin_path = plugin->path.c_str();
        context.extra_options_version = "0.4.4";
        context.log_utf8 = host_log;
        void *before = call_target(kCallSite);
        BOOL ok = init && init(&context);
        check(ok == TRUE, std::string(plugin->name) + ": FH2PluginInitialize returns TRUE");
        if (plugin->letter == 'g') {
            auto set_rates = (void (*)(const int *, int))(uintptr_t)GetProcAddress(plugin->module, "AzgTestSetFrameRates");
            static const int rates[] = {30, 60, 90, 120, 144, 165, 240};
            if (set_rates) set_rates(rates, 7);
        }
        void *after = call_target(kCallSite);
        if (plugin->letter == 'r') {
            check(after == before, "Custom Radio leaves the XAML resource call to the others");
            continue;
        }
        check(after != before && jump_module(after) == plugin->module,
              std::string(plugin->name) + ": the resource call 0x16ebb37 now goes through its near jump to it");
        bool chained_on_other = !resource_chain.empty();
        if (plugin->letter == 'g')
            check(logged(chained_on_other ? "the resource call at 0x16ebb37 is chained to another module's hook at"
                         : eo ? "the resource call at 0x16ebb37 is chained to Extra Options' resource hook"
                              : "the resource call at 0x16ebb37 is chained to the game's resource reader"),
                  std::string("Azimuth Graphics Options: the log says it chained onto ") +
                      (chained_on_other ? "Exit Game's jump" : eo ? "Extra Options' thunk" : "the game's reader"));
        else
            check(logged(chained_on_other ? "XAML resource call 0x16ebb37 chained onto another module's hook at"
                         : eo ? "XAML resource call 0x16ebb37 chained onto Extra Options' resource thunk"
                              : "XAML resource call 0x16ebb37 chained onto the game's resource reader"),
                  std::string("Exit Game: the log says it chained onto ") +
                      (chained_on_other ? "Graphics' jump" : eo ? "Extra Options' thunk" : "the game's reader"));
        resource_chain.push_back(plugin);
    }
    printf("%s", host_log_text.c_str());
    check(call_target(kCallSite) != first_target && resource_chain.size() == 2,
          "both page plugins chained the resource call, the last one first in line");
    check(logged("CUSTOM RADIO: custom station hooks installed"), "Custom Radio: its station hooks are installed");
    bool radio_in = true;
    for (int i = 0; i < 2; ++i) {
        void *slot;
        memcpy(&slot, image + kRadioSlots[i], 8);
        radio_in = radio_in && module_of(slot) == plugins[2].module && memcmp(radio_before[i], &slot, 8);
        radio_in = radio_in && jump_module(call_target(kRadioCalls[i])) == plugins[2].module;
    }
    check(radio_in, "Custom Radio: its slots and calls lead into it");
    void *init_slot;
    memcpy(&init_slot, image + kPauseInitSlot, 8);
    check(module_of(init_slot) == plugins[1].module, "Exit Game: the pause page init slot holds its init");
    check(!logged("nothing is installed") && !logged("could not be chained") && !logged("is not as expected"),
          "no plugin refused anything");

    // The pages, through the whole chain.
    install_fakes(host, sites);
    if (!eo) jump_stub(image, kGetResource, (void *)(uintptr_t)fake_reader);
    bool replaced = false;
    std::string out = read_page(video, &replaced);
    check(replaced && out.find("AZG_GfxFrameRate") != std::string::npos && out.find("EOExitGame") == std::string::npos,
          "the Video page comes back with the Azimuth rows (and no Exit Game tile)");
    write_file(work + L"\\video_out.xaml", out);
    out = read_page(audio, &replaced);
    check(replaced && out.find("AZG_AzVolume") != std::string::npos && out.find("EOExitGame") == std::string::npos,
          "the Audio page comes back with the Azimuth rows");
    write_file(work + L"\\audio_out.xaml", out);
    out = read_page(pause, &replaced);
    check(replaced && out.find("PART_Button_EOExitGame") != std::string::npos && out.find("AZG_") == std::string::npos,
          "the pause page comes back with the Exit Game tile (and no Azimuth rows)");
    write_file(work + L"\\pause_out.xaml", out);
    int deleted = deleted_originals;
    out = read_page("<Page x:Class=\"ForzaUI.A_HUD\"/>", &replaced);
    check(!replaced && deleted_originals == deleted, "another page passes through every hook untouched");
    check(deleted_originals == 3, "each replaced page's original resource is released once");

    // The pause page init: Extra Options' stand-in first, then the tile is bound.
    events.clear();
    ((void (*)(void *))init_slot)((void *)(uintptr_t)0x5150);
    check(events.size() == 2 && events[0] == "extra options init 20816" &&
              events[1] == "bind 20816 PART_Button_EOExitGame",
          "the pause page init runs the previous init, then binds PART_Button_EOExitGame");
    check(bound.size() == 1 && module_of(bound[0].second.method) == plugins[1].module,
          "the tile is bound to Exit Game's handler");
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
