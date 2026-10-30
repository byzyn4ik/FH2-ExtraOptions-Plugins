// FH2 Extra Options plugins: all plugins together in one process.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Offline: never runs the game and never touches its files (the executables
// are only read, into private memory). Driven by tests/run_combined.py:
//
//   combined_test <order> <variant> <work> <game exe> <azimuth exe>
//                 <graphics dll> <exit game dll> <radio dll> <performance dll> <keyboard & mouse dll>
//                 <video xaml> <audio xaml> <pause xaml> <settings hub xaml>
//
// order: the install order, a permutation of "g" (Azimuth Graphics Options),
// "e" (Exit Game), "r" (Custom Radio), "p" (Performance) and "k" (Keyboard &
// Mouse), as Extra Options' loader would call FH2PluginInitialize one plugin
// after the other. variant: "stock" (the XAML resource call at game+0x16ebb37
// still calls the game's reader) or "eo" (it calls Extra Options' thunk at
// 0x13fe354, and the Video page reset slot holds Extra Options' reset, as
// Extra Options 0.4.4 patches them). The game image and azimuth.exe are mapped
// and relocated as the loader maps them; the pause page init slot holds a
// stand-in for Extra Options' hook. After every plugin is in, the game's native
// functions are replaced by fakes (the game's code cannot run here) and the
// pages are read through the one resource call that Graphics, Exit Game,
// Performance and Keyboard & Mouse all chain:
//   - the Video page (Graphics' rows and Performance's FPS counter row), the
//     Audio page (Graphics' rows), the pause page (Exit Game's tile), the
//     Settings hub (Keyboard & Mouse's tile) and a page nobody edits;
//   - the Settings hub's init binds the KEYBOARD & MOUSE tile; choosing it
//     makes the next Video page the standalone KEYBOARD & MOUSE page, which
//     the other plugins leave alone; the Video page after that is the Video
//     page with its rows again;
//   - the Video page reset slot (Keyboard & Mouse chains it) resets only the
//     plugin's options on its page and reaches Extra Options' (or the game's)
//     reset on any other page;
//   - the pause page init runs Extra Options' stand-in, then binds Exit Game's tile.
// The pages come back in <work>\*_out.xaml for run_combined.py, which runs
// each plugin's own page checks on them.
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
// Keyboard & Mouse: the Settings hub init slot and init, its tile binder, the
// Video tile's handler, the Video page reset slot and reset, the VideoOptions
// property registry (its src/kbm_menu.c and src/settings_page.c).
constexpr uint32_t kHubInitSlot = 0x2e15940u, kHubInit = 0xdc8a60u, kHubBind = 0xd9fc10u, kVideoTile = 0xdcfcd0u;
constexpr uint32_t kResetSlot = 0x2e60900u, kResetVideo = 0xebf060u, kVideoRegistry = 0x35470d0u;
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

// The log from `from` on (one plugin's FH2PluginInitialize).
bool logged(const char *what, size_t from = 0) {
    EnterCriticalSection(&log_lock);
    bool found = host_log_text.find(what, from) != std::string::npos;
    LeaveCriticalSection(&log_lock);
    return found;
}

size_t log_size() {
    EnterCriticalSection(&log_lock);
    size_t size = host_log_text.size();
    LeaveCriticalSection(&log_lock);
    return size;
}

