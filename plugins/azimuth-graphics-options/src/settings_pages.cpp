// Azimuth Graphics Options for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native calls and object layouts are those Extra Options uses for its own
// settings rows (features/video_options.c in 0.3.2 and 0.4.4), checked here
// before use.
#include "settings_pages.h"

#include <cstddef>
#include <cstring>
#include <string>

#include "call_chain.h"
#include "common.h"
#include "language.h"
#include "live.h"

namespace azg {
namespace pages {

namespace {

enum : uint32_t {
    kDeleteObject = 0x257730,
    kCollectionType = 0x1476110,
    kAllocateTyped = 0x14424e0,
    kCollectionInit = 0xaefd60,
    kMakeString = 0x13fe8e0,
    kInsertItem = 0xb52170,
    kRegisterVideoType = 0xeca710,
    kRegisterAudioType = 0xc803c0,
    kIntProperty = 0xed81f0,
    kFloatProperty = 0xed6e90,
    kCollectionProperty = 0xed77d0,
    kRegisterProperty = 0x155ee30,
    kAllocate = 0x3229c0,
    kFree = 0x31c7b0,
    kObjectInit = 0x143eb80,
    kObjectCleanup = 0x1442050,
    kBoxInt = 0x4702d0,
    kBoxFloat = 0x3f5990,
    kPropertyChanged = 0xcf9c60,
    kGetResource = 0xaabec0,
    kVideoRegistry = 0x35470d0,
    kAudioRegistry = 0x35107b0,
    kExtraOptionsThunk = 0x13fe354,
};

// The first 12 bytes of each native function, from the inspected executable.
struct Prologue {
    uint32_t rva;
    const char *name;
    unsigned char bytes[12];
};
const Prologue kPrologues[] = {
    {kDeleteObject, "DeleteObject", {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0xe8, 0xb2, 0xa2}},
    {kCollectionType, "CollectionType", {0x48, 0x8d, 0x05, 0x49, 0x10, 0x21, 0x02, 0xc3, 0xcc, 0xcc, 0xcc, 0xcc}},
    {kAllocateTyped, "AllocateTyped", {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b}},
    {kCollectionInit, "CollectionInit", {0x48, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x56, 0x57, 0x48, 0x83, 0xec, 0x30}},
    {kMakeString, "MakeString", {0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x28, 0xfe}},
    {kInsertItem, "InsertItem", {0x48, 0x8b, 0xc4, 0x57, 0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00, 0x48}},
    {kRegisterVideoType, "RegisterVideoType", {0x40, 0x53, 0x48, 0x81, 0xec, 0xd0, 0x00, 0x00, 0x00, 0x48, 0xc7, 0x44}},
    {kRegisterAudioType, "RegisterAudioType", {0x40, 0x53, 0x48, 0x81, 0xec, 0xd0, 0x00, 0x00, 0x00, 0x48, 0xc7, 0x44}},
    {kIntProperty, "IntProperty", {0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe}},
    {kFloatProperty, "FloatProperty", {0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe}},
    {kCollectionProperty, "CollectionProperty", {0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe}},
    {kRegisterProperty, "RegisterProperty", {0x48, 0x8b, 0x41, 0x40, 0x48, 0x89, 0x42, 0x08, 0x48, 0x89, 0x51, 0x40}},
    {kAllocate, "Allocate", {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x8b, 0x15}},
    {kFree, "Free", {0x48, 0x85, 0xc9, 0x74, 0x1d, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b}},
    {kObjectInit, "ObjectInit", {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8d}},
    {kObjectCleanup, "ObjectCleanup", {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8d, 0x05, 0xb3, 0xd0, 0x9f}},
    {kBoxInt, "BoxInt", {0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xd9, 0xc7, 0x44, 0x24}},
    {kBoxFloat, "BoxFloat", {0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xd9, 0xc7, 0x44, 0x24}},
    {kPropertyChanged, "PropertyChanged", {0x40, 0x57, 0x48, 0x83, 0xec, 0x60, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe}},
    {kGetResource, "GetResource", {0x48, 0x83, 0xec, 0x18, 0x48, 0x8b, 0x41, 0x38, 0xc7, 0x04, 0x24, 0x00}},
};
// "lea rax,[rip+name]" in each property template: the property it copies.
const struct {
    uint32_t lea;
    const char *name;
} kTemplateNames[] = {{0xed825b, "Subtitles"}, {0xed6efb, "Gamma"}, {0xed782e, "OnOffOptions"}};
// "lea rcx,[rip+registry]; call RegisterProperty" in each page type's set-up.
const struct {
    uint32_t lea, registry;
    const char *page;
} kRegistrations[] = {{0xeca808, kVideoRegistry, "Video"}, {0xc804b8, kAudioRegistry, "Audio"}};
// After the call: mov [rdi],r15; mov rcx,[rax]; mov [rdi+8],rcx.
const unsigned char kAfterCall[] = {0x4c, 0x89, 0x3f, 0x48, 0x8b, 0x08, 0x48, 0x89, 0x4f, 0x08};
// The collection's vtables and where they go, as Extra Options sets them.
const uint32_t kCollectionVtables[] = {0x2db73f8, 0x2db7470, 0x2db74d0, 0x2db74e8, 0x2db7508};
const uint32_t kCollectionVtableOffsets[] = {0, 0x30, 0x38, 0x40, 0x48};

struct NativeProperty {
    void **vtable;
    unsigned char header[0x20];
    void *getter, *setter, *unused;
    const char *name;
    void *type;
    uint64_t flags;
};
struct NativePropertyLink {
    NativeProperty *property;
    NativePropertyLink *next;
};
struct NativeResource {
    void **vtable;
    unsigned char header[0x20];
    char *bytes;
    int length;
    int capacity;
};
static_assert(sizeof(NativeProperty) == 0x58, "native property descriptor");
static_assert(offsetof(NativeProperty, name) == 0x40, "native property name");
static_assert(offsetof(NativeResource, bytes) == 0x28, "native resource buffer");

typedef void *(*Allocate)(size_t);
typedef void (*Free)(void *);
typedef void (*ObjectFunction)(void *);
typedef void *(*Factory)(void);
typedef void *(*TypeFactory)(void *);
typedef void *(*AllocateTyped)(size_t, void *);
typedef void **(*BoxValue)(void **, const void *);
typedef void (*PropertyChanged)(void *, void *);
typedef void **(*MakeString)(void **, const wchar_t *);
typedef void (*InsertItem)(void *, int, void *);
typedef void (*RegisterProperty)(void *, NativePropertyLink *);
typedef void **(*GetResource)(void *, void **);

const uint32_t kTypeSetUp[] = {kRegisterVideoType, kRegisterAudioType};
const uint32_t kRegistry[] = {kVideoRegistry, kAudioRegistry};
const char *const kPageName[] = {"Video", "Audio"};

unsigned char *image;
GetResource previous_reader;
LiveState live_text;
bool live_apply;
NativeProperty properties[2 * kRowCount]; // values, then choices
NativePropertyLink links[2 * kRowCount];
void *choices[kRowCount];
volatile LONG registered[2], told_layout[2], told_failure[2], told_added[2];
SRWLOCK registration_lock = SRWLOCK_INIT;
void *resource_vtable[7];

template <typename T> T native(uint32_t rva) { return (T)(void *)(image + rva); }

void release_object(void *object) {
    if (object && InterlockedDecrement((volatile LONG *)((unsigned char *)object + 0x10)) == 0)
        native<ObjectFunction>(kDeleteObject)(object);
}

void add_reference(void *object) {
    if (object) InterlockedIncrement((volatile LONG *)((unsigned char *)object + 0x10));
}

unsigned property_index(const NativeProperty *property) {
    uintptr_t address = (uintptr_t)property, first = (uintptr_t)properties;
    size_t offset = address - first;
    if (address < first || offset >= sizeof(properties) || offset % sizeof(*properties)) return 2 * kRowCount;
    return (unsigned)(offset / sizeof(*properties));
}

void **get_value(void **output, NativeProperty *property, void *) {
    unsigned index = property_index(property);
    *output = nullptr;
    if (index < kRowCount && rows::slider(index)) {
        float value = (float)rows::get(index) / 100.0f;
        return native<BoxValue>(kBoxFloat)(output, &value);
    }
    if (index < kRowCount) {
        int value = rows::get(index);
        return native<BoxValue>(kBoxInt)(output, &value);
    }
    if (index < 2 * kRowCount) {
        *output = choices[index - kRowCount];
        add_reference(*output);
    }
    return output;
}

void set_value(NativeProperty *property, void *page, void *boxed) {
    unsigned row = property_index(property);
    int value;
    if (!boxed || row >= kRowCount) return;
    if (rows::slider(row)) {
        // A 0..1 slider; the row keeps whole percent.
        float level;
        if (!safe_read((unsigned char *)boxed + 0x28, &level, sizeof(level)) || !(level >= -0.001f && level <= 1.001f))
            return;
        value = (int)(level * 100.0f + 0.5f);
        if (value < 0) value = 0;
        if (value > 100) value = 100;
    } else if (!safe_read((unsigned char *)boxed + 0x28, &value, sizeof(value))) {
        return;
    }
    if (!rows::set(row, value)) return;
    if (live_apply) live::apply(row, value);
    native<PropertyChanged>(kPropertyChanged)(page, property);
    // Shadows and Texture filtering switch Graphics to Custom (same page).
    if (row == kShadows || row == kTextureFiltering)
        native<PropertyChanged>(kPropertyChanged)(page, &properties[kPreset]);
}

bool make_choices(unsigned row, Language language) {
    void *type = native<TypeFactory>(kCollectionType)(nullptr);
    void *items = native<AllocateTyped>(kAllocateTyped)(0x58, type);
    if (!items) return false;
    native<ObjectFunction>(kCollectionInit)(items);
    for (unsigned i = 0; i < sizeof(kCollectionVtables) / sizeof(kCollectionVtables[0]); ++i)
        *(void **)((unsigned char *)items + kCollectionVtableOffsets[i]) = image + kCollectionVtables[i];
    for (unsigned index = 0; index < rows::count(row); ++index) {
        void *label = nullptr;
        native<MakeString>(kMakeString)(&label, rows::choice(row, index, language));
        if (!label) {
            release_object(items);
            return false;
        }
        native<InsertItem>(kInsertItem)(items, (int)index, label);
        release_object(label);
    }
    choices[row] = items;
    return true;
}

// Once per page and process, before the page's XAML is parsed.
bool register_properties(Page page, Language language) {
    if (InterlockedCompareExchange(&registered[page], 0, 0)) return true;
    AcquireSRWLockExclusive(&registration_lock);
    bool ok = registered[page] != 0;
    if (!ok) {
        native<void (*)(void)>(kTypeSetUp[page])();
        ok = true;
        for (unsigned row = 0; row < kRowCount && ok; ++row)
            if (rows::shown(row) && rows::page(row) == page && !rows::slider(row) && !make_choices(row, language))
                ok = false;
        NativeProperty *int_kind = (NativeProperty *)native<Factory>(kIntProperty)();
        NativeProperty *float_kind = (NativeProperty *)native<Factory>(kFloatProperty)();
        NativeProperty *list_kind = (NativeProperty *)native<Factory>(kCollectionProperty)();
        ok = ok && int_kind && float_kind && list_kind;
        if (!ok) {
            for (unsigned row = 0; row < kRowCount; ++row) {
                if (rows::page(row) != page) continue;
                release_object(choices[row]);
                choices[row] = nullptr;
            }
        } else {
            for (unsigned index = 0; index < 2 * kRowCount; ++index) {
                unsigned row = index % kRowCount;
                bool value = index < kRowCount;
                if (!rows::shown(row) || rows::page(row) != page || (!value && rows::slider(row))) continue;
                NativeProperty *property = &properties[index];
                const NativeProperty *kind = !value ? list_kind : rows::slider(row) ? float_kind : int_kind;
                native<ObjectFunction>(kObjectInit)(property);
                property->vtable = kind->vtable;
                memcpy((unsigned char *)property + 0x28, (const unsigned char *)kind + 0x28, 0x30);
                property->name = value ? rows::property(row) : rows::collection(row);
                property->getter = (void *)get_value;
                property->setter = value ? (void *)set_value : nullptr;
                links[index].property = property;
                native<RegisterProperty>(kRegisterProperty)(image + kRegistry[page], &links[index]);
            }
            InterlockedExchange(&registered[page], 1);
        }
    }
    ReleaseSRWLockExclusive(&registration_lock);
    return ok;
}

void *delete_resource(NativeResource *resource, unsigned flags) {
    native<ObjectFunction>(kObjectCleanup)(resource);
    if (flags & 1) native<Free>(kFree)(resource);
    return resource;
}

const char kVideoClass[] = "x:Class=\"ForzaUI.A_VideoOptions\"";
const char kAudioClass[] = "x:Class=\"ForzaUI.A_AudioOptions\"";
// Extra Options 0.4.4: a plugin's own page reusing a native options class.
const char kStandalone[] = "<!-- EO:StandaloneOptionsPage -->";

INIT_ONCE rates_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK find_frame_rates(PINIT_ONCE, PVOID, PVOID *) {
    if (rows::frame_rates_set()) return TRUE; // the test build's fixed list
    unsigned refresh = live::display_refresh();
    rows::set_frame_rates(live::frame_rates(refresh));
    std::string list;
    for (int rate : rows::frame_rates()) list += (list.empty() ? "" : ", ") + std::to_string(rate);
    say("Frame rate choices for a " + std::to_string(refresh) + " Hz display, as Azimuth's launcher: " + list);
    return TRUE;
}

bool page_shown(Page page) {
    for (unsigned row = 0; row < kRowCount; ++row)
        if (rows::shown(row) && rows::page(row) == page) return true;
    return false;
}

void **read_resource(void *reader, void **output) {
    previous_reader(reader, output);
    NativeResource *original = (NativeResource *)*output;
    if (!original || !original->bytes || original->length <= 0 || original->length > (1 << 20)) return output;
    size_t length = (size_t)original->length;
    Page page;
    if (find(original->bytes, length, kVideoClass))
        page = kVideoPage;
    else if (find(original->bytes, length, kAudioClass))
        page = kAudioPage;
    else
        return output;
    if (!page_shown(page) || find(original->bytes, length, kStandalone)) return output;
    size_t capacity = length + 0x10000;
    NativeResource *replacement = (NativeResource *)native<Allocate>(kAllocate)(sizeof(NativeResource) + capacity);
    if (!replacement) return output;
    char *bytes = (char *)(replacement + 1);
    // Once, when the Video page is first read (Azimuth's window is up by then).
    if (page == kVideoPage) InitOnceExecuteOnce(&rates_once, find_frame_rates, nullptr, nullptr);
    Language language = language::current(image);
    const char *why = nullptr;
    size_t extended = page == kVideoPage
                          ? extend_video(original->bytes, length, bytes, capacity, language, live_text, &why)
                          : extend_audio(original->bytes, length, bytes, capacity, language, live_text, &why);
    if (!extended) {
        native<Free>(kFree)(replacement);
        if (!InterlockedExchange(&told_layout[page], 1))
            say(std::string("the ") + kPageName[page] + " page is not as expected (" + (why ? why : "?") +
                "); its rows are not added");
        return output;
    }
    if (!register_properties(page, language)) {
        native<Free>(kFree)(replacement);
        if (!InterlockedExchange(&told_failure[page], 1))
            say(std::string("the ") + kPageName[page] + " page properties could not be made; rows not added");
        return output;
    }
    // As Extra Options does: the game's resource object with this buffer,
    // owned by the parser through the native reference count.
    native<ObjectFunction>(kObjectInit)(replacement);
    memcpy(resource_vtable, original->vtable - 1, sizeof(resource_vtable));
    resource_vtable[1] = (void *)delete_resource; // vtable slot 0, after the type information
    replacement->vtable = resource_vtable + 1;
    replacement->bytes = bytes;
    replacement->length = (int)extended;
    replacement->capacity = (int)capacity;
    *output = replacement;
    release_object(original);
    if (!InterlockedExchange(&told_added[page], 1))
        say(std::string("Azimuth rows added to the ") + kPageName[page] + " page (" +
            (language == kPolish ? "Polish" : language == kUkrainian ? "Ukrainian" : "English") + ")");
    return output;
}

// ------------------------------------------------------------------ XAML

struct Buffer {
    char *bytes;
    size_t length, capacity;
    bool valid;
    void add(const char *text, size_t size) {
        if (!valid || size >= capacity - length) {
            valid = false;
            return;
        }
        memcpy(bytes + length, text, size);
        length += size;
    }
    void add(const char *text) { add(text, strlen(text)); }
    void attribute(const char *text) {
        for (; *text; ++text) {
            const char *escaped = *text == '&' ? "&amp;" : *text == '<' ? "&lt;" : *text == '>' ? "&gt;"
                                  : *text == '"' ? "&quot;" : nullptr;
            if (escaped)
                add(escaped);
            else
                add(text, 1);
        }
    }
};

void add_resources(Buffer &out, Page page, Language language, const LiveState &live) {
    for (unsigned row = 0; row < kRowCount; ++row) {
        if (!rows::shown(row) || rows::page(row) != page) continue;
        out.add("<local:TileMenuDataItem x:Key=\"AZG_");
        out.add(rows::property(row));
        out.add("\" Heading=\"");
        out.attribute(rows::name(row, language));
        out.add("\" Description=\"");
        out.attribute(rows::description(row, language, live).c_str());
        out.add("\"/>");
    }
    if (page == kAudioPage && rows::shown(kVolume))
        // The page's own volume slider look, from 0 and in 5 % steps.
        out.add("<Style x:Key=\"AZG_VolumeSlider\" TargetType=\"Slider\" "
                "BasedOn=\"{StaticResource A_Style_Slider_Options_Audio_Min1}\">"
                "<Setter Property=\"Minimum\" Value=\"0\"/><Setter Property=\"SmallChange\" Value=\"0.05\"/></Style>");
}

void add_rows(Buffer &out, Page page, Language language) {
    for (unsigned row = 0; row < kRowCount; ++row) {
        if (!rows::shown(row) || rows::page(row) != page) continue;
        if (rows::slider(row)) {
            out.add("<Slider Value=\"{Binding ");
            out.add(rows::property(row));
            out.add("}\" Tag=\"{StaticResource AZG_");
            out.add(rows::property(row));
            out.add("}\" Style=\"{StaticResource AZG_VolumeSlider}\"/>");
            continue;
        }
        out.add("<local:SpinnerControl Tag=\"{StaticResource AZG_");
        out.add(rows::property(row));
        out.add("}\" Style=\"{StaticResource A_Style_SpinnerControl_Default}\" Label=\"");
        out.attribute(rows::name(row, language));
        out.add("\" SelectedIndex=\"{Binding ");
        out.add(rows::property(row));
        out.add("}\" ItemsSource=\"{Binding ");
        out.add(rows::collection(row));
        out.add("}\"/>");
    }
}

const char kList[] = "<local:CommonListBox x:Name=\"OptionsList\"";
const char kListEnd[] = "</local:CommonListBox>";
const char kFixedStyle[] = "Style=\"{StaticResource A_Style_ListBox_List_Default}\"";
const char kScrollableStyle[] = "Style=\"{StaticResource A_Style_ListBox_Scrollable}\"";
const char kBom[] = "\xef\xbb\xbf"; // the game's XML reader reads UTF-8 only after it

// Copies `text` with the resources added at `resources`, the rows at
// `rows_at`, the list style swapped at `style` and the height replaced at
// `height` (12 bytes) by `new_height`.
size_t write_page(const char *text, size_t length, char *output, size_t capacity, Page page, Language language,
                  const LiveState &live, const char *resources, const char *rows_at, const char *style,
                  const char *height, const char *new_height, const char **why) {
    Buffer out = {output, 0, capacity, true};
    const char *end = text + length;
    if (length < 3 || memcmp(text, kBom, 3)) out.add(kBom, 3);
    for (const char *at = text; at < end && out.valid;) {
        if (at == resources) add_resources(out, page, language, live);
        if (at == rows_at) add_rows(out, page, language);
        if (at == style) {
            // Many rows now: the scrollable list style instead of the fixed one.
            out.add(kScrollableStyle);
            at += sizeof(kFixedStyle) - 1;
        } else if (at == height) {
            out.add(new_height);
            at += 12;
        } else {
            out.add(at, 1);
            ++at;
        }
    }
    if (!out.valid) {
        if (why) *why = "the extended page does not fit its buffer";
        return 0;
    }
    output[out.length] = 0;
    return out.length;
}

} // namespace

const char *check_game(const unsigned char *game, uint32_t size, const char **target) {
    static std::string problem;
    if (target) *target = nullptr;
    if (!game || size != kImageSize) return "the game image is not the supported size (0x3a18000)";
    for (const Prologue &p : kPrologues) {
        if (!bytes_equal(game + p.rva, p.bytes, sizeof(p.bytes))) {
            problem = std::string("the game's ") + p.name + " at " + hex(p.rva) + " is not as expected";
            return problem.c_str();
        }
    }
    for (const auto &t : kTemplateNames) {
        unsigned char lea[7];
        int32_t displacement;
        char name[16] = {};
        if (!safe_read(game + t.lea, lea, sizeof(lea)) || lea[0] != 0x48 || lea[1] != 0x8d || lea[2] != 0x05 ||
            (memcpy(&displacement, lea + 3, 4), !safe_read(game + t.lea + 7 + displacement, name, strlen(t.name) + 1)) ||
            strcmp(name, t.name)) {
            problem = std::string("the game's ") + t.name + " property template is not as expected";
            return problem.c_str();
        }
    }
    for (const auto &r : kRegistrations) {
        unsigned char code[12];
        int32_t displacement, call;
        if (!safe_read(game + r.lea, code, sizeof(code)) || code[0] != 0x48 || code[1] != 0x8d || code[2] != 0x0d ||
            code[7] != 0xe8 || (memcpy(&displacement, code + 3, 4), r.lea + 7 + (uint32_t)displacement != r.registry) ||
            (memcpy(&call, code + 8, 4), r.lea + 12 + (uint32_t)call != kRegisterProperty)) {
            problem = std::string("the game's ") + r.page + " page type is not as expected";
            return problem.c_str();
        }
    }
    unsigned char code[5];
    if (!safe_read(game + kResourceCall, code, 5) || code[0] != 0xe8 ||
        !bytes_equal(game + kResourceCall + 5, kAfterCall, sizeof(kAfterCall)))
        return "the XAML resource call at 0x16ebb37 is not as expected";
    const unsigned char *called = (const unsigned char *)call_target(game + kResourceCall);
    static std::string what;
    if (called == game + kGetResource) {
        what = "the game's resource reader 0xaabec0 (Extra Options' hook is not there)";
    } else if (called == game + kExtraOptionsThunk) {
        unsigned char thunk[12];
        if (!safe_read(called, thunk, 12) || thunk[0] != 0x48 || thunk[1] != 0xb8 || thunk[10] != 0xff ||
            thunk[11] != 0xe0)
            return "the resource call goes to 0x13fe354, which is not Extra Options' thunk";
        uint64_t hook;
        memcpy(&hook, thunk + 2, 8);
        what = "Extra Options' resource hook (thunk 0x13fe354 -> " + hex(hook) + ")";
    } else {
        what = "another module's hook at " + hex((uintptr_t)called);
    }
    if (!is_code(called)) return "the resource call's target is not executable code";
    if (target) *target = what.c_str();
    return nullptr;
}

size_t extend_video(const char *text, size_t length, char *output, size_t capacity, Language language,
                    const LiveState &live, const char **why) {
    static const char kTitle[] = "Content=\"{local:Loc ScreenTitles.IDS_VideoOptions}\"";
    static const char kBrightness[] = "A_Style_ContentControl_BrightnessSquare";
    static const char kExtraOptionsRow[] = "<local:SpinnerControl Tag=\"{StaticResource EO_";
    const char *fail = nullptr;
    const char *end = text ? text + length : nullptr;
    const char *resources = find(text, length, "</ResourceDictionary>");
    const char *list = find(text, length, kList);
    const char *list_end = list ? find_in(list, end, kListEnd) : nullptr;
    const char *opening_end = list_end ? find_in(list, list_end, ">") : nullptr;
    const char *height = opening_end ? find_in(list, opening_end, "Height=\"110\"") : nullptr;
    if (!height && opening_end) height = find_in(list, opening_end, "Height=\"275\"");
    const char *style = opening_end ? find_in(list, opening_end, kFixedStyle) : nullptr;
    const char *rows_at = opening_end ? find_in(opening_end, list_end, kExtraOptionsRow) : nullptr;
    if (!rows_at) rows_at = list_end;
    if (!text || !output || capacity < 4 || !find(text, length, kVideoClass))
        fail = "not the Video page";
    else if (find(text, length, kStandalone))
        fail = "a plugin's own page on the Video page class";
    else if (find(text, length, "AZG_GfxFrameRate"))
        fail = "it already has the Azimuth rows";
    else if (!find(text, length, kTitle) || !find(text, length, kBrightness) ||
             find(text, length, "<StackPanel Visibility=\"Collapsed\"") || find(text, length, "EO_KbmMode") ||
             find(text, length, "EO_WorldTimeOfDay"))
        // The Extra Options fork's KEYBOARD & MOUSE and World pages
        // (keyboard-mouse, time-weather) are built from the Video page XAML.
        fail = "no Video title or brightness preview: a page built from it";
    else if (!resources || !list || resources > list)
        fail = "no resources before the options list";
    else if (!list_end || !opening_end || !height)
        fail = "the options list has no 110/275 px height";
    if (fail) {
        if (why) *why = fail;
        return 0;
    }
    return write_page(text, length, output, capacity, kVideoPage, language, live, resources, rows_at, style, height,
                      style ? "Height=\"440\"" : "Height=\"275\"", why);
}

size_t extend_audio(const char *text, size_t length, char *output, size_t capacity, Language language,
                    const LiveState &live, const char **why) {
    static const char kTitle[] = "Content=\"{local:Loc ScreenTitles.IDS_AudioOptions}\"";
    const char *fail = nullptr;
    const char *end = text ? text + length : nullptr;
    const char *resources = find(text, length, "</ResourceDictionary>");
    const char *list = find(text, length, kList);
    const char *list_end = list ? find_in(list, end, kListEnd) : nullptr;
    const char *opening_end = list_end ? find_in(list, list_end, ">") : nullptr;
    const char *style = opening_end ? find_in(list, opening_end, kFixedStyle) : nullptr;
    if (!text || !output || capacity < 4 || !find(text, length, kAudioClass))
        fail = "not the Audio page";
    else if (find(text, length, kStandalone))
        fail = "a plugin's own page on the Audio page class";
    else if (find(text, length, "AZG_AzAudioOutput") || find(text, length, "AZG_AzVolume"))
        fail = "it already has the Azimuth rows";
    else if (!find(text, length, kTitle))
        fail = "no Audio title";
    else if (!resources || !list || resources > list)
        fail = "no resources before the options list";
    else if (!list_end || !opening_end || find_in(opening_end, list_end, kList))
        fail = "the options list is not as expected";
    if (fail) {
        if (why) *why = fail;
        return 0;
    }
    return write_page(text, length, output, capacity, kAudioPage, language, live, resources, list_end, style, nullptr,
                      nullptr, why);
}

bool install(unsigned char *game, const LiveState &live, bool apply) {
    image = game;
    live_text = live;
    live_apply = apply;
    // previous_reader is set before the site is swapped (the game may read a
    // page on another thread at once).
    ChainResult result = chain_call(game + kResourceCall, (void *)read_resource, (void **)&previous_reader);
    if (result != kChained) {
        say(std::string("the XAML resource call could not be chained (") + chain_text(result) +
            "); the settings pages are left as they are");
        return false;
    }
    return true;
}

} // namespace pages
} // namespace azg
