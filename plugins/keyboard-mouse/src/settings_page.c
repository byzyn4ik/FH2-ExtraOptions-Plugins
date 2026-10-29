// Keyboard & Mouse for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings_page.h"
#include "kbm_menu.h"
#include "kbm_text.h"
#include "patch.h"

#include <stdio.h>
#include <string.h>

enum {
    RESOURCE_CALL = 0x16ebb37,
    GET_RESOURCE = 0xaabec0,
    EXTRA_OPTIONS_THUNK = 0x13fe354,
    RESET_SLOT = 0x2e60900,   // VideoOptions vtable 0x2e60370 + 0x590
    RESET_VIDEO = 0xebf060,
    NATIVE_ALLOCATE = 0x3229c0,
    NATIVE_FREE = 0x31c7b0,
    OBJECT_INIT = 0x143eb80,
    OBJECT_CLEANUP = 0x1442050,
    DELETE_OBJECT = 0x257730,
    PAGE_TYPE_INIT = 0xeca710,  // VideoOptions type set-up (runs once, guarded by its own flag)
    PAGE_REGISTRY = 0x35470d0,
    PAGE_LEA = 0xeca808,        // lea rcx,[PAGE_REGISTRY] ...
    PAGE_CALL = 0xeca80f,       // ... call REGISTER_PROPERTY
    REGISTER_PROPERTY = 0x155ee30,
    INT_TEMPLATE = 0xed81f0,    // returns the page's Subtitles int property
    INT_TEMPLATE_NAME = 0xed825b,
    LIST_TEMPLATE = 0xed77d0,   // returns the page's OnOffOptions (list-valued) property
    LIST_TEMPLATE_NAME = 0xed782e,
    BOX_INT = 0x4702d0,         // (out, &value) -> boxed int
    PROPERTY_CHANGED = 0xcf9c60,// (page, property): bindings read it again
    COLLECTION_TYPE = 0x1476110,
    ALLOCATE_TYPED = 0x14424e0,
    COLLECTION_INIT = 0xaefd60,
    MAKE_STRING = 0x13fe8e0,
    INSERT_ITEM = 0xb52170,
    MAX_XAML = 1 << 20,
    EXTRA_ROOM = 0x20000,
};

typedef struct {
    uint32_t rva;
    const char *name;
    unsigned length;
    const unsigned char *bytes;
} Code;

#define CODE(rva, name, ...) {rva, name, sizeof((const unsigned char[]){__VA_ARGS__}), (const unsigned char[]){__VA_ARGS__}}
static const Code expected_code[] = {
    CODE(NATIVE_ALLOCATE, "allocate", 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x8b, 0x15),
    CODE(NATIVE_FREE, "free", 0x48, 0x85, 0xc9, 0x74, 0x1d, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b),
    CODE(OBJECT_INIT, "object init", 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8d),
    CODE(OBJECT_CLEANUP, "object cleanup", 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8d, 0x05, 0xb3, 0xd0, 0x9f),
    CODE(DELETE_OBJECT, "delete object", 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0xe8, 0xb2, 0xa2),
    CODE(PAGE_TYPE_INIT, "VideoOptions type set-up", 0x40, 0x53, 0x48, 0x81, 0xec, 0xd0, 0x00, 0x00, 0x00, 0x48, 0xc7,
         0x44, 0x24, 0x20, 0xfe, 0xff),
    CODE(REGISTER_PROPERTY, "register property", 0x48, 0x8b, 0x41, 0x40, 0x48, 0x89, 0x42, 0x08, 0x48, 0x89, 0x51,
         0x40, 0xc3),
    CODE(INT_TEMPLATE, "int property template", 0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20,
         0xfe, 0xff, 0xff, 0xff, 0x48),
    CODE(LIST_TEMPLATE, "list property template", 0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20,
         0xfe, 0xff, 0xff, 0xff, 0x48),
    CODE(BOX_INT, "box int", 0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xd9, 0xc7, 0x44, 0x24),
    CODE(PROPERTY_CHANGED, "property changed", 0x40, 0x57, 0x48, 0x83, 0xec, 0x60, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe),
    CODE(COLLECTION_TYPE, "collection type", 0x48, 0x8d, 0x05, 0x49, 0x10, 0x21, 0x02, 0xc3),
    CODE(ALLOCATE_TYPED, "typed allocate", 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b),
    CODE(COLLECTION_INIT, "collection init", 0x48, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x56, 0x57, 0x48, 0x83, 0xec, 0x30),
    CODE(MAKE_STRING, "make string", 0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x28, 0xfe),
    CODE(INSERT_ITEM, "insert item", 0x48, 0x8b, 0xc4, 0x57, 0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00, 0x48),
    CODE(RESET_VIDEO, "Video page reset", 0x40, 0x57, 0x48, 0x83, 0xec, 0x60, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe),
    // ... mov [rdi],r15; mov rcx,[rax]; mov [rdi+8],rcx after the resource call.
    CODE(RESOURCE_CALL + 5, "code after the XAML resource call", 0x4c, 0x89, 0x3f, 0x48, 0x8b, 0x08, 0x48, 0x89,
         0x4f, 0x08),
};
static const unsigned char reader_prologue[] = {0x48, 0x83, 0xec, 0x18, 0x48, 0x8b, 0x41, 0x38, 0xc7, 0x04, 0x24, 0x00};
/* The list item type's vtables (EnumDataSource-like collection), as Extra
   Options builds its choice lists. */