// A PE file laid out as the loader maps it, in private read-write-execute
// memory, with its 64-bit base relocations applied (vtable slots then hold
// the mapped functions, as in the process).
unsigned char *map_image(const std::wstring &path, uint32_t *size, uint32_t *timestamp) {
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

int deleted_originals, deleted_strings, reads, property_changes;
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
void fake_property_changed(void *, void *) { ++property_changes; }
void fake_set_volume(int) {}
struct MemberHandler {
    void *method;
    int32_t adjust, unused;
};
std::vector<std::pair<std::string, MemberHandler>> bound, hub_bound;
void fake_bind_button(void *page, const char *name, const MemberHandler *handler) {
    events.push_back("bind " + std::to_string((uintptr_t)page) + " " + name);
    bound.push_back({name, *handler});
}
void fake_bind_hub_button(void *page, const char *name, const MemberHandler *handler) {
    events.push_back("bind hub " + std::to_string((uintptr_t)page) + " " + name);
    hub_bound.push_back({name, *handler});
}
void fake_extra_options_init(void *page) { events.push_back("extra options init " + std::to_string((uintptr_t)page)); }
void fake_hub_init(void *page) { events.push_back("game hub init " + std::to_string((uintptr_t)page)); }
void fake_video_tile(void *page) { events.push_back("video tile " + std::to_string((uintptr_t)page)); }
void fake_game_reset(void *page) { events.push_back("game reset " + std::to_string((uintptr_t)page)); }
void fake_extra_options_reset(void *page) {
    events.push_back("extra options reset " + std::to_string((uintptr_t)page));
}

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
        {kBindButton, (void *)(uintptr_t)fake_bind_button},  {kHubBind, (void *)(uintptr_t)fake_bind_hub_button},
        {kHubInit, (void *)(uintptr_t)fake_hub_init},        {kVideoTile, (void *)(uintptr_t)fake_video_tile},
        {kResetVideo, (void *)(uintptr_t)fake_game_reset},
    };
    for (const auto &f : fakes) jump_stub(image, f.rva, f.fake);
    // register_property (0x155ee30) keeps the game's own code: it only links
    // the descriptor into the registry's list.
    // The property templates: the Video page's Subtitles (int), Gamma (float) and OnOffOptions (list)
    // descriptors: a vtable, the name at +0x40.
    void *vtable = (void *)(uintptr_t)0x1234;
    static const char *const names[] = {"Subtitles", "Gamma", "OnOffOptions"};
    unsigned char *descriptors[] = {int_descriptor, float_descriptor, list_descriptor};
    for (int i = 0; i < 3; ++i) {
        memcpy(descriptors[i], &vtable, 8);
        memcpy(descriptors[i] + 0x40, &names[i], 8);
    }
    jump_stub(host, sites->volume_setter, (void *)(uintptr_t)fake_set_volume);
}

void *call_target(uint32_t rva) {
    int32_t displacement;
    memcpy(&displacement, image + rva + 1, 4);
    return image + rva + 5 + displacement;
}

void *slot(uint32_t rva) {
    void *value;
    memcpy(&value, image + rva, 8);
    return value;
}

// The module that holds an address; nullptr outside every module.
HMODULE module_of(const void *address) {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)address, &module);
    return module;
}

// The module a jump "jmp [rip+0]; dq x" leads into.
HMODULE jump_module(const void *jump) {
    const unsigned char *code = (const unsigned char *)jump;
    if (code[0] != 0xff || code[1] != 0x25 || memcmp(code + 2, "\0\0\0\0", 4)) return nullptr;
    void *destination;
    memcpy(&destination, code + 6, 8);
    return module_of(destination);
}

// Reads a page through the resource call site; the XAML that comes back.
std::string read_page(const std::string &xaml, bool *replaced, const char *what) {
    void *output = nullptr;
    next_resource = xaml;
    reads = 0;
    typedef void **(*Reader)(void *, void **);
    ((Reader)call_target(kCallSite))(nullptr, &output);
    check(reads == 1, std::string(what) + ": read once, through every hook of the chain");
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

size_t count(const std::string &text, const std::string &what) {
    size_t n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) ++n;
    return n;
}

// A property registered on the VideoOptions type: the registry's list of
// {descriptor, next} links starts at registry+0x40 (what register_property
// 0x155ee30 does); the descriptor's name is at +0x40, its getter at +0x28.
unsigned char *find_property(const char *name) {
    unsigned char *link;
    memcpy(&link, image + kVideoRegistry + 0x40, 8);
    for (int guard = 0; link && guard < 100000; ++guard) {
        unsigned char *descriptor, *next;
        const char *text;
        memcpy(&descriptor, link, 8);
        memcpy(&next, link + 8, 8);
        if (descriptor) {
            memcpy(&text, descriptor + 0x40, 8);
            MEMORY_BASIC_INFORMATION region;
            if (text && VirtualQuery(text, &region, sizeof(region)) == sizeof(region) && region.State == MEM_COMMIT &&
                !strcmp(text, name))
                return descriptor;
        }
        link = next;
    }
    return nullptr;
}

