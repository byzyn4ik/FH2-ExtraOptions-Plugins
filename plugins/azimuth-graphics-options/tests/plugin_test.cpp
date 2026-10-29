// Azimuth Graphics Options for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Offline test of the built plugin DLL as the Extra Options loader drives it.
// Never runs the game and never touches the game's files: the real
// executables are only read, into private memory.
//
//   plugin_test <mode> <plugin dll> <work folder> [game exe] [azimuth exe] [video xaml] [audio xaml]
//
// The plugin's INI (beside the DLL) and the azimuth.ini it names are written by
// tests\run_tests.py.
//   nohost    the shipping DLL in this process: not Azimuth, nothing installed
//   disabled  [Graphics] and [Audio] Enabled=0
// With the test build (AzimuthGraphicsOptionsTest.dll), a copy of Azimuth's
// azimuth.exe (1.0.4 or 1.0.2) as the host and a copy of the game's image:
//   badhost   the host's launcher frame rate code changed: an unknown build
//   badgame   a zero-filled game image: the native guards refuse
//   layout    azimuth.ini without [title-1a96545b]
//   native    the resource call still calls the game's reader
//   eo        the call goes to Extra Options' thunk (as 0.3.2 and 0.4.4 patch
//             it) and Realtime shadows = On is patched in; the pages are
//             Extra Options' extended pages
//   polish    native, with the game's text language PL
//   ukrainian native, with the game's text language RU and [Captions] Language=uk
// In the flows the game's native functions and Azimuth's volume setter are
// replaced by fakes after FH2PluginInitialize (whose guards saw the real
// bytes), then the Video and Audio pages are read through the call site and
// their properties are driven like the UI.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fh2_plugin.h"