static const unsigned item_vtables[] = {0x2db73f8, 0x2db7470, 0x2db74d0, 0x2db74e8, 0x2db7508};
static const unsigned item_offsets[] = {0, 0x30, 0x38, 0x40, 0x48};

typedef struct {
    void **vtable;
    unsigned char header[0x20];
    void *getter, *setter, *unused;
    const char *name;
    void *type;
    uint64_t flags;
} NativeProperty;

typedef struct Link {
    NativeProperty *property;
    struct Link *next;
} Link;

typedef struct {
    void **vtable;
    unsigned char header[0x20];
    char *bytes;
    int length;
    int capacity;
} NativeResource;

_Static_assert(sizeof(NativeProperty) == 0x58, "native property descriptor");
_Static_assert(offsetof(NativeResource, bytes) == 0x28, "native resource buffer");

typedef void **(*ReadResource)(void *reader, void **output);
typedef void (*PageFn)(void *page);
typedef struct {
    void *(*allocate)(size_t);
    void (*free_memory)(void *);
    void (*object_init)(void *);
    void (*object_cleanup)(void *);
    void (*delete_object)(void *);
    void (*type_init)(void);
    NativeProperty *(*int_template)(void);
    NativeProperty *(*list_template)(void);
    void (*register_property)(void *registry, Link *link);
    void **(*box_int)(void **output, const void *value);
    void (*property_changed)(void *page, void *property);
    void *(*collection_type)(void *);
    void *(*allocate_typed)(size_t size, void *type);
    void (*collection_init)(void *items);
    void **(*make_string)(void **output, const wchar_t *text);
    void (*insert_item)(void *items, int index, void *item);
    void *registry;
} Natives;

static unsigned char *game;
static Natives natives;
static ReadResource previous_reader;
static void *previous_reset;
static void *resource_vtable[7];
static NativeProperty value_properties[KBM_MENU_ROWS], list_properties[KBM_MENU_ROWS];
static Link value_links[KBM_MENU_ROWS], list_links[KBM_MENU_ROWS];
static void *choices[KBM_MENU_ROWS];
static volatile LONG registered, told_hub, told_page, told_layout, told_memory, told_properties;
#ifdef KBM_TEST
static Natives test_natives;
static int use_test_natives;
#endif

static int bytes_equal(const unsigned char *at, const unsigned char *expected, size_t size) {
    unsigned char copy[64];
    return size <= sizeof(copy) && memory_read(at, copy, size) && !memcmp(copy, expected, size);
}

static int in_game(const unsigned char *image, const void *address) {
    return (const unsigned char *)address >= image && (const unsigned char *)address < image + GAME_IMAGE_SIZE;
}

static void *call_target(const unsigned char *site) {
    unsigned char code[5];
    int32_t displacement;
    if (!memory_read(site, code, sizeof(code)) || code[0] != 0xe8) return NULL;
    memcpy(&displacement, code + 1, 4);
    return (void *)(site + 5 + displacement);
}

static uint32_t rip_target(const unsigned char *image, uint32_t at, unsigned length) {
    int32_t displacement;
    memcpy(&displacement, image + at + length - 4, 4);
    return (uint32_t)((int64_t)at + length + displacement);
}