struct Plugin {
    char letter;
    const char *name;
    std::wstring path;
    HMODULE module;
};

// What a page plugin's log says it chained the resource call onto: [first in
// line on the game's reader, on Extra Options' thunk, on another plugin's jump].
struct ChainText {
    char letter;
    const char *texts[3];
};
const ChainText kChainTexts[] = {
    {'g',
     {"the resource call at 0x16ebb37 is chained to the game's resource reader",
      "the resource call at 0x16ebb37 is chained to Extra Options' resource hook",
      "the resource call at 0x16ebb37 is chained to another module's hook at"}},
    {'e',
     {"XAML resource call 0x16ebb37 chained onto the game's resource reader",
      "XAML resource call 0x16ebb37 chained onto Extra Options' resource thunk",
      "XAML resource call 0x16ebb37 chained onto another module's hook at"}},
    {'p',
     {"the resource call at 0x16ebb37 is chained to the game's resource reader 0xaabec0",
      "the resource call at 0x16ebb37 is chained to Extra Options' resource hook",
      "the resource call at 0x16ebb37 is chained to another module's hook at"}},
    {'k',
     {"KBM MENU: XAML resource call 0x16ebb37 chained onto the game's resource reader 0xaabec0",
      "KBM MENU: XAML resource call 0x16ebb37 chained onto Extra Options' resource thunk 0x13fe354",
      "KBM MENU: XAML resource call 0x16ebb37 chained onto another module's hook at"}},
};

} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc < 15) {
        fprintf(stderr, "usage: combined_test <order> <variant> <work> <game exe> <azimuth exe> <graphics dll> "
                        "<exit game dll> <radio dll> <performance dll> <keyboard & mouse dll> <video xaml> "
                        "<audio xaml> <pause xaml> <settings hub xaml>\n");
        return 2;
    }
    InitializeCriticalSection(&log_lock);
    char order[8] = {}, variant_text[8] = {};
    WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, order, sizeof(order) - 1, nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, variant_text, sizeof(variant_text) - 1, nullptr, nullptr);
    bool eo = !strcmp(variant_text, "eo");
    work = argv[3];
    Plugin plugins[] = {{'g', "Azimuth Graphics Options", argv[6], nullptr},
                        {'e', "Exit Game", argv[7], nullptr},
                        {'r', "Custom Radio", argv[8], nullptr},
                        {'p', "Performance", argv[9], nullptr},
                        {'k', "Keyboard & Mouse", argv[10], nullptr}};
    auto plugin_of = [&](char letter) -> Plugin * {
        for (Plugin &p : plugins)
            if (p.letter == letter) return &p;
        return nullptr;
    };
    std::string video = read_file(argv[11]), audio = read_file(argv[12]), pause = read_file(argv[13]),
                hub = read_file(argv[14]);
    printf("combined_test %s %s\n", order, variant_text);

    uint32_t game_size = 0, host_size = 0, timestamp = 0;
    image = map_image(argv[4], &game_size, nullptr);
    unsigned char *host = map_image(argv[5], &host_size, &timestamp);
    const HostSites *sites = nullptr;
    for (const HostSites &h : kHosts)
        if (h.timestamp == timestamp) sites = &h;
    check(image && game_size == kImageSize, "the game's image is mapped and relocated");
    check(host && sites, std::string("Azimuth's azimuth.exe is mapped and relocated (") + (sites ? sites->name : "?") +
                             ")");
    check(!video.empty() && !audio.empty() && !pause.empty() && !hub.empty(),
          "the Video, Audio, pause and Settings hub page XAML are read");
    if (!image || !host || !sites || video.empty() || audio.empty() || pause.empty() || hub.empty()) return 1;

    // As Extra Options leaves the game: its pause page init hook, and in the
    // eo variant its resource thunk and its Video page reset.
    void *eo_init = (void *)(uintptr_t)fake_extra_options_init;
    memcpy(image + kPauseInitSlot, &eo_init, 8);
    if (eo) {
        jump_stub(image, kThunk, (void *)(uintptr_t)fake_reader);
        int32_t displacement = (int32_t)(kThunk - (kCallSite + 5));
        memcpy(image + kCallSite + 1, &displacement, 4);
        void *eo_reset = (void *)(uintptr_t)fake_extra_options_reset;
        memcpy(image + kResetSlot, &eo_reset, 8);
    }
    void *first_target = call_target(kCallSite);
    unsigned char radio_before[4][8];
    for (int i = 0; i < 2; ++i) memcpy(radio_before[i], image + kRadioSlots[i], 8);
    for (int i = 0; i < 2; ++i) memcpy(radio_before[2 + i], image + kRadioCalls[i], 5);
    void *reset_before = slot(kResetSlot), *hub_init_before = slot(kHubInitSlot);

    // The loader: FH2PluginInitialize of each plugin, in `order`.
    std::vector<Plugin *> resource_chain;  // the page plugins, in the order they chained the call
    for (const char *letter = order; *letter; ++letter) {
        Plugin *plugin = plugin_of(*letter);
        if (!plugin) return 2;
        plugin->module = LoadLibraryExW(plugin->path.c_str(), nullptr,
                                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        check(plugin->module != nullptr, std::string(plugin->name) + ": the DLL loads");
        if (!plugin->module) return 1;
        if (plugin->letter == 'g') {
            auto use = (void (*)(unsigned char *))(uintptr_t)GetProcAddress(plugin->module, "AzgTestUseHost");
            if (use) use(host);
        }
        if (plugin->letter == 'k') {
            // The host is the mapped azimuth.exe, not this process; no input thread of its own.
            auto use = (void (*)(unsigned char *))(uintptr_t)GetProcAddress(plugin->module, "KbmTestUseHost");
            auto no_thread = (void (*)(int))(uintptr_t)GetProcAddress(plugin->module, "KbmTestNoThread");
            check(use && no_thread, "Keyboard & Mouse: its test build takes the mapped Azimuth as its host");
            if (use) use(host);
            if (no_thread) no_thread(1);
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
        size_t log_from = log_size();
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
        int which = !resource_chain.empty() ? 2 : eo ? 1 : 0;
        const char *expected = nullptr;
        for (const ChainText &c : kChainTexts)
            if (c.letter == plugin->letter) expected = c.texts[which];
        static const char *const onto[] = {"the game's reader", "Extra Options' thunk", "the previous plugin's jump"};
        check(expected && logged(expected, log_from),
              std::string(plugin->name) + ": its log says it chained the resource call onto " + onto[which]);
        resource_chain.push_back(plugin);
    }
    printf("%s", host_log_text.c_str());
    size_t pages_log = log_size();
    check(call_target(kCallSite) != first_target && resource_chain.size() == 4 &&
              jump_module(call_target(kCallSite)) == resource_chain.back()->module,
          "one chain at 0x16ebb37: the four page plugins, the last one installed first in line");
    check(logged("CUSTOM RADIO: custom station hooks installed"), "Custom Radio: its station hooks are installed");
    Plugin *radio = plugin_of('r'), *exit_game = plugin_of('e'), *keyboard = plugin_of('k');
    bool radio_in = true;
    for (int i = 0; i < 2; ++i) {
        void *at = slot(kRadioSlots[i]);
        radio_in = radio_in && module_of(at) == radio->module && memcmp(radio_before[i], &at, 8);
        radio_in = radio_in && jump_module(call_target(kRadioCalls[i])) == radio->module;
    }
    check(radio_in, "Custom Radio: its slots and calls lead into it");
    void *init_slot = slot(kPauseInitSlot);
    check(module_of(init_slot) == exit_game->module, "Exit Game: the pause page init slot holds its init");
    check(logged("KBM: native keyboard & mouse hooks installed") &&
              logged("KBM MENU: Settings hub init (slot 0x2e15940) chained onto the game's init 0xdc8a60"),
          "Keyboard & Mouse: its input hooks and the Settings hub init are in");
    check(logged(eo ? "Video page reset chained onto a hook at" :
                      "Video page reset chained onto the game's Video page reset 0xebf060"),
          std::string("Keyboard & Mouse: its log says it chained the Video page reset onto ") +
              (eo ? "Extra Options' reset" : "the game's reset"));
    check(module_of(slot(kHubInitSlot)) == keyboard->module && slot(kHubInitSlot) != hub_init_before &&
              module_of(slot(kResetSlot)) == keyboard->module && slot(kResetSlot) != reset_before,
          "Keyboard & Mouse: the Settings hub init and the Video page reset slots lead into it");
    check(logged("PERFORMANCE: ShowFps=0 and PerfLog=0: nothing is hooked until the counter is turned on"),
          "Performance: no DXGI/D3D12 hook with the counter and the log off (its GPU part is not tested here)");
    check(!logged("nothing is installed") && !logged("could not be chained") && !logged("is not as expected") &&
              !logged("not as expected") && !logged("REFUSED") && !logged("no Video page row"),
          "no plugin refused anything");

    // The pages, through the whole chain.
    install_fakes(host, sites);
    if (!eo) jump_stub(image, kGetResource, (void *)(uintptr_t)fake_reader);
    bool replaced = false;
    std::string out = read_page(video, &replaced, "the Video page");
    check(replaced && count(out, "SelectedIndex=\"{Binding GfxFrameRate}\"") == 1 &&
              count(out, "{Binding PerfFpsCounter}") == 1 && out.find("EOExitGame") == std::string::npos &&
              out.find("EOKeyboard") == std::string::npos && out.find("Kbm") == std::string::npos,
          "the Video page comes back with the Azimuth rows and the FPS counter row, once each (no tile, no "
          "KEYBOARD & MOUSE row)");
    write_file(work + L"\\video_out.xaml", out);
    std::string first_video = out;
    out = read_page(audio, &replaced, "the Audio page");
    check(replaced && out.find("AZG_AzVolume") != std::string::npos && out.find("EOExitGame") == std::string::npos &&
              out.find("PerfFpsCounter") == std::string::npos && out.find("Kbm") == std::string::npos,
          "the Audio page comes back with the Azimuth rows (and nothing else of the plugins)");
    write_file(work + L"\\audio_out.xaml", out);
    out = read_page(pause, &replaced, "the pause page");
    check(replaced && out.find("PART_Button_EOExitGame") != std::string::npos && out.find("AZG_") == std::string::npos &&
              out.find("PERF_") == std::string::npos && out.find("EOKeyboard") == std::string::npos,
          "the pause page comes back with the Exit Game tile (and nothing else of the plugins)");
    write_file(work + L"\\pause_out.xaml", out);
    out = read_page(hub, &replaced, "the Settings hub");
    check(replaced && count(out, "x:Name=\"PART_Button_EOKeyboard\"") == 1 && out.find("AZG_") == std::string::npos &&
              out.find("PERF_") == std::string::npos && out.find("EOExitGame") == std::string::npos,
          "the Settings hub comes back with the KEYBOARD & MOUSE tile, once (and nothing else of the plugins)");
    write_file(work + L"\\hub_out.xaml", out);
    int deleted = deleted_originals;
    out = read_page("<Page x:Class=\"ForzaUI.A_HUD\"/>", &replaced, "a page nobody edits");
    check(!replaced && deleted_originals == deleted, "another page passes through every hook untouched");

    // The Settings hub init: the game's first, then the tile is bound; choosing it opens the Video page.
    events.clear();
    ((void (*)(void *))slot(kHubInitSlot))((void *)(uintptr_t)0x5150);
    check(events.size() == 2 && events[0] == "game hub init 20816" && events[1] == "bind hub 20816 PART_Button_EOKeyboard",
          "the Settings hub init runs the game's init, then binds PART_Button_EOKeyboard");
    check(hub_bound.size() == 1 && module_of(hub_bound[0].second.method) == keyboard->module,
          "the tile is bound to Keyboard & Mouse's handler");
    if (hub_bound.size() == 1) {
        events.clear();
        ((void (*)(void *))hub_bound[0].second.method)((void *)(uintptr_t)0x5150);
        check(events.size() == 1 && events[0] == "video tile 20816",
              "choosing the tile starts the game's transition to the Video page");
    }
    // The Video page right after the tile: the standalone KEYBOARD & MOUSE page, left alone by the others.
    out = read_page(video, &replaced, "the KEYBOARD & MOUSE page");
    check(replaced && out.find("<!-- EO:StandaloneOptionsPage -->") != std::string::npos &&
              out.find("{Binding KbmMode}") != std::string::npos,
          "right after the tile, the Video page comes back as the KEYBOARD & MOUSE page");
    check(out.find("{Binding Gfx") == std::string::npos && out.find("{Binding PerfFpsCounter") == std::string::npos &&
              out.find("{Binding AzVolume") == std::string::npos,
          "no Azimuth Graphics Options or FPS counter row is on the KEYBOARD & MOUSE page");
    write_file(work + L"\\keyboard_out.xaml", out);
    // The next Video page (no new tile choice): the Video page with its rows again, the same as the first.
    out = read_page(video, &replaced, "the Video page again");
    check(replaced && out == first_video, "the next Video page is the Video page with its rows again, as before");
    check(deleted_originals == 6, "each replaced page's original resource is released once (" +
                                      std::to_string(deleted_originals) + " of 6)");

    // The Video page reset: Keyboard & Mouse's page resets only its options; any other page reaches Extra
    // Options' (or the game's) reset. The plugin knows its page once a row of it was read.
    unsigned char *mode = find_property("KbmMode");
    check(mode != nullptr, "Keyboard & Mouse's KbmMode property is registered on the VideoOptions type");
    if (mode) {
        Fake *page = (Fake *)calloc(1, sizeof(Fake));
        page->references = 1;
        void *getter, *boxed = nullptr;
        memcpy(&getter, mode + 0x28, 8);
        ((void **(*)(void **, void *, void *))getter)(&boxed, mode, page);
        check(boxed != nullptr && page->references == 2, "the KEYBOARD & MOUSE page's Mode row is read (the page is held)");
        events.clear();
        property_changes = 0;
        ((void (*)(void *))slot(kResetSlot))(page);
        check(events.empty() && property_changes == 9,
              "Reset on the KEYBOARD & MOUSE page resets its 9 options only (" + std::to_string(property_changes) +
                  " rows read again; " + std::to_string(events.size()) + " other resets)");
    }
    events.clear();
    ((void (*)(void *))slot(kResetSlot))((void *)(uintptr_t)0x6160);
    check(events.size() == 1 && events[0] == (eo ? "extra options reset 24928" : "game reset 24928"),
          std::string("Reset on the Video page reaches ") + (eo ? "Extra Options' reset" : "the game's reset"));

    // The pause page init: Extra Options' stand-in first, then the tile is bound.
    events.clear();
    ((void (*)(void *))init_slot)((void *)(uintptr_t)0x5150);
    check(events.size() == 2 && events[0] == "extra options init 20816" &&
              events[1] == "bind 20816 PART_Button_EOExitGame",
          "the pause page init runs the previous init, then binds PART_Button_EOExitGame");
    check(bound.size() == 1 && module_of(bound[0].second.method) == exit_game->module,
          "the tile is bound to Exit Game's handler");
    printf("%s", host_log_text.substr(pages_log).c_str());
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