namespace {

constexpr uint32_t kImageSize = 0x3a18000u;
constexpr uint32_t kCallSite = 0x16ebb37u, kThunk = 0x13fe354u, kGetResource = 0xaabec0u;
constexpr uint32_t kVideoRegistry = 0x35470d0u, kAudioRegistry = 0x35107b0u, kTextTables = 0x3662990u;
constexpr uint32_t kCascades = 0x2207288u;
constexpr uint32_t kRendererPointer = 0x36609a0u, kRendererVtable = 0x2e82b88u, kShadowRead = 0x1131c02u;
constexpr uint32_t kFrameRateVtable = 0x2eb98d0u;

// Azimuth's sites, written down here independently of the plugin's table.
struct HostSites {
    uint32_t timestamp, target_rate, volume_setter, volume, rate_list;
    const char *name;
};
const HostSites kHosts[] = {{0x6ae393ebu, 0x7d8554u, 0xc37f0u, 0x7da2e0u, 0x10528u, "1.0.4"},
                            {0x6ae05dd1u, 0x8e7540u, 0xbaea0u, 0x8e7be8u, 0x105f8u, "1.0.2"}};
const HostSites *sites;

int failures;
std::wstring work;
std::string mode;

void check(bool ok, const std::string &what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

std::string read_text(const std::wstring &path) {
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

void write_file(const std::wstring &path, const char *data, size_t size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD written;
    if (file != INVALID_HANDLE_VALUE) {
        WriteFile(file, data, (DWORD)size, &written, nullptr);
        CloseHandle(file);
    }
}

void host_log(const char *message) {
    HANDLE file = CreateFileW((work + L"\\host.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(file, message, (DWORD)strlen(message), &written, nullptr);
    CloseHandle(file);
}

bool contains(const std::string &text, const char *what) { return text.find(what) != std::string::npos; }

std::wstring ini_value(const wchar_t *section, const wchar_t *key) {
    wchar_t value[256];
    GetPrivateProfileStringW(section, key, L"", value, 256, (work + L"\\azimuth.ini").c_str());
    return value;
}

// A PE file laid out as the loader maps it (no relocations, no imports), in
// private read-write-execute memory.
unsigned char *map_image(const std::wstring &path, uint32_t *size, uint32_t *timestamp) {
    std::string file = read_text(path);
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
    return image;
}

// ---------------------------------------------------------------- fakes

unsigned char *image;

struct Fake {
    void **vtable;
    unsigned char pad[8];
    volatile LONG references;
    unsigned char rest[0x60];
};

int deleted_originals, deleted_strings, cleanups, video_registrations, audio_registrations;
std::vector<std::string> changed;
std::vector<std::pair<void *, std::wstring>> inserted;
int last_boxed = -12345;
float last_boxed_float = -1.0f;
std::vector<int> volumes_set;

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
void fake_object_cleanup(void *) { ++cleanups; }
void fake_delete_object(void *object) {
    void **vtable = ((Fake *)object)->vtable;
    ((void *(*)(void *, unsigned))vtable[0])(object, 1);
}
void *fake_collection_type(void *) { return (void *)(uintptr_t)0x7777; }
void *fake_allocate_typed(size_t size, void *) { return calloc(1, size < sizeof(Fake) ? sizeof(Fake) : size); }
void fake_collection_init(void *object) { ((Fake *)object)->references = 1; }
void **fake_make_string(void **out, const wchar_t *text) {
    Fake *object = (Fake *)calloc(1, sizeof(Fake));
    object->vtable = string_vtable + 1;
    object->references = 1;
    std::wstring *copy = new std::wstring(text ? text : L"");
    memcpy(object->rest, &copy, sizeof(copy));
    *out = object;
    return out;
}
void fake_insert_item(void *items, int, void *label) {
    InterlockedIncrement(&((Fake *)label)->references);
    std::wstring *text;
    memcpy(&text, ((Fake *)label)->rest, sizeof(text));
    inserted.push_back({items, *text});
}
void fake_register_video_type() { ++video_registrations; }
void fake_register_audio_type() { ++audio_registrations; }
unsigned char int_descriptor[0x58], float_descriptor[0x58], list_descriptor[0x58];
void *fake_int_property() { return int_descriptor; }
void *fake_float_property() { return float_descriptor; }
void *fake_collection_property() { return list_descriptor; }
void **fake_box_int(void **out, const int *value) {
    unsigned char *box = (unsigned char *)calloc(1, 0x40);
    memcpy(box + 0x28, value, 4);
    last_boxed = *value;
    *out = box;
    return out;
}
void **fake_box_float(void **out, const float *value) {
    unsigned char *box = (unsigned char *)calloc(1, 0x40);
    memcpy(box + 0x28, value, 4);
    last_boxed_float = *value;
    *out = box;
    return out;
}
void fake_property_changed(void *, void *property) {
    const char *name;
    memcpy(&name, (unsigned char *)property + 0x40, sizeof(name));
    changed.push_back(name ? name : "?");
}
void fake_set_volume(int volume) { volumes_set.push_back(volume); }

std::string next_resource;
Fake *last_original;

// The game's reader (native mode) or Extra Options' hook (eo mode).
void **fake_reader(void *, void **output) {
    if (next_resource.empty()) {
        *output = nullptr;
        return output;
    }
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

void jump_stub(unsigned char *base, uint32_t rva, const void *target) {
    unsigned char code[12] = {0x48, 0xb8};
    uintptr_t address = (uintptr_t)target;
    memcpy(code + 2, &address, 8);
    code[10] = 0xff;
    code[11] = 0xe0;
    memcpy(base + rva, code, sizeof(code));
    FlushInstructionCache(GetCurrentProcess(), base + rva, sizeof(code));
}

void install_fakes(unsigned char *host) {
    const struct {
        uint32_t rva;
        const void *fake;
    } fakes[] = {
        {0x3229c0, (void *)(uintptr_t)fake_allocate},        {0x31c7b0, (void *)(uintptr_t)fake_free},
        {0x143eb80, (void *)(uintptr_t)fake_object_init},    {0x1442050, (void *)(uintptr_t)fake_object_cleanup},
        {0x257730, (void *)(uintptr_t)fake_delete_object},   {0x1476110, (void *)(uintptr_t)fake_collection_type},
        {0x14424e0, (void *)(uintptr_t)fake_allocate_typed}, {0xaefd60, (void *)(uintptr_t)fake_collection_init},
        {0x13fe8e0, (void *)(uintptr_t)fake_make_string},    {0xb52170, (void *)(uintptr_t)fake_insert_item},
        {0xeca710, (void *)(uintptr_t)fake_register_video_type},
        {0xc803c0, (void *)(uintptr_t)fake_register_audio_type},
        {0xed81f0, (void *)(uintptr_t)fake_int_property},    {0xed6e90, (void *)(uintptr_t)fake_float_property},
        {0xed77d0, (void *)(uintptr_t)fake_collection_property},
        {0x4702d0, (void *)(uintptr_t)fake_box_int},         {0x3f5990, (void *)(uintptr_t)fake_box_float},
        {0xcf9c60, (void *)(uintptr_t)fake_property_changed},
    };
    for (const auto &f : fakes) jump_stub(image, f.rva, f.fake);
    // register_property (0x155ee30) keeps the game's own code: it only links
    // the descriptor into the registry's list.
    void *vtable = (void *)(uintptr_t)0x1234;
    memcpy(int_descriptor, &vtable, 8);
    memcpy(float_descriptor, &vtable, 8);
    memcpy(list_descriptor, &vtable, 8);
    memset(int_descriptor + 0x28, 0x11, 0x30);
    memset(float_descriptor + 0x28, 0x33, 0x30);
    memset(list_descriptor + 0x28, 0x22, 0x30);
    // Azimuth's volume setter takes its audio lock: a fake records the calls.
    jump_stub(host, sites->volume_setter, (void *)(uintptr_t)fake_set_volume);
}

void *call_target() {
    int32_t displacement;
    memcpy(&displacement, image + kCallSite + 1, 4);
    return image + kCallSite + 5 + displacement;
}

void **read_page(const std::string &xaml) {
    static void *output;
    next_resource = xaml;
    output = nullptr;
    typedef void **(*Reader)(void *, void **);
    ((Reader)call_target())(nullptr, &output);
    return &output;
}

std::string page_text(void **out) {
    std::string text;
    if (*out && *out != last_original) {
        char *bytes;
        int length;
        memcpy(&bytes, (unsigned char *)*out + 0x28, 8);
        memcpy(&length, (unsigned char *)*out + 0x30, 4);
        text.assign(bytes, (size_t)length);
    }
    return text;
}

struct Property {
    unsigned char *descriptor;
    std::string name;
};

std::vector<Property> registered(uint32_t registry) {
    std::vector<Property> list;
    unsigned char *link;
    memcpy(&link, image + registry + 0x40, sizeof(link));
    for (int guard = 0; link && guard < 1000; ++guard) {
        unsigned char *descriptor;
        memcpy(&descriptor, link, 8);
        const char *name;
        memcpy(&name, descriptor + 0x40, 8);
        list.push_back({descriptor, name ? name : ""});
        memcpy(&link, link + 8, 8);
    }
    return list;
}

unsigned char *property(const char *name) {
    for (uint32_t registry : {kVideoRegistry, kAudioRegistry})
        for (const auto &p : registered(registry))
            if (p.name == name) return p.descriptor;
    return nullptr;
}

int count_prefixed(uint32_t registry, const char *prefix) {
    int n = 0;
    for (const auto &p : registered(registry)) n += p.name.rfind(prefix, 0) == 0;
    return n;
}

void *call_getter(const char *name) {
    unsigned char *descriptor = property(name);
    if (!descriptor) return nullptr;
    typedef void **(*Getter)(void **, void *, void *);
    Getter getter;
    memcpy(&getter, descriptor + 0x28, 8);
    void *out = nullptr;
    getter(&out, descriptor, nullptr);
    return out;
}

int get_value(const char *name) {
    last_boxed = -12345;
    call_getter(name);
    return last_boxed;
}

float get_float(const char *name) {
    last_boxed_float = -1.0f;
    call_getter(name);
    return last_boxed_float;
}

void *get_object(const char *name) { return call_getter(name); }

bool set_raw(const char *name, const void *value) {
    unsigned char *descriptor = property(name);
    if (!descriptor) return false;
    typedef void (*Setter)(void *, void *, void *);
    Setter setter;
    memcpy(&setter, descriptor + 0x30, 8);
    if (!setter) return false;
    unsigned char box[0x40] = {};
    memcpy(box + 0x28, value, 4);
    changed.clear();
    setter(descriptor, (void *)(uintptr_t)0x5150, box);
    return true;
}
bool set_value(const char *name, int value) { return set_raw(name, &value); }
bool set_float(const char *name, float value) { return set_raw(name, &value); }

std::vector<std::wstring> labels(void *collection) {
    std::vector<std::wstring> out;
    for (const auto &item : inserted)
        if (item.first == collection) out.push_back(item.second);
    return out;
}

bool cascades_are(const float *expected) {
    float now[8];
    memcpy(now, image + kCascades, sizeof(now));
    for (int i = 0; i < 8; ++i)
        if (i != 3 && i != 7 && now[i] != expected[i]) return false;
    return true;
}

// -------------------------------------------------------------- the plugin

HMODULE plugin;
FH2PluginContext context;
std::wstring plugin_path, plugin_folder;

BOOL initialize(unsigned char *host) {
    plugin = LoadLibraryExW(plugin_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    check(plugin != nullptr, "the plugin DLL loads");
    if (!plugin) return FALSE;
    auto init = (FH2PluginInitializeFn)(uintptr_t)GetProcAddress(plugin, "FH2PluginInitialize");
    check(init != nullptr, "FH2PluginInitialize is exported");
    if (!init) return FALSE;
    if (host) {
        auto use = (void (*)(unsigned char *))(uintptr_t)GetProcAddress(plugin, "AzgTestUseHost");
        check(use != nullptr, "the test build takes a host image");
        if (use) use(host);
    }
    context.size = sizeof(context);
    context.api_version = FH2_PLUGIN_API_VERSION;
    context.adapter = FH2_PLUGIN_AZIMUTH;
    context.game_image_size = kImageSize;
    context.game_image = image;
    context.plugin_module = plugin;
    context.plugins_directory = plugin_folder.c_str();
    context.plugin_path = plugin_path.c_str();
    context.extra_options_version = "0.4.4";
    context.log_utf8 = host_log;
    return init(&context);
}

std::wstring widen(const char *text) {
    int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring out((size_t)(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, text, -1, &out[0], n);
    return out;
}

const float kLow[8] = {5.0f, 15.0f, 40.0f, 0, 1.6f, 15.0f, 35.0f, 0};
const float kHigh[8] = {5.0f, 15.0f, 124.0f, 0, 1.6f, 15.0f, 120.0f, 0};
const float kUltra[8] = {17.0f, 130.0f, 360.0f, 0, 4.0f, 60.0f, 270.0f, 0};

void set_language(const char *code) {
    static unsigned char manager[0x60];
    unsigned char *text = manager + 0x30;
    memset(text, 0, 16);
    memcpy(text, code, strlen(code));
    uint64_t length = strlen(code), capacity = 15;
    memcpy(text + 16, &length, 8);
    memcpy(text + 24, &capacity, 8);
    void *pointer = manager;
    memcpy(image + kTextTables, &pointer, 8);
}

void video_flow(unsigned char *host, const std::string &video, bool eo, bool polish, bool ukrainian) {
    // A page that is not a settings page passes through untouched.
    std::string hud = "<Page x:Class=\"ForzaUI.A_HudOptions\"/>";
    void **out = read_page(hud);
    check(*out == last_original && deleted_originals == 0, "another page passes through unchanged");
    std::string standalone = "<!-- EO:StandaloneOptionsPage -->" + video;
    out = read_page(standalone);
    check(*out == last_original && deleted_originals == 0, "a plugin's standalone page on the Video class is left alone");

    out = read_page(video);
    check(*out && *out != last_original, "the Video page comes back as a new resource");
    check(deleted_originals == 1, "the original resource is released once");
    std::string extended = page_text(out);
    if (!extended.empty()) {
        void **vtable = ((Fake *)*out)->vtable;
        check(vtable[0] != original_vtable[1], "the new resource deletes itself through the plugin");
    }
    write_file(work + L"\\video_out.xaml", extended.data(), extended.size());
    check(contains(extended, "SelectedIndex=\"{Binding GfxFrameRate}\""), "the Video page has the Frame rate row");
    if (eo) check(contains(extended, "EO_RealtimeShadows"), "Extra Options' own Video rows are kept");
    check(contains(extended, ukrainian ? "Частота кадрів" : polish ? "Liczba klatek" : "Label=\"Frame rate\""),
          ukrainian ? "Ukrainian captions" : polish ? "Polish captions" : "English captions");
    check(video_registrations == 1, "the Video page type is set up before its properties");
    check(audio_registrations == 0, "the Audio page type is not touched by the Video page");

    int ours = count_prefixed(kVideoRegistry, "Gfx");
    check(ours == 18, "18 Video properties registered (9 rows and their choices): " + std::to_string(ours));
    check(!property("GfxFramePacing"), "no Frame pacing row on Azimuth 1.x");

    std::vector<std::wstring> fps = labels(get_object("GfxFrameRateChoices"));
    check(fps.size() == 7 && fps[0] == L"30 FPS" && fps[4] == L"144 FPS" && fps[6] == L"240 FPS",
          "Frame rate choices are Azimuth's list (30 ... 240 FPS)");
    std::vector<std::wstring> resolutions = labels(get_object("GfxResolutionChoices"));
    check(resolutions.size() == 10 && resolutions[9] == L"3840 x 2160 (4K)", "Resolution choices");
    std::vector<std::wstring> hdr = labels(get_object("GfxHdrChoices"));
    check(hdr.size() == 2 && hdr[0] == (ukrainian ? widen("Вимк") : polish ? widen("Wył.") : L"Off"),
          "HDR choices in the page's language");

    static const int rates[] = {30, 60, 90, 120, 144, 165, 240};
    check(get_value("GfxFrameRate") == 3, "Frame rate shows 120 FPS from azimuth.ini");
    LONG *target = (LONG *)(host + sites->target_rate);
    for (int index = 0; index < 7; ++index) {
        int rate = rates[index];
        if (index == 3) continue; // already 120
        set_value("GfxFrameRate", index);
        check(ini_value(L"title-1a96545b", L"frame_rate") == std::to_wstring(rate) && *target == rate &&
                  get_value("GfxFrameRate") == index,
              "Frame rate " + std::to_string(rate) + ": saved and applied at once (Azimuth's target " +
                  std::to_string(*target) + ")");
    }
    set_value("GfxFrameRate", 3);
    check(changed.size() == 1 && changed[0] == "GfxFrameRate", "a change notifies its row");
    changed.clear();
    set_value("GfxFrameRate", 3);
    check(changed.empty(), "choosing the current value writes nothing");

    set_value("GfxShadows", 1);
    check(cascades_are(kLow), "Shadows Default: the console's cascade distances at once");
    set_value("GfxShadows", 3);
    check(changed.size() == 2 && changed[1] == "GfxPreset", "Shadows notifies its row and Graphics");
    check(ini_value(L"title-1a96545b", L"shadow_detail") == L"ultra" &&
              ini_value(L"title-1a96545b", L"shadow_distance") == L"ultra" &&
              ini_value(L"title-1a96545b", L"shadow_redraw") == L"ultra" &&
              ini_value(L"title-1a96545b", L"graphics") == L"custom",
          "Shadows writes shadow_detail/distance/redraw and Graphics = Custom");
    check(get_value("GfxPreset") == 4, "Graphics shows Custom");
    check(cascades_are(kUltra), "Shadows Ultra: the cascade distances are Ultra's at once");
    // A shadow renderer, as the game makes it.
    unsigned char *renderer = (unsigned char *)calloc(1, 0x2400);
    void *vtable = image + kRendererVtable;
    memcpy(renderer, &vtable, 8);
    LONG one = 1;
    memcpy(renderer + 0x21bc, &one, 4);
    memcpy(image + kRendererPointer, &renderer, 8);
    set_value("GfxShadows", 2);
    LONG updates;
    memcpy(&updates, renderer + 0x21bc, 4);
    check(cascades_are(kHigh), "Shadows High: High's cascades");
    check(eo ? updates == 1 : updates == 4,
          eo ? "Realtime shadows on: the renderer's update rate is left alone" : "Shadows High: 4 maps per frame");
    set_value("GfxPreset", 0);
    memcpy(&updates, renderer + 0x21bc, 4);
    check(ini_value(L"title-1a96545b", L"graphics") == L"low" && cascades_are(kLow) && updates == 1,
          "Graphics Low: saved, Low's cascades, 1 map per frame");
    set_value("GfxDisplay", 0);
    check(ini_value(L"launcher", L"display") == L"windowed", "Display mode writes [launcher] display");
    set_value("GfxTextureFiltering", 2);
    check(ini_value(L"title-1a96545b", L"texture_filtering") == L"4x" &&
              ini_value(L"title-1a96545b", L"graphics") == L"custom",
          "Texture filtering writes 4x and Graphics = Custom");
    set_value("GfxAntiAliasing", 1);
    set_value("GfxVSync", 0);
    set_value("GfxHdr", 1);
    set_value("GfxResolution", 5);
    check(ini_value(L"title-1a96545b", L"antialiasing") == L"msaax2" &&
              ini_value(L"title-1a96545b", L"vsync") == L"off" && ini_value(L"title-1a96545b", L"hdr") == L"on" &&
              ini_value(L"title-1a96545b", L"resolution") == L"1440",
          "Anti-aliasing, V-sync, HDR and Resolution write their keys");
    set_value("GfxHdr", 7);
    check(ini_value(L"title-1a96545b", L"hdr") == L"on", "an out-of-range value is ignored");

    size_t before = registered(kVideoRegistry).size();
    out = read_page(video);
    check(*out && *out != last_original && registered(kVideoRegistry).size() == before,
          "a second visit extends the page again without registering twice");
}

void audio_flow(const std::string &audio, bool eo, bool polish, bool ukrainian) {
    int deleted = deleted_originals;
    std::string standalone = "<!-- EO:StandaloneOptionsPage -->" + audio;
    void **out = read_page(standalone);
    check(*out == last_original && deleted_originals == deleted, "a plugin's standalone page on the Audio class is left alone");
    out = read_page(audio);
    check(*out && *out != last_original && deleted_originals == deleted + 1, "the Audio page comes back as a new resource");
    std::string extended = page_text(out);
    write_file(work + L"\\audio_out.xaml", extended.data(), extended.size());
    check(contains(extended, "SelectedIndex=\"{Binding AzAudioOutput}\"") && contains(extended, "Value=\"{Binding AzVolume}\""),
          "the Audio page has the audio output and volume rows");
    if (eo) check(contains(extended, "{Binding MasterVolume}") && contains(extended, "{Binding CheckpointSound}"),
                  "Extra Options' own Audio rows are kept");
    check(contains(extended, ukrainian ? "Аудіовихід (Azimuth)" : polish ? "Wyjście audio (Azimuth)" : "Audio output (Azimuth)"),
          "Audio captions in the page's language");
    check(audio_registrations == 1, "the Audio page type is set up before its properties");
    int ours = count_prefixed(kAudioRegistry, "Az");
    check(ours == 3, "3 Audio properties registered (output, its choices, volume): " + std::to_string(ours));
    check(count_prefixed(kAudioRegistry, "Gfx") == 0 && count_prefixed(kVideoRegistry, "Az") == 0,
          "each page's properties are in its own type");
    std::vector<std::wstring> outputs = labels(get_object("AzAudioOutputChoices"));
    check(outputs.size() == 3 && outputs[0] == (ukrainian ? widen("Стерео") : L"Stereo") &&
              outputs[2] == (ukrainian ? widen("Багатоканальний") : polish ? widen("Wielokanałowe") : L"Multichannel"),
          "Audio output choices: Stereo, System, Multichannel");
    check(get_value("AzAudioOutput") == 0, "Audio output shows stereo from azimuth.ini");
    set_value("AzAudioOutput", 2);
    check(ini_value(L"title-1a96545b", L"audio") == L"multichannel" && volumes_set.empty() &&
              changed.size() == 1 && changed[0] == "AzAudioOutput",
          "Audio output Multichannel: audio=multichannel (after a restart), only its row notified");
    set_value("AzAudioOutput", 1);
    check(ini_value(L"title-1a96545b", L"audio") == L"system", "Audio output System: audio=system");
    set_value("AzAudioOutput", 3);
    check(ini_value(L"title-1a96545b", L"audio") == L"system", "an out-of-range output is ignored");

    float level = get_float("AzVolume");
    check(std::fabs(level - 0.8f) < 0.001f, "Volume shows 80 % from azimuth.ini as 0.80: " + std::to_string(level));
    set_float("AzVolume", 0.55f);
    check(ini_value(L"title-1a96545b", L"volume") == L"55" && volumes_set.size() == 1 && volumes_set[0] == 55,
          "Volume 0.55: volume=55, Azimuth's volume setter called with 55 at once");
    set_float("AzVolume", 0.0f);
    check(ini_value(L"title-1a96545b", L"volume") == L"0" && volumes_set.size() == 2 && volumes_set[1] == 0,
          "Volume 0: volume=0, applied");
    set_float("AzVolume", 1.0f);
    check(ini_value(L"title-1a96545b", L"volume") == L"100" && volumes_set.back() == 100, "Volume 1.0: volume=100");
    size_t calls = volumes_set.size();
    set_float("AzVolume", 1.0f);
    check(volumes_set.size() == calls, "the same volume again writes and applies nothing");
    set_float("AzVolume", 3.0f);
    check(ini_value(L"title-1a96545b", L"volume") == L"100" && volumes_set.size() == calls,
          "an out-of-range volume is ignored");
    size_t before = registered(kAudioRegistry).size();
    out = read_page(audio);
    check(*out && *out != last_original && registered(kAudioRegistry).size() == before,
          "a second visit extends the Audio page again without registering twice");
}

int run_flow(unsigned char *host, const std::string &video, const std::string &audio) {
    bool eo = mode == "eo", polish = mode == "polish", ukrainian = mode == "ukrainian";
    unsigned char frame_rate_vtable[0x40];
    memcpy(frame_rate_vtable, image + kFrameRateVtable, sizeof(frame_rate_vtable));
    if (eo) {
        // As Extra Options 0.3.2 and 0.4.4: its thunk in padding, the call
        // pointed at it, and Realtime shadows = On (all cascades every frame).
        jump_stub(image, kThunk, (void *)(uintptr_t)fake_reader);
        int32_t displacement = (int32_t)(kThunk - (kCallSite + 5));
        memcpy(image + kCallSite + 1, &displacement, 4);
        static const unsigned char all_cascades[] = {0xb8, 0x04, 0x00, 0x00, 0x00, 0x90, 0x90};
        memcpy(image + kShadowRead, all_cascades, sizeof(all_cascades));
    }
    if (polish) set_language("PL");
    if (ukrainian) set_language("RU");
    void *before = call_target();
    BOOL ok = initialize(host);
    check(ok == TRUE, "FH2PluginInitialize returns TRUE");
    auto set_rates = (void (*)(const int *, int))(uintptr_t)GetProcAddress(plugin, "AzgTestSetFrameRates");
    static const int rates[] = {30, 60, 90, 120, 144, 165, 240}; // Azimuth's list for a 240 Hz display
    if (set_rates) set_rates(rates, 7);
    void *after = call_target();
    check(after != before, "the resource call site now calls the plugin's jump");
    unsigned char jump[6];
    memcpy(jump, after, 6);
    check(jump[0] == 0xff && jump[1] == 0x25, "the jump is jmp [rip+0] (within 2 GB of the site)");
    std::string log = read_text(work + L"\\host.log");
    check(contains(log, (std::string("host: Azimuth ") + sites->name).c_str()), std::string("the log names Azimuth ") + sites->name);
    check(contains(log, eo ? "chained to Extra Options' resource hook (thunk 0x13fe354"
                           : "chained to the game's resource reader 0xaabec0"),
          "the log names what the call was chained to");
    check(contains(log, "frame rate applies at once"), "frame rate applies at once");
    check(contains(log, "display mode applies at once"), "display mode applies at once");
    check(contains(log, "volume applies at once"), "volume applies at once");
    check(contains(log, "shadow distance applies at once"), "shadow distance applies at once");
    check(contains(log, eo ? "Realtime shadows updates every cascade each frame; shadow update rate left to it"
                           : "shadow update rate applies at once"),
          eo ? "Extra Options' Realtime shadows patch is left alone" : "shadow update rate applies at once");
    if (ukrainian) check(contains(log, "captions: Ukrainian"), "[Captions] Language=uk is logged");

    install_fakes(host);
    if (!eo) jump_stub(image, kGetResource, (void *)(uintptr_t)fake_reader);
    video_flow(host, video, eo, polish, ukrainian);
    if (!audio.empty()) audio_flow(audio, eo, polish, ukrainian);
    check(!memcmp(frame_rate_vtable, image + kFrameRateVtable, sizeof(frame_rate_vtable)),
          "the game's FrameRate render scenario vtable (smooth menus) is not touched");
    return 0;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: plugin_test <mode> <plugin dll> <work> [game exe] [azimuth exe] [video xaml] [audio xaml]\n");
        return 2;
    }
    char narrow_mode[32] = {};
    WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, narrow_mode, sizeof(narrow_mode) - 1, nullptr, nullptr);
    mode = narrow_mode;
    plugin_path = argv[2];
    plugin_folder = plugin_path.substr(0, plugin_path.find_last_of(L"\\/"));
    work = argv[3];
    printf("plugin_test %s\n", mode.c_str());

    if (mode == "nohost" || mode == "disabled") {
        image = (unsigned char *)VirtualAlloc(nullptr, kImageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        BOOL ok = initialize(nullptr);
        std::string log = read_text(work + L"\\host.log");
        check(ok == TRUE, "FH2PluginInitialize returns TRUE");
        if (mode == "nohost")
            check(contains(log, "the host is not Azimuth 1.0.4 or 1.0.2") && contains(log, "nothing is installed"),
                  "not on Azimuth 1.0.4 or 1.0.2: nothing is installed, and the log says why");
        else
            check(contains(log, "[Graphics] and [Audio] Enabled=0; nothing is installed"), "Enabled=0: nothing is installed");
        unsigned char zero[16] = {};
        check(!memcmp(image + kCallSite, zero, sizeof(zero)), "the game image is left as it is");
        printf("%d failure(s)\n", failures);
        return failures ? 1 : 0;
    }
    if (argc < 6) {
        fprintf(stderr, "%s needs the game and azimuth executables\n", mode.c_str());
        return 2;
    }
    uint32_t host_size = 0, game_size = 0, timestamp = 0;
    unsigned char *host = map_image(argv[5], &host_size, &timestamp);
    for (const HostSites &h : kHosts)
        if (h.timestamp == timestamp) sites = &h;
    check(host != nullptr && sites, std::string("Azimuth's azimuth.exe is mapped (") + (sites ? sites->name : "?") + ")");
    if (mode == "badgame") {
        image = (unsigned char *)VirtualAlloc(nullptr, kImageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    } else {
        image = map_image(argv[4], &game_size, nullptr);
        check(image != nullptr && game_size == kImageSize, "the game's image is mapped");
    }
    if (!host || !sites || !image) return 1;
    unsigned char call[16];
    memcpy(call, image + kCallSite, sizeof(call));
    if (mode == "badhost") host[sites->rate_list + 4] = 0x3d; // the launcher's 60 becomes 61
    if (mode == "badhost" || mode == "badgame" || mode == "layout") {
        BOOL ok = initialize(host);
        std::string log = read_text(work + L"\\host.log");
        check(ok == TRUE, "FH2PluginInitialize returns TRUE");
        std::string expected = mode == "badhost"   ? std::string("is not Azimuth ") + sites->name +
                                                         "'s; an unknown build; nothing is installed"
                               : mode == "badgame" ? "DeleteObject at 0x257730 is not as expected; nothing is installed"
                                                   : "has no [title-1a96545b] section";
        check(contains(log, expected.c_str()), mode + ": nothing is installed, and the log says why");
        check(!memcmp(call, image + kCallSite, sizeof(call)), "the resource call site is left as it is");
        printf("%d failure(s)\n", failures);
        return failures ? 1 : 0;
    }
    std::string video = argc > 6 ? read_text(argv[6]) : std::string();
    std::string audio = argc > 7 ? read_text(argv[7]) : std::string();
    check(!video.empty(), "the Video page XAML is read");
    if (argc > 7) check(!audio.empty(), "the Audio page XAML is read");
    run_flow(host, video, audio);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