// The thunk Extra Options places at 0x13fe354: mov rax, <its reader>; jmp rax.
static void *extra_options_reader(const unsigned char *image) {
    unsigned char thunk[12];
    void *hook;
    if (!memory_read(image + EXTRA_OPTIONS_THUNK, thunk, sizeof(thunk)) || thunk[0] != 0x48 || thunk[1] != 0xb8 ||
        thunk[10] != 0xff || thunk[11] != 0xe0)
        return NULL;
    memcpy(&hook, thunk + 2, sizeof(hook));
    return !in_game(image, hook) && patch_is_code(hook) ? hook : NULL;
}

const char *settings_page_check(const unsigned char *image, const char **resource_target, const char **reset_target) {
    static char problem[200], resource_text[200], reset_text[160];
    char where[120];
    if (resource_target) *resource_target = NULL;
    if (reset_target) *reset_target = NULL;
    for (unsigned i = 0; i < ARRAY_COUNT(expected_code); ++i) {
        const Code *code = &expected_code[i];
        if (!bytes_equal(image + code->rva, code->bytes, code->length)) {
            snprintf(problem, sizeof(problem), "the game's %s at 0x%x is not as expected", code->name, code->rva);
            return problem;
        }
    }
    unsigned char lea[7];
    if (!memory_read(image + PAGE_LEA, lea, sizeof(lea)) || lea[0] != 0x48 || lea[1] != 0x8d || lea[2] != 0x0d ||
        rip_target(image, PAGE_LEA, 7) != PAGE_REGISTRY || call_target(image + PAGE_CALL) != image + REGISTER_PROPERTY)
        return "the VideoOptions type set-up does not register on 0x35470d0";
    static const struct {
        uint32_t lea;
        const char *name;
    } templates[] = {{INT_TEMPLATE_NAME, "Subtitles"}, {LIST_TEMPLATE_NAME, "OnOffOptions"}};
    for (unsigned i = 0; i < ARRAY_COUNT(templates); ++i) {
        if (!memory_read(image + templates[i].lea, lea, sizeof(lea)) || lea[0] != 0x48 || lea[1] != 0x8d ||
            lea[2] != 0x05 ||
            !bytes_equal(image + rip_target(image, templates[i].lea, 7), (const unsigned char *)templates[i].name,
                         strlen(templates[i].name) + 1))
            return "the property templates do not name Subtitles and OnOffOptions";
    }
    for (unsigned i = 0; i < ARRAY_COUNT(item_vtables); ++i) {
        void *first = NULL;
        if (!memory_read(image + item_vtables[i], &first, sizeof(first)) || !in_game(image, first))
            return "the list item vtables are not as expected";
    }

    const unsigned char *called = call_target(image + RESOURCE_CALL);
    if (!called) return "the XAML resource site 0x16ebb37 is not a call";
    if (called == image + GET_RESOURCE) {
        if (!bytes_equal(called, reader_prologue, sizeof(reader_prologue)))
            return "the game's resource reader at 0xaabec0 is not as expected";
        snprintf(resource_text, sizeof(resource_text), "the game's resource reader 0x%x", GET_RESOURCE);
    } else if (called == image + EXTRA_OPTIONS_THUNK) {
        void *hook = extra_options_reader(image);
        if (!hook) return "the XAML resource call goes to 0x13fe354, which is not Extra Options' thunk";
        patch_describe(hook, where, sizeof(where));
        snprintf(resource_text, sizeof(resource_text), "Extra Options' resource thunk 0x%x -> %s", EXTRA_OPTIONS_THUNK,
                 where);
    } else if (!in_game(image, called) && patch_is_code(called)) {
        patch_describe(called, where, sizeof(where));
        snprintf(resource_text, sizeof(resource_text), "another module's hook at %s", where);
    } else {
        return "the XAML resource call goes somewhere unexpected";
    }

    void *reset = NULL;
    if (!memory_read(image + RESET_SLOT, &reset, sizeof(reset))) return "the Video page reset slot cannot be read";
    if (reset == image + RESET_VIDEO) {
        snprintf(reset_text, sizeof(reset_text), "the game's Video page reset 0x%x", RESET_VIDEO);
    } else if (!in_game(image, reset) && patch_is_code(reset)) {
        patch_describe(reset, where, sizeof(where));
        snprintf(reset_text, sizeof(reset_text), "a hook at %s (Extra Options' or another plugin's)", where);
    } else {
        return "the Video page reset slot 0x2e60900 holds neither the game's reset nor executable code";
    }
    if (resource_target) *resource_target = resource_text;
    if (reset_target) *reset_target = reset_text;
    return NULL;
}

