// Azimuth Graphics Options for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native calls and object layouts are those Extra Options 0.3.2 uses for its
// own Video page rows (features/video_options.c), checked here before use.
#include "video_page.h"

#include <cstddef>
#include <cstring>
#include <string>

#include "call_chain.h"
#include "common.h"
#include "live.h"

namespace azg {
namespace video {

namespace {

enum : uint32_t {
    kDeleteObject = 0x257730,
    kCollectionType = 0x1476110,
    kAllocateTyped = 0x14424e0,
    kCollectionInit = 0xaefd60,
    kMakeString = 0x13fe8e0,
    kInsertItem = 0xb52170,
    kRegisterVideoType = 0xeca710,
    kIntProperty = 0xed81f0,
    kCollectionProperty = 0xed77d0,
    kRegisterProperty = 0x155ee30,
    kAllocate = 0x3229c0,
    kFree = 0x31c7b0,
    kObjectInit = 0x143eb80,
    kObjectCleanup = 0x1442050,
    kBoxInt = 0x4702d0,
    kPropertyChanged = 0xcf9c60,
    kGetResource = 0xaabec0,
    kVideoRegistry = 0x35470d0,
    kTextTables = 0x3662990,
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
    {kIntProperty, "IntProperty", {0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe}},
    {kCollectionProperty, "CollectionProperty", {0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe}},
    {kRegisterProperty, "RegisterProperty", {0x48, 0x8b, 0x41, 0x40, 0x48, 0x89, 0x42, 0x08, 0x48, 0x89, 0x51, 0x40}},
    {kAllocate, "Allocate", {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x8b, 0x15}},
    {kFree, "Free", {0x48, 0x85, 0xc9, 0x74, 0x1d, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b}},
    {kObjectInit, "ObjectInit", {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8d}},
    {kObjectCleanup, "ObjectCleanup", {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8d, 0x05, 0xb3, 0xd0, 0x9f}},
    {kBoxInt, "BoxInt", {0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xd9, 0xc7, 0x44, 0x24}},
    {kPropertyChanged, "PropertyChanged", {0x40, 0x57, 0x48, 0x83, 0xec, 0x60, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe}},
    {kGetResource, "GetResource", {0x48, 0x83, 0xec, 0x18, 0x48, 0x8b, 0x41, 0x38, 0xc7, 0x04, 0x24, 0x00}},
};
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

typedef void **(*GetResource)(void *reader, void **output);
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

unsigned char *image;
GetResource previous_reader;
LiveState live_text;
bool live_apply;
NativeProperty properties[2 * kRowCount]; // values, then choices
NativePropertyLink links[2 * kRowCount];
void *choices[kRowCount];
volatile LONG registered, told_layout, told_failure, told_added;
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
    if (!boxed || row >= kRowCount || !safe_read((unsigned char *)boxed + 0x28, &value, sizeof(value))) return;
    if (!rows::set(row, value)) return;
    if (live_apply) live::apply(row, value);
    native<PropertyChanged>(kPropertyChanged)(page, property);
    // Shadows and Texture filtering switch Graphics to Custom.
    if (row != kPreset) native<PropertyChanged>(kPropertyChanged)(page, &properties[kPreset]);
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

// Once per process, before the page's XAML is parsed.
bool register_properties(Language language) {
    if (InterlockedCompareExchange(&registered, 0, 0)) return true;
    AcquireSRWLockExclusive(&registration_lock);
    bool ok = registered != 0;
    if (!ok) {
        native<void (*)(void)>(kRegisterVideoType)();
        ok = true;
        for (unsigned row = 0; row < kRowCount && ok; ++row)
            if (rows::shown(row) && !make_choices(row, language)) ok = false;
        if (!ok) {
            for (unsigned row = 0; row < kRowCount; ++row) {
                release_object(choices[row]);
                choices[row] = nullptr;
            }
        } else {
            NativeProperty *int_kind = (NativeProperty *)native<Factory>(kIntProperty)();
            NativeProperty *list_kind = (NativeProperty *)native<Factory>(kCollectionProperty)();
            ok = int_kind && list_kind;
            for (unsigned index = 0; index < 2 * kRowCount && ok; ++index) {
                unsigned row = index % kRowCount;
                if (!rows::shown(row)) continue;
                bool value = index < kRowCount;
                NativeProperty *property = &properties[index];
                const NativeProperty *kind = value ? int_kind : list_kind;
                native<ObjectFunction>(kObjectInit)(property);
                property->vtable = kind->vtable;
                memcpy((unsigned char *)property + 0x28, (const unsigned char *)kind + 0x28, 0x30);
                property->name = value ? rows::property(row) : rows::collection(row);
                property->getter = (void *)get_value;
                property->setter = value ? (void *)set_value : nullptr;
                links[index].property = property;
                native<RegisterProperty>(kRegisterProperty)(image + kVideoRegistry, &links[index]);
            }
            if (ok) InterlockedExchange(&registered, 1);
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

// Once, when the Video page is first read (Azimuth's window is up by then).
void prepare_frame_rates() { InitOnceExecuteOnce(&rates_once, find_frame_rates, nullptr, nullptr); }

void **read_resource(void *reader, void **output) {
    previous_reader(reader, output);
    NativeResource *original = (NativeResource *)*output;
    if (!original || !original->bytes || original->length <= 0 || original->length > (1 << 20) ||
        !find(original->bytes, (size_t)original->length, kVideoClass))
        return output;
    size_t capacity = (size_t)original->length + 0x10000;
    NativeResource *replacement = (NativeResource *)native<Allocate>(kAllocate)(sizeof(NativeResource) + capacity);
    if (!replacement) return output;
    char *bytes = (char *)(replacement + 1);
    prepare_frame_rates();
    Language language = game_language(image);
    const char *why = nullptr;
    size_t length = extend_xaml(original->bytes, (size_t)original->length, bytes, capacity, language, live_text, &why);
    if (!length) {
        native<Free>(kFree)(replacement);
        if (!InterlockedExchange(&told_layout, 1))
            say(std::string("the Video page is not as expected (") + (why ? why : "?") + "); its rows are not added");
        return output;
    }
    if (!register_properties(language)) {
        native<Free>(kFree)(replacement);
        if (!InterlockedExchange(&told_failure, 1)) say("the Video page properties could not be made; rows not added");
        return output;
    }
    // As Extra Options does: the game's resource object with this buffer,
    // owned by the parser through the native reference count.
    native<ObjectFunction>(kObjectInit)(replacement);
    memcpy(resource_vtable, original->vtable - 1, sizeof(resource_vtable));
    resource_vtable[1] = (void *)delete_resource; // vtable slot 0, after the type information
    replacement->vtable = resource_vtable + 1;
    replacement->bytes = bytes;
    replacement->length = (int)length;
    replacement->capacity = (int)capacity;
    *output = replacement;
    release_object(original);
    if (!InterlockedExchange(&told_added, 1))
        say(std::string("Azimuth rows added to the Video page (") + (language == kPolish ? "Polish" : "English") + ")");
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

void add_resources(Buffer &out, Language language, const LiveState &live) {
    for (unsigned row = 0; row < kRowCount; ++row) {
        if (!rows::shown(row)) continue;
        out.add("<local:TileMenuDataItem x:Key=\"EO_");
        out.add(rows::property(row));
        out.add("\" Heading=\"");
        out.attribute(rows::name(row, language));
        out.add("\" Description=\"");
        out.attribute(rows::description(row, language, live).c_str());
        out.add("\"/>");
    }
}

void add_rows(Buffer &out, Language language) {
    for (unsigned row = 0; row < kRowCount; ++row) {
        if (!rows::shown(row)) continue;
        out.add("<local:SpinnerControl Tag=\"{StaticResource EO_");
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
    unsigned char code[5];
    if (!safe_read(game + kResourceCall, code, 5) || code[0] != 0xe8 ||
        !bytes_equal(game + kResourceCall + 5, kAfterCall, sizeof(kAfterCall)))
        return "the XAML resource call at 0x16ebb37 is not as expected";
    const unsigned char *called = (const unsigned char *)call_target(game + kResourceCall);
    static std::string what;
    if (called == game + kGetResource) {
        what = "the game's resource reader 0xaabec0 (Extra Options' Video hook is not there)";
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

Language game_language(const unsigned char *game) {
    void *manager = nullptr;
    unsigned char code[32];
    if (!game || !safe_read(game + kTextTables, &manager, sizeof(manager)) || !manager ||
        !safe_read((unsigned char *)manager + 0x30, code, sizeof(code)))
        return kEnglish;
    size_t length, capacity;
    memcpy(&length, code + 16, sizeof(length));
    memcpy(&capacity, code + 24, sizeof(capacity));
    if (capacity != 15 || length != 2 || code[2] != 0) return kEnglish;
    return (code[0] | 0x20) == 'p' && (code[1] | 0x20) == 'l' ? kPolish : kEnglish;
}

size_t extend_xaml(const char *text, size_t length, char *output, size_t capacity, Language language,
                   const LiveState &live, const char **why) {
    static const char kTitle[] = "Content=\"{local:Loc ScreenTitles.IDS_VideoOptions}\"";
    static const char kBrightness[] = "A_Style_ContentControl_BrightnessSquare";
    static const char kList[] = "<local:CommonListBox x:Name=\"OptionsList\"";
    static const char kFixedStyle[] = "Style=\"{StaticResource A_Style_ListBox_List_Default}\"";
    static const char kExtraOptionsRow[] = "<local:SpinnerControl Tag=\"{StaticResource EO_";
    const char *fail = nullptr;
    const char *end = text + length;
    const char *resources = find(text, length, "</ResourceDictionary>");
    const char *list = find(text, length, kList);
    const char *list_end = list ? find_in(list, end, "</local:CommonListBox>") : nullptr;
    const char *opening_end = list_end ? find_in(list, list_end, ">") : nullptr;
    const char *height = opening_end ? find_in(list, opening_end, "Height=\"110\"") : nullptr;
    if (!height && opening_end) height = find_in(list, opening_end, "Height=\"275\"");
    const char *style = opening_end ? find_in(list, opening_end, kFixedStyle) : nullptr;
    const char *rows_at = opening_end ? find_in(opening_end, list_end, kExtraOptionsRow) : nullptr;
    if (!rows_at) rows_at = list_end;
    if (!text || !output || capacity < 4 || !find(text, length, kVideoClass))
        fail = "not the Video page";
    else if (find(text, length, "EO_GfxFrameRate"))
        fail = "it already has the Azimuth rows";
    else if (!find(text, length, kTitle) || !find(text, length, kBrightness) ||
             find(text, length, "<StackPanel Visibility=\"Collapsed\"") || find(text, length, "EO_KbmMode") ||
             find(text, length, "EO_WorldTimeOfDay"))
        // Extra Options' KEYBOARD & MOUSE and World pages (keyboard-mouse,
        // time-weather) are built from the Video page XAML.
        fail = "no Video title or brightness preview: a page built from it";
    else if (!resources || !list || resources > list)
        fail = "no resources before the options list";
    else if (!list_end || !opening_end || !height)
        fail = "the options list has no 110/275 px height";
    if (fail) {
        if (why) *why = fail;
        return 0;
    }
    Buffer out = {output, 0, capacity, true};
    static const char bom[] = "\xef\xbb\xbf"; // the game's XML reader reads UTF-8 only after it
    if (length < 3 || memcmp(text, bom, 3)) out.add(bom, 3);
    for (const char *at = text; at < end && out.valid;) {
        if (at == resources) add_resources(out, language, live);
        if (at == rows_at) add_rows(out, language);
        if (at == style) {
            // Many rows now: the scrollable list style instead of the fixed one.
            out.add("Style=\"{StaticResource A_Style_ListBox_Scrollable}\"");
            at += sizeof(kFixedStyle) - 1;
        } else if (at == height) {
            out.add(style ? "Height=\"440\"" : "Height=\"275\"");
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

bool install(unsigned char *game, const LiveState &live, bool apply) {
    image = game;
    live_text = live;
    live_apply = apply;
    // previous_reader is set before the site is swapped (the game may read a
    // page on another thread at once).
    ChainResult result = chain_call(game + kResourceCall, (void *)read_resource, (void **)&previous_reader);
    if (result != kChained) {
        say(std::string("the XAML resource call could not be chained (") + chain_text(result) +
            "); the Video page is left as it is");
        return false;
    }
    return true;
}

} // namespace video
} // namespace azg