// ------------------------------------------------------------ properties

static void release_object(void *object) {
    if (object && InterlockedDecrement((volatile LONG *)((unsigned char *)object + 0x10)) == 0)
        natives.delete_object(object);
}

static void add_reference(void *object) {
    if (object) InterlockedIncrement((volatile LONG *)((unsigned char *)object + 0x10));
}

static int is_key_row(unsigned row) { return row >= KBM_MENU_OPTION_ROWS && row < KBM_MENU_ROWS; }

/* The KEYBOARD & MOUSE page object, held with a reference so the key capture
   can update it from the UI tick (kbm_rebind.c); older pages are released by
   the tick once the game has let them go. */
static void *rebind_page, *retired_pages[4];

static void track_page(void *page) {
    if (!page || page == rebind_page) return;
    add_reference(page);
    if (rebind_page) {
        release_object(retired_pages[ARRAY_COUNT(retired_pages) - 1]);
        for (unsigned i = ARRAY_COUNT(retired_pages) - 1; i; --i) retired_pages[i] = retired_pages[i - 1];
        retired_pages[0] = rebind_page;
    }
    rebind_page = page;
}

static int only_reference(void *object) {
    LONG references = 0;
    return !memory_read((unsigned char *)object + 0x10, &references, sizeof(references)) || references <= 1;
}

void *settings_page_rebind_page(int collect) {
    for (unsigned i = 0; collect && i < ARRAY_COUNT(retired_pages); ++i) {
        if (retired_pages[i] && only_reference(retired_pages[i])) {
            release_object(retired_pages[i]);
            retired_pages[i] = NULL;
        }
    }
    if (collect && rebind_page && only_reference(rebind_page)) {
        release_object(rebind_page);
        rebind_page = NULL;
    }
    return rebind_page;
}

static void notify_key_rows(void *page) {
    if (!page || !registered) return;
    for (unsigned row = KBM_MENU_OPTION_ROWS; row < KBM_MENU_ROWS; ++row)
        if (kbm_menu_take_dirty(row)) natives.property_changed(page, &value_properties[row]);
}

void settings_page_rebind_tick(void) { notify_key_rows(settings_page_rebind_page(0)); }

static void *make_choices(unsigned row) {
    void *type = natives.collection_type(NULL);
    void *items = natives.allocate_typed(0x58, type);
    if (!items) return NULL;
    natives.collection_init(items);
    for (unsigned i = 0; i < ARRAY_COUNT(item_vtables); ++i)
        *(void **)((unsigned char *)items + item_offsets[i]) = game + item_vtables[i];
    unsigned count = kbm_menu_choice_count(row);
    for (unsigned index = 0; index < count; ++index) {
        void *label = NULL;
        const wchar_t *text = kbm_menu_choice(game, row, index);
        if (text) natives.make_string(&label, text);
        if (!label) {
            release_object(items);
            return NULL;
        }
        natives.insert_item(items, (int)index, label);
        release_object(label);
    }
    return items;
}

static void **get_value(void **output, NativeProperty *property, void *page) {
    *output = NULL;
    unsigned row = (unsigned)(property - value_properties);
    if (row >= KBM_MENU_ROWS) return output;
    track_page(page);
    int value = kbm_menu_get(row);
    return natives.box_int(output, &value);
}

static void **get_choices(void **output, NativeProperty *property, void *page) {
    *output = NULL;
    unsigned row = (unsigned)(property - list_properties);
    if (row >= KBM_MENU_ROWS) return output;
    track_page(page);
    if (is_key_row(row) && kbm_menu_labels_stale(row)) {
        /* A new list: its labels hold the row's keys as they are now. */
        void *fresh = make_choices(row);
        if (fresh) {
            void *old = choices[row];
            choices[row] = fresh;
            release_object(old);
        }
    }
    *output = choices[row];
    add_reference(*output);
    return output;
}

static void set_value(NativeProperty *property, void *page, void *boxed) {
    int value;
    unsigned row = (unsigned)(property - value_properties);
    if (!boxed || row >= KBM_MENU_ROWS) return;
    track_page(page);
    if (memory_read((unsigned char *)boxed + 0x28, &value, sizeof(value)) && kbm_menu_set(row, value)) {
        natives.property_changed(page, property);
        if (is_key_row(row)) notify_key_rows(page); /* Reset changes every key row */
    }
}

static int make_property(NativeProperty *property, const NativeProperty *template_property, const char *name,
                         void *getter, void *setter) {
    natives.object_init(property);
    property->vtable = template_property->vtable;
    memcpy((unsigned char *)property + 0x28, (const unsigned char *)template_property + 0x28, 0x30);
    property->name = name;
    property->getter = getter;
    property->setter = setter;
    return 1;
}

/* On the UI thread, before the page binds: the choice lists, then every
   property linked into the VideoOptions type. */
static int register_properties(void) {
    if (registered) return 1;
    natives.type_init();
    NativeProperty *int_template = natives.int_template(), *list_template = natives.list_template();
    if (!int_template || !int_template->vtable || !int_template->name || strcmp(int_template->name, "Subtitles") ||
        !list_template || !list_template->vtable || !list_template->name ||
        strcmp(list_template->name, "OnOffOptions")) {
        if (!InterlockedExchange(&told_properties, 1))
            log_append("KBM MENU: the Video page's property templates looked wrong at run time; no KEYBOARD & "
                       "MOUSE page.\r\n");
        return 0;
    }
    for (unsigned row = 0; row < KBM_MENU_ROWS; ++row) {
        if (!kbm_menu_collection(row)) continue;
        choices[row] = make_choices(row);
        if (!choices[row]) {
            for (unsigned j = 0; j < row; ++j) {
                release_object(choices[j]);
                choices[j] = NULL;
            }
            if (!InterlockedExchange(&told_properties, 1))
                log_append("KBM MENU: the page's choice lists could not be made; no KEYBOARD & MOUSE page.\r\n");
            return 0;
        }
    }
    for (unsigned row = 0; row < KBM_MENU_ROWS; ++row) {
        make_property(&value_properties[row], int_template, kbm_menu_property(row), (void *)(uintptr_t)get_value,
                      (void *)(uintptr_t)set_value);
        value_links[row].property = &value_properties[row];
        natives.register_property(natives.registry, &value_links[row]);
        if (!kbm_menu_collection(row)) continue;
        make_property(&list_properties[row], list_template, kbm_menu_collection(row),
                      (void *)(uintptr_t)get_choices, NULL);
        list_links[row].property = &list_properties[row];
        natives.register_property(natives.registry, &list_links[row]);
    }
    InterlockedExchange(&registered, 1);
    log_line("KBM MENU: %u KEYBOARD & MOUSE page properties registered on the VideoOptions type.",
             (unsigned)(2 * KBM_MENU_ROWS - 1));
    return 1;
}

static void reset_hook(void *page) {
    if (page && page == rebind_page && registered) {
        /* The KEYBOARD & MOUSE page's Reset to default: its options back to
           the plugin's defaults; the key rows have their own Reset row. */
        kbm_menu_reset();
        for (unsigned row = 0; row < KBM_MENU_OPTION_ROWS; ++row)
            natives.property_changed(page, &value_properties[row]);
        log_append("KBM MENU: KEYBOARD & MOUSE options reset to the defaults.\r\n");
        return;
    }
    ((PageFn)previous_reset)(page);
}

// ------------------------------------------------------------------- XAML

typedef struct {
    char *bytes;
    size_t length, capacity;
    int valid;
} Buffer;

static void append_bytes(Buffer *buffer, const char *text, size_t length) {
    if (!buffer->valid || length >= buffer->capacity - buffer->length) {
        buffer->valid = 0;
        return;
    }
    memcpy(buffer->bytes + buffer->length, text, length);
    buffer->length += length;
}

static void append_text(Buffer *buffer, const char *text) { append_bytes(buffer, text, strlen(text)); }

static void append_attribute(Buffer *buffer, const char *text) {
    for (; *text; ++text) {
        const char *escaped = *text == '&'   ? "&amp;"
                              : *text == '<' ? "&lt;"
                              : *text == '>' ? "&gt;"
                              : *text == '"' ? "&quot;"
                                             : NULL;
        append_bytes(buffer, escaped ? escaped : text, escaped ? strlen(escaped) : 1);
    }
}

static const char standalone_marker[] = "<!-- EO:StandaloneOptionsPage -->";

/* One row: its description resource (TileMenuDataItem) or its spinner. */
static void append_row(Buffer *buffer, unsigned char *image, unsigned row, int resource) {
    append_text(buffer, resource ? "<local:TileMenuDataItem x:Key=\"EO_"
                        : row    ? "<local:SpinnerControl Tag=\"{StaticResource EO_"
                                 : "<local:SpinnerControl x:Name=\"FirstItem\" Tag=\"{StaticResource EO_");
    append_text(buffer, kbm_menu_property(row));
    append_text(buffer, resource ? "\" Heading=\"" : "}\" Style=\"{StaticResource A_Style_SpinnerControl_Default}\" Label=\"");
    append_attribute(buffer, kbm_menu_name(image, row));
    if (resource) {
        append_text(buffer, "\" Description=\"");
        append_attribute(buffer, kbm_menu_description(image, row));
        append_text(buffer, "\"/>");
        return;
    }
    append_text(buffer, "\" SelectedIndex=\"{Binding ");
    append_text(buffer, kbm_menu_property(row));
    append_text(buffer, "}\" ItemsSource=\"{Binding ");
    append_text(buffer, kbm_menu_collection(row));
    append_text(buffer, "}\"/>");
}

/* The native Video page (as the game, Extra Options and other plugins left
   it) with its own title, the brightness preview hidden and its option list
   holding only the [KeyboardMouse] rows, the key binding rows and Reset. The
   list keeps its style, which scrolls to the focused row, and is made 440
   pixels high (eight rows); its SelectedIndex (the focused row) goes
   OneWayToSource to the list-selection tracker so Enter can tell which key
   row is focused. */
size_t settings_page_keyboard_xaml(unsigned char *image, const char *text, size_t length, char *output,
                                   size_t capacity) {
    static const char title[] = "Content=\"{local:Loc ScreenTitles.IDS_VideoOptions}\"";
    static const char panel[] = "<StackPanel";
    static const char list_open[] = "<local:CommonListBox";
    static const char list_name[] = "x:Name=\"OptionsList\"";
    static const char list_close[] = "</local:CommonListBox>";
    if (!capacity || !text_find(text, length, "x:Class=\"ForzaUI.A_VideoOptions\"") ||
        text_find(text, length, standalone_marker) || text_find(text, length, "EO_KbmMode"))
        return 0;
    const char *heading = text_find_unique(text, length, title);
    const char *resources = text_find(text, length, "</ResourceDictionary>");
    const char *name = text_find_unique(text, length, list_name);
    if (!heading || !resources || !name || resources >= heading || heading >= name) return 0;
    /* The brightness preview: the black StackPanel between the title and the
       list (Extra Options 0.4.4 rewrites its start tag, so it is found by its
       background, not by the game's exact text). */
    const char *black = text_find(heading, (size_t)(name - heading), "Background=\"Black\"");
    const char *preview = black;
    while (preview && preview > heading && memcmp(preview, panel, sizeof(panel) - 1)) --preview;
    if (!black || preview <= heading || text_find(preview, (size_t)(black - preview), ">") ||
        text_find(preview, (size_t)(black - preview), "Visibility="))
        return 0;
    // The list's start tag: <local:CommonListBox ... x:Name="OptionsList" ... >
    const char *list = name;
    while (list > text && *list != '<') --list;
    if ((size_t)(length - (size_t)(list - text)) < sizeof(list_open) - 1 ||
        memcmp(list, list_open, sizeof(list_open) - 1))
        return 0;
    const char *rows_end = text_find(name, length - (size_t)(name - text), list_close);
    const char *opening_end = rows_end ? text_find(name, (size_t)(rows_end - name), ">") : NULL;
    if (!opening_end || opening_end[-1] == '/' ||
        text_find(opening_end, (size_t)(rows_end - opening_end), list_open))
        return 0;
    size_t opening_length = (size_t)(opening_end - list);
    if (text_find(list, opening_length, "SelectedIndex=")) return 0;
    const char *height = text_find(list, opening_length, " Height=\"");
    const char *height_end = height ? text_find(height + 9, (size_t)(opening_end - height - 9), "\"") : NULL;
    if (height && !height_end) return 0;
    const char *items = opening_end + 1;
    const char *after_name = name + sizeof(list_name) - 1;
    const KbmText *localized = kbm_text(image);
    Buffer buffer = {output, 0, capacity, 1};
    // FH2's XML reader only decodes UTF-8 after its byte-order mark.
    size_t index = 0;
    append_bytes(&buffer, "\xef\xbb\xbf", 3);
    if (length >= 3 && !memcmp(text, "\xef\xbb\xbf", 3)) index = 3;
    append_text(&buffer, standalone_marker);
    append_text(&buffer, "\r\n");
    while (index < length && buffer.valid) {
        const char *at = text + index;
        if (at == resources) {
            for (unsigned row = 0; row < KBM_MENU_LIST_ROW; ++row) append_row(&buffer, image, row, 1);
        }
        if (at == after_name) {
            append_text(&buffer, " SelectedIndex=\"{Binding ");
            append_text(&buffer, kbm_menu_property(KBM_MENU_LIST_ROW));
            append_text(&buffer, ", Mode=OneWayToSource}\"");
            if (!height) append_text(&buffer, " Height=\"440\"");
        }
        if (height && at == height) {
            append_text(&buffer, " Height=\"440\""); /* eight 55-pixel rows */
            index = (size_t)(height_end + 1 - text);
        } else if (at == heading) {
            append_text(&buffer, "Content=\"");
            append_attribute(&buffer, localized->keyboard_mouse);
            append_text(&buffer, "\"");
            index += sizeof(title) - 1;
        } else if (at == preview) {
            append_text(&buffer, "<StackPanel Visibility=\"Collapsed\"");
            index += sizeof(panel) - 1;
        } else if (at == items) {
            for (unsigned row = 0; row < KBM_MENU_LIST_ROW; ++row) append_row(&buffer, image, row, 0);
            index = (size_t)(rows_end - text);
        } else {
            append_bytes(&buffer, at, 1);
            ++index;
        }
    }
    if (!buffer.valid) return 0;
    output[buffer.length] = 0;
    return buffer.length;
}

// ------------------------------------------------------------- the reader

static void *delete_resource(NativeResource *resource, unsigned flags) {
    natives.object_cleanup(resource);
    if (flags & 1) natives.free_memory(resource);
    return resource;
}

static int is_video_page(const char *text, size_t length) {
    return text_find(text, length, "x:Class=\"ForzaUI.A_VideoOptions\"") && !text_find(text, length, standalone_marker);
}

static void **read_resource(void *reader, void **output) {
    previous_reader(reader, output);
    NativeResource *original = output ? (NativeResource *)*output : NULL;
    if (!original || !original->bytes || original->length <= 0 || original->length > MAX_XAML) return output;
    size_t length = (size_t)original->length;
    int hub = kbm_menu_is_hub_xaml(original->bytes, length);
    int keyboard = !hub && is_video_page(original->bytes, length) && kbm_menu_take_request();
    if (!hub && !keyboard) return output;
    if (keyboard && !register_properties()) return output;
    size_t capacity = length + EXTRA_ROOM;
    NativeResource *replacement = natives.allocate(sizeof(NativeResource) + capacity);
    if (!replacement) {
        if (!InterlockedExchange(&told_memory, 1)) log_append("KBM MENU: no memory for the page copy; left as it is.\r\n");
        return output;
    }
    char *bytes = (char *)(replacement + 1);
    size_t made = hub ? kbm_menu_hub_xaml(game, original->bytes, length, bytes, capacity)
                      : settings_page_keyboard_xaml(game, original->bytes, length, bytes, capacity);
    if (!made) {
        natives.free_memory(replacement);
        if (keyboard && !InterlockedExchange(&told_layout, 1))
            log_append("KBM MENU: Video page layout did not match; the Video page is shown instead.\r\n");
        return output;
    }
    if (keyboard) kbm_menu_page_built();
    // As Extra Options does: the game's resource object with this buffer,
    // owned by the parser through the native reference count.
    natives.object_init(replacement);
    memcpy(resource_vtable, original->vtable - 1, sizeof(resource_vtable));
    resource_vtable[1] = (void *)(uintptr_t)delete_resource;  // vtable slot 0, after the type information
    replacement->vtable = resource_vtable + 1;
    replacement->bytes = bytes;
    replacement->length = (int)made;
    replacement->capacity = (int)capacity;
    *output = replacement;
    release_object(original);
    if (hub ? !InterlockedExchange(&told_hub, 1) : !InterlockedExchange(&told_page, 1))
        log_append(hub ? "KBM MENU: KEYBOARD & MOUSE tile placed next to Controls in the Settings hub.\r\n"
                       : "KBM MENU: KEYBOARD & MOUSE page built from the Video page.\r\n");
    return output;
}

int settings_page_install(unsigned char *image) {
    game = image;
    natives.allocate = (void *(*)(size_t))(void *)(image + NATIVE_ALLOCATE);
    natives.free_memory = (void (*)(void *))(void *)(image + NATIVE_FREE);
    natives.object_init = (void (*)(void *))(void *)(image + OBJECT_INIT);
    natives.object_cleanup = (void (*)(void *))(void *)(image + OBJECT_CLEANUP);
    natives.delete_object = (void (*)(void *))(void *)(image + DELETE_OBJECT);
    natives.type_init = (void (*)(void))(void *)(image + PAGE_TYPE_INIT);
    natives.int_template = (NativeProperty * (*)(void))(void *)(image + INT_TEMPLATE);
    natives.list_template = (NativeProperty * (*)(void))(void *)(image + LIST_TEMPLATE);
    natives.register_property = (void (*)(void *, Link *))(void *)(image + REGISTER_PROPERTY);
    natives.box_int = (void **(*)(void **, const void *))(void *)(image + BOX_INT);
    natives.property_changed = (void (*)(void *, void *))(void *)(image + PROPERTY_CHANGED);
    natives.collection_type = (void *(*)(void *))(void *)(image + COLLECTION_TYPE);
    natives.allocate_typed = (void *(*)(size_t, void *))(void *)(image + ALLOCATE_TYPED);
    natives.collection_init = (void (*)(void *))(void *)(image + COLLECTION_INIT);
    natives.make_string = (void **(*)(void **, const wchar_t *))(void *)(image + MAKE_STRING);
    natives.insert_item = (void (*)(void *, int, void *))(void *)(image + INSERT_ITEM);
    natives.registry = image + PAGE_REGISTRY;
#ifdef KBM_TEST
    if (use_test_natives) natives = test_natives;
#endif
    // The reset first: alone it changes nothing until a page is tracked.
    const char *problem = patch_slot((void **)(image + RESET_SLOT), image + RESET_VIDEO, image, GAME_IMAGE_SIZE,
                                     (void *)(uintptr_t)reset_hook, &previous_reset);
    if (problem) {
        log_line("KBM MENU: the Video page reset slot 0x%x could not be chained (%s); no KEYBOARD & MOUSE page.",
                 RESET_SLOT, problem);
        return 0;
    }
    problem = patch_call_also(image + RESOURCE_CALL, image + GET_RESOURCE,
                              extra_options_reader(image) ? image + EXTRA_OPTIONS_THUNK : NULL, image, GAME_IMAGE_SIZE,
                              (void *)(uintptr_t)read_resource, (void **)&previous_reader);
    if (problem) {
        log_line("KBM MENU: the XAML resource call 0x%x could not be chained (%s); no KEYBOARD & MOUSE tile.",
                 RESOURCE_CALL, problem);
        return 0;
    }
    return 1;
}

#ifdef KBM_TEST
__declspec(dllexport) void KbmTestPageNatives(const Natives *given) {
    test_natives = *given;
    use_test_natives = 1;
}

__declspec(dllexport) const char *KbmTestPageCheck(const unsigned char *image, int which) {
    const char *resource = NULL, *reset = NULL;
    const char *problem = settings_page_check(image, &resource, &reset);
    return problem ? problem : which ? reset : resource;
}

__declspec(dllexport) size_t KbmTestKeyboardXaml(const char *text, size_t length, char *out, size_t capacity) {
    return settings_page_keyboard_xaml(NULL, text, length, out, capacity);
}

/* 0 reader, 1 reset, 2 value getter, 3 list getter, 4 setter. */
__declspec(dllexport) void *KbmTestPageHandlers(int which) {
    void *handlers[] = {(void *)(uintptr_t)read_resource, (void *)(uintptr_t)reset_hook, (void *)(uintptr_t)get_value,
                        (void *)(uintptr_t)get_choices, (void *)(uintptr_t)set_value};
    return which >= 0 && which < 5 ? handlers[which] : NULL;
}

__declspec(dllexport) void *KbmTestPageProperty(unsigned row, int list) {
    return row < KBM_MENU_ROWS ? (list ? (void *)&list_properties[row] : (void *)&value_properties[row]) : NULL;
}

__declspec(dllexport) void *KbmTestPagePrevious(int which) {
    return which ? previous_reset : (void *)(uintptr_t)previous_reader;
}
#endif
